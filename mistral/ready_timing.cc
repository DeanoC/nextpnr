/* Read-only final-signoff HPS-ready timing diagnostic. SPDX-License-Identifier: ISC */
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Quad = std::tuple<delay_t, delay_t, delay_t, delay_t>;
Quad quad(const DelayQuad &d) { return {d.minRiseDelay(), d.maxRiseDelay(), d.minFallDelay(), d.maxFallDelay()}; }
void qheader(std::ostream &out, const char *prefix)
{
    for (auto suffix : {"rise_min", "rise_max", "fall_min", "fall_max"})
        out << '\t' << prefix << '_' << suffix;
}
void qwrite(std::ostream &out, const DelayQuad &d)
{
    out << '\t' << d.minRiseDelay() << '\t' << d.maxRiseDelay() << '\t' << d.minFallDelay() << '\t' << d.maxFallDelay();
}
} // namespace
void Arch::dump_ready_timing(const char *prefix) const
{
    if (!prefix || !*prefix)
        return;
    NPNR_ASSERT(bitstream_configured);
    auto ctx = getCtx();
    const bool saved_calibrated = pip_delay_calibrated, saved_cache_valid = analogue_cache_valid,
               saved_configured = bitstream_configured;
    const float saved_prior = pip_delay_prior;
    auto observed_before = pip_delay_observed;
    auto types_before = pip_type_calibration;
    std::map<const PortRef *, std::pair<bool, Quad>> cache_before;
    for (auto &e : analogue_arc_cache)
        cache_before[e.first] = {e.second.ok, quad(e.second.delay)};
    std::map<IdString, std::pair<BelId, PlaceStrength>> placements;
    std::map<IdString, std::map<WireId, std::pair<PipId, PlaceStrength>>> routes;
    for (auto &e : cells)
        placements[e.first] = {e.second->bel, e.second->belStrength};
    for (auto &e : nets) {
        auto &r = routes[e.first];
        for (auto &w : e.second->wires)
            r[w.first] = {w.second.pip, w.second.strength};
    }
    auto checksum_before = ctx->checksum();
    auto name = [&](WireId w) { return std::string(nameOfWire(w)); };
    auto type = [&](WireId w) { return w.is_nextpnr_created() ? -1 : int(w.node.t()); };
    std::ofstream arcs(std::string(prefix) + ".arcs.tsv"), hops_file(std::string(prefix) + ".hops.tsv"),
            observed(std::string(prefix) + ".observed.tsv"), types(std::string(prefix) + ".types.tsv");
    arcs << "arc_id\tnet\tsource_cell\tsource_port\tsink_cell\tsink_port\tuser_index\tsource_wire\tsink_wire\troute_"
            "complete\tcache_present\tcache_ok\trecomputed_ok\tfailure_reason\tfailure_hop\toverride_used\tcontext_"
            "scalar\tfallback_scalar";
    for (auto label : {"context", "fallback", "source_wire_delay", "cached", "recomputed", "override"})
        qheader(arcs, label);
    arcs << '\n';
    hops_file << "arc_id\thop\tpip_src\tpip_dst\ttype_id\tsrc_generated\tobserved_present\tobserved_"
                 "ps\tprovenance\tanalogue_mode\tinput_rise_samples\tinput_fall_samples\tstatus\tcompleted\trise_"
                 "ps\tfall_ps";
    for (auto label : {"table", "effective", "destination_wire_delay"})
        qheader(hops_file, label);
    hops_file << '\n';
    observed << "pip_src\tpip_dst\ttype_id\tsrc_generated\tobserved_ps";
    qheader(observed, "table");
    observed << '\n';
    types << "type_id\ttable_ps\tanalogue_ps\thops\n" << std::setprecision(std::numeric_limits<double>::max_digits10);
    std::map<PipId, delay_t> sorted_observed;
    for (auto &e : pip_delay_observed)
        sorted_observed[e.first] = e.second;
    for (auto &e : sorted_observed) {
        auto src = getPipSrcWire(e.first), dst = getPipDstWire(e.first);
        observed << name(src) << '\t' << name(dst) << '\t' << type(src) << '\t' << src.is_nextpnr_created() << '\t'
                 << e.second;
        qwrite(observed, getPipDelayTable(e.first));
        observed << '\n';
    }
    for (size_t i = 0; i < pip_type_calibration.size(); ++i) {
        auto &c = pip_type_calibration[i];
        types << i << '\t' << c.table_ps << '\t' << c.analogue_ps << '\t' << c.hops << '\n';
    }
    std::map<std::pair<std::string, std::string>, NetInfo *> selected;
    for (auto &e : nets) {
        auto n = e.second.get();
        if (!n->driver.cell || n->driver.cell->type != id_cyclonev_hps_interface_fpga2sdram)
            continue;
        auto port = n->driver.port.str(ctx);
        if (port.compare(0, 10, "cmd_ready_") != 0 || n->users.empty())
            continue;
        selected[{n->driver.cell->name.str(ctx), port}] = n;
    }
    NPNR_ASSERT(!selected.empty());
    int arc_count = 0, hop_count = 0, failed_count = 0, complete_count = 0;
    for (auto &e : selected) {
        auto net = e.second;
        std::map<std::pair<std::string, std::string>, std::pair<const PortRef *, int>> users;
        for (auto item : net->users.enumerate())
            users[{item.value.cell->name.str(ctx), item.value.port.str(ctx)}] = {&item.value, item.index.idx()};
        for (auto &u : users) {
            const auto &sink = *u.second.first;
            auto src = ctx->getNetinfoSourceWire(net);
            auto sink_wires = ctx->getNetinfoSinkWires(net, sink);
            // The current FPGA2SDRAM-ready fixtures have one physical wire per logical sink.
            if (sink_wires.size() != 1)
                log_error("Ready timing trace requires one physical wire for %s.%s.\n", sink.cell->name.c_str(ctx),
                          sink.port.c_str(ctx));
            auto dst = sink_wires[0];
            std::vector<PipId> path;
            std::set<WireId> seen;
            auto cursor = dst;
            while (cursor != WireId() && cursor != src) {
                NPNR_ASSERT(seen.insert(cursor).second);
                auto it = net->wires.find(cursor);
                if (it == net->wires.end() || it->second.pip == PipId())
                    break;
                path.push_back(it->second.pip);
                cursor = getPipSrcWire(it->second.pip);
            }
            bool complete = cursor == src;
            complete_count += complete;
            std::reverse(path.begin(), path.end());
            DelayQuad fallback(0);
            delay_t fallback_scalar = 0;
            for (auto p : path) {
                fallback += getPipDelay(p);
                fallback += getWireDelay(getPipDstWire(p));
                fallback_scalar += getPipDelay(p).maxDelay() + getWireDelay(getPipDstWire(p)).maxDelay();
            }
            auto source_delay = getWireDelay(src);
            if (complete) {
                fallback += source_delay;
                fallback_scalar += source_delay.maxDelay();
            } else {
                fallback = DelayQuad(ctx->predictArcDelay(net, sink));
                fallback_scalar = fallback.maxDelay();
            }
            AnalogueTrace trace;
            std::vector<AnalogueHop> analogue_hops;
            DelayQuad recomputed;
            bool ok = analogue_arc_delay(net, sink, recomputed, &analogue_hops, &trace);
            failed_count += !ok;
            NPNR_ASSERT(trace.route_complete == complete && trace.hops.size() == path.size());
            auto cached = analogue_arc_cache.find(&sink);
            bool cache_present = cached != analogue_arc_cache.end(), cache_ok = cache_present && cached->second.ok;
            DelayQuad cached_delay;
            if (cache_present)
                cached_delay = cached->second.delay;
            DelayQuad override_delay;
            bool override_used = getArcDelayOverride(net, sink, override_delay);
            auto actual = ctx->getNetinfoRouteDelayQuad(net, sink);
            auto scalar = ctx->getNetinfoRouteDelay(net, sink);
            if (override_used) {
                NPNR_ASSERT(quad(actual) == quad(override_delay) && scalar == override_delay.maxDelay());
            } else {
                NPNR_ASSERT(quad(actual) == quad(fallback) && scalar == fallback_scalar);
            }
            arcs << arc_count << '\t' << net->name.str(ctx) << '\t' << net->driver.cell->name.str(ctx) << '\t'
                 << net->driver.port.str(ctx) << '\t' << sink.cell->name.str(ctx) << '\t' << sink.port.str(ctx) << '\t'
                 << u.second.second << '\t' << name(src) << '\t' << name(dst) << '\t' << complete << '\t'
                 << cache_present << '\t' << cache_ok << '\t' << ok << '\t' << trace.reason << '\t' << trace.failure_hop
                 << '\t' << override_used << '\t' << scalar << '\t' << fallback_scalar;
            for (auto d : {actual, fallback, source_delay, cached_delay, recomputed, override_delay})
                qwrite(arcs, d);
            arcs << '\n';
            std::map<PipId, AnalogueHop> by_pip;
            for (auto &h : analogue_hops)
                by_pip[h.pip] = h;
            for (size_t i = 0; i < path.size(); ++i) {
                auto p = path[i];
                auto s = getPipSrcWire(p), d = getPipDstWire(p);
                auto &t = trace.hops.at(i);
                NPNR_ASSERT(t.pip == p);
                auto obs = pip_delay_observed.find(p);
                auto ah = by_pip.find(p);
                hops_file << arc_count << '\t' << i << '\t' << name(s) << '\t' << name(d) << '\t' << type(s) << '\t'
                          << s.is_nextpnr_created() << '\t' << (obs != pip_delay_observed.end()) << '\t'
                          << (obs == pip_delay_observed.end() ? 0 : obs->second) << '\t' << pip_delay_provenance(p)
                          << '\t' << t.mode << '\t' << t.input_rise_samples << '\t' << t.input_fall_samples << '\t'
                          << t.status << '\t' << t.completed << '\t' << (ah == by_pip.end() ? 0 : ah->second.rise)
                          << '\t' << (ah == by_pip.end() ? 0 : ah->second.fall);
                for (auto q : {getPipDelayTable(p), getPipDelay(p), getWireDelay(d)})
                    qwrite(hops_file, q);
                hops_file << '\n';
                ++hop_count;
            }
            ++arc_count;
        }
    }
    arcs.close();
    hops_file.close();
    observed.close();
    types.close();
    if (!arcs || !hops_file || !observed || !types)
        log_error("Cannot write ready timing trace.\n");
    NPNR_ASSERT(pip_delay_calibrated == saved_calibrated && analogue_cache_valid == saved_cache_valid &&
                bitstream_configured == saved_configured && pip_delay_prior == saved_prior);
    NPNR_ASSERT(pip_delay_observed.size() == observed_before.size());
    for (auto &e : observed_before)
        NPNR_ASSERT(pip_delay_observed.at(e.first) == e.second);
    for (size_t i = 0; i < types_before.size(); ++i) {
        auto &a = types_before[i];
        auto &b = pip_type_calibration[i];
        NPNR_ASSERT(a.table_ps == b.table_ps && a.analogue_ps == b.analogue_ps && a.hops == b.hops);
    }
    NPNR_ASSERT(analogue_arc_cache.size() == cache_before.size());
    for (auto &e : cache_before) {
        auto &c = analogue_arc_cache.at(e.first);
        NPNR_ASSERT(c.ok == e.second.first && quad(c.delay) == e.second.second);
    }
    NPNR_ASSERT(cells.size() == placements.size() && nets.size() == routes.size());
    for (auto &e : placements) {
        auto c = cells.at(e.first).get();
        NPNR_ASSERT(c->bel == e.second.first && c->belStrength == e.second.second);
    }
    for (auto &e : routes) {
        auto n = nets.at(e.first).get();
        NPNR_ASSERT(n->wires.size() == e.second.size());
        for (auto &w : e.second) {
            auto &a = n->wires.at(w.first);
            NPNR_ASSERT(a.pip == w.second.first && a.strength == w.second.second);
        }
    }
    NPNR_ASSERT(ctx->checksum() == checksum_before);
    uint32_t prior_bits;
    static_assert(sizeof(prior_bits) == sizeof(pip_delay_prior), "binary32 prior expected");
    std::memcpy(&prior_bits, &pip_delay_prior, sizeof(prior_bits));
    std::ostringstream prior_hex;
    prior_hex << std::hexfloat << pip_delay_prior;
    std::ofstream state(std::string(prefix) + ".state.json");
    state << std::setprecision(std::numeric_limits<double>::max_digits10) << "{\"prior\":" << double(pip_delay_prior)
          << ",\"prior_bits\":" << prior_bits << ",\"prior_hex\":" << std::quoted(prior_hex.str())
          << ",\"pip_delay_calibrated\":" << (pip_delay_calibrated ? "true" : "false")
          << ",\"analogue_cache_valid\":" << (analogue_cache_valid ? "true" : "false")
          << ",\"bitstream_configured\":true,\"ready_nets\":" << selected.size() << ",\"arcs\":" << arc_count
          << ",\"hops\":" << hop_count << ",\"failed_recomputations\":" << failed_count
          << ",\"complete_paths\":" << complete_count << ",\"observed_pips\":" << pip_delay_observed.size()
          << ",\"calibration_types\":256,\"calibration_unchanged\":true,\"cache_unchanged\":true,\"routes_unchanged\":"
             "true,\"placements_unchanged\":true,\"context_checksum_unchanged\":true,\"wire_delays_zero_contract\":"
             "true,\"failed_jobs_may_contribute_partial_hops\":true}\n";
    state.close();
    if (!state)
        log_error("Cannot write ready timing state.\n");
    log_info("Ready timing trace: %d arcs, %d recomputation failures, %d path hops; all compiler state unchanged.\n",
             arc_count, failed_count, hop_count);
}
NEXTPNR_NAMESPACE_END
