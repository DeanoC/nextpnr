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
// analogue delay and make getPipDelay return those observations (and a
// per-type calibrated table for unobserved pips). Then, first, ask the GPU
// router for a few materially different routes for each failing sink and
// keep the one the analogue model likes best (candidate selection: the
// scalar search still generates the routes, the model that signoff uses
// picks between them); and if the design still misses, rip up the nets with
// near-critical arcs and run the GPU router again, with the kept nets timed
// by their analogue delays. The routing of the best round is kept. Once
// every clock passes, a few more candidate passes take the arcs near the
// worst analogue slack, which the table may rate as far from critical; a
// pass is kept only if no clock loses setup slack, Fmax or hold slack.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>

#include "analogue_jobs.h"
#include "analogue_repair.h"
#include "critical_cohort.h"
#include "gpurouter.h"
#include "log.h"
#include "nextpnr.h"
#include "private_pair_decomposition_native.h"
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

std::vector<SavedRouting::Entry> snapshot_net(const NetInfo *ni)
{
    std::vector<SavedRouting::Entry> entries;
    for (auto &w : ni->wires)
        entries.push_back(SavedRouting::Entry{w.first, w.second.pip, w.second.strength});
    return entries;
}

// Replace the routing of `ni` with `entries`. Returns false, leaving the net
// partly bound, if the Arch refuses a wire or pip.
bool bind_net(Context *ctx, NetInfo *ni, const std::vector<SavedRouting::Entry> &entries)
{
    ctx->ripupNet(ni->name);
    for (auto &e : entries) {
        if (e.pip != PipId())
            continue;
        if (!ctx->checkWireAvail(e.wire))
            return false;
        ctx->bindWire(e.wire, ni, e.strength);
    }
    for (auto &e : entries) {
        if (e.pip == PipId())
            continue;
        if (!ctx->checkWireAvail(e.wire) || !ctx->checkPipAvailForNet(e.pip, ni))
            return false;
        ctx->bindPip(e.pip, ni, e.strength);
    }
    return true;
}

// Worst enforced setup margin in ps, using each path's actual clock window.
float worst_clock_slack(Context *ctx, TimingAnalyser &tmg, std::string &summary)
{
    summary.clear();
    for (auto &clock : tmg.get_timing_result().clock_fmax) {
        summary += stringf("%s%s %.2f MHz", summary.empty() ? "" : ", ", clock.first.c_str(ctx), clock.second.achieved);
    }
    return tmg.get_worst_setup_slack();
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
    const auto &cal = pip_type_calibration.at(src.node.t());
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
        DelayQuad lut_input_delay;
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
    nextpnr_mistral_workers::run(jobs.size(), nthreads, [&](size_t index) {
        auto &job = jobs[index];
        job.ok = analogue_arc_delay(job.ni, *job.usr, job.delay, observe && !job.ni->is_global ? &job.hops : nullptr,
                                    &job.lut_input_delay);
    });
    analogue_arc_cache.clear();
    analogue_arc_cache.reserve(jobs.size());
    for (auto &job : jobs)
        analogue_arc_cache.emplace(job.usr, AnalogueArc{job.delay, job.ok, job.lut_input_delay});
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
        auto &cal = pip_type_calibration.at(src.node.t());
        cal.table_ps += getPipDelayTable(obs.first).maxDelay();
        cal.analogue_ps += obs.second;
        cal.hops++;
    }
    pip_delay_calibrated = true;
}

void Arch::analogue_relink(const std::vector<PipId> &removed, const std::vector<PipId> &added)
{
    pool<PipId> was, now;
    for (auto p : removed)
        was.insert(p);
    for (auto p : added)
        now.insert(p);
    for (auto p : removed) {
        if (now.count(p) || WireId(p.src).is_nextpnr_created() || WireId(p.dst).is_nextpnr_created())
            continue;
        WireId dst(p.dst);
#ifdef MISTRAL_RNODE_UNLINK
        cyclonev->rnode_unlink(cyclonev->rc2ri(dst.node));
#else
        // The pinned libmistral declares but does not implement
        // rnode_unlink(), so a routing mux cannot be returned to its
        // default; select an input no net drives instead, which takes the
        // mux off the load of the wire this net no longer uses. The
        // bitstream is rebuilt from scratch before it is written, so this
        // state is only ever simulated.
        for (PipId up : getPipsUphill(dst)) {
            WireId s = getPipSrcWire(up);
            if (s.node == p.src || s.is_nextpnr_created() || getBoundWireNet(s) != nullptr)
                continue;
            cyclonev->rnode_link(cyclonev->rc2ri(s.node), cyclonev->rc2ri(dst.node));
            break;
        }
#endif
    }
    for (auto p : added) {
        if (was.count(p) || WireId(p.src).is_nextpnr_created() || WireId(p.dst).is_nextpnr_created())
            continue;
        cyclonev->rnode_link(cyclonev->rc2ri(p.src), cyclonev->rc2ri(p.dst));
    }
}

bool Arch::analogue_candidate_pass(TimingAnalyser &tmg, float limit, bool improve, const GpuRouterCfg &repair_cfg)
{
    Context *ctx = getCtx();
    NPNR_ASSERT(bitstream_configured && analogue_cache_valid);
    // Candidates per sink, sinks per pass, minimum worst-sink slack gain to
    // accept a change (ps), and the fanout above which a net is left alone
    // (every sink of the net is simulated per candidate)
    const int count = ctx->setting<int>("gpurouter/analogueCandidates", 4);
    const int max_arcs = ctx->setting<int>("gpurouter/analogueCandidateArcs", 200);
    const float min_gain = ctx->setting<float>("gpurouter/analogueCandidateGain", 20.0f);
    const int max_fanout = ctx->setting<int>("gpurouter/analogueCandidateFanout", 64);
    // Delay prior for pips no analogue observation exists for while
    // searching candidates; 1.0 (the plain per-type ratio) rather than the
    // pessimistic repair prior, so the search does not simply prefer the
    // route it has already seen
    const float search_prior = ctx->setting<float>("gpurouter/analogueCandidatePrior", 1.0f);
    auto t0 = std::chrono::steady_clock::now();
    const double lease_wait0 = gpurouter_lease_wait_seconds();

    struct Item
    {
        float slack;
        NetInfo *ni;
        store_index<PortRef> user;
    };
    std::vector<Item> items;
    for (auto &net : nets) {
        NetInfo *ni = net.second.get();
        if (ni->driver.cell == nullptr || ni->is_global || ni->wires.empty())
            continue;
        if (int(ni->users.entries()) > max_fanout)
            continue;
        bool movable = true;
        for (auto &w : ni->wires)
            movable &= (w.second.strength <= STRENGTH_STRONG);
        if (!movable)
            continue;
        for (auto usr : ni->users.enumerate()) {
            float sl = tmg.get_setup_slack(CellPortKey(usr.value));
            if (sl == std::numeric_limits<float>::lowest() || sl == std::numeric_limits<float>::max())
                continue;
            if (sl < limit)
                items.push_back(Item{sl, ni, usr.index});
        }
    }
    if (items.empty())
        return false;
    std::stable_sort(items.begin(), items.end(), [&](const Item &a, const Item &b) {
        if (a.slack != b.slack)
            return a.slack < b.slack;
        if (a.ni->name != b.ni->name)
            return a.ni->name.str(ctx) < b.ni->name.str(ctx);
        return a.user.idx() < b.user.idx();
    });
    const size_t selected = items.size();
    if (items.size() > size_t(max_arcs))
        items.resize(max_arcs);

    const float saved_prior = pip_delay_prior;
    pip_delay_prior = search_prior;
    GpuRouterCfg cfg = repair_cfg;
    // The top-level router owns the exclusive telemetry stream. Candidate
    // searches are downstream helpers and must not try to create it again.
    cfg.telemetry_path.clear();
    std::unique_ptr<GpuCandidateRouter> router;
    try {
        router = std::make_unique<GpuCandidateRouter>(ctx, cfg);
    } catch (...) {
        pip_delay_prior = saved_prior;
        throw;
    }
    pip_delay_prior = saved_prior;
    auto &cr = *router;

    int tried = 0, generated = 0, improved = 0, rejected = 0;
    float total_gain = 0.0f;
    double search_secs = 0.0, eval_secs = 0.0;
    const int nthreads = std::max(1, std::min(int(std::thread::hardware_concurrency()), 32));
    // All sinks are searched together so the device is busy
    auto ts = std::chrono::steady_clock::now();
    std::vector<GpuCandidateRouter::Sink> sinks;
    for (auto &it : items)
        sinks.push_back(GpuCandidateRouter::Sink{it.ni, it.user});
    auto all_cands = cr.candidates(sinks, count);
    search_secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
    for (size_t idx = 0; idx < items.size(); idx++) {
        auto &it = items[idx];
        NetInfo *ni = it.ni;
        auto &cands = all_cands[idx];
        if (cands.empty())
            continue;
        // Analogue delay and slack of every sink before the change; a sink
        // without an observation or a constraint does not take part
        std::vector<const PortRef *> users;
        std::vector<float> slack_old;
        std::vector<delay_t> d_old;
        std::vector<bool> counted;
        float score_old = std::numeric_limits<float>::max();
        for (auto &usr : ni->users) {
            float sl = tmg.get_setup_slack(CellPortKey(usr));
            auto fnd = analogue_arc_cache.find(&usr);
            bool ok = fnd != analogue_arc_cache.end() && fnd->second.ok &&
                      sl != std::numeric_limits<float>::lowest() && sl != std::numeric_limits<float>::max();
            // An improvement must not slow another clock's critical path, so
            // there a sink scores by its margin over the worst path of its
            // own clock pairs instead of by its slack
            if (ok && improve)
                sl = float(tmg.get_setup_slack_margin(CellPortKey(usr)));
            users.push_back(&usr);
            slack_old.push_back(sl);
            d_old.push_back(ok ? fnd->second.delay.maxDelay() : 0);
            counted.push_back(ok);
            if (ok)
                score_old = std::min(score_old, sl);
        }
        if (score_old == std::numeric_limits<float>::max())
            continue;
        generated += int(cands.size());
        tried++;
        const auto old = snapshot_net(ni);
        dict<WireId, PlaceStrength> old_strength;
        std::vector<PipId> old_pips;
        for (auto &e : old) {
            old_strength[e.wire] = e.strength;
            if (e.pip != PipId())
                old_pips.push_back(e.pip);
        }
        float best_score = score_old + min_gain;
        int best = -1;
        std::vector<DelayQuad> best_delay;
        std::vector<DelayQuad> best_lut_input_delay;
        std::vector<SavedRouting::Entry> best_entries;
        for (size_t c = 0; c < cands.size(); c++) {
            cfg.check_stop();
            std::vector<SavedRouting::Entry> entries;
            std::vector<PipId> new_pips;
            for (auto &w : cands[c].wires) {
                auto fs = old_strength.find(w.first);
                entries.push_back(SavedRouting::Entry{w.first, w.second,
                                                      fs != old_strength.end() ? fs->second : STRENGTH_WEAK});
                if (w.second != PipId())
                    new_pips.push_back(w.second);
            }
            if (!bind_net(ctx, ni, entries)) {
                rejected++;
                bool restored = bind_net(ctx, ni, old);
                NPNR_ASSERT(restored);
                continue;
            }
            analogue_relink(old_pips, new_pips);
            auto te = std::chrono::steady_clock::now();
            // Every sink is an independent simulation over const state
            std::vector<DelayQuad> d_new(users.size());
            std::vector<DelayQuad> input_new(users.size());
            std::vector<uint8_t> ok(users.size(), 1);
            auto simulate = [&](size_t first, size_t step) {
                for (size_t i = first; i < users.size(); i += step) {
                    if (!counted[i])
                        continue;
                    DelayQuad d;
                    ok[i] = analogue_arc_delay(ni, *users[i], d, nullptr, &input_new[i]);
                    d_new[i] = d;
                }
            };
            const size_t nt = std::min<size_t>(nthreads, users.size());
            if (nt <= 1) {
                simulate(0, 1);
            } else {
                std::vector<std::thread> threads;
                for (size_t t = 0; t < nt; t++)
                    threads.emplace_back(simulate, t, nt);
                for (auto &th : threads)
                    th.join();
            }
            float score = std::numeric_limits<float>::max();
            bool ok_all = true;
            for (size_t i = 0; i < users.size(); i++) {
                if (!counted[i])
                    continue;
                ok_all &= bool(ok[i]);
                score = std::min(score, slack_old[i] + float(d_old[i] - d_new[i].maxDelay()));
            }
            eval_secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - te).count();
            if (ok_all && score > best_score) {
                best_score = score;
                best = int(c);
                best_delay = d_new;
                best_lut_input_delay = input_new;
                best_entries = entries;
            }
            // back to the original routing, then the original mux state
            bool restored = bind_net(ctx, ni, old);
            NPNR_ASSERT(restored);
            analogue_relink(new_pips, old_pips);
        }
        if (best < 0)
            continue;
        std::vector<PipId> new_pips;
        for (auto &e : best_entries)
            if (e.pip != PipId())
                new_pips.push_back(e.pip);
        bool bound = bind_net(ctx, ni, best_entries);
        NPNR_ASSERT(bound);
        analogue_relink(old_pips, new_pips);
        for (size_t i = 0; i < users.size(); i++)
            if (counted[i])
                analogue_arc_cache[users[i]] = AnalogueArc{best_delay[i], true, best_lut_input_delay[i]};
        cr.resync(ni);
        improved++;
        total_gain += best_score - score_old;
        if (ctx->verbose)
            log_info("      %s: candidate %d (variant %d) raises the net's worst sink %s %.3f -> %.3f ns\n",
                     ctx->nameOf(ni), best, cands[best].variant, improve ? "margin" : "slack", score_old / 1000.0f,
                     best_score / 1000.0f);
    }
    log_info("    analogue candidate selection: %d of %zu %s sinks tried with %d candidates, %d nets "
             "re-routed (%d candidates refused by the Arch), worst-sink %s gained %.3f ns in total, %.2fs "
             "(%.2fs searching, %.2fs simulating)\n",
             tried, selected, improve ? "near-critical" : "failing", generated, improved, rejected,
             improve ? "margin" : "slack", total_gain / 1000.0f,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() -
                     (gpurouter_lease_wait_seconds() - lease_wait0),
             search_secs, eval_secs);
    return improved > 0;
}

bool Arch::analogue_repair()
{
    Context *ctx = getCtx();
    const double budget = ctx->setting<double>("gpurouter/analogueTimeBudget", 300.0);
    if (!std::isfinite(budget) || budget < 0)
        log_error("analogueTimeBudget must be finite and nonnegative (seconds; 0 disables).\n");
    const auto started = std::chrono::steady_clock::now();
    // Time spent waiting for a gpu-run phase lease is not repair work
    const double lease_wait_started = gpurouter_lease_wait_seconds();
    auto expired = [budget, started, lease_wait_started]() {
        if (budget <= 0)
            return false;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return elapsed - (gpurouter_lease_wait_seconds() - lease_wait_started) >= budget;
    };
    const int rounds = ctx->setting<int>("gpurouter/analogueRounds", 3);
    // Candidate-selection passes before each full re-route (0 disables)
    const int cand_rounds = ctx->setting<int>("gpurouter/analogueCandidateRounds", 2);
    const bool candidates = ctx->setting<int>("gpurouter/analogueCandidates", 4) > 0;
    // After a full re-route, put back the old route of every re-routed net
    // whose worst sink got slower by more than this (ps), where its old
    // wires are still free
    const bool revert = ctx->setting<bool>("gpurouter/analogueRevert", true);
    const float revert_margin = ctx->setting<float>("gpurouter/analogueRevertMargin", 20.0f);
    // A round that lands this far (ps) below the best routing so far is
    // abandoned: the best routing is restored before the next round, so a
    // re-route that wrecked the design is not the base of the next tries
    // (the next re-route differs anyway, the route order is shuffled)
    const float restore_margin = ctx->setting<float>("gpurouter/analogueRestoreMargin", 1000.0f);
    // At most this many nets (the worst by analogue slack) are ripped per
    // re-route (0: every net with an arc below analogueRipSlack). A
    // re-route of several hundred nets regularly loses nanoseconds; one of
    // a few dozen perturbs the routes the next candidate passes choose
    // from without wrecking the rest.
    const int rip_max = ctx->setting<int>("gpurouter/analogueRipNets", 64);
    // Stop once every clock has at least this much analogue slack (ps).
    const float target = ctx->setting<float>("gpurouter/analogueSlack", 0.0f);
    // Re-route every net with an arc whose analogue slack is below this (ps).
    const float rip_slack = ctx->setting<float>("gpurouter/analogueRipSlack", 300.0f);
    // Once every clock meets the target, up to this many candidate passes
    // more take the arcs within analogueImproveBand (ps) of the worst slack
    // (0 disables)
    const int improve_rounds = ctx->setting<int>("gpurouter/analogueImproveRounds", 4);
    const float improve_band = ctx->setting<float>("gpurouter/analogueImproveBand", 300.0f);
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
    int reroutes = 0, cand_passes = 0;
    // Set once the best routing has been put back: the next timing is its
    // slack under the current calibration, which is the baseline from then on
    bool restored = false;
    // Every round is a timing, a candidate pass or a re-route; this bounds
    // the loop even if a round neither improves nor re-routes
    const int max_rounds = (rounds + 1) * (cand_rounds + 3);
    // Analysis of the latest timing
    std::unique_ptr<TimingAnalyser> tmg;
    auto time_routing = [&](std::string &summary) {
        configure_bitstream();
        // Same analysis as the post-bitstream signoff report
        tmg = std::make_unique<TimingAnalyser>(ctx);
        tmg->setup_only = false;
        tmg->with_clock_skew = true;
        tmg->setup(false, false, true);
        return worst_clock_slack(ctx, *tmg, summary);
    };
    for (int round = 0;; round++) {
        auto t0 = std::chrono::steady_clock::now();
        std::string summary;
        current = time_routing(summary);
        current_timed = true;
        log_info("Analogue signoff %s: worst clock slack %.3f ns (%s) in %.2fs\n",
                 round == 0 ? "check" : stringf("repair round %d", round).c_str(), current / 1000.0f,
                 summary.c_str(), secs_since(t0));
        if (current >= target || reroutes >= rounds)
            break;
        if (round >= max_rounds) {
            log_info("    stopping after %d rounds without reaching the target\n", round);
            break;
        }
        if (restored) {
            // The delay table behind arcs the analogue model cannot time
            // is recalibrated from every observation, so the slack recorded
            // for the best routing can be one it no longer has. Comparing
            // with that would restore the same routing indefinitely.
            restored = false;
            best.slack = current;
        } else if (current > best.slack) {
            best = save_routing(ctx);
            best.slack = current;
        } else if (current < best.slack - restore_margin) {
            log_info("    %.3f ns below the best routing so far; restoring it (%.3f ns) before the next round\n",
                     (best.slack - current) / 1000.0f, best.slack / 1000.0f);
            restore_routing(ctx, best);
            bitstream_configured = false;
            current_timed = false;
            restored = true;
            continue;
        }

        if (expired()) {
            log_info("Analogue repair time budget exhausted (%.3f seconds); keeping the best measured routing.\n", budget);
            break;
        }

        auto t1 = std::chrono::steady_clock::now();
        compute_analogue_arcs(true);
        if (candidates && cand_passes < cand_rounds) {
            cand_passes++;
            GpuRouterCfg cfg(ctx);
            cfg.stop_requested = expired;
            // A cancelled pass may already have rebound some candidates.
            current_timed = false;
            bool changed;
            try {
                changed = analogue_candidate_pass(*tmg, target, false, cfg);
            } catch (const GpuRouterCancelled &) {
                log_info("Analogue repair time budget exhausted during candidate selection; restoring the best routing.\n");
                result = false;
                break;
            }
            bitstream_configured = false;
            if (changed) {
                current_timed = false;
                continue; // re-time the new routing before deciding on a full re-route
            }
        }
        std::vector<std::pair<float, IdString>> worst; // slack, net
        for (auto &net : nets) {
            NetInfo *ni = net.second.get();
            if (ni->driver.cell == nullptr || ni->is_global || ni->wires.empty())
                continue;
            float net_slack = std::numeric_limits<float>::max();
            for (auto &usr : ni->users) {
                delay_t sl = tmg->get_setup_slack(CellPortKey(usr));
                if (sl != std::numeric_limits<delay_t>::max() && sl != std::numeric_limits<delay_t>::lowest())
                    net_slack = std::min(net_slack, float(sl));
            }
            if (net_slack < rip_slack)
                worst.emplace_back(net_slack, net.first);
        }
        std::stable_sort(worst.begin(), worst.end(), [&](const auto &a, const auto &b) {
            if (a.first != b.first)
                return a.first < b.first;
            return a.second.str(ctx) < b.second.str(ctx);
        });
        if (rip_max > 0 && int(worst.size()) > rip_max)
            worst.resize(rip_max);
        pool<IdString> rip;
        for (auto &w : worst)
            rip.insert(w.second);
        bitstream_configured = false;
        if (rip.empty())
            break;
        log_info("    re-routing %zu nets with arcs below %.3f ns analogue slack (%zu pips observed in %.2fs)\n",
                 rip.size(), rip_slack / 1000.0f, pip_delay_observed.size(), secs_since(t1));
        // What the ripped nets had, to give back what the re-route makes worse
        struct Ripped
        {
            std::vector<SavedRouting::Entry> entries;
            std::vector<float> slack;
            std::vector<delay_t> delay;
            std::vector<bool> ok;
        };
        dict<IdString, Ripped> ripped;
        for (auto n : rip) {
            NetInfo *ni = nets.at(n).get();
            Ripped r;
            r.entries = snapshot_net(ni);
            for (auto &usr : ni->users) {
                float sl = tmg->get_setup_slack(CellPortKey(usr));
                auto fnd = analogue_arc_cache.find(&usr);
                bool ok = fnd != analogue_arc_cache.end() && fnd->second.ok &&
                          sl != std::numeric_limits<float>::lowest() && sl != std::numeric_limits<float>::max();
                r.slack.push_back(sl);
                r.delay.push_back(ok ? fnd->second.delay.maxDelay() : 0);
                r.ok.push_back(ok);
            }
            ripped[n] = std::move(r);
            // The observations of everything that stays routed remain valid
            // for the router's timing; drop the ones of the nets it moves
            for (auto &usr : ni->users)
                analogue_arc_cache.erase(&usr);
            ctx->ripupNet(n);
        }
        reroutes++;
        cand_passes = 0;
        // The calibrated table now tracks the analogue model, so ask the GPU
        // router's delay repair for the same margin.
        if (!user_repair_slack)
            settings[repair_slack_key] = std::to_string(rip_slack);
        current_timed = false;
        try {
            result = try_analogue_repair_reroute(round + 1, [&]() {
                GpuRouterCfg cfg(ctx);
                cfg.stop_requested = expired;
                cfg.legality_timing_gate = !signoff_after_route;
                // The top-level router has already closed its complete stream;
                // analogue repair helpers must not reopen the exclusive path.
                cfg.telemetry_path.clear();
                return gpurouter(ctx, cfg, false);
            });
        } catch (log_execution_error_exception &) {
            if (!user_repair_slack)
                settings.erase(repair_slack_key);
            throw;
        }
        if (!user_repair_slack)
            settings.erase(repair_slack_key);
        if (!result) {
            if (expired())
                log_info("Analogue repair time budget exhausted during rerouting; restoring the best routing.\n");
            break;
        }
        if (revert) {
            // A full re-route optimises the calibrated table and can leave
            // many nets slower under the analogue model than they were;
            // give those their old routes back where the wires are free,
            // worst regression first, and keep the ones it improved
            auto t2 = std::chrono::steady_clock::now();
            configure_bitstream();
            std::vector<std::pair<float, IdString>> order;
            for (auto &r : ripped) {
                NetInfo *ni = nets.at(r.first).get();
                float before = std::numeric_limits<float>::max(), after = before;
                size_t i = 0;
                for (auto &usr : ni->users) {
                    if (r.second.ok.at(i)) {
                        auto fnd = analogue_arc_cache.find(&usr);
                        delay_t d = (fnd != analogue_arc_cache.end() && fnd->second.ok) ? fnd->second.delay.maxDelay()
                                                                                        : r.second.delay[i];
                        before = std::min(before, r.second.slack[i]);
                        after = std::min(after, r.second.slack[i] + float(r.second.delay[i] - d));
                    }
                    i++;
                }
                if (before != std::numeric_limits<float>::max() && after < before - revert_margin)
                    order.emplace_back(after - before, r.first);
            }
            std::stable_sort(order.begin(), order.end(), [&](const auto &a, const auto &b) {
                if (a.first != b.first)
                    return a.first < b.first;
                return a.second.str(ctx) < b.second.str(ctx);
            });
            int reverted = 0, taken = 0;
            float regained = 0.0f;
            for (auto &o : order) {
                NetInfo *ni = nets.at(o.second).get();
                auto current = snapshot_net(ni);
                if (bind_net(ctx, ni, ripped.at(o.second).entries)) {
                    reverted++;
                    regained -= o.first;
                } else {
                    bool restored = bind_net(ctx, ni, current);
                    NPNR_ASSERT(restored);
                    taken++;
                }
            }
            bitstream_configured = false;
            log_info("    re-route made %zu of %zu nets slower at their worst sink; gave %d their old route back "
                     "(%.3f ns of sink slack), %d could not have it (wires taken), %.2fs\n",
                     order.size(), ripped.size(), reverted, regained / 1000.0f, taken, secs_since(t2));
        }
    }
    if (current_timed && current >= target && candidates && improve_rounds > 0) {
        // Meeting the target is not the end of it: Fmax is set by the worst
        // arcs, and the router ranks those by the delay table. One the table
        // rates as harmless can be the slowest under the analogue model (on
        // the FES menu a chain of 13 V4 wires into an HPS port set Fmax with
        // 2.04 ns of analogue slack where the table gave the design 3.83 ns),
        // and the repair above never looks at a passing clock. So candidate
        // passes go on working on the arcs near the worst analogue slack,
        // scored by their margin over their own clock pairs' worst paths so
        // that no clock's critical path is slowed, and a pass is kept only
        // if no clock lost setup slack, Fmax or hold slack.
        AnalogueClockTiming kept_timing(tmg->get_timing_result());
        SavedRouting kept = save_routing(ctx);
        // The searches use the calibrated table, recalibrated from each
        // pass's routing, but signoff times the arcs the analogue model
        // cannot simulate with the table the repair left (the plain one if
        // it had nothing to repair). So every pass is timed with that table,
        // as it was when the stage started, and it is put back afterwards.
        const bool calibrated = pip_delay_calibrated;
        const auto observed = pip_delay_observed;
        const auto type_calibration = pip_type_calibration;
        auto signoff_table = [&]() {
            pip_delay_calibrated = calibrated;
            pip_delay_observed = observed;
            pip_type_calibration = type_calibration;
        };
        // (A design without a timed clock has nothing to improve.)
        for (int pass = 1; pass <= improve_rounds && !kept_timing.setup.empty(); pass++) {
            if (expired()) {
                log_info("Analogue repair time budget exhausted (%.3f seconds); keeping the best measured routing.\n",
                         budget);
                break;
            }
            auto t1 = std::chrono::steady_clock::now();
            compute_analogue_arcs(true);
            GpuRouterCfg cfg(ctx);
            cfg.stop_requested = expired;
            bool changed;
            try {
                changed = analogue_candidate_pass(*tmg, current + improve_band, true, cfg);
            } catch (const GpuRouterCancelled &) {
                log_info("Analogue repair time budget exhausted during candidate selection; restoring the best "
                         "routing.\n");
                restore_routing(ctx, kept);
                break;
            }
            bitstream_configured = false;
            signoff_table();
            if (!changed)
                break;
            std::string summary;
            const float slack = time_routing(summary);
            log_info("Analogue signoff improvement round %d: worst clock slack %.3f ns (%s) in %.2fs\n", pass,
                     slack / 1000.0f, summary.c_str(), secs_since(t1));
            AnalogueClockTiming timing(tmg->get_timing_result());
            if (timing.worse_than(kept_timing)) {
                log_info("    a clock lost setup slack, Fmax or hold slack; restoring the routing before this round\n");
                // `current` is still the slack of `kept`, which meets the
                // target, so the final check below keeps it
                restore_routing(ctx, kept);
                break;
            }
            // Only a kept pass updates the slack of the bound routing
            const bool progress = timing.better_than(kept_timing);
            current = slack;
            kept_timing = std::move(timing);
            if (!progress)
                break;
            kept = save_routing(ctx);
        }
        signoff_table();
    }
    bitstream_configured = false;
    analogue_cache_valid = false;
    if (best.slack != std::numeric_limits<float>::lowest() && (!current_timed || current < best.slack)) {
        log_info("    restoring the routing with the best analogue slack (%.3f ns)\n", best.slack / 1000.0f);
        restore_routing(ctx, best);
        result = true;
    }
    return result;
}

bool critical_route_trial(Context *ctx, NetInfo *ni, const GpuRouteTree &candidate,
                          const std::function<bool()> &accept)
{
    if (!ni || ni->is_global || ni->clkconstr || ni->wires.empty() || !ni->driver.cell) return false;
    const auto old = snapshot_net(ni);
    dict<WireId, PlaceStrength> strengths;
    for (const auto &e : old) {
        if (e.strength > STRENGTH_STRONG) return false;
        strengths[e.wire] = e.strength;
    }
    const WireId root = ctx->getNetinfoSourceWire(ni);
    dict<WireId, PipId> tree;
    for (const auto &e : candidate.wires) {
        if (tree.count(e.first) || (e.second == PipId() ? e.first != root : ctx->getPipDstWire(e.second) != e.first))
            return false;
        tree[e.first] = e.second;
    }
    if (!tree.count(root) || tree.at(root) != PipId()) return false;
    // Every proposed wire must connect to the original driver without cycles.
    for (const auto &entry : tree) {
        pool<WireId> seen;
        WireId w = entry.first;
        while (w != root) {
            if (!seen.insert(w).second || !tree.count(w) || tree.at(w) == PipId()) return false;
            w = ctx->getPipSrcWire(tree.at(w));
        }
    }
    for (const auto &usr : ni->users)
        for (WireId sink : ctx->getNetinfoSinkWires(ni, usr))
            if (!tree.count(sink)) return false;
    std::vector<SavedRouting::Entry> entries;
    for (const auto &e : candidate.wires)
        entries.push_back({e.first, e.second, strengths.count(e.first) ? strengths.at(e.first) : STRENGTH_WEAK});
    auto restore = [&]() {
        NPNR_ASSERT(bind_net(ctx, ni, old));
        // Rebuild physical mux state as well as the per-sink analogue cache.
        ctx->configure_bitstream();
    };
    try {
        if (bind_net(ctx, ni, entries)) {
            ctx->configure_bitstream();
            if (accept()) return true;
        }
    } catch (...) {
        restore();
        throw;
    }
    restore();
    return false;
}

void Arch::repair_critical_routes(bool final_after_lut)
{
    const int budget = final_after_lut ? critical_final_route_budget : critical_route_budget;
    if (!budget) return;
    Context *ctx = getCtx();
    if (final_after_lut && (fes_fence_active || fes_has_reserved_rect || fes_has_cram_region ||
        !fes_frozen_cells.empty() || !fes_frozen_pips.empty() || ctx->force ||
        (ctx->settings.count(ctx->id("timing/allowFail")) && ctx->setting<bool>("timing/allowFail")) ||
        critical_move_route_budget || (!critical_lut6_swap_budget && !critical_private_pair_budget && !critical_lut_permute_budget)))
        log_error("Final critical route rediscovery requires ordinary completed LUT repairs and unforced timing.\n");
    if (budget < 0 || budget > 32 || fes_any_slot_region_active ||
        !ctx->attrs.count(id_step) || ctx->attrs.at(id_step).as_string() != "route")
        log_error("Critical route repair requires an ordinary completed route and budget 1..32.\n");
    configure_bitstream();
    auto analyse = [&](bool skew = true) {
        auto t = std::make_unique<TimingAnalyser>(ctx);
        t->with_clock_skew = skew;
        t->setup(false, false, true);
        return t;
    };
    auto snapshot = [&](TimingAnalyser &t, TimingAnalyser *reference = nullptr) {
        if (final_after_lut) {
            NPNR_ASSERT(reference != nullptr);
            return critical_swap_live_endpoint_snapshot(ctx,t,*reference);
        }
        TimingAnalyser unskewed(ctx);
        unskewed.setup(false, false, true);
        return critical_swap_endpoint_snapshot(ctx, t, unskewed);
    };
    auto before = analyse();
    auto reference = final_after_lut ? analyse(false) : nullptr;
    if (before->have_loops || (reference && reference->have_loops)) log_error("Critical route repair refuses timing loops.\n");
    auto inventory = snapshot(*before,reference.get());
    if (final_after_lut) {
        std::string reason;
        if (!private_pair_clock_safe(before->get_timing_result(),before->get_timing_result(),&reason) ||
            !private_pair_clock_safe(reference->get_timing_result(),reference->get_timing_result(),&reason))
            log_error("Final critical route rediscovery lacks complete native clocks: %s.\n",reason.c_str());
        log_info("Final critical route rediscovery: fresh skew/reference timing and live inventory; budget=%d.\n",budget);
    }
    const bool allow_buffer_users = final_after_lut && critical_final_route_buf_users;
    auto has_buffer_users = [&](NetInfo *net) {
        if (!allow_buffer_users) return false;
        for (const auto &user : net->users) if (user.cell && user.cell->type == id_MISTRAL_BUF) return true;
        return false;
    };
    auto covered = [&](NetInfo *net, TimingAnalyser &skewed, TimingAnalyser &plain, std::string &reason) {
        if (has_buffer_users(net) && !critical_final_route_buffer_users(ctx,net,&reason)) return false;
        return critical_final_route_source_coverage(ctx,skewed,net,&reason) &&
               critical_final_route_source_coverage(ctx,plain,net,&reason) &&
               critical_lut6_swap_output_coverage(ctx,skewed,net,&reason) &&
               critical_lut6_swap_output_coverage(ctx,plain,net,&reason);
    };
    auto excluded = [&](NetInfo *net, const char *reason) {
        if (!final_after_lut) return;
        for (const auto &user : net->users) if (user.cell) {
            const float slack=before->get_setup_slack(CellPortKey(user));
            if (std::isfinite(slack) && slack<0 && slack!=std::numeric_limits<float>::lowest()) {
                log_info("Final critical route filter: net %s: %s.\n",ctx->nameOf(net),reason);
                break;
            }
        }
    };
    struct Item { NetInfo *net; store_index<PortRef> user; float slack; };
    std::vector<Item> items;
    auto ordinary = [&](CellInfo *c) {
        if (!c || c->isPseudo() || c->region || c->cluster != ClusterId() || c->bel == BelId()) return false;
        for (const char *a : {"keep", "dont_touch", "FES_SLOT"}) if (c->attrs.count(id(a))) return false;
        return c->type.in(id_MISTRAL_FF, id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                          id_MISTRAL_ALUT5, id_MISTRAL_ALUT6);
    };
    for (const auto &entry : nets) {
        auto *ni = entry.second.get();
        if (ni->is_global || ni->clkconstr || ni->wires.empty() || !ordinary(ni->driver.cell) ||
            ni->driver.port != id_Q || !ni->users.entries() || ni->users.entries() > 64) {
            excluded(ni,"ordinary routed Q-driver/global/clock/user-count boundary"); continue;
        }
        bool allowed = true;
        for (const auto &port : ctx->ports) if (port.second.net == ni) allowed = false;
        for (const auto &w : ni->wires) if (w.second.strength > STRENGTH_STRONG) allowed = false;
        bool needs_buffer_check = false;
        for (const auto &usr : ni->users) {
            if (!ordinary(usr.cell) || (usr.cell->type == id_MISTRAL_FF ? usr.port != id_DATAIN :
                !usr.port.in(id_A,id_B,id_C,id_D,id_E,id_F))) {
                if (allow_buffer_users && usr.cell && usr.cell->type == id_MISTRAL_BUF && usr.port == id_A)
                    needs_buffer_check = true;
                else allowed = false;
            }
        }
        if (allowed && needs_buffer_check) {
            std::string reason;
            if (!critical_final_route_buffer_users(ctx,ni,&reason)) { excluded(ni,reason.c_str()); continue; }
        }
        if (!allowed) { excluded(ni,"ordinary complete-user/public/protected-route boundary"); continue; }
        if (final_after_lut) {
            bool failing=false;
            for (const auto &usr : ni->users) {
                const float slack=before->get_setup_slack(CellPortKey(usr));
                failing |= std::isfinite(slack) && slack<0 && slack!=std::numeric_limits<float>::lowest();
            }
            if (!failing) continue;
            std::string reason;
            if (!covered(ni,*before,*reference,reason)) { excluded(ni,reason.c_str()); continue; }
        }
        for (auto usr : ni->users.enumerate()) {
            const float slack = before->get_setup_slack(CellPortKey(usr.value));
            if (std::isfinite(slack) && slack < 0 && slack != std::numeric_limits<float>::lowest())
                items.push_back({ni, usr.index, slack});
        }
    }
    std::sort(items.begin(), items.end(), [&](const Item &a, const Item &b) {
        if (a.slack != b.slack) return a.slack < b.slack;
        if (a.net->name != b.net->name) return a.net->name.str(ctx) < b.net->name.str(ctx);
        return a.user.idx() < b.user.idx();
    });
    std::vector<GpuCandidateRouter::Sink> sinks;
    pool<IdString> selected;
    for (const auto &it : items) if (selected.insert(it.net->name).second) {
        sinks.push_back({it.net,it.user});
        if (final_after_lut)
            log_info("Final critical route selected %zu: net %s sink %s.%s slack %.3f ps.\n",sinks.size(),
                     ctx->nameOf(it.net),ctx->nameOf(it.net->users.at(it.user).cell),
                     it.net->users.at(it.user).port.c_str(ctx),it.slack);
        if (sinks.size() == size_t(budget)) break;
    }
    if (final_after_lut) {
        pool<IdString> listed;
        for (const auto &item : items)
            if (listed.insert(item.net->name).second && !selected.count(item.net->name))
                log_info("Final critical route filter: net %s: selected-net budget exhausted.\n",ctx->nameOf(item.net));
        log_info("Final critical route shortlist: failing ordinary sink rows=%zu selected nets=%zu.\n",items.size(),sinks.size());
    }
    if (sinks.empty()) {
        log_info("%s route repair: no eligible failing data nets.\n",final_after_lut?"Final critical":"Critical"); return;
    }
    // Acquire every helper boundary before candidate generation or measurement can configure muxes.
    std::vector<std::shared_ptr<const CriticalFinalRouteBufferState>> helper_measurement_states;
    if (allow_buffer_users) for (const auto &sink : sinks) if (has_buffer_users(sink.net)) {
        std::string reason;
        auto state = critical_final_route_buffer_snapshot(ctx,sink.net,&reason);
        if (!state) log_error("Final route helper preflight failed: %s.\n",reason.c_str());
        helper_measurement_states.push_back(std::move(state));
    }
    // Calibrate only the search's flattened delay table. Trial STA always uses
    // the exact same fallback table as the initial full-route baseline.
    const auto observed = pip_delay_observed;
    const auto calibration = pip_type_calibration;
    const bool calibrated = pip_delay_calibrated;
    const float prior = pip_delay_prior;
    auto restore_table = [&]() {
        pip_delay_observed = observed; pip_type_calibration = calibration;
        pip_delay_calibrated = calibrated; pip_delay_prior = prior;
    };
    auto learned = observed;
    std::vector<std::vector<std::pair<float, GpuRouteTree>>> ranked(sinks.size());
    std::vector<std::vector<std::vector<std::pair<WireId, PipId>>>> seen(sinks.size());
    // One feedback generation: measured rejected-route hop costs steer the
    // search away from optimistic unobserved wires. Observations are a search
    // heuristic only; topology-dependent loads cannot become signoff timing.
    for (int generation = 0; generation < 2; ++generation) {
        std::unique_ptr<GpuCandidateRouter> router;
        try {
            pip_delay_observed = learned;
            compute_analogue_arcs(true);
            learned = pip_delay_observed;
            GpuRouterCfg cfg(ctx); cfg.telemetry_path.clear();
            router = std::make_unique<GpuCandidateRouter>(ctx, cfg);
        } catch (...) { restore_table(); throw; }
        restore_table();
        auto proposals = router->candidates(sinks, 4, true);
        router.reset(); // Release the GPU lease before native CPU evaluation.
        log_info("%s route generation %d: learned observations=%zu.\n",final_after_lut?"Final critical":"Critical",generation,learned.size());
        for (size_t i = 0; i < sinks.size(); ++i) {
            auto *ni = sinks[i].net;
            std::vector<CriticalRouteSinkDelay> base;
            bool known = true;
            for (const auto &usr : ni->users) {
                auto found = analogue_arc_cache.find(&usr);
                if (found == analogue_arc_cache.end() || !found->second.ok) { known = false; break; }
                base.push_back({float(before->get_setup_slack(CellPortKey(usr))),
                                float(found->second.delay.maxDelay()), 0});
            }
            if (!known) { excluded(ni,"one or more sinks lack a characterized native analogue cache entry"); continue; }
            if (final_after_lut)
                log_info("Final critical route candidates: generation %d net %s proposals=%zu.\n",generation,ctx->nameOf(ni),proposals[i].size());
            for (auto &candidate : proposals[i]) {
                auto identity = candidate.wires;
                std::sort(identity.begin(), identity.end());
                if (std::find(seen[i].begin(), seen[i].end(), identity) != seen[i].end()) continue;
                seen[i].push_back(std::move(identity));
                float gain = -std::numeric_limits<float>::infinity();
                critical_route_trial(ctx, ni, candidate, [&]() {
                    auto trial = base;
                    size_t u = 0;
                    for (const auto &usr : ni->users) {
                        DelayQuad delay;
                        std::vector<AnalogueHop> hops;
                        if (!analogue_arc_delay(ni, usr, delay, &hops)) return false;
                        trial.at(u++).after = float(delay.maxDelay());
                        for (const auto &hop : hops) {
                            auto &value = learned[hop.pip];
                            value = std::max(value, std::max(hop.rise, hop.fall));
                        }
                    }
                    gain = critical_route_local_gain(trial);
                    return false; // Ranking always restores the original physical tree.
                });
                log_info("%s route proposal: net %s variant %d native worst-sink gain %.3f ps.\n",
                         final_after_lut?"Final critical":"Critical",ctx->nameOf(ni),candidate.variant,gain);
                if (gain >= 20) ranked[i].emplace_back(gain, std::move(candidate));
            }
        }
    }
    // No learned costs escape the search, including for uncovered fallback arcs.
    restore_table();
    for (const auto &state : helper_measurement_states) {
        std::string reason;
        if (!critical_final_route_buffer_unchanged(ctx,*state,&reason))
            log_error("Final route helper measurement rollback failed: %s.\n",reason.c_str());
    }
    std::vector<std::vector<GpuRouteTree>> candidates(sinks.size());
    for (size_t i = 0; i < sinks.size(); ++i) {
        std::stable_sort(ranked[i].begin(), ranked[i].end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (auto &entry : ranked[i]) candidates[i].push_back(std::move(entry.second));
    }
    int tried = 0, kept = 0;
    // Round-robin across failing nets: one net cannot consume the whole budget.
    for (size_t rank = 0; rank < 8 && tried < budget; ++rank)
        for (size_t i = 0; i < sinks.size() && tried < budget; ++i) {
            if (rank >= candidates[i].size()) continue;
            auto *ni = sinks[i].net;
            std::string rejection = "route topology or binding rejected";
            if (final_after_lut && !covered(ni,*before,*reference,rejection)) {
                excluded(ni,rejection.c_str()); continue;
            }
            std::shared_ptr<const CriticalFinalRouteBufferState> helper_state;
            if (has_buffer_users(ni)) {
                helper_state=critical_final_route_buffer_snapshot(ctx,ni,&rejection);
                if (!helper_state) { excluded(ni,rejection.c_str()); continue; }
            }
            ++tried;
            std::unique_ptr<TimingAnalyser> after,next_reference;
            json11::Json next_inventory;
            bool accepted = critical_route_trial(ctx, ni, candidates[i][rank], [&]() {
                if (helper_state && !critical_final_route_buffer_unchanged(ctx,*helper_state,&rejection)) return false;
                after = analyse();
                if (after->have_loops) { rejection="timing loop"; return false; }
                if (final_after_lut) {
                    next_reference=analyse(false);
                    if (next_reference->have_loops) { rejection="reference timing loop"; return false; }
                    if (!covered(ni,*after,*next_reference,rejection) ||
                        !private_pair_clock_safe(before->get_timing_result(),after->get_timing_result(),&rejection) ||
                        !private_pair_clock_safe(reference->get_timing_result(),next_reference->get_timing_result(),&rejection))
                        return false;
                }
                const auto &a = before->get_timing_result(), &b = after->get_timing_result();
                auto nonregressing = [&](const auto &old, const auto &now, const char *label) {
                    if (old.size() != now.size()) { rejection=std::string(label)+" clock coverage changed"; return false; }
                    for (const auto &row : old) {
                        auto n = now.find(row.first);
                        if (n == now.end() || !std::isfinite(double(n->second)) || n->second < row.second) {
                            rejection=std::string(label)+" clock regression: "+row.first.str(ctx);
                            return false;
                        }
                    }
                    return true;
                };
                if (!nonregressing(a.clock_setup_slack,b.clock_setup_slack,"setup") ||
                    !nonregressing(a.clock_hold_slack,b.clock_hold_slack,"hold") || a.clock_fmax.size()!=b.clock_fmax.size()) return false;
                for (const auto &clock : a.clock_fmax) {
                    auto found=b.clock_fmax.find(clock.first);
                    if (found==b.clock_fmax.end() || !std::isfinite(found->second.achieved) || found->second.achieved<clock.second.achieved) { rejection="clock Fmax regression: "+clock.first.str(ctx); return false; }
                }
                next_inventory = snapshot(*after,next_reference.get());
                return critical_route_snapshot_safe(inventory, next_inventory, &rejection);
            });
            if (helper_state) {
                std::string reason;
                if (!critical_final_route_buffer_unchanged(ctx,*helper_state,&reason))
                    log_error("Final route helper trial restoration failed: %s.\n",reason.c_str());
            }
            if (final_after_lut && accepted) rejection="complete native inventory and both-frame clock guards passed";
            log_info("%s route trial %d: net %s variant %d accepted=%d.\n",final_after_lut?"Final critical":"Critical",tried,ctx->nameOf(ni),
                     candidates[i][rank].variant,int(accepted));
            log_info("    %s route decision: %s.\n",final_after_lut?"Final critical":"Critical",rejection.c_str());
            if (accepted) {
                before=std::move(after); inventory=std::move(next_inventory); ++kept;
                if (final_after_lut) reference=std::move(next_reference);
                // Remaining proposals were generated from the old tree. Do not
                // undo this accepted change by trying an old variant of it.
                candidates[i].clear();
            }
        }
    log_info("%s route repair: tried=%d/%d kept=%d; unchanged unsupported coverage is not qualification.\n",
             final_after_lut?"Final critical":"Critical",tried,budget,kept);
}

NEXTPNR_NAMESPACE_END
