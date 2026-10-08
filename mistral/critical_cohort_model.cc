/* Optional measured-wire calibration. SPDX-License-Identifier: ISC */
#include "critical_cohort_model.h"
#include <cmath>
#include <map>
#include <sstream>
#include "json11.hpp"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
using J = json11::Json;
J pair(DelayPair d) { return J::array{d.minDelay(), d.maxDelay()}; }
J quad(DelayQuad d) { return J::array{d.minRiseDelay(), d.maxRiseDelay(), d.minFallDelay(), d.maxFallDelay()}; }

// Only routing's exact inserted buffer may disappear from the logical graph.
CellInfo *route_through_ff(Context *ctx, CellInfo *cell)
{
    if (!cell || cell->type != id_MISTRAL_BUF || cell->bel == BelId())
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
        if (key == "target_freq" || key.find("timing/") == 0 || key.find("sdc/") == 0)
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
};
Prediction predict(Context *ctx, NetInfo *net, PortRef sink)
{
    Prediction p{ctx->predictArcDelay(net, sink)};
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
    NPNR_ASSERT(ctx->get_lut_pin_delay(half ? id_D : id_C, false, d));
    p.wire = ctx->predictDelay(net->driver.cell->bel, pins.front(), lut, half ? id_D : id_C);
    p.local = ctx->predictDelay(lut, id_COMBOUT, sink.cell->bel, id_DATAIN);
    p.logic = d.delayPair();
    p.rt = true;
    return p;
}
DelayPair measured(Context *ctx, NetInfo *net, PortRef sink)
{
    return ctx->settings.count(ctx->id("timing/io_delays")) ? ctx->getNetinfoRouteDelayQuad(net, sink).delayPair()
                                                            : DelayPair(ctx->getNetinfoRouteDelay(net, sink));
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
        arcs.push_back(J::object{{"sink", entry.first.first},
                                 {"port", entry.first.second},
                                 {"driver", a.net->driver.cell->name.str(ctx)},
                                 {"driver_port", a.net->driver.port.str(ctx)},
                                 {"net", a.net->name.str(ctx)},
                                 {"source_bel", ctx->getBelName(a.net->driver.cell->bel).str(ctx)},
                                 {"sink_bel", ctx->getBelName(a.sink.cell->bel).str(ctx)},
                                 {"wire", pair(measured(ctx, a.net, a.physical_sink))},
                                 {"route_through", a.rt},
                                 {"local", pair(local)}});
    }
    document["cohort_route_model"] = J::object{{"version", 1}, {"identity", identity(ctx)}, {"arcs", arcs}};
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
        if (!p.rt && (local.minDelay() != 0 || local.maxDelay() != 0))
            log_error("Critical cohort route model has an unexpected local wire.\n");
        arcs.emplace(CellPortKey(a.sink),
                     Arc{a.net, a.sink, wire - DelayPair(p.wire), local - DelayPair(p.local), p.rt, p.direct});
        pending.erase(found);
    }
    if (!pending.empty())
        log_error("Critical cohort route model does not cover every data arc.\n");
    log_info("Critical cohort: calibrated %zu data arcs from a matching baseline route.\n", arcs.size());
}

void CriticalCohortRouteModel::apply(Context *ctx, TimingAnalyser &timing) const
{
    if (!active())
        return;
    for (const auto &entry : arcs) {
        const auto &arc = entry.second;
        auto p = predict(ctx, arc.net, arc.sink);
        // A newly dedicated COMBOUT->matching FF path has no programmable wire;
        // its previous general-route detour does not follow it into the ALM.
        auto delay = p.direct && !arc.was_direct ? DelayPair() : nonnegative(arc.offset + DelayPair(p.wire));
        if (p.rt)
            delay += p.logic +
                     nonnegative(DelayPair(p.local) + (arc.was_route_through ? arc.local_offset : DelayPair()));
        timing.set_route_delay(CellPortKey(arc.sink), delay);
    }
    timing.run(false, false, false, true);
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
