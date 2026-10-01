/* Bounded original-LUT pair placement, with no graph rewrites. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "json11.hpp"
#include "timing.h"
#include "lut_pair_placement.h"
#include "remap_report.h"
#include "remap_clock_guard.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Lab = std::pair<int, int>;
const std::array<IdString, 6> pair_pins = {id_A, id_B, id_C, id_D, id_E, id_F};
bool protected_attrs(const dict<IdString, Property> &attrs, Context *ctx)
{
    for (const auto &entry : attrs) {
        auto name = entry.first.str(ctx);
        if (name == "keep" || name == "dont_touch") return true;
    }
    return false;
}
bool pins_equal(const dict<IdString, ArchPinInfo> &a, const dict<IdString, ArchPinInfo> &b)
{
    if (a.size() != b.size()) return false;
    for (const auto &pin : a)
        if (!b.count(pin.first) || pin.second.state != b.at(pin.first).state ||
            pin.second.bel_pins != b.at(pin.first).bel_pins) return false;
    return true;
}
bool info_equal(Context *ctx, CellInfo *cell, const ArchCellInfo &saved)
{
    if (cell->constr_children != saved.constr_children || cell->constr_x != saved.constr_x ||
        cell->constr_y != saved.constr_y || cell->constr_z != saved.constr_z || cell->constr_abs_z != saved.constr_abs_z ||
        !pins_equal(cell->pin_data, saved.pin_data)) return false;
    if (ctx->is_comb_cell(cell->type) || cell->type.in(id_MISTRAL_BUF, id_MISTRAL_MLAB)) {
        const auto &a = cell->combInfo, &b = saved.combInfo;
        if (a.comb_out != b.comb_out || a.lut_input_count != b.lut_input_count ||
            a.used_lut_input_count != b.used_lut_input_count || a.lut_bits_count != b.lut_bits_count ||
            a.chain_shared_input_count != b.chain_shared_input_count || a.is_carry != b.is_carry ||
            a.is_shared != b.is_shared || a.is_extended != b.is_extended || a.carry_start != b.carry_start ||
            a.carry_end != b.carry_end || a.mlab_group != b.mlab_group) return false;
        for (int i = 0; i < a.lut_input_count; ++i) if (a.lut_in[i] != b.lut_in[i]) return false;
        if (cell->type == id_MISTRAL_MLAB && (!(a.wclk == b.wclk) || !(a.we == b.we))) return false;
    } else if (cell->type == id_MISTRAL_FF) {
        if (!(cell->ffInfo.ctrlset == saved.ffInfo.ctrlset) || cell->ffInfo.sdata != saved.ffInfo.sdata ||
            cell->ffInfo.datain != saved.ffInfo.datain) return false;
    }
    return true;
}
} // namespace

bool Arch::remap_lut_pair_critical(const std::string &report, int selection)
{
    namespace guard = mistral_remap_clock_guard;
    Context *ctx = getCtx();
    if (selection < -1) log_error("Invalid LUT pair placement candidate index.\n");
    if (selection < 0 && !lut_driver_copy_report.empty())
        log_error("A LUT pair placement listing must be final; it cannot precede LUT driver copy.\n");
    if (fes_any_slot_region_active) log_error("LUT pair placement requires ordinary placement.\n");
    prevalidate_lut_pair_prefix(ctx);
    for (const auto &entry : nets)
        if (!entry.second->wires.empty()) log_error("LUT pair placement requires an unrouted design.\n");
    std::string error;
    auto json = json11::Json::parse(report, error);
    if (!error.empty() || !json.is_object() || !json["critical_paths"].is_array())
        log_error("Invalid LUT pair placement timing report.\n");
    auto lab = [&](BelId bel) { auto at = getBelLocation(bel); return Lab(at.x, at.y); };
    auto movable = [&](const CellInfo *cell) {
        return cell && cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK && cell->cluster == ClusterId() &&
            !cell->region && !cell->isPseudo() && !protected_attrs(cell->attrs, ctx);
    };
    std::set<Lab> protected_labs;
    for (const auto &entry : cells)
        if (entry.second->bel != BelId() && (!movable(entry.second.get()) || entry.second->type.in(id_MISTRAL_MLAB, id_MISTRAL_ALUT_ARITH)))
            protected_labs.insert(lab(entry.second->bel));
    std::set<const NetInfo *> boundary;
    for (const auto &entry : ctx->ports) if (entry.second.net) boundary.insert(entry.second.net);
    auto ordinary = [&](const NetInfo *net) {
        return net && net->driver.cell && !net->is_global && !net->clkconstr && !net->region && net->wires.empty() &&
            net->constant_value == IdString() && !boundary.count(net) && !protected_attrs(net->attrs, ctx) &&
            net->driver.cell->ports.count(net->driver.port) && net->driver.cell->ports.at(net->driver.port).type == PORT_OUT &&
            net->driver.cell->getPort(net->driver.port) == net;
    };
    auto table_valid = [&](const CellInfo *cell) {
        int width = mistral_remap_report::lut_width(cell->type);
        if (!width || cell->params.size() != 1 || !cell->params.count(id_LUT) ||
            !cell->params.at(id_LUT).is_fully_def() || cell->params.at(id_LUT).size() != (1u << width) ||
            cell->ports.size() != size_t(width + 1) || cell->get_pin_state(id_Q) != PIN_SIG ||
            !cell->ports.count(id_Q) || cell->ports.at(id_Q).type != PORT_OUT || !ordinary(cell->getPort(id_Q))) return false;
        auto output = cell->getPort(id_Q);
        if (output->driver.cell != cell || output->driver.port != id_Q) return false;
        for (int i = 0; i < width; ++i) {
            if (!cell->ports.count(pair_pins[i]) || cell->ports.at(pair_pins[i]).type != PORT_IN) return false;
            auto state = cell->get_pin_state(pair_pins[i]); auto input = cell->getPort(pair_pins[i]);
            if (state == PIN_0 || state == PIN_1) { if (input) return false; }
            else if ((state != PIN_SIG && state != PIN_INV) || !ordinary(input)) return false;
        }
        return true;
    };
    struct Cone { CellInfo *inner, *outer; PortRef sink; double excess; };
    std::vector<Cone> cones;
    std::map<std::tuple<IdString, IdString, IdString, IdString>, size_t> seen;
    // Complete path validation precedes any binding mutation.
    for (const auto &path : mistral_remap_report::validate(ctx, json, true)) {
        if (path.edges.size() < 2) continue;
        auto a = path.edges[path.edges.size() - 2], b = path.edges.back();
        int count = 0;
        if (a.first.port != id_Q || b.first.port != id_Q || a.second.cell != b.first.cell ||
            !mistral_remap_report::lut_width(a.first.cell->type) || !mistral_remap_report::lut_width(b.first.cell->type) ||
            a.first.cell == b.first.cell || getPortTimingClass(b.second.cell, b.second.port, count) != TMG_REGISTER_INPUT) continue;
        auto key = std::make_tuple(a.first.cell->name, b.first.cell->name, b.second.cell->name, b.second.port);
        auto found = seen.find(key);
        if (found == seen.end()) { seen.emplace(key, cones.size()); cones.push_back({a.first.cell, b.first.cell, b.second, path.excess}); }
        else cones.at(found->second).excess = std::max(cones.at(found->second).excess, path.excess);
    }
    std::sort(cones.begin(), cones.end(), [&](const Cone &a, const Cone &b) {
        return std::make_tuple(-a.excess, a.inner->name.str(ctx), a.outer->name.str(ctx), a.sink.cell->name.str(ctx), a.sink.port.str(ctx)) <
               std::make_tuple(-b.excess, b.inner->name.str(ctx), b.outer->name.str(ctx), b.sink.cell->name.str(ctx), b.sink.port.str(ctx));
    });
    if (cones.size() > 8) cones.resize(8);
    log_info("LUT pair placement discovery: %zu bounded pairs.\n", cones.size());
    struct Original {
        CellInfo *cell;
        const std::unique_ptr<CellInfo> *owner;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        ClusterId cluster;
        Region *region;
        decltype(CellInfo::params) params, attrs;
        decltype(CellInfo::ports) ports;
        std::vector<IdString> port_order;
        ArchCellInfo info;
    };
    std::vector<Original> originals;
    for (const auto &entry : cells) {
        auto *cell = entry.second.get();
        std::vector<IdString> ports;
        for (const auto &port : cell->ports) ports.push_back(port.first);
        originals.push_back({cell, &entry.second, cell->type, cell->bel, cell->belStrength, cell->cluster, cell->region,
            cell->params, cell->attrs, cell->ports, std::move(ports), static_cast<const ArchCellInfo &>(*cell)});
    }
    const auto saved_labs = labs;
    std::vector<std::pair<IdString, const NetInfo *>> net_order;
    std::vector<std::pair<IdString, IdString>> alias_order;
    for (const auto &entry : nets) net_order.emplace_back(entry.first, entry.second.get());
    for (const auto &entry : net_aliases) alias_order.emplace_back(entry.first, entry.second);
    using ClockRow = std::tuple<delay_t, delay_t, delay_t, delay_t, delay_t, delay_t, IdString, delay_t>;
    auto clock_constraints = [&]() {
        std::map<IdString, ClockRow> result;
        for (const auto &entry : nets) if (entry.second->clkconstr) {
            const auto &c = *entry.second->clkconstr;
            result.emplace(entry.first, ClockRow(c.period.minDelay(), c.period.maxDelay(), c.high.minDelay(), c.high.maxDelay(),
                c.low.minDelay(), c.low.maxDelay(), c.phase_group, c.phase_shift));
        }
        return result;
    };
    const auto original_clocks = clock_constraints();
    int qualified = 0;
    for (const auto &cone : cones) {
        auto *inner = cone.inner, *outer = cone.outer;
        auto *mid = inner->getPort(id_Q), *root = outer->getPort(id_Q);
        const char *reason = nullptr;
        auto reject = [&](const char *value) { if (!reason) reason = value; };
        auto report_rejection = [&]() {
            log_info("LUT pair placement rejection inner=%s outer=%s sink=%s.%s reason=%s\n", nameOf(inner), nameOf(outer),
                nameOf(cone.sink.cell), cone.sink.port.c_str(ctx), reason ? reason : "pretrial-ineligible");
        };
        if (!movable(inner) || !movable(outer) || protected_labs.count(lab(inner->bel)) || protected_labs.count(lab(outer->bel)) ||
            !table_valid(inner) || !table_valid(outer)) { reject("source-pair-not-safe-ordinary-luts"); report_rejection(); continue; }
        bool eligible = true;
        std::vector<NetInfo *> inputs;
        for (auto *cell : {inner, outer}) for (int i = 0; i < mistral_remap_report::lut_width(cell->type); ++i) {
            auto net = cell->getPort(pair_pins[i]);
            if (!net || net == mid) continue;
            if (net == root) { reject("source-pair-feedback"); eligible = false; }
            if (std::find(inputs.begin(), inputs.end(), net) == inputs.end()) inputs.push_back(net);
        }
        auto clocked = [&](CellInfo *cell, IdString port, int count) {
            if (count <= 0 || cell->bel == BelId() || getBelPinsForCellPin(cell, port).empty()) return false;
            for (int i = 0; i < count; ++i) {
                auto info = getPortClockingInfo(cell, port, i); auto clock = cell->getPort(info.clock_port);
                if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay() <= 0) return false;
            }
            return true;
        };
        std::set<CellPortKey> endpoint_keys;
        std::map<NetInfo *, int> visit;
        std::function<void(NetInfo *)> follow = [&](NetInfo *net) {
            if (!ordinary(net) || visit[net] == 1) { reject(!ordinary(net) ? "fanout-net-not-ordinary" : "fanout-feedback"); eligible = false; return; }
            if (visit[net] == 2) return;
            visit[net] = 1;
            for (auto user : net->users) {
                if (!user.cell || !user.cell->ports.count(user.port) || user.cell->ports.at(user.port).type != PORT_IN ||
                    user.cell->getPort(user.port) != net || !net->users.count(user.cell->ports.at(user.port).user_idx)) {
                    reject("fanout-user-invalid"); eligible = false; continue;
                }
                auto indexed = net->users.at(user.cell->ports.at(user.port).user_idx);
                if (indexed.cell != user.cell || indexed.port != user.port) { reject("fanout-user-index-invalid"); eligible = false; continue; }
                int count = 0; auto kind = getPortTimingClass(user.cell, user.port, count);
                if (kind == TMG_REGISTER_INPUT) {
                    if (!clocked(user.cell, user.port, count)) { reject("fanout-register-clock-unavailable"); eligible = false; }
                    else endpoint_keys.insert(CellPortKey(user));
                    continue;
                }
                bool known = mistral_remap_report::lut_width(user.cell->type) || user.cell->type.in(id_MISTRAL_NOT, id_MISTRAL_BUF, id_MISTRAL_ALUT_ARITH);
                if (kind != TMG_COMB_INPUT || !known || user.cell->bel == BelId() ||
                    (mistral_remap_report::lut_width(user.cell->type) && !table_valid(user.cell))) {
                    reject("fanout-combinational-boundary-unsupported"); eligible = false; continue;
                }
                if (user.cell->type == id_MISTRAL_ALUT_ARITH)
                    for (auto table : {id_LUT0, id_LUT1}) if (!user.cell->params.count(table) ||
                        !user.cell->params.at(table).is_fully_def() || user.cell->params.at(table).size() != 16) {
                        reject("fanout-arithmetic-table-invalid"); eligible = false;
                    }
                bool arc = false;
                for (const auto &port : user.cell->ports) {
                    if (port.second.type != PORT_OUT || !port.second.net) continue;
                    int clocks = 0; DelayQuad delay;
                    if (getPortTimingClass(user.cell, port.first, clocks) != TMG_COMB_OUTPUT ||
                        !getCellDelay(user.cell, user.port, port.first, delay)) continue;
                    arc = true; follow(port.second.net);
                }
                if (!arc) { reject("fanout-combinational-arc-unavailable"); eligible = false; }
            }
            visit[net] = 2;
        };
        follow(mid); follow(root);
        for (auto input : inputs) {
            int count = 0; auto kind = getPortTimingClass(input->driver.cell, input->driver.port, count);
            if (input->driver.cell->bel == BelId() || getBelPinsForCellPin(input->driver.cell, input->driver.port).empty() ||
                (kind == TMG_REGISTER_OUTPUT ? !clocked(input->driver.cell, input->driver.port, count) : kind != TMG_COMB_OUTPUT)) {
                reject("input-launch-clock-or-pin-unavailable"); eligible = false;
            }
            follow(input);
        }
        if (!eligible || endpoint_keys.empty() || !endpoint_keys.count(CellPortKey(cone.sink))) {
            reject("complete-endpoint-coverage-unavailable"); report_rejection(); continue;
        }
        TimingAnalyser before(ctx); before.with_clock_skew = true; before.setup(false, false, true);
        float old_slack = before.get_setup_slack(CellPortKey(cone.sink));
        if (before.have_loops || !guard::timed(old_slack) || before.get_timing_result().clock_fmax.empty()) {
            reject("original-native-timing-unavailable"); report_rejection(); continue;
        }
        std::map<CellPortKey, guard::Rows> endpoints, reference_endpoints;
        size_t timed_pairs = 0, unrelated_pairs = 0, hold_pairs = 0;
        for (auto key : endpoint_keys) {
            guard::Rows rows;
            if (!before.get_endpoint_clock_pair_timings(key, rows)) { reject("endpoint-domain-coverage-unavailable"); eligible = false; }
            else {
                for (const auto &row : rows) { timed_pairs += row.setup_timed; unrelated_pairs += !row.setup_timed; hold_pairs += row.hold_related; }
                endpoints.emplace(key, std::move(rows));
            }
        }
        TimingAnalyser reference(ctx);
        if (eligible && unrelated_pairs) {
            reference.with_clock_skew = false; reference.setup(false, false, true);
            for (const auto &entry : endpoints) {
                guard::Rows rows;
                if (!reference.get_endpoint_clock_pair_timings(entry.first, rows) || !guard::rows_match(entry.second, rows)) {
                    reject("endpoint-reference-coverage-unavailable"); eligible = false;
                } else reference_endpoints.emplace(entry.first, std::move(rows));
            }
        }
        std::map<NetInfo *, delay_t> arrivals;
        for (auto input : inputs) {
            delay_t value = 0;
            if (!before.get_max_arrival(CellPortKey(input->driver), value) || !std::isfinite(double(value)) ||
                value == std::numeric_limits<delay_t>::max() || value == std::numeric_limits<delay_t>::lowest()) {
                reject("input-arrival-unavailable"); eligible = false;
            } else arrivals.emplace(input, value);
        }
        delay_t intermediate_arrival = 0;
        if (!before.get_max_arrival(CellPortKey(mid->driver), intermediate_arrival) ||
            !std::isfinite(double(intermediate_arrival)) || intermediate_arrival == std::numeric_limits<delay_t>::max() ||
            intermediate_arrival == std::numeric_limits<delay_t>::lowest()) { reject("intermediate-arrival-unavailable"); eligible = false; }
        if (!eligible) { report_rejection(); continue; }
        auto old_holds = guard::holds(before);
        log_info("LUT pair placement domains inner=%s outer=%s sink=%s.%s endpoints=%zu timed_pairs=%zu unrelated_pairs=%zu hold_pairs=%zu reference_free=%d\n",
            nameOf(inner), nameOf(outer), nameOf(cone.sink.cell), cone.sink.port.c_str(ctx), endpoints.size(), timed_pairs,
            unrelated_pairs, hold_pairs, int(unrelated_pairs != 0));
        const auto inner_bel = inner->bel, outer_bel = outer->bel;
        const auto inner_strength = inner->belStrength, outer_strength = outer->belStrength;
        const auto inner_info = static_cast<const ArchCellInfo &>(*inner), outer_info = static_cast<const ArchCellInfo &>(*outer);
        std::set<uint32_t> touched{bel_data(inner_bel).lab_data.lab, bel_data(outer_bel).lab_data.lab};
        std::set<std::pair<uint32_t, uint8_t>> changed_alms;
        auto remember = [&](BelId bel) { const auto &data = bel_data(bel).lab_data; touched.insert(data.lab); changed_alms.emplace(data.lab, data.alm); };
        auto restore = [&](bool complete = false) {
            if (inner->bel != inner_bel) {
                if (inner->bel != BelId()) unbindBel(inner->bel);
                bindBel(inner_bel, inner, inner_strength);
            }
            if (outer->bel != outer_bel) {
                if (outer->bel != BelId()) unbindBel(outer->bel);
                bindBel(outer_bel, outer, outer_strength);
            }
            static_cast<ArchCellInfo &>(*inner) = inner_info;
            static_cast<ArchCellInfo &>(*outer) = outer_info;
            // Binding updates only its ALM input count. Heuristic probes do not
            // run STA; every actual STA probe also restores the whole cache.
            if (complete) {
                for (const auto &saved : originals) static_cast<ArchCellInfo &>(*saved.cell) = saved.info;
                labs = saved_labs;
            } else for (auto index : touched) labs.at(index) = saved_labs.at(index);
            changed_alms.clear();
        };
        auto fixed = [&]() {
            if (cells.size() != originals.size() || nets.size() != net_order.size() || net_aliases.size() != alias_order.size() || labs.size() != saved_labs.size() ||
                clock_constraints() != original_clocks) return false;
            size_t index = 0;
            for (const auto &entry : cells) {
                const auto &saved = originals.at(index++); auto *cell = entry.second.get();
                if (cell != saved.cell || &entry.second != saved.owner || cell->type != saved.type || cell->cluster != saved.cluster ||
                    cell->region != saved.region || cell->params != saved.params || cell->attrs != saved.attrs ||
                    cell->belStrength != saved.strength || (cell != inner && cell != outer && cell->bel != saved.bel) ||
                    cell->ports.size() != saved.ports.size() || !info_equal(ctx, cell, saved.info)) return false;
                for (const auto &port : saved.ports) {
                    if (!cell->ports.count(port.first)) return false;
                    const auto &now = cell->ports.at(port.first);
                    if (now.name != port.second.name || now.type != port.second.type || now.net != port.second.net ||
                        now.user_idx != port.second.user_idx) return false;
                }
                size_t pin_index = 0;
                for (const auto &port : cell->ports) if (port.first != saved.port_order.at(pin_index++)) return false;
            }
            index = 0;
            for (const auto &entry : nets) if (entry.first != net_order[index].first || entry.second.get() != net_order[index++].second) return false;
            index = 0;
            for (const auto &entry : net_aliases) if (entry.first != alias_order[index].first || entry.second != alias_order[index++].second) return false;
            for (size_t i = 0; i < labs.size(); ++i) {
                const auto &a = saved_labs[i], &b = labs[i];
                if (a.is_mlab != b.is_mlab || a.clk_wires != b.clk_wires || a.ena_wires != b.ena_wires || a.aclr_wires != b.aclr_wires ||
                    a.sclr_wire != b.sclr_wire || a.sload_wire != b.sload_wire || a.aclr_used != b.aclr_used) return false;
                for (size_t j = 0; j < a.alms.size(); ++j) {
                    const auto &x = a.alms[j], &y = b.alms[j];
                    if (x.comb_out != y.comb_out || x.sel_clk != y.sel_clk || x.sel_ena != y.sel_ena || x.sel_aclr != y.sel_aclr ||
                        x.sel_ef != y.sel_ef || x.ff_in != y.ff_in || x.ff_out != y.ff_out || x.lut_bels != y.lut_bels ||
                        x.ff_bels != y.ff_bels || x.l6_mode != y.l6_mode || x.carry_mode != y.carry_mode ||
                        x.clk_ena_idx != y.clk_ena_idx || x.aclr_idx != y.aclr_idx ||
                        (!changed_alms.count({uint32_t(i), uint8_t(j)}) && x.unique_input_count != y.unique_input_count)) return false;
                }
            }
            return true;
        };
        auto isolated = [&](BelId bel) {
            const auto &target = bel_data(bel).lab_data; auto at = getBelLocation(bel);
            for (auto neighbour : getBelsByTile(at.x, at.y)) {
                if (!getBelType(neighbour).in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF)) continue;
                const auto &data = bel_data(neighbour).lab_data;
                if (data.lab == target.lab && data.alm == target.alm && getBoundBelCell(neighbour)) return false;
            }
            return true;
        };
        auto legal = [&]() {
            for (auto index : touched) for (const auto &alm : labs.at(index).alms)
                for (auto bel : {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2], alm.ff_bels[3]})
                    if (getBoundBelCell(bel) && !isBelLocationValid(bel)) return false;
            return true;
        };
        auto compatible_pins = [&](CellInfo *cell, BelId bel) {
            // Preserve the entire original physical pin frame. Half-specific
            // E/F and C/D mappings cannot migrate to the opposite bank.
            if (bel_data(bel).block_index % 6 != bel_data(cell == inner ? inner_bel : outer_bel).block_index % 6) return false;
            for (const auto &port : cell->ports) {
                if (!port.second.net) continue;
                if (!cell->pin_data.count(port.first) || cell->pin_data.at(port.first).bel_pins.empty()) return false;
                for (auto physical : cell->pin_data.at(port.first).bel_pins)
                    if (getBelPinWire(bel, physical) == WireId() || getBelPinType(bel, physical) != port.second.type) return false;
            }
            return true;
        };
        auto finite_delay = [](delay_t value) {
            return value >= 0 && value != std::numeric_limits<delay_t>::max() && std::isfinite(double(value));
        };
        auto arrival = [&](CellInfo *cell, int64_t mid_arrival, int64_t &result) {
            result = std::numeric_limits<int64_t>::lowest();
            for (int i = 0; i < mistral_remap_report::lut_width(cell->type); ++i) {
                auto pin = pair_pins[i]; auto net = cell->getPort(pin); if (!net) continue;
                DelayQuad logic; auto wire = ctx->predictArcDelay(net, {cell, pin});
                if (getBelPinsForCellPin(cell, pin).empty() || !getCellDelay(cell, pin, id_Q, logic) ||
                    !finite_delay(logic.maxDelay()) || !finite_delay(wire)) return false;
                int64_t input = net == mid ? mid_arrival : int64_t(arrivals.at(net));
                result = std::max(result, input + int64_t(wire) + int64_t(logic.maxDelay()));
            }
            return result != std::numeric_limits<int64_t>::lowest();
        };
        auto side_cost = [&](CellInfo *cell, int64_t incoming, int64_t &score) {
            auto net = cell->getPort(id_Q);
            if (getBelPinsForCellPin(cell, id_Q).empty()) return false;
            for (auto user : net->users) {
                auto wire = ctx->predictArcDelay(net, user);
                if (user.cell->bel == BelId() || getBelPinsForCellPin(user.cell, user.port).empty() || !finite_delay(wire)) return false;
                score = std::max(score, incoming + int64_t(wire));
            }
            return true;
        };
        struct Site { BelId bel; int64_t score; };
        struct Pair { BelId inner, outer; int64_t score; };
        try {
            auto sites = [&](CellInfo *cell) {
                std::vector<Site> result; auto center = lab(cone.sink.cell->bel);
                for (int x = center.first - 3; x <= center.first + 3; ++x) for (int y = center.second - 3; y <= center.second + 3; ++y) {
                    if (std::abs(x - center.first) + std::abs(y - center.second) > 3 || protected_labs.count({x, y})) continue;
                    for (auto bel : getBelsByTile(x, y)) {
                        if (!checkBelAvail(bel) || !isValidBelForCellType(cell->type, bel) || !isolated(bel) || !compatible_pins(cell, bel)) continue;
                        remember(cell->bel); remember(bel); unbindBel(cell->bel); bindBel(bel, cell, cell == inner ? inner_strength : outer_strength);
                        int64_t incoming = 0, score = std::numeric_limits<int64_t>::lowest();
                        if (isBelLocationValid(bel) && arrival(cell, int64_t(intermediate_arrival), incoming) && side_cost(cell, incoming, score))
                            result.push_back({bel, score});
                        restore();
                    }
                }
                std::sort(result.begin(), result.end(), [&](const Site &a, const Site &b) {
                    auto x = getBelLocation(a.bel), y = getBelLocation(b.bel);
                    return std::make_tuple(a.score, x.x, x.y, x.z) < std::make_tuple(b.score, y.x, y.y, y.z);
                });
                // Keep at most two isolated ALMs per tile so one good LAB
                // cannot consume the complete bounded search budget.
                std::set<std::pair<uint32_t, uint8_t>> seen_alms;
                std::map<Lab, int> tile_count;
                std::vector<Site> unique;
                for (auto site : result) {
                    const auto &data = bel_data(site.bel).lab_data;
                    if (seen_alms.emplace(data.lab, data.alm).second && tile_count[lab(site.bel)] < 2) {
                        ++tile_count[lab(site.bel)]; unique.push_back(site);
                    }
                    if (unique.size() == 24) break;
                }
                return unique;
            };
            auto inner_sites = sites(inner), outer_sites = sites(outer);
            std::vector<Pair> pairs;
            auto bind_pair = [&](BelId a, BelId b) {
                remember(inner_bel); remember(outer_bel); remember(a); remember(b);
                unbindBel(inner->bel); unbindBel(outer->bel);
                bindBel(a, inner, inner_strength); bindBel(b, outer, outer_strength);
            };
            for (auto a : inner_sites) for (auto b : outer_sites) {
                const auto &x = bel_data(a.bel).lab_data, &y = bel_data(b.bel).lab_data;
                if (x.lab == y.lab && x.alm == y.alm) continue;
                bind_pair(a.bel, b.bel);
                int64_t first = 0, second = 0, score = std::numeric_limits<int64_t>::lowest();
                if (legal() && arrival(inner, int64_t(intermediate_arrival), first) && arrival(outer, first, second) &&
                    side_cost(inner, first, score) && side_cost(outer, second, score)) pairs.push_back({a.bel, b.bel, score});
                restore();
            }
            std::sort(pairs.begin(), pairs.end(), [&](const Pair &a, const Pair &b) {
                auto x = getBelLocation(a.inner), y = getBelLocation(a.outer), u = getBelLocation(b.inner), v = getBelLocation(b.outer);
                return std::make_tuple(a.score, x.x, x.y, x.z, y.x, y.y, y.z) < std::make_tuple(b.score, u.x, u.y, u.z, v.x, v.y, v.z);
            });
            std::set<std::pair<Lab, Lab>> site_pairs;
            std::vector<Pair> diverse_pairs;
            for (auto pair : pairs) {
                if (site_pairs.emplace(lab(pair.inner), lab(pair.outer)).second) diverse_pairs.push_back(pair);
                if (diverse_pairs.size() == 24) break;
            }
            int examined = 0;
            for (auto pair : diverse_pairs) {
                if (examined++ == 16) break;
                bind_pair(pair.inner, pair.outer);
                if (!legal() || !fixed()) { restore(true); continue; }
                TimingAnalyser after(ctx); after.with_clock_skew = true; after.setup(false, false, true);
                float slack = after.get_setup_slack(CellPortKey(cone.sink));
                bool improve = !after.have_loops && guard::timed(slack) && slack >= old_slack + 250;
                bool endpoints_ok = true;
                for (const auto &entry : endpoints) {
                    guard::Rows rows;
                    endpoints_ok &= after.get_endpoint_clock_pair_timings(entry.first, rows) && guard::rows_nonregressing(entry.second, rows, false);
                }
                if (!reference_endpoints.empty()) {
                    TimingAnalyser unskewed(ctx); unskewed.with_clock_skew = false; unskewed.setup(false, false, true);
                    for (const auto &entry : reference_endpoints) {
                        guard::Rows rows;
                        endpoints_ok &= unskewed.get_endpoint_clock_pair_timings(entry.first, rows) && guard::rows_nonregressing(entry.second, rows, true);
                    }
                }
                bool clocks = guard::clocks_nonregressing(before, after), hold = guard::holds_nonregressing(old_holds, guard::holds(after));
                bool fixed_graph = legal() && fixed();
                log_info("LUT pair placement trial inner=%s outer=%s sink=%s.%s inner_bel=%s outer_bel=%s gain=%.0fps improve=%d endpoints=%d clocks=%d hold=%d fixed=%d\n",
                    nameOf(inner), nameOf(outer), nameOf(cone.sink.cell), cone.sink.port.c_str(ctx), nameOfBel(pair.inner), nameOfBel(pair.outer),
                    slack - old_slack, int(improve), int(endpoints_ok), int(clocks), int(hold), int(fixed_graph));
                if (improve && endpoints_ok && clocks && hold && fixed_graph) {
                    log_info("LUT pair placement candidate %d: inner=%s outer=%s sink=%s.%s inner_bel=%s outer_bel=%s gain=%.0fps.\n", qualified,
                        nameOf(inner), nameOf(outer), nameOf(cone.sink.cell), cone.sink.port.c_str(ctx), nameOfBel(pair.inner), nameOfBel(pair.outer), slack - old_slack);
                    if (selection == qualified++) {
                        ctx->check();
                        log_info("LUT pair placement applied candidate %d; full routing and signoff still required.\n", selection);
                        return true;
                    }
                }
                restore(true);
            }
            restore(true); ctx->check();
        } catch (...) {
            restore(true); ctx->check(); throw;
        }
    }
    log_info("LUT pair placement: %d qualified candidates; no candidate applied.\n", qualified);
    return false;
}
NEXTPNR_NAMESPACE_END
