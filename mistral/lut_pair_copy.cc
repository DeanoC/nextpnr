/* Optional isolated composition copy for a whole enable cohort. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "json11.hpp"
#include "timing.h"
#include "lut_pair_placement.h"
#include "local_remap_policy.h"
#include "remap_report.h"
#include "remap_clock_guard.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Lab = std::pair<int, int>;
const std::array<IdString, 6> copy_pins = {id_A, id_B, id_C, id_D, id_E, id_F};
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
bool users_equal(const indexed_store<PortRef> &a, const indexed_store<PortRef> &b)
{
    if (a.entries() != b.entries() || a.capacity() != b.capacity()) return false;
    auto left = a.enumerate(), right = b.enumerate();
    auto x = left.begin(), y = right.begin();
    for (; x != left.end() && y != right.end(); ++x, ++y)
        if ((*x).index != (*y).index || (*x).value.cell != (*y).value.cell || (*x).value.port != (*y).value.port) return false;
    if (x != left.end() || y != right.end()) return false;
    // Probe copies only: compare the complete hole/free-list sequence with a
    // fixed bound captured before add() can grow either copy.
    auto ac = a, bc = b;
    const size_t probes = size_t(a.capacity()) + 1;
    for (size_t i = 0; i < probes; ++i)
        if (ac.add(PortRef{}) != bc.add(PortRef{})) return false;
    return true;
}
} // namespace

bool Arch::remap_lut_pair_copy_critical(const std::string &report, int selection)
{
    namespace guard = mistral_remap_clock_guard;
    namespace policy = local_remap_policy;
    Context *ctx = getCtx();
    if (selection < -1) log_error("Invalid LUT pair copy candidate index.\n");
    if (selection < 0 && !lut_driver_copy_report.empty())
        log_error("A LUT pair placement listing must be final; it cannot precede LUT driver copy.\n");
    if (fes_any_slot_region_active) log_error("LUT pair copy requires ordinary placement.\n");
    prevalidate_lut_pair_prefix(ctx);
    for (const auto &entry : nets) if (!entry.second->wires.empty())
        log_error("LUT pair copy requires an unrouted design.\n");
    std::string error;
    auto json = json11::Json::parse(report, error);
    if (!error.empty() || !json.is_object() || !json["critical_paths"].is_array())
        log_error("Invalid LUT pair copy timing report.\n");
    auto lab = [&](BelId bel) { auto at = getBelLocation(bel); return Lab(at.x, at.y); };
    auto weak = [&](const CellInfo *cell) {
        return cell && cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK &&
               cell->cluster == ClusterId() && !cell->region && !cell->isPseudo() && !protected_attrs(cell->attrs, ctx);
    };
    std::set<Lab> protected_labs;
    for (const auto &entry : cells) {
        auto *cell = entry.second.get();
        if (cell->bel != BelId() && (!weak(cell) || cell->type.in(id_MISTRAL_MLAB, id_MISTRAL_ALUT_ARITH)))
            protected_labs.insert(lab(cell->bel));
    }
    std::set<const NetInfo *> boundary;
    for (const auto &entry : ctx->ports) if (entry.second.net) boundary.insert(entry.second.net);
    auto ordinary = [&](const NetInfo *net) {
        return net && net->driver.cell && !net->is_global && !net->clkconstr && !net->region &&
               net->wires.empty() && net->constant_value == IdString() && !boundary.count(net) &&
               !protected_attrs(net->attrs, ctx) && net->driver.cell->ports.count(net->driver.port) &&
               net->driver.cell->ports.at(net->driver.port).type == PORT_OUT &&
               net->driver.cell->getPort(net->driver.port) == net;
    };
    auto physical_pins = [&](const CellInfo *cell) {
        if (cell->bel == BelId()) return false;
        for (const auto &port : cell->ports) {
            auto state = cell->get_pin_state(port.first);
            if (state == PIN_0 || state == PIN_1) continue;
            if (!cell->pin_data.count(port.first) || cell->pin_data.at(port.first).bel_pins.empty()) return false;
            for (auto pin : cell->pin_data.at(port.first).bel_pins)
                if (getBelPinWire(cell->bel, pin) == WireId() || getBelPinType(cell->bel, pin) != port.second.type) return false;
        }
        return true;
    };
    auto table_valid = [&](const CellInfo *cell) {
        int width = mistral_remap_report::lut_width(cell->type);
        if (!width || cell->params.size() != 1 || !cell->params.count(id_LUT) ||
            !cell->params.at(id_LUT).is_fully_def() || cell->params.at(id_LUT).size() != (1u << width) ||
            cell->ports.size() != size_t(width + 1) || cell->get_pin_state(id_Q) != PIN_SIG || !physical_pins(cell)) return false;
        for (int i = 0; i < width; ++i) {
            auto pin = copy_pins[i];
            if (!cell->ports.count(pin) || cell->ports.at(pin).type != PORT_IN) return false;
            auto state = cell->get_pin_state(pin); auto net = cell->getPort(pin);
            if (state == PIN_0 || state == PIN_1) { if (net) return false; }
            else if ((state != PIN_SIG && state != PIN_INV) || !ordinary(net)) return false;
        }
        auto output = cell->getPort(id_Q);
        return cell->ports.count(id_Q) && cell->ports.at(id_Q).type == PORT_OUT && ordinary(output) &&
               output->driver.cell == cell && output->driver.port == id_Q;
    };
    auto clocked = [&](CellInfo *cell, IdString port, int count) {
        if (count <= 0 || cell->bel == BelId() || getBelPinsForCellPin(cell, port).empty()) return false;
        for (int i = 0; i < count; ++i) {
            auto info = getPortClockingInfo(cell, port, i); auto clock = cell->getPort(info.clock_port);
            if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay() <= 0) return false;
        }
        return true;
    };
    struct Cone { CellInfo *inner, *outer; PortRef sink; double excess; };
    std::vector<Cone> cones;
    std::set<std::tuple<IdString, IdString, Lab>> seen;
    // Complete path validation precedes even name reservation or graph changes.
    for (const auto &path : mistral_remap_report::validate(ctx, json, true)) {
        if (path.edges.size() < 2) continue;
        const auto &a = path.edges.at(path.edges.size() - 2), &b = path.edges.back();
        if (a.first.port != id_Q || b.first.port != id_Q || a.second.cell != b.first.cell ||
            !mistral_remap_report::lut_width(a.first.cell->type) || !mistral_remap_report::lut_width(b.first.cell->type) ||
            a.first.cell == b.first.cell || b.second.cell->type != id_MISTRAL_FF || b.second.port != id_ENA) continue;
        auto key = std::make_tuple(a.first.cell->name, b.first.cell->name, lab(b.second.cell->bel));
        if (seen.insert(key).second) cones.push_back({a.first.cell, b.first.cell, b.second, path.excess});
    }
    std::sort(cones.begin(), cones.end(), [&](const Cone &a, const Cone &b) {
        return std::make_tuple(-a.excess, a.inner->name.str(ctx), a.outer->name.str(ctx), a.sink.cell->name.str(ctx)) <
               std::make_tuple(-b.excess, b.inner->name.str(ctx), b.outer->name.str(ctx), b.sink.cell->name.str(ctx));
    });
    if (cones.size() > 8) cones.resize(8);
    log_info("LUT pair copy discovery: %zu bounded cones.\n", cones.size());
    int qualified = 0;
    for (const auto &cone : cones) {
        auto *inner = cone.inner, *outer = cone.outer, *sink = cone.sink.cell;
        auto *mid = inner->getPort(id_Q), *root = outer->getPort(id_Q);
        auto reject = [&](const char *reason) {
            log_info("LUT pair copy rejection inner=%s outer=%s sink=%s.ENA reason=%s\n",
                     nameOf(inner), nameOf(outer), nameOf(sink), reason);
        };
        if (!weak(inner) || !weak(outer) || protected_labs.count(lab(inner->bel)) || protected_labs.count(lab(outer->bel)) ||
            !table_valid(inner) || !table_valid(outer)) { reject("source-pair-not-safe-ordinary-luts"); continue; }
        if (!weak(sink) || !sink->ports.count(id_ENA) || sink->ports.at(id_ENA).type != PORT_IN ||
            (sink->get_pin_state(id_ENA) != PIN_SIG && sink->get_pin_state(id_ENA) != PIN_INV)) {
            reject("target-not-safe-ordinary-ff-enable"); continue;
        }
        auto *clock = sink->getPort(id_CLK);
        if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay() <= 0 ||
            (sink->get_pin_state(id_CLK) != PIN_SIG && sink->get_pin_state(id_CLK) != PIN_INV)) {
            reject("target-clock-unavailable"); continue;
        }
        std::vector<CellInfo *> cohort;
        bool eligible = true;
        for (auto user : root->users) {
            if (!user.cell || !user.cell->ports.count(user.port) || user.cell->getPort(user.port) != root ||
                user.cell->ports.at(user.port).type != PORT_IN) { eligible = false; break; }
            if (user.cell->bel == BelId()) { eligible = false; break; }
            if (lab(user.cell->bel) != lab(sink->bel)) continue;
            auto *cell = user.cell;
            if (cell->type != id_MISTRAL_FF || user.port != id_ENA || !weak(cell) || cell->getPort(id_CLK) != clock ||
                cell->get_pin_state(id_CLK) != sink->get_pin_state(id_CLK) ||
                (cell->get_pin_state(id_ENA) != PIN_SIG && cell->get_pin_state(id_ENA) != PIN_INV)) { eligible = false; break; }
            cohort.push_back(cell);
        }
        std::sort(cohort.begin(), cohort.end(), [&](CellInfo *a, CellInfo *b) { return a->name.str(ctx) < b->name.str(ctx); });
        if (!eligible || cohort.empty() || std::find(cohort.begin(), cohort.end(), sink) == cohort.end()) {
            reject("whole-enable-cohort-unavailable"); continue;
        }
        std::vector<NetInfo *> external;
        auto encode = [&](const CellInfo *cell, bool outer_input) {
            std::vector<policy::Pin> result;
            for (int i = 0; i < mistral_remap_report::lut_width(cell->type); ++i) {
                auto pin = copy_pins[i]; auto state = cell->get_pin_state(pin); auto net = cell->getPort(pin);
                int signal;
                if (state == PIN_0 || state == PIN_1) signal = state == PIN_1 ? policy::ONE : policy::ZERO;
                else if (outer_input && net == mid) signal = policy::INTERMEDIATE;
                else {
                    if (net == mid || net == root) eligible = false;
                    auto found = std::find(external.begin(), external.end(), net);
                    if (found == external.end()) { signal = int(external.size()); external.push_back(net); }
                    else signal = int(found - external.begin());
                }
                result.push_back({signal, state == PIN_INV});
            }
            return result;
        };
        auto inner_inputs = encode(inner, false), outer_inputs = encode(outer, true);
        uint64_t inner_mask = inner->params.at(id_LUT).as_int64(), outer_mask = outer->params.at(id_LUT).as_int64();
        auto composition = policy::compose(inner_mask, inner_inputs, outer_mask, outer_inputs);
        if (!eligible || !composition.valid || composition.signals.size() > 6) { reject("composition-unavailable"); continue; }
        const int width = std::max(2, int(composition.signals.size()));
        uint64_t clone_mask = 0;
        // Re-evaluate the actual source pins, including constants, inversions
        // and shared leaves. The new LUT uses plain inputs in canonical order.
        for (unsigned row = 0; row < (1u << width); ++row) {
            auto value = [&](policy::Pin pin, bool intermediate) {
                bool bit = pin.signal == policy::ONE || (pin.signal == policy::INTERMEDIATE && intermediate);
                if (pin.signal >= 0) {
                    auto index = std::find(composition.signals.begin(), composition.signals.end(), pin.signal);
                    NPNR_ASSERT(index != composition.signals.end());
                    bit = (row >> (index - composition.signals.begin())) & 1;
                }
                return bit != pin.inverted;
            };
            unsigned input_row = 0, output_row = 0;
            for (size_t i = 0; i < inner_inputs.size(); ++i) input_row |= unsigned(value(inner_inputs[i], false)) << i;
            bool intermediate = (inner_mask >> input_row) & 1;
            for (size_t i = 0; i < outer_inputs.size(); ++i) output_row |= unsigned(value(outer_inputs[i], intermediate)) << i;
            bool expected = (outer_mask >> output_row) & 1;
            unsigned meaningful = row & ((1u << composition.signals.size()) - 1);
            NPNR_ASSERT(expected == bool((composition.mask >> meaningful) & 1));
            if (expected) clone_mask |= uint64_t(1) << row;
        }
        std::vector<NetInfo *> leaves;
        for (int signal : composition.signals) leaves.push_back(external.at(signal));
        std::set<CellPortKey> endpoint_keys;
        std::set<NetInfo *> visiting, complete;
        std::function<bool(NetInfo *)> follow = [&](NetInfo *net) {
            if (!ordinary(net)) return false;
            if (complete.count(net)) return true;
            if (!visiting.insert(net).second) return false;
            for (auto user : net->users) {
                auto *cell = user.cell;
                if (!cell || !cell->ports.count(user.port) || cell->getPort(user.port) != net ||
                    cell->ports.at(user.port).type != PORT_IN || !net->users.count(cell->ports.at(user.port).user_idx)) return false;
                auto slot = net->users.at(cell->ports.at(user.port).user_idx);
                if (slot.cell != cell || slot.port != user.port) return false;
                int count = 0; auto timing_class = getPortTimingClass(cell, user.port, count);
                if (timing_class == TMG_REGISTER_INPUT) {
                    if (!clocked(cell, user.port, count)) return false;
                    endpoint_keys.insert(CellPortKey(user)); continue;
                }
                if (timing_class != TMG_COMB_INPUT || cell->bel == BelId() ||
                    !(mistral_remap_report::lut_width(cell->type) || cell->type.in(id_MISTRAL_BUF, id_MISTRAL_NOT, id_MISTRAL_ALUT_ARITH))) return false;
                if (mistral_remap_report::lut_width(cell->type) && !table_valid(cell)) return false;
                if (cell->type == id_MISTRAL_ALUT_ARITH &&
                    (!cell->params.count(id_LUT0) || !cell->params.count(id_LUT1) ||
                     !cell->params.at(id_LUT0).is_fully_def() || !cell->params.at(id_LUT1).is_fully_def() ||
                     cell->params.at(id_LUT0).size() != 16 || cell->params.at(id_LUT1).size() != 16)) return false;
                bool arc = false;
                for (const auto &port : cell->ports) if (port.second.type == PORT_OUT) {
                    int outputs = 0; DelayQuad delay;
                    if (getPortTimingClass(cell, port.first, outputs) == TMG_COMB_OUTPUT && getCellDelay(cell, user.port, port.first, delay)) {
                        arc = true; if (!follow(port.second.net)) return false;
                    }
                }
                if (!arc) return false;
            }
            visiting.erase(net); complete.insert(net); return true;
        };
        eligible = follow(mid) && follow(root);
        for (auto leaf : leaves) {
            int count = 0; auto cls = getPortTimingClass(leaf->driver.cell, leaf->driver.port, count);
            eligible &= (cls == TMG_COMB_OUTPUT || (cls == TMG_REGISTER_OUTPUT && clocked(leaf->driver.cell, leaf->driver.port, count))) &&
                        leaf->driver.cell->bel != BelId() && !getBelPinsForCellPin(leaf->driver.cell, leaf->driver.port).empty() && follow(leaf);
        }
        for (auto cell : cohort) eligible &= endpoint_keys.count(CellPortKey(cell->name, id_ENA)) != 0;
        if (!eligible || endpoint_keys.empty()) { reject("boundary-clock-or-fanout-unavailable"); continue; }
        TimingAnalyser before(ctx); before.with_clock_skew = true; before.setup(false, false, true);
        float old_slack = before.get_setup_slack(CellPortKey(cone.sink));
        if (before.have_loops || before.clock_fmax.empty() || !guard::timed(old_slack)) { reject("native-baseline-unavailable"); continue; }
        std::map<CellPortKey, guard::Rows> endpoints, reference_endpoints;
        size_t timed_pairs = 0, unrelated_pairs = 0, hold_pairs = 0;
        for (auto key : endpoint_keys) {
            guard::Rows rows;
            if (!before.get_endpoint_clock_pair_timings(key, rows) || rows.empty()) { eligible = false; break; }
            for (const auto &row : rows) {
                timed_pairs += row.setup_timed; unrelated_pairs += !row.setup_timed; hold_pairs += row.hold_related;
                if (row.setup_timed && (!row.setup_window || !row.setup_margin || !row.hold_related || !row.hold_margin)) eligible = false;
            }
            endpoints.emplace(key, std::move(rows));
        }
        if (eligible && unrelated_pairs) {
            TimingAnalyser reference(ctx); reference.with_clock_skew = false; reference.setup(false, false, true);
            for (const auto &entry : endpoints) {
                guard::Rows rows;
                if (!reference.get_endpoint_clock_pair_timings(entry.first, rows) || !guard::rows_match(entry.second, rows)) eligible = false;
                else reference_endpoints.emplace(entry.first, std::move(rows));
            }
        }
        std::map<NetInfo *, delay_t> arrivals;
        for (auto leaf : leaves) {
            delay_t value = 0;
            if (!before.get_max_arrival(CellPortKey(leaf->driver), value) || !std::isfinite(double(value)) ||
                value == std::numeric_limits<delay_t>::max() || value == std::numeric_limits<delay_t>::lowest()) eligible = false;
            else arrivals.emplace(leaf, value);
        }
        if (!eligible) { reject("endpoint-or-arrival-coverage-unavailable"); continue; }
        auto old_holds = guard::holds(before);
        log_info("LUT pair copy composition inner=%s outer=%s sink=%s.ENA cohort=%zu leaves=%zu truth_rows=%zu mask=0x%016llx\n",
                 nameOf(inner), nameOf(outer), nameOf(sink), cohort.size(), leaves.size(), size_t(1) << leaves.size(), (unsigned long long)clone_mask);
        log_info("LUT pair copy domains inner=%s outer=%s sink=%s.ENA endpoints=%zu timed_pairs=%zu unrelated_pairs=%zu hold_pairs=%zu reference_free=%d\n",
                 nameOf(inner), nameOf(outer), nameOf(sink), endpoints.size(), timed_pairs, unrelated_pairs, hold_pairs, int(unrelated_pairs != 0));

        struct Original {
            CellInfo *cell; IdString name, type, hierpath; BelId bel; PlaceStrength strength; ClusterId cluster; Region *region; PseudoCell *pseudo;
            dict<IdString, Property> params, attrs; dict<IdString, PortInfo> ports; std::vector<IdString> port_order; ArchCellInfo info;
        };
        struct Net {
            NetInfo *net; IdString name, hierpath, constant; Region *region; bool global; PortRef driver;
            dict<IdString, Property> attrs; std::vector<IdString> aliases; indexed_store<PortRef> users;
        };
        std::vector<Original> originals;
        std::vector<Net> original_nets;
        std::vector<std::pair<IdString, IdString>> alias_order;
        for (const auto &entry : cells) {
            auto *cell = entry.second.get(); std::vector<IdString> order;
            for (const auto &port : cell->ports) order.push_back(port.first);
            originals.push_back({cell, cell->name, cell->type, cell->hierpath, cell->bel, cell->belStrength, cell->cluster, cell->region, cell->pseudo_cell.get(),
                                 cell->params, cell->attrs, cell->ports, std::move(order), static_cast<const ArchCellInfo &>(*cell)});
        }
        for (const auto &entry : nets) {
            auto *net = entry.second.get();
            original_nets.push_back({net, net->name, net->hierpath, net->constant_value, net->region, net->is_global,
                                     net->driver, net->attrs, net->aliases, net->users});
        }
        for (const auto &entry : net_aliases) alias_order.emplace_back(entry.first, entry.second);
        auto clock_constraints = [&]() {
            std::map<IdString, std::tuple<delay_t, delay_t, delay_t, delay_t, delay_t, delay_t, IdString, delay_t>> result;
            for (const auto &entry : nets) if (entry.second->clkconstr) {
                const auto &c = *entry.second->clkconstr;
                result.emplace(entry.first, std::make_tuple(c.period.minDelay(), c.period.maxDelay(), c.high.minDelay(),
                               c.high.maxDelay(), c.low.minDelay(), c.low.maxDelay(), c.phase_group, c.phase_shift));
            }
            return result;
        };
        const auto original_clocks = clock_constraints();
        const auto original_labs = labs;
        // Probe both namespaces without interning rejected suffixes. One
        // occupied name blocks each pair; the finite owner count bounds search.
        std::set<std::string> occupied;
        for (const auto &entry : cells) occupied.insert(entry.first.str(ctx));
        for (const auto &entry : nets) occupied.insert(entry.first.str(ctx));
        for (const auto &entry : net_aliases) occupied.insert(entry.first.str(ctx));
        std::string stem = outer->name.str(ctx) + "$lut_pair_copy", clone_name;
        const size_t name_bound = occupied.size();
        if (name_bound == std::numeric_limits<size_t>::max()) { reject("private-name-space-exhausted"); continue; }
        for (size_t suffix = 0; suffix <= name_bound; ++suffix) {
            auto candidate = stem + (suffix ? "$" + std::to_string(suffix) : std::string());
            if (!occupied.count(candidate) && !occupied.count(candidate + "$Q")) { clone_name = candidate; break; }
        }
        if (clone_name.empty()) { reject("private-name-space-exhausted"); continue; }
        auto cname = id(clone_name), nname = id(clone_name + "$Q");
        decltype(cells) parked_cells;
        decltype(nets) parked_nets;
        decltype(net_aliases) parked_aliases;
        CellInfo *clone = nullptr;
        NetInfo *output = nullptr;
        bool live = false;
        auto rollback = [&]() {
            if (!live) return;
            auto found = cells.find(cname);
            if (found != cells.end() && found->second) {
                auto *created = found->second.get();
                if (created->bel != BelId()) unbindBel(created->bel);
                for (auto &port : created->ports) created->disconnectPort(port.first);
                cells.erase(cname);
            }
            nets.erase(nname); net_aliases.erase(nname);
            // Original owners are stable unique_ptr objects, although adding a
            // clone may reallocate the dictionary's entry storage.
            for (auto &entry : cells) parked_cells.at(entry.first) = std::move(entry.second);
            for (auto &entry : nets) parked_nets.at(entry.first) = std::move(entry.second);
            cells.swap(parked_cells); nets.swap(parked_nets); net_aliases.swap(parked_aliases);
            for (auto &saved : originals) {
                saved.cell->ports = saved.ports;
                static_cast<ArchCellInfo &>(*saved.cell) = saved.info;
            }
            for (auto &saved : original_nets) std::swap(saved.net->users, saved.users);
            labs = original_labs; live = false; ctx->check();
        };
        try {
            cells.swap(parked_cells); nets.swap(parked_nets); net_aliases.swap(parked_aliases); live = true;
            for (auto it = originals.rbegin(); it != originals.rend(); ++it) cells[it->name] = std::move(parked_cells.at(it->name));
            for (auto it = original_nets.rbegin(); it != original_nets.rend(); ++it) nets[it->name] = std::move(parked_nets.at(it->name));
            net_aliases = parked_aliases;
            const std::array<IdString, 5> types = {id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
            clone = ctx->createCell(cname, types.at(width - 2));
            clone->params[id_LUT] = Property(int64_t(clone_mask), 1u << width);
            for (int i = 0; i < width; ++i) {
                clone->addInput(copy_pins[i]);
                if (i < int(leaves.size())) clone->connectPort(copy_pins[i], leaves[i]);
                else clone->pin_data[copy_pins[i]].state = PIN_0;
            }
            output = ctx->createNet(nname); clone->addOutput(id_Q); clone->connectPort(id_Q, output);
            assign_comb_info(clone); assign_default_pinmap(clone);
            for (auto cell : cohort) { cell->disconnectPort(id_ENA); cell->connectPort(id_ENA, output); assign_ff_info(cell); update_bel(cell->bel); }
            std::map<NetInfo *, indexed_store<PortRef>> expected_users;
            for (const auto &saved : original_nets) expected_users.emplace(saved.net, saved.net->users);
            std::map<CellInfo *, PortInfo> expected_enable;
            std::map<CellInfo *, ArchCellInfo> expected_info;
            for (const auto &saved : originals) {
                auto info = saved.info;
                if (std::find(cohort.begin(), cohort.end(), saved.cell) != cohort.end()) info.ffInfo.ctrlset.ena.net = output;
                expected_info.emplace(saved.cell, std::move(info));
            }
            for (auto cell : cohort) expected_enable.emplace(cell, cell->ports.at(id_ENA));
            const auto post_labs = labs;
            const auto clone_info = static_cast<const ArchCellInfo &>(*clone);
            const auto clone_ports = clone->ports;
            const auto output_users = output->users;
            std::set<uint32_t> affected_labs;
            std::set<std::pair<uint32_t, uint8_t>> enable_alms;
            for (auto cell : cohort) {
                const auto &data = bel_data(cell->bel).lab_data;
                affected_labs.insert(data.lab); enable_alms.emplace(data.lab, data.alm);
            }
            auto restore_probe = [&](bool complete = true) {
                if (clone->bel != BelId()) unbindBel(clone->bel);
                if (complete) {
                    for (const auto &saved : originals) static_cast<ArchCellInfo &>(*saved.cell) = expected_info.at(saved.cell);
                    labs = post_labs;
                } else {
                    // Pure geometric ranking only binds the clone and updates
                    // ALM input counts; no timing or original cell cache runs.
                    for (auto index : affected_labs) labs.at(index) = post_labs.at(index);
                }
                static_cast<ArchCellInfo &>(*clone) = clone_info;
            };
            auto fixed = [&]() {
                if (cells.size() != originals.size() + 1 || nets.size() != original_nets.size() + 1 ||
                    net_aliases.size() != alias_order.size() + 1 || clock_constraints() != original_clocks ||
                    !net_aliases.count(nname) || net_aliases.at(nname) != nname) return false;
                size_t index = 0;
                for (const auto &entry : cells) {
                    if (entry.first == cname) { if (entry.second.get() != clone) return false; continue; }
                    if (index >= originals.size()) return false;
                    const auto &saved = originals.at(index++); auto *cell = entry.second.get();
                    if (cell != saved.cell || entry.first != saved.name || cell->name != saved.name || cell->type != saved.type || cell->hierpath != saved.hierpath || cell->bel != saved.bel ||
                        cell->belStrength != saved.strength || cell->cluster != saved.cluster || cell->region != saved.region ||
                        cell->pseudo_cell.get() != saved.pseudo ||
                        cell->params != saved.params || cell->attrs != saved.attrs || cell->ports.size() != saved.ports.size() ||
                        !info_equal(ctx, cell, expected_info.at(cell))) return false;
                    size_t pin = 0;
                    for (const auto &port : cell->ports) if (port.first != saved.port_order.at(pin++)) return false;
                    for (const auto &port : saved.ports) {
                        auto now = cell->ports.find(port.first); if (now == cell->ports.end()) return false;
                        const auto &expected = expected_enable.count(cell) && port.first == id_ENA ? expected_enable.at(cell) : port.second;
                        if (now->second.name != expected.name || now->second.type != expected.type || now->second.net != expected.net ||
                            now->second.user_idx != expected.user_idx) return false;
                    }
                }
                if (index != originals.size()) return false;
                index = 0;
                for (const auto &entry : nets) {
                    if (entry.first == nname) { if (entry.second.get() != output) return false; continue; }
                    if (index >= original_nets.size()) return false;
                    const auto &saved = original_nets.at(index++); auto *net = entry.second.get();
                    if (net != saved.net || entry.first != saved.name || net->name != saved.name || net->hierpath != saved.hierpath ||
                        net->constant_value != saved.constant || net->region != saved.region || net->is_global != saved.global ||
                        net->attrs != saved.attrs || net->aliases != saved.aliases || !net->wires.empty() ||
                        net->driver.cell != saved.driver.cell || net->driver.port != saved.driver.port ||
                        !users_equal(net->users, expected_users.at(net))) return false;
                }
                if (index != original_nets.size()) return false;
                index = 0;
                for (const auto &entry : net_aliases) {
                    if (entry.first == nname) continue;
                    if (index >= alias_order.size() || entry.first != alias_order[index].first || entry.second != alias_order[index++].second) return false;
                }
                if (index != alias_order.size() || output->name != nname || output->driver.cell != clone || output->driver.port != id_Q ||
                    output->users.entries() != int(cohort.size()) || !users_equal(output->users, output_users) ||
                    !output->attrs.empty() || !output->aliases.empty() || output->is_global || output->clkconstr || output->region ||
                    output->constant_value != IdString() || !output->wires.empty() || clone->getPort(id_Q) != output ||
                    clone->type != types.at(width - 2) || clone->cluster != ClusterId() || clone->region || clone->isPseudo() ||
                    clone->belStrength != STRENGTH_WEAK || !clone->attrs.empty() || clone->params.size() != 1 ||
                    clone->params.at(id_LUT).as_int64() != int64_t(clone_mask) || !info_equal(ctx, clone, clone_info) ||
                    clone->ports.size() != clone_ports.size() || !physical_pins(clone)) return false;
                for (const auto &port : clone_ports) {
                    if (!clone->ports.count(port.first)) return false;
                    const auto &now = clone->ports.at(port.first);
                    if (now.name != port.second.name || now.type != port.second.type || now.net != port.second.net ||
                        now.user_idx != port.second.user_idx) return false;
                }
                for (auto user : output->users) if (!expected_enable.count(user.cell) || user.port != id_ENA) return false;
                if (labs.size() != original_labs.size()) return false;
                const auto &target = bel_data(clone->bel).lab_data;
                for (size_t i = 0; i < labs.size(); ++i) {
                    const auto &a = original_labs[i], &b = labs[i];
                    if (a.is_mlab != b.is_mlab || a.clk_wires != b.clk_wires || a.ena_wires != b.ena_wires || a.aclr_wires != b.aclr_wires ||
                        a.sclr_wire != b.sclr_wire || a.sload_wire != b.sload_wire || a.aclr_used != b.aclr_used) return false;
                    for (size_t j = 0; j < a.alms.size(); ++j) {
                        const auto &x = a.alms[j], &y = b.alms[j]; bool added = i == target.lab && j == target.alm;
                        if (x.comb_out != y.comb_out || x.sel_clk != y.sel_clk || x.sel_ena != y.sel_ena || x.sel_aclr != y.sel_aclr ||
                            x.sel_ef != y.sel_ef || x.ff_in != y.ff_in || x.ff_out != y.ff_out || x.lut_bels != y.lut_bels ||
                            x.ff_bels != y.ff_bels || x.carry_mode != y.carry_mode || x.clk_ena_idx != y.clk_ena_idx || x.aclr_idx != y.aclr_idx ||
                            x.l6_mode != y.l6_mode ||
                            (!added && !enable_alms.count({uint32_t(i), uint8_t(j)}) && x.unique_input_count != y.unique_input_count)) return false;
                    }
                }
                return true;
            };
            auto isolated = [&](BelId bel) {
                const auto &data = bel_data(bel).lab_data;
                for (auto other : getBelsByTile(getBelLocation(bel).x, getBelLocation(bel).y)) {
                    if (!getBelType(other).in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF)) continue;
                    const auto &candidate = bel_data(other).lab_data;
                    if (candidate.lab == data.lab && candidate.alm == data.alm && getBoundBelCell(other)) return false;
                }
                return true;
            };
            auto legal = [&]() {
                for (auto index : affected_labs) for (const auto &alm : labs.at(index).alms)
                    for (auto bel : {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2], alm.ff_bels[3]})
                        if (getBoundBelCell(bel) && !isBelLocationValid(bel)) return false;
                return true;
            };
            auto bind = [&](BelId bel) {
                bindBel(bel, clone, STRENGTH_WEAK);
                affected_labs.insert(bel_data(bel).lab_data.lab);
            };
            struct Site { BelId bel; int64_t score; };
            std::vector<Site> sites;
            auto center = lab(sink->bel);
            for (int x = center.first - 3; x <= center.first + 3; ++x) for (int y = center.second - 3; y <= center.second + 3; ++y) {
                if (std::abs(x - center.first) + std::abs(y - center.second) > 3 || protected_labs.count({x, y})) continue;
                for (auto bel : getBelsByTile(x, y)) {
                    if (!checkBelAvail(bel) || !isValidBelForCellType(clone->type, bel) || !isolated(bel)) continue;
                    bind(bel);
                    int64_t incoming = 0, score = std::numeric_limits<int64_t>::lowest();
                    bool finite = physical_pins(clone) && isBelLocationValid(bel);
                    for (size_t i = 0; i < leaves.size() && finite; ++i) {
                        DelayQuad delay; auto wire = ctx->predictArcDelay(leaves[i], {clone, copy_pins[i]});
                        finite &= getCellDelay(clone, copy_pins[i], id_Q, delay) && wire >= 0 &&
                                  wire != std::numeric_limits<delay_t>::max() && delay.maxDelay() >= 0 &&
                                  delay.maxDelay() != std::numeric_limits<delay_t>::max();
                        if (finite) incoming = std::max(incoming, int64_t(arrivals.at(leaves[i])) + int64_t(wire) + int64_t(delay.maxDelay()));
                    }
                    for (auto cell : cohort) if (finite) {
                        auto wire = ctx->predictArcDelay(output, {cell, id_ENA});
                        finite &= wire >= 0 && wire != std::numeric_limits<delay_t>::max();
                        if (finite) score = std::max(score, incoming + int64_t(wire));
                    }
                    if (finite) sites.push_back({bel, score});
                    restore_probe(false);
                }
            }
            std::sort(sites.begin(), sites.end(), [&](const Site &a, const Site &b) {
                auto x = getBelLocation(a.bel), y = getBelLocation(b.bel);
                return std::make_tuple(a.score, x.x, x.y, x.z) < std::make_tuple(b.score, y.x, y.y, y.z);
            });
            std::set<Lab> selected_tiles;
            std::vector<Site> diverse;
            for (auto site : sites) if (selected_tiles.insert(lab(site.bel)).second) {
                diverse.push_back(site); if (diverse.size() == 16) break;
            }
            for (auto site : diverse) {
                bind(site.bel);
                if (!legal() || !fixed()) { restore_probe(); continue; }
                TimingAnalyser after(ctx); after.with_clock_skew = true; after.setup(false, false, true);
                float slack = after.get_setup_slack(CellPortKey(cone.sink));
                bool improve = !after.have_loops && guard::timed(slack) && slack >= old_slack + 250;
                bool endpoints_ok = true;
                for (const auto &entry : endpoints) {
                    guard::Rows rows;
                    endpoints_ok &= after.get_endpoint_clock_pair_timings(entry.first, rows) && guard::rows_nonregressing(entry.second, rows, false);
                }
                if (!reference_endpoints.empty()) {
                    TimingAnalyser reference(ctx); reference.with_clock_skew = false; reference.setup(false, false, true);
                    for (const auto &entry : reference_endpoints) {
                        guard::Rows rows;
                        endpoints_ok &= reference.get_endpoint_clock_pair_timings(entry.first, rows) && guard::rows_nonregressing(entry.second, rows, true);
                    }
                }
                bool clocks = guard::clocks_nonregressing(before, after), hold = guard::holds_nonregressing(old_holds, guard::holds(after));
                bool fixed_graph = legal() && fixed();
                log_info("LUT pair copy trial inner=%s outer=%s sink=%s.ENA clone_bel=%s gain=%.0fps improve=%d endpoints=%d clocks=%d hold=%d fixed=%d cohort=%zu\n",
                         nameOf(inner), nameOf(outer), nameOf(sink), nameOfBel(site.bel), slack - old_slack,
                         int(improve), int(endpoints_ok), int(clocks), int(hold), int(fixed_graph), cohort.size());
                if (improve && endpoints_ok && clocks && hold && fixed_graph) {
                    log_info("LUT pair copy candidate %d: inner=%s outer=%s sink=%s.ENA clone_bel=%s gain=%.0fps cohort=%zu.\n",
                             qualified, nameOf(inner), nameOf(outer), nameOf(sink), nameOfBel(site.bel), slack - old_slack, cohort.size());
                    if (selection == qualified++) {
                        ctx->check();
                        log_info("LUT pair copy applied candidate %d; full routing and signoff still required.\n", selection);
                        live = false; return true;
                    }
                }
                restore_probe();
            }
            rollback();
        } catch (...) { rollback(); throw; }
    }
    log_info("LUT pair copy: %d qualified candidates; no candidate applied.\n", qualified);
    return false;
}
NEXTPNR_NAMESPACE_END
