/* Bounded, report-guided internal LUT cut cloning. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "json11.hpp"
#include "timing.h"
#include "enable_replication_policy.h"
#include "local_remap_cut_policy.h"
#include "remap_report.h"
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
const std::vector<IdString> cut_pins = {id_A,id_B,id_C,id_D,id_E,id_F};
int cut_width(IdString type)
{
    if (type == id_MISTRAL_ALUT2) return 2;
    if (type == id_MISTRAL_ALUT3) return 3;
    if (type == id_MISTRAL_ALUT4) return 4;
    if (type == id_MISTRAL_ALUT5) return 5;
    if (type == id_MISTRAL_ALUT6) return 6;
    return 0;
}
bool cut_timed(float value)
{
    return std::isfinite(value) && value < float(std::numeric_limits<delay_t>::max());
}
std::map<std::string,int> cut_holds(TimingAnalyser &timing)
{
    std::map<std::string,int> result;
    for (const auto &path : timing.get_timing_result().min_delay_violations) {
        if (path.segments.empty()) continue;
        int slack = 0;
        for (const auto &segment : path.segments) slack += segment.delay;
        auto endpoint = path.segments.back().to;
        auto key = std::to_string(path.clock_pair.start.clock.index) + ":" +
            std::to_string(int(path.clock_pair.start.edge)) + ":" + std::to_string(path.clock_pair.end.clock.index) + ":" +
            std::to_string(int(path.clock_pair.end.edge)) + ":" + std::to_string(endpoint.first.index) + ":" +
            std::to_string(endpoint.second.index);
        if (!result.count(key)) result.emplace(key,slack);
        else result.at(key) = std::min(result.at(key),slack);
    }
    return result;
}
}

bool Arch::remap_comb_critical(const std::string &report, int selection)
{
    namespace policy = local_remap_cut_policy;
    Context *ctx = getCtx();
    if (selection < -1) log_error("Comb remap selection must be -1 (list) or nonnegative.\n");
    if (fes_any_slot_region_active) log_error("Comb remap requires ordinary full-design placement.\n");
    std::string error;
    auto json = json11::Json::parse(report,error);
    if (!error.empty() || !json["critical_paths"].is_array()) log_error("Comb remap needs a valid timing report.\n");
    auto lab = [&](const CellInfo *cell) { auto loc = getBelLocation(cell->bel); return std::make_pair(loc.x,loc.y); };
    auto movable = [&](const CellInfo *cell) {
        return cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK && cell->cluster == ClusterId() &&
            !cell->region && !cell->isPseudo() && !cell->attrs.count(id("keep")) && !cell->attrs.count(id("dont_touch"));
    };
    std::set<std::pair<int,int>> protected_labs;
    for (const auto &entry : cells)
        if (entry.second->bel != BelId() && (!movable(entry.second.get()) || entry.second->type == id_MISTRAL_MLAB))
            protected_labs.insert(lab(entry.second.get()));
    auto safe = [&](const CellInfo *cell) { return movable(cell) && !protected_labs.count(lab(cell)); };
    auto table_valid = [&](const CellInfo *cell) {
        int width = cut_width(cell->type);
        if (!width || cell->get_pin_state(id_Q) != PIN_SIG || cell->params.size() != 1 ||
            !cell->params.count(id_LUT)) return false;
        const auto &table = cell->params.at(id_LUT);
        return table.is_fully_def() && table.size() == (1u << width);
    };
    std::set<NetInfo *> boundary;
    for (const auto &port : ctx->ports) if (port.second.net) boundary.insert(port.second.net);
    auto ordinary = [&](NetInfo *net, bool allow_timed_data = false) {
        if (!net || !net->driver.cell || net->is_global || net->clkconstr || net->region || !net->wires.empty() ||
            net->constant_value != IdString() || boundary.count(net) || net->attrs.count(id("keep")) ||
            net->attrs.count(id("dont_touch"))) return false;
        for (auto user : net->users) {
            int clocks = 0;
            auto kind = getPortTimingClass(user.cell,user.port,clocks);
            if (kind == TMG_CLOCK_INPUT) return false;
            if (cut_width(user.cell->type) || user.cell->type == id_MISTRAL_FF) continue;
            if (!allow_timed_data || kind != TMG_REGISTER_INPUT || clocks <= 0 || !movable(user.cell) ||
                getBelPinsForCellPin(user.cell,user.port).empty()) return false;
            for (int i = 0; i < clocks; ++i) {
                auto info = getPortClockingInfo(user.cell,user.port,i);
                auto clock = user.cell->getPort(info.clock_port);
                if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay() <= 0) return false;
            }
        }
        return true;
    };
    struct Cut { std::vector<CellInfo *> nodes; PortRef sink; double excess; size_t distance; };
    std::vector<Cut> cuts;
    std::map<std::tuple<std::vector<IdString>,IdString,IdString>,size_t> seen;
    // Validate all violated relevant paths before mutating any graph edge.
    for (const auto &path : mistral_remap_report::validate(ctx,json,true)) {
        const auto &edges = path.edges;
        for (size_t end = 0; end < edges.size(); ++end) {
            auto target = edges[end].second;
            int target_width = cut_width(target.cell->type);
            if (!target_width || std::find(cut_pins.begin(),cut_pins.begin()+target_width,target.port) ==
                cut_pins.begin()+target_width) continue;
            for (size_t length : {size_t(3),size_t(2)}) {
                if (end+1 < length) continue;
                std::vector<CellInfo *> nodes;
                bool connected = true;
                for (size_t i = end+1-length; i <= end; ++i) {
                    auto source = edges[i].first;
                    if (source.port != id_Q || !cut_width(source.cell->type) ||
                        (i < end && edges[i].second.cell != edges[i+1].first.cell) ||
                        std::find(nodes.begin(),nodes.end(),source.cell) != nodes.end()) connected = false;
                    nodes.push_back(source.cell);
                }
                if (!connected || std::find(nodes.begin(),nodes.end(),target.cell) != nodes.end()) continue;
                std::vector<IdString> identities;
                for (auto node : nodes) identities.push_back(node->name);
                auto key = std::make_tuple(identities,target.cell->name,target.port);
                auto excess = path.excess;
                auto found = seen.find(key);
                if (found == seen.end()) {
                    seen.emplace(key,cuts.size());
                    cuts.push_back({nodes,target,excess,edges.size()-end});
                } else {
                    auto &retained = cuts.at(found->second);
                    retained.excess = std::max(retained.excess,excess);
                    retained.distance = std::min(retained.distance,edges.size()-end);
                }
            }
        }
    }
    std::sort(cuts.begin(),cuts.end(),[&](const Cut &a,const Cut &b) {
        return std::make_tuple(-a.excess,a.distance,-int(a.nodes.size()),a.sink.cell->name.str(ctx),a.sink.port.str(ctx),a.nodes.front()->name.str(ctx)) <
               std::make_tuple(-b.excess,b.distance,-int(b.nodes.size()),b.sink.cell->name.str(ctx),b.sink.port.str(ctx),b.nodes.front()->name.str(ctx));
    });
    if (cuts.size() > 16) cuts.resize(16);
    int qualified = 0;
    for (const auto &cut : cuts) {
        auto sink = cut.sink.cell; auto sink_pin = cut.sink.port;
        auto root = cut.nodes.back(); auto old_net = sink->getPort(sink_pin);
        bool eligible = safe(sink) && table_valid(sink) && ordinary(old_net);
        std::map<NetInfo *,size_t> internal;
        for (size_t i = 0; i < cut.nodes.size(); ++i) {
            auto cell = cut.nodes[i]; auto output = cell->getPort(id_Q);
            eligible &= safe(cell) && table_valid(cell) && ordinary(output);
            if (!output || !internal.emplace(output,i).second) eligible = false;
        }
        if (!eligible || root->getPort(id_Q) != old_net) continue;
        std::vector<NetInfo *> inputs;
        std::vector<policy::Node> nodes;
        for (size_t index = 0; index < cut.nodes.size(); ++index) {
            auto cell = cut.nodes[index];
            policy::Node node; node.mask = uint64_t(cell->params.at(id_LUT).as_int64());
            for (int i = 0; i < cut_width(cell->type); ++i) {
                auto pin = cut_pins[i]; auto net = cell->getPort(pin); auto state = cell->get_pin_state(pin);
                if (state == PIN_0 || state == PIN_1) {
                    if (net) eligible = false;
                    node.pins.push_back({state == PIN_0 ? policy::ZERO : policy::ONE,false});
                } else if (state == PIN_SIG || state == PIN_INV) {
                    if (internal.count(net)) {
                        auto prior = internal.at(net);
                        if (prior >= index) eligible = false;
                        node.pins.push_back({policy::node_source(int(prior)),state == PIN_INV});
                    } else {
                        if (!ordinary(net,true)) eligible = false;
                        auto found = std::find(inputs.begin(),inputs.end(),net);
                        if (found == inputs.end()) { inputs.push_back(net); found = inputs.end()-1; }
                        node.pins.push_back({int(found-inputs.begin()),state == PIN_INV});
                    }
                } else eligible = false;
            }
            nodes.push_back(node);
        }
        if (!eligible || inputs.size() > 6) continue;
        auto composition = policy::compose(nodes);
        if (!composition.valid || composition.signals.size() < 2) continue;
        std::set<CellPortKey> endpoint_keys;
        std::map<NetInfo *,int> visit;
        std::function<void(NetInfo *)> follow = [&](NetInfo *net) {
            if (!net || visit[net] == 1) { eligible = false; return; }
            if (visit[net] == 2) return;
            if (!ordinary(net,true)) { eligible = false; return; }
            visit[net] = 1;
            for (auto user : net->users) {
                int clocks = 0;
                auto kind = getPortTimingClass(user.cell,user.port,clocks);
                if (cut_width(user.cell->type) && kind == TMG_COMB_INPUT) {
                    if (!safe(user.cell) || !table_valid(user.cell)) eligible = false;
                    else follow(user.cell->getPort(id_Q));
                }
                else if (kind == TMG_REGISTER_INPUT && clocks > 0) {
                    endpoint_keys.insert(CellPortKey(user));
                } else eligible = false;
            }
            visit[net] = 2;
        };
        follow(sink->getPort(id_Q));
        if (!eligible || endpoint_keys.empty()) continue;
        // Structural guards run before STA can inspect any downstream LUT properties.
        TimingAnalyser before(ctx); before.setup(false,false,true);
        float old_slack = before.get_setup_slack(CellPortKey(cut.sink));
        if (!cut_timed(old_slack)) continue;
        auto old_hold = cut_holds(before);
        std::map<CellPortKey,float> endpoints, boundaries;
        for (auto key : endpoint_keys) {
            auto value = before.get_setup_slack(key);
            if (!cut_timed(value)) eligible = false;
            else endpoints.emplace(key,value);
        }
        for (auto input : inputs) for (auto user : input->users) {
            if (cut_width(user.cell->type) || user.cell->type == id_MISTRAL_FF) continue;
            auto key = CellPortKey(user); auto value = before.get_setup_slack(key);
            if (!cut_timed(value)) eligible = false;
            else boundaries.emplace(key,value);
        }
        if (!eligible || endpoints.empty()) continue;
        std::map<NetInfo *,indexed_store<PortRef>> saved_users;
        saved_users.emplace(old_net,old_net->users);
        for (auto input : inputs) saved_users.emplace(input,input->users);
        auto saved_port = sink->ports.at(sink_pin);
        auto cname = id(root->name.str(ctx) + "$comb_remap$" + sink->name.str(ctx) + "$" + sink_pin.str(ctx));
        auto nname = id(cname.str(ctx) + "$Q");
        if (cells.count(cname) || nets.count(nname) || net_aliases.count(nname)) continue;
        int count = int(composition.signals.size());
        const IdString types[] = {id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5,id_MISTRAL_ALUT6};
        auto clone = ctx->createCell(cname,types[count-2]);
        clone->params[id_LUT] = Property(int64_t(composition.mask),1 << count);
        for (int i = 0; i < count; ++i) {
            clone->addInput(cut_pins[i]); clone->connectPort(cut_pins[i],inputs.at(composition.signals[i]));
        }
        clone->addOutput(id_Q); auto net = ctx->createNet(nname); clone->connectPort(id_Q,net);
        sink->disconnectPort(sink_pin); sink->connectPort(sink_pin,net);
        assignArchInfo();
        auto rollback = [&]() {
            if (clone->bel != BelId()) unbindBel(clone->bel);
            sink->disconnectPort(sink_pin); sink->ports.at(sink_pin) = saved_port;
            for (int i = 0; i < count; ++i) clone->disconnectPort(cut_pins[i]);
            clone->disconnectPort(id_Q);
            cells.erase(cname); nets.erase(nname); net_aliases.erase(nname);
            for (auto &entry : saved_users) std::swap(entry.first->users,entry.second);
            assignArchInfo(); ctx->check();
        };
        auto full_legal = [&]() {
            for (const auto &entry : cells)
                if (entry.second->bel != BelId() && !isBelLocationValid(entry.second->bel)) return false;
            return true;
        };
        struct Site { BelId bel; int score; };
        std::vector<Site> sites;
        auto center = lab(sink);
        for (int x = center.first-3; x <= center.first+3; ++x)
            for (int y = center.second-3; y <= center.second+3; ++y) {
                if (std::abs(x-center.first)+std::abs(y-center.second) > 3 || protected_labs.count({x,y})) continue;
                for (auto bel : getBelsByTile(x,y)) {
                    if (!checkBelAvail(bel) || !isValidBelForCellType(clone->type,bel)) continue;
                    bindBel(bel,clone,STRENGTH_WEAK);
                    bool legal = isBelLocationValid(bel);
                    for (auto neighbor : getBelsByTile(x,y))
                        if (getBoundBelCell(neighbor) && !isBelLocationValid(neighbor)) legal = false;
                    if (legal) {
                        int incoming = 0;
                        for (int i = 0; i < count; ++i) {
                            DelayQuad logic;
                            if (!getCellDelay(clone,cut_pins[i],id_Q,logic)) { legal = false; break; }
                            incoming = std::max(incoming,int(ctx->predictArcDelay(clone->getPort(cut_pins[i]),{clone,cut_pins[i]}))+logic.maxDelay());
                        }
                        if (legal) sites.push_back({bel,incoming+int(ctx->predictArcDelay(net,cut.sink))});
                    }
                    unbindBel(bel);
                }
            }
        std::sort(sites.begin(),sites.end(),[&](const Site &a,const Site &b) {
            auto x = getBelLocation(a.bel), y = getBelLocation(b.bel);
            return std::make_tuple(a.score,x.x,x.y,x.z) < std::make_tuple(b.score,y.x,y.y,y.z);
        });
        std::set<std::pair<int,int>> tested;
        int examined = 0; bool keep = false;
        for (const auto &site : sites) {
            auto loc = getBelLocation(site.bel);
            if (!tested.emplace(loc.x,loc.y).second) continue;
            if (examined++ == 12) break;
            bindBel(site.bel,clone,STRENGTH_WEAK);
            if (!full_legal()) { unbindBel(site.bel); continue; }
            TimingAnalyser after(ctx); after.setup(false,false,true);
            float slack = after.get_setup_slack(CellPortKey(cut.sink));
            bool endpoint_ok = true, boundary_ok = true, clocks = true;
            for (const auto &entry : endpoints) {
                float now = after.get_setup_slack(entry.first);
                endpoint_ok &= cut_timed(now) && now >= entry.second;
            }
            for (const auto &entry : boundaries) {
                float now = after.get_setup_slack(entry.first);
                boundary_ok &= cut_timed(now) && now >= entry.second;
            }
            for (const auto &clock : before.get_timing_result().clock_fmax) {
                auto &now = after.get_timing_result().clock_fmax;
                if (!now.count(clock.first) || now.at(clock.first).achieved+1e-4 < clock.second.achieved) clocks = false;
            }
            bool hold = enable_replication_policy::hold_nonregressing(old_hold,cut_holds(after));
            log_info("Comb remap trial root=%s sink=%s.%s cut=%zu bel=%s gain=%.0fps endpoints=%d clocks=%d hold=%d boundaries=%d\n",
                     nameOf(root),nameOf(sink),sink_pin.c_str(ctx),cut.nodes.size(),nameOfBel(site.bel),slack-old_slack,
                     int(endpoint_ok),int(clocks),int(hold),int(boundary_ok));
            if (cut_timed(slack) && slack >= old_slack+250 && endpoint_ok && clocks && hold && boundary_ok) {
                log_info("Comb remap candidate %d: root=%s sink=%s.%s cut=%zu -> %s, predicted gain %.0fps.\n",
                         qualified,nameOf(root),nameOf(sink),sink_pin.c_str(ctx),cut.nodes.size(),nameOfBel(site.bel),slack-old_slack);
                if (selection == qualified++) { keep = true; break; }
            }
            unbindBel(site.bel);
        }
        if (keep) {
            ctx->check();
            log_info("Comb remap applied candidate %d; full routing and signoff still required.\n",selection);
            return true;
        }
        rollback();
    }
    log_info("Comb remap: %d qualified candidates; no candidate applied.\n",qualified);
    return false;
}
NEXTPNR_NAMESPACE_END
