/* Optional measured-wire calibration. SPDX-License-Identifier: ISC */
#include "critical_cohort_model.h"
#include <cmath>
#include <map>
#include <sstream>
#include "json11.hpp"
#include "log.h"
#include "routed_placement.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
using J = json11::Json;
J pair(DelayPair d) { return J::array{d.minDelay(), d.maxDelay()}; }
J quad(DelayQuad d) { return J::array{d.minRiseDelay(), d.maxRiseDelay(), d.minFallDelay(), d.maxFallDelay()}; }
std::vector<IdString> physical_pins(CellInfo *cell, IdString port)
{
    auto found = cell->pin_data.find(port);
    return found == cell->pin_data.end() ? std::vector<IdString>{} : found->second.bel_pins;
}

// Only routing's exact inserted buffer may disappear from the logical graph.
CellInfo *route_through_ff(Context *ctx, CellInfo *cell)
{
    if (!cell || cell->type != id_MISTRAL_BUF || cell->bel == BelId() || cell->ports.size() != 2 ||
        !cell->params.empty() || !cell->getPort(id_A) || cell->get_pin_state(id_A) != PIN_SIG ||
        cell->get_pin_state(id_Q) != PIN_SIG)
        return nullptr;
    auto *net = cell->getPort(id_Q);
    if (!net || net->users.entries() != 1)
        return nullptr;
    auto user = *net->users.begin();
    if (user.port != id_DATAIN || user.cell->type != id_MISTRAL_FF || user.cell->bel == BelId() ||
        cell->name.str(ctx) != user.cell->name.str(ctx) + "$ROUTETHRU")
        return nullptr;
    const auto &src = ctx->bel_data(cell->bel).lab_data;
    const auto &dst = ctx->bel_data(user.cell->bel).lab_data;
    if (src.lab != dst.lab || src.alm != dst.alm || src.idx != dst.idx / 2)
        return nullptr;
    return user.cell;
}
NetInfo *logical_net(Context *ctx, CellInfo *cell, IdString port)
{
    auto *net = cell->getPort(port);
    if (cell->type == id_MISTRAL_FF && port == id_DATAIN && net && route_through_ff(ctx, net->driver.cell) == cell)
        return net->driver.cell->getPort(id_A);
    return net;
}
J identity(Context *ctx)
{
    J::object cells, clocks, settings, lut_delays;
    for (const auto &entry : ctx->cells) {
        auto *cell = entry.second.get();
        if (route_through_ff(ctx, cell))
            continue;
        J::object params, ports, timing;
        for (const auto &p : cell->params)
            params[p.first.str(ctx)] = p.second.to_string();
        for (const auto &p : cell->ports) {
            auto *net = logical_net(ctx, cell, p.first);
            auto pins = cell->pin_data.find(p.first);
            ports[p.first.str(ctx)] = J::array{int(p.second.type), net ? net->name.str(ctx) : "",
                                               pins == cell->pin_data.end() ? int(PIN_SIG) : int(pins->second.state)};
            int count = 0;
            auto klass = ctx->getPortTimingClass(cell, p.first, count);
            J::array info{int(klass)};
            if (klass == TMG_REGISTER_INPUT || klass == TMG_REGISTER_OUTPUT)
                for (int i = 0; i < count; ++i) {
                    auto c = ctx->getPortClockingInfo(cell, p.first, i);
                    info.push_back(J::array{c.clock_port.str(ctx), int(c.edge), pair(c.setup), pair(c.hold),
                                            quad(c.clockToQ)});
                }
            timing[p.first.str(ctx)] = info;
        }
        // Combinational physical LUT tables are recorded independently of pin assignment.
        // Other primitives (including IO profiles) keep their connected arc tables.
        if (!ctx->is_comb_cell(cell->type) && cell->type != id_MISTRAL_BUF) {
            J::object delays;
            for (const auto &a : cell->ports)
                if (a.second.type == PORT_IN)
                    for (const auto &b : cell->ports) {
                        DelayQuad delay;
                        if (b.second.type == PORT_OUT && ctx->getCellDelay(cell, a.first, b.first, delay))
                            delays[a.first.str(ctx) + "/" + b.first.str(ctx)] = quad(delay);
                    }
            timing["arcs"] = delays;
        }
        cells[entry.first.str(ctx)] =
                J::object{{"type", cell->type.str(ctx)}, {"params", params}, {"ports", ports}, {"timing", timing}};
    }
    for (IdString pin : {id_A, id_B, id_C, id_D, id_E0, id_E1, id_F0, id_F1})
        for (bool l6 : {false, true}) {
            DelayQuad d;
            NPNR_ASSERT(ctx->get_lut_pin_delay(pin, l6, d));
            lut_delays[pin.str(ctx) + (l6 ? "6" : "5")] = quad(d);
        }
    for (const auto &entry : ctx->nets)
        if (entry.second->clkconstr) {
            const auto &c = *entry.second->clkconstr;
            clocks[entry.first.str(ctx)] =
                    J::array{pair(c.period), pair(c.high), pair(c.low), c.phase_group.str(ctx), c.phase_shift};
        }
    for (const auto &entry : ctx->settings) {
        const auto key = entry.first.str(ctx);
        if (key == "target_freq" || key.find("timing/") == 0 || key.find("sdc/") == 0 ||
            key == "mistral/routed_delay_ranges" || key == "mistral/clock_edge_ranges")
            settings[key] = entry.second.to_string();
    }
    return J::object{{"device", ctx->getChipName()},
                     {"ff4", ctx->lab_ff4},
                     {"clkb", ctx->args.lab_clkb},
                     {"cells", cells},
                     {"clocks", clocks},
                     {"settings", settings},
                     {"lut_delays", lut_delays}};
}

using ArcKey = std::pair<std::string, std::string>;
struct LogicalArc
{
    NetInfo *net;
    PortRef sink, physical_sink;
    bool rt;
};
std::map<ArcKey, LogicalArc> logical_arcs(Context *ctx)
{
    std::map<ArcKey, LogicalArc> result;
    for (const auto &entry : ctx->nets) {
        auto *net = entry.second.get();
        if (net->is_global || net->clkconstr || !net->driver.cell || net->driver.cell->bel == BelId() ||
            net->driver.cell->type.in(id_MISTRAL_CONST, id_GND, id_VCC) || route_through_ff(ctx, net->driver.cell))
            continue;
        for (auto user : net->users) {
            if (!user.cell || user.cell->isPseudo() || user.cell->bel == BelId())
                continue;
            int count = 0;
            if (ctx->getPortTimingClass(user.cell, user.port, count) == TMG_CLOCK_INPUT)
                continue;
            auto *ff = route_through_ff(ctx, user.cell);
            PortRef sink = ff ? PortRef{ff, id_DATAIN} : user;
            result.emplace(ArcKey{sink.cell->name.str(ctx), sink.port.str(ctx)},
                           LogicalArc{net, sink, user, ff != nullptr});
        }
    }
    return result;
}
struct Prediction
{
    delay_t wire, local = 0;
    DelayPair logic;
    bool rt = false, direct = false;
    WireId source;
    std::vector<WireId> sinks;
};
Prediction predict(Context *ctx, NetInfo *net, PortRef sink)
{
    Prediction p{ctx->predictArcDelay(net, sink)};
    if (ctx->routed_placement_preserve_routes) {
        p.source = ctx->getNetinfoSourceWire(net);
        for (auto wire : ctx->getNetinfoSinkWires(net, sink)) p.sinks.push_back(wire);
    }
    if (sink.cell->type != id_MISTRAL_FF || sink.port != id_DATAIN)
        return p;
    const auto &data = ctx->bel_data(sink.cell->bel).lab_data;
    const auto half = uint8_t(data.idx / 2);
    auto pins = ctx->getBelPinsForCellPin(net->driver.cell, net->driver.port);
    if (pins.empty())
        return p;
    if (pins.front() == id_COMBOUT && ctx->is_comb_cell(net->driver.cell->type)) {
        const auto &driver = ctx->bel_data(net->driver.cell->bel).lab_data;
        p.direct = data.lab == driver.lab && data.alm == driver.alm && half == driver.idx;
    }
    if (ctx->get_alm_route_through_ff(data.lab, data.alm, half) != sink.cell)
        return p;
    auto lut = ctx->labs.at(data.lab).alms.at(data.alm).lut_bels.at(half);
    DelayQuad d;
    const IdString rt_pin = ctx->alm_route_through_pin(data.lab, data.alm, half);
    NPNR_ASSERT(ctx->get_lut_pin_delay(rt_pin, false, d));
    // The model folds this temporary BUF into the FF input's route delay.
    // Preserve the same cell-side terminal GOUT mux charge as an inserted BUF.
    CellInfo route_through(ctx, ctx->id("$critical_cohort_route_through"), id_MISTRAL_BUF);
    d += ctx->get_lut_input_delay(&route_through, id_A);
    p.wire = ctx->predictDelay(net->driver.cell->bel, pins.front(), lut, rt_pin);
    p.local = ctx->predictDelay(lut, id_COMBOUT, sink.cell->bel, id_DATAIN);
    p.logic = d.delayPair();
    p.rt = true;
    if (ctx->routed_placement_preserve_routes)
        p.sinks = {ctx->getBelPinWire(lut, rt_pin)};
    return p;
}
std::optional<Arch::AnalogueArc> native_arc(Context *ctx, const CriticalCohortRouteCosts &costs, NetInfo *net,
    PortRef sink, const Prediction &prediction, const std::map<WireId, PipId> &tree)
{
    CellInfo terminal(ctx, ctx->id("$cohort_analogue_terminal"), prediction.rt ? id_MISTRAL_BUF : sink.cell->type);
    terminal.bel = sink.cell->bel;
    auto port = sink.port;
    std::vector<IdString> pins = ctx->getBelPinsForCellPin(sink.cell, sink.port);
    if (prediction.rt) {
        const auto &data = ctx->bel_data(sink.cell->bel).lab_data;
        terminal.bel = ctx->labs.at(data.lab).alms.at(data.alm).lut_bels.at(data.idx / 2);
        pins = {ctx->alm_route_through_pin(data.lab, data.alm, data.idx / 2)};
        port = id_A;
    }
    // The native arc override currently observes one physical input. Do not
    // silently invent a multi-pin electrical timing model here.
    if (pins.size() != 1) return {};
    terminal.addInput(port);
    terminal.pin_data[port].bel_pins = pins;
    return costs.analogue(ctx, net, {&terminal, port}, tree);
}
const PortRef &native_sink(NetInfo *net, const PortRef &sink)
{
    // The analogue cache is keyed by the native net user's address. Logical
    // arc records hold copies; recover that address only after checking the
    // complete cell/port/net relationship and its live user index.
    auto port = sink.cell->ports.find(sink.port);
    if (port != sink.cell->ports.end() && port->second.net == net && net->users.count(port->second.user_idx)) {
        const auto &user = net->users.at(port->second.user_idx);
        if (user.cell == sink.cell && user.port == sink.port)
            return user;
    }
    return sink;
}
DelayPair measured(Context *ctx, NetInfo *net, const PortRef &sink)
{
    const auto &user = native_sink(net, sink);
    // Use the native user address and preserve the same data-route intervals
    // as STA, including explicit range mode without IO delays.
    const bool ranges = ctx->settings.count(ctx->id("timing/io_delays")) || ctx->useRoutedDelayRanges();
    return ranges ? ctx->getNetinfoRouteDelayQuad(net, user).delayPair()
                  : DelayPair(ctx->getNetinfoRouteDelay(net, user));
}
DelayPair read_pair(const J &value)
{
    const auto &a = value.array_items();
    if (a.size() != 2 || !a[0].is_number() || !a[1].is_number())
        log_error("Critical cohort route model has invalid wire delays.\n");
    for (const auto &x : a)
        if (!std::isfinite(x.number_value()) || x.number_value() != std::floor(x.number_value()) ||
            x.number_value() < 0 || x.number_value() > 10000000)
            log_error("Critical cohort route model has invalid wire delays.\n");
    if (a[0].number_value() > a[1].number_value())
        log_error("Critical cohort route model has reversed wire delays.\n");
    return DelayPair(delay_t(a[0].number_value()), delay_t(a[1].number_value()));
}
DelayPair nonnegative(DelayPair p)
{
    return DelayPair(std::max(delay_t(0), p.minDelay()), std::max(delay_t(0), p.maxDelay()));
}
} // namespace

NetInfo *critical_cohort_logical_net(Context *ctx, CellInfo *cell, IdString port) { return logical_net(ctx, cell, port); }
CellInfo *critical_cohort_route_through_ff(Context *ctx, CellInfo *cell) { return route_through_ff(ctx, cell); }

void write_critical_cohort_route_model(Context *ctx, std::ostream &out)
{
    if (!ctx->timing_result_is_final_analogue)
        log_error("Critical cohort route model export requires final analogue timing.\n");
    std::ostringstream report;
    ctx->writeJsonReport(report);
    std::string error;
    auto document = J::parse(report.str(), error).object_items();
    NPNR_ASSERT(error.empty());
    J::array arcs;
    for (const auto &entry : logical_arcs(ctx)) {
        const auto &a = entry.second;
        DelayPair local;
        if (a.rt)
            local = measured(ctx, a.sink.cell->getPort(id_DATAIN), a.sink);
        J::object row{{"sink", entry.first.first},
                                 {"port", entry.first.second},
                                 {"driver", a.net->driver.cell->name.str(ctx)},
                                 {"driver_port", a.net->driver.port.str(ctx)},
                                 {"net", a.net->name.str(ctx)},
                                 {"source_bel", ctx->getBelName(a.net->driver.cell->bel).str(ctx)},
                                 {"sink_bel", ctx->getBelName(a.sink.cell->bel).str(ctx)},
                                 {"wire", pair(measured(ctx, a.net, a.physical_sink))},
                                 {"route_through", a.rt},
                                 {"local", pair(local)}};
        if (ctx->routed_placement_preserve_routes || ctx->routed_placement_record_calibration) {
            J::array sinks;
            for (auto wire : ctx->getNetinfoSinkWires(a.net, a.physical_sink))
                sinks.push_back(ctx->getWireName(wire).str(ctx));
            row["source_wire"] = ctx->getWireName(ctx->getNetinfoSourceWire(a.net)).str(ctx);
            row["sink_wires"] = sinks;
            DelayQuad full, input;
            bool have_electrical = false;
            const auto &user = native_sink(a.net, a.physical_sink);
            if (ctx->bitstream_configured && !a.net->wires.empty()) {
                const auto cached = ctx->analogue_arc_cache.find(&user);
                if (ctx->analogue_cache_valid && cached != ctx->analogue_arc_cache.end() && cached->second.ok) {
                    full = cached->second.delay;
                    input = cached->second.lut_input_delay;
                    have_electrical = true;
                } else
                    have_electrical = ctx->analogue_arc_delay(a.net, user, full, nullptr, &input);
            }
            if (have_electrical)
                row["electrical"] = J::object{{"full_ps", pair(full.delayPair())},
                    {"input_ps", pair(input.delayPair())}, {"wire_ps", pair((full - input).delayPair())}};
        }
        arcs.push_back(row);
    }
    J::object model{{"version", 1}, {"identity", identity(ctx)}, {"arcs", arcs}};
    if (ctx->routed_placement_preserve_routes || ctx->routed_placement_record_calibration) {
        std::string error;
        // A capture-only record describes the final placement, including
        // accepted late refinements. Preserve-route proposals retain their
        // original parent contract and behavior.
        auto parent = ctx->routed_placement_preserve_routes ? J::parse(ctx->routed_placement_current, error) :
                                                           routed_placement_snapshot(ctx);
        NPNR_ASSERT(error.empty() && parent.is_object());
        model["route_costs"] = CriticalCohortRouteCosts::snapshot(ctx, parent);
    }
    document["cohort_route_model"] = model;
    out << J(document).dump() << '\n';
    if (!out)
        log_error("Failed to write critical cohort route model.\n");
}

CriticalCohortRouteModel::CriticalCohortRouteModel(Context *ctx, const std::string &report)
{
    if (report.empty())
        return;
    std::string error;
    auto document = J::parse(report, error);
    if (!error.empty())
        log_error("Critical cohort route model report is invalid JSON.\n");
    auto model = document["cohort_route_model"];
    if (model.is_null())
        return;
    if (!document["timing_summary"]["final_analogue_model"].bool_value() || model["version"] != J(1) ||
        model["identity"] != identity(ctx) || !model["arcs"].is_array())
        log_error("Critical cohort route model does not match this design and timing model.\n");
    auto pending = logical_arcs(ctx);
    if (ctx->routed_placement_preserve_routes) {
        auto parent = J::parse(ctx->routed_placement_current, error);
        if (!error.empty() || !parent.is_object())
            log_error("Routing-cost proposals require the matching parent placement.\n");
        route_costs = std::make_unique<CriticalCohortRouteCosts>(ctx, model["route_costs"], parent);
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            placement[cell] = cell->bel;
            for (const auto &port : cell->ports)
                pins[{cell->name, port.first}] = physical_pins(cell, port.first);
        }
    }
    for (const auto &row : model["arcs"].array_items()) {
        auto found = pending.find({row["sink"].string_value(), row["port"].string_value()});
        if (found == pending.end())
            log_error("Critical cohort route model has missing, duplicate or unknown arcs.\n");
        auto a = found->second;
        if (row["driver"] != J(a.net->driver.cell->name.str(ctx)) ||
            row["driver_port"] != J(a.net->driver.port.str(ctx)) || row["net"] != J(a.net->name.str(ctx)) ||
            row["source_bel"] != J(ctx->getBelName(a.net->driver.cell->bel).str(ctx)) ||
            row["sink_bel"] != J(ctx->getBelName(a.sink.cell->bel).str(ctx)) || !row["route_through"].is_bool())
            log_error("Critical cohort route model requires the matching original placement and data arcs.\n");
        auto p = predict(ctx, a.net, a.sink);
        if (p.rt != row["route_through"].bool_value())
            log_error("Critical cohort route model register route-through does not match this placement.\n");
        auto wire = read_pair(row["wire"]), local = read_pair(row["local"]);
        std::optional<delay_t> reference;
        if (route_costs) {
            J::array sinks;
            for (auto target : p.sinks) sinks.push_back(ctx->getWireName(target).str(ctx));
            if (row["source_wire"] != J(ctx->getWireName(p.source).str(ctx)) || row["sink_wires"] != J(sinks))
                log_error("Routing-cost model physical pins do not match the parent placement.\n");
            reference = route_costs->reference(a.net->name, p.source, p.sinks);
        }
        if (!p.rt && (local.minDelay() != 0 || local.maxDelay() != 0))
            log_error("Critical cohort route model has an unexpected local wire.\n");
        arcs.emplace(CellPortKey(a.sink),
                     Arc{a.net, a.sink, wire - DelayPair(p.wire), local - DelayPair(p.local), p.rt, p.direct,
                         wire, reference, p.source, p.sinks});
        pending.erase(found);
    }
    if (!pending.empty())
        log_error("Critical cohort route model does not cover every data arc.\n");
    if (route_costs) {
        int matches = 0;
        route_costs->with_analogue(ctx, {}, {}, [&]() {
            for (auto &entry : arcs) {
                auto &arc = entry.second;
                if (!arc.reference_cost) continue;
                auto p = predict(ctx, arc.net, arc.sink);
                auto native = native_arc(ctx, *route_costs, arc.net, arc.sink, p, route_costs->parent_tree(arc.net->name));
                if (!native) continue;
                auto wire = (native->delay - native->lut_input_delay).delayPair();
                arc.native_error = wire.maxDelay() - arc.measured_wire.maxDelay();
                if (arc.native_error == 0 &&
                    (!(ctx->settings.count(ctx->id("timing/io_delays")) || ctx->useRoutedDelayRanges()) ||
                     wire.minDelay() == arc.measured_wire.minDelay())) {
                    arc.native_full = native->delay.delayPair();
                    ++matches;
                }
            }
        });
        log_info("Critical cohort: native electrical parent matches %d/%zu data arcs.\n", matches, arcs.size());
    }
    log_info("Critical cohort: calibrated %zu data arcs from a matching baseline route.\n", arcs.size());
}

std::vector<CriticalCohortBranchRepair> CriticalCohortRouteModel::branch_repairs(Context *ctx, PortRef sink) const
{
    std::vector<CriticalCohortBranchRepair> result;
    auto found = arcs.find(CellPortKey(sink));
    if (!route_costs || found == arcs.end()) return result;
    auto *net = found->second.net;
    std::set<WireId> sinks;
    for (const auto &entry : arcs)
        if (entry.second.net == net) sinks.insert(entry.second.sinks.begin(), entry.second.sinks.end());
    std::map<WireId, std::vector<PipId>> choices;
    for (auto target : found->second.sinks)
        choices[target] = route_costs->branch_alternatives(ctx, net->name, target, sinks);
    for (size_t i = 0; i < 3; ++i)
        for (const auto &entry : choices)
            if (i < entry.second.size() && result.size() < 3)
                result.push_back({net->name, entry.first, entry.second[i]});
    return result;
}

bool CriticalCohortRouteModel::apply(Context *ctx, TimingAnalyser &timing, const CriticalCohortBranchRepair *branch) const
{
    if (branch && !route_costs) log_error("Branch proposals require a complete native route model.\n");
    last_cost_nodes = 0;
    J::object net_trace;
    J::array arc_trace;
    last_routing_trace = J::object{};
    if (!active())
        return true;
    bool complete = true, fixed_registers = true;
    std::set<IdString> released;
    if (branch) released.insert(branch->net);
    std::map<WireId, IdString> drivers;
    std::map<IdString, std::set<WireId>> targets;
    std::map<CellPortKey, Prediction> physical;
    std::map<NetInfo *, std::map<WireId, delay_t>> estimated;
    std::map<IdString, std::map<WireId, PipId>> trees;
    std::map<CellPortKey, Arch::AnalogueArc> native_delays;
    if (route_costs) {
        for (const auto &entry : placement) {
            auto *cell = entry.first;
            if (cell->type == id_MISTRAL_FF && cell->bel != entry.second)
                complete = fixed_registers = false;
            bool changed = cell->bel != entry.second;
            for (const auto &port : cell->ports)
                changed |= pins.at({cell->name, port.first}) != physical_pins(cell, port.first);
            if (branch && changed) complete = false;
            if (changed)
                for (const auto &port : cell->ports)
                    if (port.second.net) released.insert(port.second.net->name);
        }
        for (const auto &entry : arcs) {
            const auto &arc = entry.second;
            auto p = predict(ctx, arc.net, arc.sink);
            if (p.source != arc.source || p.sinks != arc.sinks)
                released.insert(arc.net->name);
            if (p.rt != arc.was_route_through) {
                // Inserting/removing a route-through changes the FF's actual
                // DATAIN net, part of every physical contract for that FF.
                // A moved driver alone does not change any consumer cell.
                for (const auto &port : arc.sink.cell->ports)
                    if (port.second.net) released.insert(port.second.net->name);
            }
            drivers[p.source] = arc.net->name;
            physical.emplace(entry.first, std::move(p));
        }
        for (const auto &entry : arcs) {
            const auto &arc = entry.second;
            const auto &p = physical.at(entry.first);
            if (released.count(arc.net->name))
                targets[arc.net->name].insert(p.sinks.begin(), p.sinks.end());
        }
        int budget = CriticalCohortRouteCosts::node_limit;
        for (const auto &entry : targets) {
            auto *net = ctx->nets.at(entry.first).get();
            auto source = ctx->getNetinfoSourceWire(net);
            J::object paths;
            auto &tree = trees[net->name];
            if (branch && branch->net == net->name)
                estimated[net] = route_costs->repair_branch(ctx, net, source, entry.second, branch->target,
                    branch->avoid, released, drivers, budget, &paths, &tree);
            else
                estimated[net] = route_costs->estimate(ctx, net, source, entry.second, released, drivers, budget, &paths, &tree);
            for (const auto &wire : tree) {
                auto owner = drivers.emplace(wire.first, net->name);
                complete &= owner.second || owner.first->second == net->name;
            }
            J::array sinks;
            for (auto sink : entry.second) sinks.push_back(ctx->getWireName(sink).str(ctx));
            J::array bindings;
            for (const auto &wire : tree) bindings.push_back(J::array{ctx->getWireName(wire.first).str(ctx),
                wire.second == PipId() ? "" : ctx->getPipName(wire.second).str(ctx)});
            net_trace[net->name.str(ctx)] = J::object{{"source", ctx->getWireName(source).str(ctx)},
                {"sinks", sinks}, {"paths", paths}, {"tree", bindings},
                {"complete", estimated[net].size() == entry.second.size()}};
            complete &= estimated[net].size() == entry.second.size();
        }
        last_cost_nodes = CriticalCohortRouteCosts::node_limit - budget;
        if (complete && !trees.empty())
            route_costs->with_analogue(ctx, released, trees, [&]() {
                for (const auto &entry : arcs) {
                    const auto &arc = entry.second;
                    const auto &p = physical.at(entry.first);
                    if (!released.count(arc.net->name) || (p.direct && !arc.was_direct)) continue;
                    auto tree = trees.find(arc.net->name);
                    if (tree == trees.end() || !arc.native_full) { complete = false; continue; }
                    auto native = native_arc(ctx, *route_costs, arc.net, arc.sink, p, tree->second);
                    if (!native) { complete = false; continue; }
                    native_delays.emplace(entry.first, *native);
                }
            });
    }
    for (const auto &entry : arcs) {
        const auto &arc = entry.second;
        auto p = route_costs ? physical.at(entry.first) : predict(ctx, arc.net, arc.sink);
        // A newly dedicated COMBOUT->matching FF path has no programmable wire;
        // its previous general-route detour does not follow it into the ALM.
        auto delay = p.direct && !arc.was_direct ? DelayPair() : nonnegative(arc.offset + DelayPair(p.wire));
        if (route_costs && !(p.direct && !arc.was_direct)) {
            delay = arc.measured_wire;
            if (released.count(arc.net->name)) {
                auto &costs = estimated[arc.net];
                bool found = arc.reference_cost.has_value() && !p.sinks.empty();
                delay_t maximum = 0;
                for (auto sink : p.sinks) {
                    auto value = costs.find(sink);
                    if (value == costs.end()) found = false;
                    else maximum = std::max(maximum, value->second);
                }
                complete &= found;
                auto native = native_delays.find(entry.first);
                if (found && native != native_delays.end() && arc.native_full) {
                    auto delta = native->second.delay.delayPair() - *arc.native_full;
                    if (!ctx->settings.count(ctx->id("timing/io_delays")) && !ctx->useRoutedDelayRanges())
                        delta = DelayPair(delta.maxDelay());
                    delay = nonnegative(arc.measured_wire + delta);
                    if (delay.minDelay() > delay.maxDelay()) {
                        complete = false;
                        delay = arc.measured_wire;
                    }
                } else complete = false;
            }
        }
        const auto predicted_wire = delay;
        if (p.rt)
            delay += p.logic +
                     nonnegative(DelayPair(p.local) + (arc.was_route_through ? arc.local_offset : DelayPair()));
        if (route_costs && released.count(arc.net->name)) {
            J::array sinks;
            for (auto sink : p.sinks) sinks.push_back(ctx->getWireName(sink).str(ctx));
            arc_trace.push_back(J::object{{"sink", arc.sink.cell->name.str(ctx)}, {"port", arc.sink.port.str(ctx)},
                {"net", arc.net->name.str(ctx)}, {"source", ctx->getWireName(p.source).str(ctx)}, {"sinks", sinks},
                {"parent_wire_ps", pair(arc.measured_wire)}, {"predicted_route_ps", pair(delay)},
                {"predicted_wire_ps", pair(predicted_wire)},
                {"native_parent_ps", arc.native_full ? pair(*arc.native_full) : J()},
                {"native_parent_error_ps", arc.native_error},
                {"native_trial_ps", native_delays.count(entry.first) ? pair(native_delays.at(entry.first).delay.delayPair()) : J()},
                {"reference_cost_ps", arc.reference_cost ? J(*arc.reference_cost) : J()},
                {"direct", p.direct}, {"route_through", p.rt}});
        }
        timing.set_route_delay(CellPortKey(arc.sink), delay);
    }
    timing.run(false, false, false, true);
    last_routing_trace = J::object{{"nets", net_trace}, {"arcs", arc_trace}, {"joint_trees", true},
        {"native_analogue", true}, {"native_reservations", route_costs != nullptr}, {"fixed_registers", fixed_registers},
        {"route_only", branch != nullptr},
        {"branch_repair", branch ? J(J::object{{"net", branch->net.str(ctx)},
            {"target", ctx->getWireName(branch->target).str(ctx)},
            {"avoided_pip", ctx->getPipName(branch->avoid).str(ctx)}}) : J()}};
    return complete;
}
delay_t CriticalCohortRouteModel::route_delay(Context *ctx, PortRef sink) const
{
    auto found = arcs.find(CellPortKey(sink));
    if (found == arcs.end())
        return ctx->predictArcDelay(sink.cell->getPort(sink.port), sink);
    const auto &arc = found->second;
    auto p = predict(ctx, arc.net, sink);
    // A newly dedicated COMBOUT->matching FF path has no programmable wire;
    // its previous general-route detour does not follow it into the ALM.
    auto delay = p.direct && !arc.was_direct ? DelayPair() : nonnegative(arc.offset + DelayPair(p.wire));
    if (p.rt)
        delay += p.logic + nonnegative(DelayPair(p.local) + (arc.was_route_through ? arc.local_offset : DelayPair()));
    return delay.maxDelay();
}
NEXTPNR_NAMESPACE_END
