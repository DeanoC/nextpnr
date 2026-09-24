/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  The nextpnr authors
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// Analogue signoff repair for the GPU router.
//
// The routers optimise the per-pip delay table in delay.cc, but signoff after
// bitstream generation uses Mistral's analogue model. The table has one delay
// per wire type, whereas the analogue delay depends on the physical line and
// tap, the driven load and the input slope, so a route whose table timing
// passes can still miss signoff by several hundred picoseconds.
//
// After the GPU router finishes, configure the bitstream and time the design
// with the analogue model. If a clock misses, record every routed pip's
// analogue delay, make getPipDelay return those observations (and a per-type
// calibrated table for unobserved pips), rip up the nets with near-critical
// arcs and run the GPU router again. The routing of the best round is kept.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>

#include "gpurouter.h"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"
#include "util.h"

#include "cyclonev.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

struct SavedRouting
{
    struct Entry
    {
        WireId wire;
        PipId pip;
        PlaceStrength strength;
    };
    std::vector<std::pair<IdString, std::vector<Entry>>> nets;
    float slack = std::numeric_limits<float>::lowest();
};

SavedRouting save_routing(Context *ctx)
{
    SavedRouting saved;
    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        if (ni->is_global)
            continue;
        std::vector<SavedRouting::Entry> entries;
        for (auto &w : ni->wires)
            entries.push_back(SavedRouting::Entry{w.first, w.second.pip, w.second.strength});
        saved.nets.emplace_back(net.first, std::move(entries));
    }
    return saved;
}

void restore_routing(Context *ctx, const SavedRouting &saved)
{
    for (auto &net : saved.nets)
        ctx->ripupNet(net.first);
    for (auto &net : saved.nets) {
        NetInfo *ni = ctx->nets.at(net.first).get();
        for (auto &e : net.second)
            if (e.pip == PipId())
                ctx->bindWire(e.wire, ni, e.strength);
        for (auto &e : net.second)
            if (e.pip != PipId())
                ctx->bindPip(e.pip, ni, e.strength);
    }
}

// Worst clock slack in ps: requested period minus achieved period.
float worst_clock_slack(Context *ctx, TimingAnalyser &tmg, std::string &summary)
{
    float worst = std::numeric_limits<float>::max();
    summary.clear();
    for (auto &clock : tmg.get_timing_result().clock_fmax) {
        float slack = 1e6f / clock.second.constraint - 1e6f / clock.second.achieved;
        worst = std::min(worst, slack);
        summary += stringf("%s%s %.2f MHz", summary.empty() ? "" : ", ", clock.first.c_str(ctx), clock.second.achieved);
    }
    return worst;
}

} // namespace

DelayQuad Arch::getPipDelayCalibrated(PipId pip, const DelayQuad &table) const
{
    auto fnd = pip_delay_observed.find(pip);
    if (fnd != pip_delay_observed.end())
        return DelayQuad(fnd->second);
    WireId src = getPipSrcWire(pip);
    if (src.is_nextpnr_created())
        return table;
    const auto &cal = pip_type_calibration.at(CycloneV::rn2t(src.node));
    if (cal.table_ps <= 0)
        return table;
    // Table entries of 0 or 20 ps are placeholders; use the observed mean.
    if (table.maxDelay() <= 20)
        return DelayQuad(delay_t(cal.analogue_ps / cal.hops * pip_delay_prior));
    // Unobserved pips get a pessimistic prior so that repairs prefer wires
    // whose analogue delay is known over ones that merely look fast.
    float k = float(cal.analogue_ps / cal.table_ps) * pip_delay_prior;
    return DelayQuad(delay_t(table.minDelay() * k), delay_t(table.maxDelay() * k));
}

void Arch::compute_analogue_arcs(bool observe)
{
    NPNR_ASSERT(bitstream_configured);
    struct Job
    {
        const NetInfo *ni;
        const PortRef *usr;
        DelayQuad delay;
        bool ok = false;
        std::vector<AnalogueHop> hops;
    };
    std::vector<Job> jobs;
    for (auto &net : nets) {
        const NetInfo *ni = net.second.get();
        if (ni->driver.cell == nullptr || ni->wires.empty())
            continue;
        for (auto &usr : ni->users)
            jobs.push_back(Job{ni, &usr, DelayQuad(), false, {}});
    }
    // Each arc is an independent simulation over const device data.
    const int nthreads = std::max(1, std::min(int(std::thread::hardware_concurrency()), 32));
    std::vector<std::thread> threads;
    std::atomic<size_t> next(0);
    for (int t = 0; t < nthreads; t++)
        threads.emplace_back([&]() {
            for (size_t i = next.fetch_add(64); i < jobs.size(); i = next.fetch_add(64))
                for (size_t j = i; j < std::min(i + 64, jobs.size()); j++) {
                    auto &job = jobs[j];
                    job.ok = analogue_arc_delay(job.ni, *job.usr, job.delay,
                                                observe && !job.ni->is_global ? &job.hops : nullptr);
                }
        });
    for (auto &th : threads)
        th.join();
    analogue_arc_cache.clear();
    analogue_arc_cache.reserve(jobs.size());
    for (auto &job : jobs)
        analogue_arc_cache.emplace(job.usr, AnalogueArc{job.delay, job.ok});
    analogue_cache_valid = true;
    if (!observe)
        return;
    for (auto &job : jobs)
        for (auto &h : job.hops) {
            // A pip shared by several arcs of the net has one analogue delay
            // (the loads are per node); keep the largest seen.
            auto &obs = pip_delay_observed[h.pip];
            obs = std::max(obs, std::max(h.rise, h.fall));
        }
    // Re-derive the per-type table calibration from every observation.
    for (auto &cal : pip_type_calibration)
        cal = TypeCalibration();
    for (auto &obs : pip_delay_observed) {
        WireId src = getPipSrcWire(obs.first);
        if (src.is_nextpnr_created())
            continue;
        auto &cal = pip_type_calibration.at(CycloneV::rn2t(src.node));
        cal.table_ps += getPipDelayTable(obs.first).maxDelay();
        cal.analogue_ps += obs.second;
        cal.hops++;
    }
    pip_delay_calibrated = true;
}

bool Arch::analogue_repair()
{
    Context *ctx = getCtx();
    const int rounds = ctx->setting<int>("gpurouter/analogueRounds", 3);
    // Stop once every clock has at least this much analogue slack (ps).
    const float target = ctx->setting<float>("gpurouter/analogueSlack", 0.0f);
    // Re-route every net with an arc whose analogue slack is below this (ps).
    const float rip_slack = ctx->setting<float>("gpurouter/analogueRipSlack", 300.0f);
    pip_delay_prior = ctx->setting<float>("gpurouter/analoguePrior", 1.25f);
    if (rounds <= 0)
        return true;

    const IdString repair_slack_key = id("gpurouter/repairSlack");
    const bool user_repair_slack = settings.count(repair_slack_key);

    SavedRouting best;
    bool result = true;
    // Analogue slack of the routing currently bound, if it has been timed
    float current = std::numeric_limits<float>::lowest();
    bool current_timed = false;
    auto secs_since = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    };
    for (int round = 0;; round++) {
        auto t0 = std::chrono::steady_clock::now();
        configure_bitstream();
        // Same analysis as the post-bitstream signoff report
        TimingAnalyser tmg(ctx);
        tmg.setup_only = false;
        tmg.with_clock_skew = true;
        tmg.setup(false, false, true);
        std::string summary;
        current = worst_clock_slack(ctx, tmg, summary);
        current_timed = true;
        log_info("Analogue signoff %s: worst clock slack %.3f ns (%s) in %.2fs\n",
                 round == 0 ? "check" : stringf("repair round %d", round).c_str(), current / 1000.0f,
                 summary.c_str(), secs_since(t0));
        if (current >= target || round >= rounds)
            break;
        if (current > best.slack) {
            best = save_routing(ctx);
            best.slack = current;
        }

        auto t1 = std::chrono::steady_clock::now();
        compute_analogue_arcs(true);
        pool<IdString> rip;
        for (auto &net : nets) {
            NetInfo *ni = net.second.get();
            if (ni->driver.cell == nullptr || ni->is_global || ni->wires.empty())
                continue;
            for (auto &usr : ni->users) {
                delay_t sl = tmg.get_setup_slack(CellPortKey(usr));
                if (sl != std::numeric_limits<delay_t>::max() && sl != std::numeric_limits<delay_t>::lowest() &&
                    sl < rip_slack) {
                    rip.insert(net.first);
                    break;
                }
            }
        }
        bitstream_configured = false;
        if (rip.empty())
            break;
        log_info("    re-routing %zu nets with arcs below %.3f ns analogue slack (%zu pips observed in %.2fs)\n",
                 rip.size(), rip_slack / 1000.0f, pip_delay_observed.size(), secs_since(t1));
        for (auto n : rip)
            ctx->ripupNet(n);
        // The calibrated table now tracks the analogue model, so ask the GPU
        // router's delay repair for the same margin.
        if (!user_repair_slack)
            settings[repair_slack_key] = std::to_string(rip_slack);
        current_timed = false;
        try {
            result = gpurouter(ctx, GpuRouterCfg(ctx));
        } catch (log_execution_error_exception &) {
            // Keep the best legal routing rather than failing the design.
            log_warning("analogue repair round %d could not route; keeping the best earlier round\n", round + 1);
            result = false;
        }
        if (!user_repair_slack)
            settings.erase(repair_slack_key);
        if (!result)
            break;
    }
    bitstream_configured = false;
    if (best.slack != std::numeric_limits<float>::lowest() && (!current_timed || current < best.slack)) {
        log_info("    restoring the routing with the best analogue slack (%.3f ns)\n", best.slack / 1000.0f);
        restore_routing(ctx, best);
        result = true;
    }
    return result;
}

NEXTPNR_NAMESPACE_END
