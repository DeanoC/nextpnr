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
// by their analogue delays. The routing of the best round is kept.

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

bool Arch::analogue_candidate_pass(TimingAnalyser &tmg, float target)
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
            if (sl < target)
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
    const size_t failing = items.size();
    if (items.size() > size_t(max_arcs))
        items.resize(max_arcs);

    const float saved_prior = pip_delay_prior;
    pip_delay_prior = search_prior;
    GpuRouterCfg cfg(ctx);
    GpuCandidateRouter cr(ctx, cfg);
    pip_delay_prior = saved_prior;

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
        std::vector<SavedRouting::Entry> best_entries;
        for (size_t c = 0; c < cands.size(); c++) {
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
            std::vector<uint8_t> ok(users.size(), 1);
            auto simulate = [&](size_t first, size_t step) {
                for (size_t i = first; i < users.size(); i += step) {
                    if (!counted[i])
                        continue;
                    DelayQuad d;
                    ok[i] = analogue_arc_delay(ni, *users[i], d, nullptr);
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
                analogue_arc_cache[users[i]] = AnalogueArc{best_delay[i], true};
        cr.resync(ni);
        improved++;
        total_gain += best_score - score_old;
        if (ctx->verbose)
            log_info("      %s: candidate %d (variant %d) raises the net's worst sink slack %.3f -> %.3f ns\n",
                     ctx->nameOf(ni), best, cands[best].variant, score_old / 1000.0f, best_score / 1000.0f);
    }
    log_info("    analogue candidate selection: %d of %zu failing sinks tried with %d candidates, %d nets "
             "re-routed (%d candidates refused by the Arch), worst-sink slack gained %.3f ns in total, %.2fs "
             "(%.2fs searching, %.2fs simulating)\n",
             tried, failing, generated, improved, rejected, total_gain / 1000.0f,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), search_secs, eval_secs);
    return improved > 0;
}

bool Arch::analogue_repair()
{
    Context *ctx = getCtx();
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
        if (current >= target || reroutes >= rounds)
            break;
        if (current > best.slack) {
            best = save_routing(ctx);
            best.slack = current;
        } else if (current < best.slack - restore_margin) {
            log_info("    %.3f ns below the best routing so far; restoring it (%.3f ns) before the next round\n",
                     (best.slack - current) / 1000.0f, best.slack / 1000.0f);
            restore_routing(ctx, best);
            bitstream_configured = false;
            current_timed = false;
            continue;
        }

        auto t1 = std::chrono::steady_clock::now();
        compute_analogue_arcs(true);
        if (candidates && cand_passes < cand_rounds) {
            cand_passes++;
            bool changed = analogue_candidate_pass(tmg, target);
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
                delay_t sl = tmg.get_setup_slack(CellPortKey(usr));
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
                float sl = tmg.get_setup_slack(CellPortKey(usr));
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
    bitstream_configured = false;
    analogue_cache_valid = false;
    if (best.slack != std::numeric_limits<float>::lowest() && (!current_timed || current < best.slack)) {
        log_info("    restoring the routing with the best analogue slack (%.3f ns)\n", best.slack / 1000.0f);
        restore_routing(ctx, best);
        result = true;
    }
    return result;
}

NEXTPNR_NAMESPACE_END
