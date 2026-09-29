/* Report-guided, opt-in local LUT composition. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "json11.hpp"
#include "timing.h"
#include "enable_replication_policy.h"
#include "local_remap_policy.h"
#include "local_remap_pin_policy.h"
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Lab = std::pair<int, int>;
const std::vector<IdString> pins = {id_A, id_B, id_C, id_D, id_E, id_F};
int width(IdString t)
{
    if (t == id_MISTRAL_ALUT2) return 2;
    if (t == id_MISTRAL_ALUT3) return 3;
    if (t == id_MISTRAL_ALUT4) return 4;
    if (t == id_MISTRAL_ALUT5) return 5;
    if (t == id_MISTRAL_ALUT6) return 6;
    return 0;
}
std::map<std::string, int> holds(TimingAnalyser &t)
{
    std::map<std::string, int> result;
    for (const auto &p : t.get_timing_result().min_delay_violations) {
        if (p.segments.empty()) continue;
        int slack = 0;
        for (auto &s : p.segments) slack += s.delay;
        auto ep = p.segments.back().to;
        std::string key = std::to_string(p.clock_pair.start.clock.index) + ":" +
            std::to_string(int(p.clock_pair.start.edge)) + ":" + std::to_string(p.clock_pair.end.clock.index) + ":" +
            std::to_string(int(p.clock_pair.end.edge)) + ":" + std::to_string(ep.first.index) + ":" + std::to_string(ep.second.index);
        if (!result.count(key)) result.emplace(key, slack);
        else result.at(key) = std::min(result.at(key), slack);
    }
    return result;
}
}

bool Arch::remap_critical(const std::string &report, int selection, int group_budget)
{
    namespace policy = local_remap_policy;
    Context *ctx = getCtx();
    if (group_budget < 1 || group_budget > 8) log_error("Local remap group budget must be 1..8.\n");
    if (selection < -1) log_error("Local remap selection must be -1 (list) or nonnegative.\n");
    if (fes_any_slot_region_active) log_error("Local remap requires ordinary full-design placement.\n");
    std::string error;
    auto json = json11::Json::parse(report, error);
    if (!error.empty() || !json["critical_paths"].is_array()) log_error("Local remap needs a valid timing report.\n");
    auto lab = [&](const CellInfo *c) { auto l = getBelLocation(c->bel); return Lab(l.x, l.y); };
    auto movable = [&](const CellInfo *c) {
        return c->bel != BelId() && c->belStrength <= STRENGTH_WEAK && c->cluster == ClusterId() &&
            !c->region && !c->isPseudo() && !c->attrs.count(id("keep")) && !c->attrs.count(id("dont_touch"));
    };
    std::set<Lab> protected_labs;
    for (auto &c : cells)
        if (c.second->bel != BelId() && (!movable(c.second.get()) || c.second->type == id_MISTRAL_MLAB))
            protected_labs.insert(lab(c.second.get()));
    std::set<NetInfo *> boundary;
    for (auto &p : ctx->ports) if (p.second.net) boundary.insert(p.second.net);
    auto ordinary_net = [&](NetInfo *n, bool allow_timed_data = false) {
        if (!n || !n->driver.cell || n->is_global || n->clkconstr || n->region || !n->wires.empty() ||
            n->constant_value != IdString() || boundary.count(n) || n->attrs.count(id("keep")) ||
            n->attrs.count(id("dont_touch"))) return false;
        // A fabric signal can also feed an implicit clock or hard/I/O boundary.
        // Hard-block sources remain allowed; these are checks on consumers.
        for (auto user : n->users) {
            int clocks = 0;
            auto kind = getPortTimingClass(user.cell, user.port, clocks);
            if (kind == TMG_CLOCK_INPUT) return false;
            if (width(user.cell->type) || user.cell->type == id_MISTRAL_FF) continue;
            if (!allow_timed_data || kind != TMG_REGISTER_INPUT || clocks <= 0 || !movable(user.cell) ||
                getBelPinsForCellPin(user.cell, user.port).empty()) return false;
            for (int i = 0; i < clocks; ++i) {
                auto info = getPortClockingInfo(user.cell, user.port, i);
                auto clock = user.cell->getPort(info.clock_port);
                if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay() <= 0) return false;
            }
        }
        return true;
    };
    auto safe_cell = [&](CellInfo *c) { return movable(c) && !protected_labs.count(lab(c)); };
    // Validate every routing edge of a relevant path before any mutation.
    auto endpoint = [&](const json11::Json &v) -> PortRef {
        if (!v["cell"].is_string() || !v["port"].is_string() || v["loc"].array_items().size() != 2)
            log_error("Malformed local-remap path endpoint.\n");
        auto it = cells.find(id(v["cell"].string_value()));
        if (it == cells.end()) log_error("Stale local-remap report cell '%s'.\n", v["cell"].string_value().c_str());
        auto c = it->second.get();
        IdString p = id(v["port"].string_value());
        auto xy = v["loc"].array_items();
        if (!c->ports.count(p) || c->bel == BelId() || !xy[0].is_number() || !xy[1].is_number() ||
            xy[0].number_value() != lab(c).first || xy[1].number_value() != lab(c).second)
            log_error("Stale local-remap report port or placement for '%s'.\n", nameOf(c));
        return {c, p};
    };
    struct Cone { CellInfo *inner, *outer, *sink; double excess; };
    std::vector<Cone> cones;
    std::set<std::tuple<IdString, IdString, Lab>> seen;
    for (auto &path : json["critical_paths"].array_items()) {
        const auto &segments = path["path"].array_items();
        if (segments.empty()) continue;
        const auto &last = segments.back();
        if (last["type"].string_value() != "setup" || last["to"]["port"].string_value() != "ENA") continue;
        if (!path["max_delay"].is_number() || path["max_delay"].number_value() <= 0)
            log_error("Malformed local-remap path constraint.\n");
        double delay = 0;
        std::vector<std::pair<PortRef, PortRef>> edges;
        for (auto &s : segments) {
            if (!s["delay"].is_number() || !std::isfinite(s["delay"].number_value()))
                log_error("Malformed local-remap path delay.\n");
            delay += s["delay"].number_value();
            auto from = endpoint(s["from"]), to = endpoint(s["to"]);
            if (s["type"].string_value() != "routing") continue;
            NetInfo *n = from.cell->getPort(from.port);
            if (!n || n != to.cell->getPort(to.port) || n->driver.cell != from.cell ||
                n->driver.port != from.port || to.cell->ports.at(to.port).type != PORT_IN ||
                s["net"].string_value() != n->name.str(ctx))
                log_error("Stale local-remap report edge.\n");
            edges.emplace_back(from, to);
        }
        if (delay <= path["max_delay"].number_value() || edges.size() < 2) continue;
        auto a = edges[edges.size()-2], b = edges.back();
        if (a.first.port != id_Q || b.first.port != id_Q || b.second.port != id_ENA ||
            a.second.cell != b.first.cell || !width(a.first.cell->type) || !width(b.first.cell->type) ||
            b.second.cell->type != id_MISTRAL_FF) continue;
        auto key = std::make_tuple(a.first.cell->name, b.first.cell->name, lab(b.second.cell));
        if (seen.insert(key).second) cones.push_back({a.first.cell, b.first.cell, b.second.cell, delay-path["max_delay"].number_value()});
    }
    std::sort(cones.begin(), cones.end(), [&](const Cone &a, const Cone &b) {
        if (a.excess != b.excess) return a.excess > b.excess;
        return std::make_tuple(a.outer->name.str(ctx), a.inner->name.str(ctx), lab(a.sink)) <
               std::make_tuple(b.outer->name.str(ctx), b.inner->name.str(ctx), lab(b.sink));
    });
    if (cones.size() > 8) cones.resize(8);
    int qualified = 0;
    for (auto &cone : cones) {
        auto inner = cone.inner, outer = cone.outer;
        NetInfo *old_net = outer->getPort(id_Q), *mid = inner->getPort(id_Q);
        if (!safe_cell(inner) || !safe_cell(outer) || !ordinary_net(old_net) || !ordinary_net(mid)) continue;
        std::vector<CellInfo *> group;
        bool eligible = true;
        for (auto u : old_net->users) {
            if (u.cell->bel == BelId() || lab(u.cell) != lab(cone.sink)) continue;
            if (u.cell->type != id_MISTRAL_FF || u.port != id_ENA || !safe_cell(u.cell) ||
                u.cell->getPort(id_CLK) != cone.sink->getPort(id_CLK) ||
                u.cell->get_pin_state(id_CLK) != cone.sink->get_pin_state(id_CLK)) { eligible = false; break; }
            group.push_back(u.cell);
        }
        if (!eligible || group.empty()) continue;
        std::sort(group.begin(), group.end(), [&](CellInfo *a, CellInfo *b) { return a->name.str(ctx) < b->name.str(ctx); });
        std::vector<NetInfo *> inputs;
        auto collect = [&](CellInfo *c, bool upstream) {
            std::vector<policy::Pin> result;
            for (int i = 0; i < width(c->type); ++i) {
                auto p = pins[i]; auto n = c->getPort(p); auto state = c->get_pin_state(p);
                if (state == PIN_0 || state == PIN_1) {
                    if (n) eligible = false;
                    result.push_back({state == PIN_0 ? policy::ZERO : policy::ONE, false}); continue;
                }
                if ((state != PIN_SIG && state != PIN_INV) || !ordinary_net(n, true) || n == old_net ||
                    (upstream && n == mid)) { eligible = false; continue; }
                if (n == mid) { result.push_back({policy::INTERMEDIATE, state == PIN_INV}); continue; }
                auto it = std::find(inputs.begin(), inputs.end(), n);
                if (it == inputs.end()) { inputs.push_back(n); it = inputs.end()-1; }
                result.push_back({int(it-inputs.begin()), state == PIN_INV});
            }
            return result;
        };
        auto a = collect(inner, true), b = collect(outer, false);
        if (!eligible) continue;
        auto composition = policy::compose(inner->params.at(id_LUT).as_int64(), a, outer->params.at(id_LUT).as_int64(), b);
        if (!composition.valid) continue;
        // Preserve exact indexed user slots/free lists and port indices on rollback.
        std::map<NetInfo *, indexed_store<PortRef>> users;
        for (auto n : inputs) users.emplace(n, n->users);
        users.emplace(old_net, old_net->users);
        std::map<CellInfo *, PortInfo> ena;
        std::map<CellInfo *, std::pair<BelId, PlaceStrength>> placements;
        for (auto c : group) { ena.emplace(c, c->ports.at(id_ENA)); placements[c] = {c->bel, c->belStrength}; }
        auto cname = id(outer->name.str(ctx) + "$local_remap"), nname = id(cname.str(ctx) + "$Q");
        if (cells.count(cname) || nets.count(nname) || net_aliases.count(nname)) continue;
        TimingAnalyser before(ctx); before.setup(false, false, true);
        auto old_hold = holds(before);
        auto timed_slack = [](float value) {
            // TimingAnalyser uses INT_MAX, not infinity, for an untimed endpoint.
            return std::isfinite(value) && value < float(std::numeric_limits<delay_t>::max());
        };
        std::map<std::pair<IdString, IdString>, float> boundary_slacks;
        for (auto input : inputs) for (auto user : input->users) {
            if (width(user.cell->type) || user.cell->type == id_MISTRAL_FF) continue;
            float slack = before.get_setup_slack(CellPortKey(user));
            if (!timed_slack(slack)) eligible = false;
            boundary_slacks[{user.cell->name, user.port}] = slack;
        }
        if (!eligible) continue;
        float old_slack = std::numeric_limits<float>::max();
        for (auto c : group) old_slack = std::min(old_slack, before.get_setup_slack(CellPortKey(c->name, id_ENA)));
        struct ExtraGroup { Lab location; std::vector<CellInfo *> cells; float slack; };
        std::vector<ExtraGroup> extra_groups;
        if (group_budget > 1) {
            std::map<Lab, std::vector<CellInfo *>> by_lab;
            std::set<Lab> rejected;
            for (auto user : old_net->users) {
                auto c = user.cell;
                if (c->bel == BelId() || lab(c) == lab(cone.sink)) continue;
                if (c->type != id_MISTRAL_FF || user.port != id_ENA || !safe_cell(c) ||
                    c->getPort(id_CLK) != cone.sink->getPort(id_CLK) ||
                    c->get_pin_state(id_CLK) != cone.sink->get_pin_state(id_CLK)) {
                    rejected.insert(lab(c)); continue;
                }
                by_lab[lab(c)].push_back(c);
            }
            for (auto &entry : by_lab) {
                if (rejected.count(entry.first)) continue;
                float slack = std::numeric_limits<float>::max();
                bool timed = true;
                for (auto c : entry.second) {
                    float value = before.get_setup_slack(CellPortKey(c->name, id_ENA));
                    timed &= timed_slack(value); slack = std::min(slack, value);
                }
                if (!timed) continue;
                std::sort(entry.second.begin(), entry.second.end(), [&](CellInfo *a, CellInfo *b) {
                    return a->name.str(ctx) < b->name.str(ctx);
                });
                extra_groups.push_back({entry.first, entry.second, slack});
            }
            std::sort(extra_groups.begin(), extra_groups.end(), [](const ExtraGroup &a, const ExtraGroup &b) {
                return std::make_pair(a.slack, a.location) < std::make_pair(b.slack, b.location);
            });
            if (extra_groups.size() > 32) extra_groups.resize(32);
            for (auto &g : extra_groups) for (auto c : g.cells) ena.emplace(c, c->ports.at(id_ENA));
        }
        std::vector<CellInfo *> extra_selected;
        int count = std::max(2, int(composition.signals.size()));
        IdString types[] = {id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
        auto clone = ctx->createCell(cname, types[count-2]);
        clone->params[id_LUT] = Property(int64_t(composition.mask), 1 << count);
        for (int i = 0; i < count; ++i) {
            clone->addInput(pins[i]);
            if (i < int(composition.signals.size())) clone->connectPort(pins[i], inputs.at(composition.signals[i]));
            else clone->pin_data[pins[i]].state = PIN_0;
        }
        auto net = ctx->createNet(nname); clone->addOutput(id_Q); clone->connectPort(id_Q, net);
        for (auto c : group) { c->disconnectPort(id_ENA); c->connectPort(id_ENA, net); }
        assignArchInfo();
        std::vector<NetInfo *> clone_inputs;
        std::map<NetInfo *, store_index<PortRef>> clone_slots;
        std::vector<int> canonical_order;
        for (int i = 0; i < int(composition.signals.size()); ++i) {
            auto input = clone->getPort(pins[i]);
            clone_inputs.push_back(input);
            clone_slots.emplace(input, clone->ports.at(pins[i]).user_idx);
            canonical_order.push_back(i);
        }
        const bool optimize_pins = local_remap_optimize_pins && clone_inputs.size() >= 2;
        auto apply_order = [&](const std::vector<int> &order) {
            if (!optimize_pins) return;
            uint64_t mask = 0;
            NPNR_ASSERT(local_remap_pin_policy::permute_mask(composition.mask, order, mask));
            for (size_t pin = 0; pin < order.size(); ++pin) {
                auto input = clone_inputs.at(order.at(pin));
                auto &port = clone->ports.at(pins.at(pin));
                port.net = input; port.user_idx = clone_slots.at(input);
                input->users.at(port.user_idx) = {clone, pins.at(pin)};
            }
            clone->params[id_LUT] = Property(int64_t(mask), 1 << count);
            assignArchInfo();
        };
        auto restore_placement = [&]() {
            if (!extra_selected.empty()) {
                for (auto c : extra_selected) { c->disconnectPort(id_ENA); c->connectPort(id_ENA, old_net); }
                extra_selected.clear();
                assignArchInfo();
            }
            if (clone->bel != BelId()) unbindBel(clone->bel);
            for (auto c : group) if (c->bel != BelId()) unbindBel(c->bel);
            for (auto c : group) bindBel(placements.at(c).first, c, placements.at(c).second);
        };
        auto rollback = [&]() {
            restore_placement();
            for (auto &entry : ena) {
                entry.first->disconnectPort(id_ENA); entry.first->ports.at(id_ENA) = entry.second;
            }
            for (auto p : pins) if (clone->ports.count(p)) clone->disconnectPort(p);
            clone->disconnectPort(id_Q);
            cells.erase(cname); nets.erase(nname); net_aliases.erase(nname);
            for (auto &entry : users) std::swap(entry.first->users, entry.second);
            assignArchInfo(); ctx->check();
        };
        auto center = lab(cone.sink);
        bool may_move = true;
        for (auto c : group) may_move &= ordinary_net(c->getPort(id_Q));
        auto move_group = [&](int dx, int dy) {
            restore_placement();
            if ((dx || dy) && !may_move) return false;
            std::vector<BelId> targets;
            for (auto c : group) {
                auto loc = getBelLocation(c->bel); loc.x += dx; loc.y += dy;
                auto bel = getBelByLocation(loc);
                if (bel == BelId() || protected_labs.count({loc.x, loc.y}) ||
                    !isValidBelForCellType(c->type, bel) || ((dx || dy) && !checkBelAvail(bel))) return false;
                targets.push_back(bel);
            }
            for (auto c : group) unbindBel(c->bel);
            for (size_t i = 0; i < group.size(); ++i) bindBel(targets[i], group[i], placements.at(group[i]).second);
            return true;
        };
        auto legal = [&]() {
            // Control-set and shared input legality can change for cells other than those moved.
            for (auto &entry : cells) if (entry.second->bel != BelId() && !isBelLocationValid(entry.second->bel)) return false;
            return true;
        };
        struct Trial { BelId bel; int dx, dy, score; std::vector<int> order; };
        std::vector<Trial> trials;
        for (auto shift : std::vector<Lab>{{0,0},{-1,0},{1,0},{0,-1},{0,1}}) {
            if (!move_group(shift.first, shift.second)) continue;
            for (auto bel : getBels()) {
                auto loc = getBelLocation(bel);
                if (std::abs(loc.x-center.first-shift.first)+std::abs(loc.y-center.second-shift.second) > 3 ||
                    !checkBelAvail(bel) || !isValidBelForCellType(clone->type, bel) || protected_labs.count({loc.x,loc.y})) continue;
                bindBel(bel, clone, STRENGTH_WEAK);
                std::vector<int> order = canonical_order;
                if (optimize_pins) {
                    std::vector<std::vector<int>> costs(clone_inputs.size(), std::vector<int>(clone_inputs.size()));
                    bool timed = true;
                    for (size_t source = 0; source < clone_inputs.size(); ++source)
                        for (size_t pin = 0; pin < clone_inputs.size(); ++pin) {
                            DelayQuad logic;
                            timed &= getCellDelay(clone, pins.at(pin), id_Q, logic);
                            costs[source][pin] = int(ctx->predictArcDelay(clone_inputs[source], {clone,pins.at(pin)})) + logic.maxDelay();
                        }
                    const auto ranked = timed ? local_remap_pin_policy::orders(costs) : std::vector<local_remap_pin_policy::RankedOrder>{};
                    if (!ranked.empty()) order = ranked.front().input_for_pin;
                    apply_order(order);
                    // Permutation changes shared ALM inputs; check neighboring
                    // cells as well as this LUT, and retain a legal fallback.
                    bool legal_order = true;
                    for (const auto &entry : cells)
                        if (entry.second->bel != BelId() && lab(entry.second.get()) == Lab(loc.x,loc.y))
                            legal_order &= isBelLocationValid(entry.second->bel);
                    if (!legal_order) { order = canonical_order; apply_order(order); }
                }
                if (isBelLocationValid(bel)) {
                    int in = 0, out = 0;
                    for (int i = 0; i < int(composition.signals.size()); ++i) {
                        int delay = int(ctx->predictArcDelay(clone->getPort(pins[i]), {clone,pins[i]}));
                        if (optimize_pins) { DelayQuad logic; if (getCellDelay(clone,pins[i],id_Q,logic)) delay += logic.maxDelay(); }
                        in = std::max(in, delay);
                    }
                    for (auto c : group) out = std::max(out, int(ctx->predictArcDelay(net,{c,id_ENA})));
                    trials.push_back({bel,shift.first,shift.second,in+out,order});
                }
                unbindBel(bel);
            }
        }
        restore_placement();
        std::sort(trials.begin(), trials.end(), [&](const Trial &a, const Trial &b) {
            auto x = getBelLocation(a.bel), y = getBelLocation(b.bel);
            return std::make_tuple(a.score,std::abs(a.dx)+std::abs(a.dy),a.dx,a.dy,x.x,x.y,x.z) <
                   std::make_tuple(b.score,std::abs(b.dx)+std::abs(b.dy),b.dx,b.dy,y.x,y.y,y.z);
        });
        // One BEL per tile/shift avoids spending the STA budget on equivalent sites.
        std::set<std::tuple<int,int,int,int>> tested;
        int examined = 0;
        bool keep = false;
        for (auto t : trials) {
            auto loc = getBelLocation(t.bel);
            if (!tested.insert({loc.x,loc.y,t.dx,t.dy}).second) continue;
            if (examined++ == 12) break;
            if (!move_group(t.dx,t.dy) || !checkBelAvail(t.bel)) continue;
            apply_order(t.order);
            bindBel(t.bel, clone, STRENGTH_WEAK);
            if (!legal()) continue;
            int selected_groups = 1;
            if (!extra_groups.empty()) {
                // Probe all bounded secondary groups, then keep only whole groups
                // for which every endpoint improves. The final subset gets a fresh
                // complete analysis; provisional legality is never accepted.
                for (auto &g : extra_groups) for (auto c : g.cells) {
                    c->disconnectPort(id_ENA); c->connectPort(id_ENA, net); extra_selected.push_back(c);
                }
                assignArchInfo();
                std::set<CellInfo *> keep_extra;
                {
                    TimingAnalyser all(ctx); all.setup(false, false, true);
                    for (auto &g : extra_groups) {
                        if (selected_groups == group_budget) break;
                        bool improves = true;
                        for (auto c : g.cells) {
                            auto port = CellPortKey(c->name, id_ENA);
                            float value = all.get_setup_slack(port);
                            improves &= isBelLocationValid(c->bel) && timed_slack(value) &&
                                value >= before.get_setup_slack(port) + 250;
                        }
                        if (!improves) continue;
                        ++selected_groups;
                        keep_extra.insert(g.cells.begin(), g.cells.end());
                    }
                }
                std::vector<CellInfo *> retained;
                for (auto c : extra_selected) {
                    if (keep_extra.count(c)) retained.push_back(c);
                    else { c->disconnectPort(id_ENA); c->connectPort(id_ENA, old_net); }
                }
                extra_selected.swap(retained);
                assignArchInfo();
                if (!legal()) continue;
            }
            TimingAnalyser after(ctx); after.setup(false, false, true);
            bool extra_improve = true;
            for (auto c : extra_selected) {
                auto port = CellPortKey(c->name, id_ENA);
                float value = after.get_setup_slack(port);
                extra_improve &= timed_slack(value) && value >= before.get_setup_slack(port) + 250;
            }
            float slack = std::numeric_limits<float>::max();
            for (auto c : group) slack = std::min(slack, after.get_setup_slack(CellPortKey(c->name,id_ENA)));
            bool clocks = true;
            for (auto &clock : before.get_timing_result().clock_fmax) {
                auto &now = after.get_timing_result().clock_fmax;
                if (!now.count(clock.first) || now.at(clock.first).achieved + 1e-4 < clock.second.achieved) clocks = false;
            }
            bool boundaries = true;
            for (auto &endpoint : boundary_slacks) {
                float now = after.get_setup_slack(CellPortKey(endpoint.first.first, endpoint.first.second));
                if (!timed_slack(now) || now < endpoint.second) boundaries = false;
            }
            bool hold = enable_replication_policy::hold_nonregressing(old_hold, holds(after));
            log_info("Local remap trial inner=%s outer=%s lab=%d,%d bel=%s shift=%d,%d gain=%.0fps clocks=%d hold=%d boundaries=%d groups=%d sinks=%zu\n",
                nameOf(inner),nameOf(outer),center.first,center.second,nameOfBel(t.bel),t.dx,t.dy,slack-old_slack,int(clocks),int(hold),int(boundaries),selected_groups,group.size()+extra_selected.size());
            if (!timed_slack(slack) || !timed_slack(old_slack) || slack < old_slack+250 || !clocks || !hold || !boundaries || !extra_improve) continue;
            log_info("Local remap candidate %d: %s + %s -> %s, %zu ENA users, predicted gain %.0fps.\n",
                qualified,nameOf(inner),nameOf(outer),nameOfBel(t.bel),group.size()+extra_selected.size(),slack-old_slack);
            if (selection == qualified++) { keep = true; break; }
        }
        if (keep) {
            // Probing secondary groups can recycle user-store slots. Preserve the
            // original indices of every unselected consumer in the retained graph.
            if (!extra_groups.empty()) {
                old_net->users = std::move(users.at(old_net));
                for (auto &entry : ena) {
                    if (entry.first->getPort(id_ENA) == old_net) entry.first->ports.at(id_ENA) = entry.second;
                    else old_net->users.remove(entry.second.user_idx);
                }
            }
            ctx->check();
            log_info("Local remap applied candidate %d; full routing and signoff still required.\n", selection);
            return true;
        }
        rollback();
    }
    log_info("Local remap: %d qualified candidates; no candidate applied.\n", qualified);
    return false;
}
NEXTPNR_NAMESPACE_END
