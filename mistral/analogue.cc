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
#include "gpurouter.h"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"
#include "analogue_routing.h"
#include "analogue_lut_pins.h"
#include "analogue_lut_moves.h"
#include "analogue_lut_pairs.h"
#include "critical_cohort.h"
#include "critical_cohort_route_cost.h"
#include "routed_placement.h"
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

SavedRouting save_routing(Context *ctx, bool include_globals = false)
{
    SavedRouting saved;
    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        if (ni->is_global && !include_globals)
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

// Keep the parent's routing trunk and every other sink branch. Only the
// exclusive branch of this LUT terminal is released. Shared trunks, every
// other sink and the actual source remain bound.
std::optional<std::vector<SavedRouting::Entry>> lut_input_prefix(Context *ctx, NetInfo *net, PortRef user)
{
    std::vector<WireId> targets;
    for (auto wire : ctx->getNetinfoSinkWires(net, user)) targets.push_back(wire);
    if (targets.size() != 1) return {};
    const auto target = targets.front(), source = ctx->getNetinfoSourceWire(net);
    std::map<WireId, SavedRouting::Entry> tree;
    std::map<WireId, int> children;
    for (const auto &binding : net->wires) {
        tree.emplace(binding.first, SavedRouting::Entry{binding.first, binding.second.pip, STRENGTH_PLACER});
        if (binding.second.pip != PipId()) ++children[ctx->getPipSrcWire(binding.second.pip)];
    }
    std::set<WireId> other_sinks;
    for (const auto &other : net->users) {
        if (other.cell == user.cell && other.port == user.port) continue;
        for (auto wire : ctx->getNetinfoSinkWires(net, other)) other_sinks.insert(wire);
    }
    if (!tree.count(target) || !tree.count(source) || target == source || children[target] || other_sinks.count(target))
        return {};
    WireId cursor = target;
    bool removed = false;
    for (size_t steps = 0; steps < net->wires.size(); ++steps) {
        if (cursor == source || children[cursor] || other_sinks.count(cursor)) break;
        auto found = tree.find(cursor);
        if (found == tree.end() || found->second.pip == PipId()) return {};
        WireId parent = ctx->getPipSrcWire(found->second.pip);
        if (!tree.count(parent)) return {};
        tree.erase(found);
        --children[parent];
        cursor = parent;
        removed = true;
    }
    if (!removed || tree.count(target) || !tree.count(source)) return {};
    for (auto sink : other_sinks) if (!tree.count(sink)) return {};
    std::vector<SavedRouting::Entry> result;
    for (const auto &entry : tree) result.push_back(entry.second);
    return result;
}

// Release all selected LUT terminals together, retaining the source and
// every unselected sink. Shared portions disappear only when no retained
// consumer uses them. Ordinary single-terminal callers remain unchanged.
std::optional<std::vector<SavedRouting::Entry>> lut_inputs_prefix(Context *ctx, NetInfo *net,
                                                                const std::vector<PortRef> &users)
{
    std::set<WireId> targets, other_sinks;
    for (const auto &user : users) {
        std::vector<WireId> wires;
        for (auto wire : ctx->getNetinfoSinkWires(net, user)) wires.push_back(wire);
        if (wires.size() != 1 || !targets.insert(wires.front()).second) return {};
    }
    for (const auto &other : net->users) {
        bool selected = false;
        for (const auto &user : users) selected |= other.cell == user.cell && other.port == user.port;
        if (!selected)
            for (auto wire : ctx->getNetinfoSinkWires(net, other)) other_sinks.insert(wire);
    }
    const auto source = ctx->getNetinfoSourceWire(net);
    std::map<WireId, SavedRouting::Entry> tree;
    std::map<WireId, int> children;
    for (const auto &wire : net->wires) {
        tree.emplace(wire.first, SavedRouting::Entry{wire.first, wire.second.pip, STRENGTH_PLACER});
        if (wire.second.pip != PipId()) ++children[ctx->getPipSrcWire(wire.second.pip)];
    }
    if (targets.empty() || !tree.count(source)) return {};
    for (auto target : targets)
        if (!tree.count(target) || target == source || children[target] || other_sinks.count(target)) return {};
    for (auto cursor : targets) {
        for (size_t step = 0; step < net->wires.size(); ++step) {
            if (cursor == source || children[cursor] || other_sinks.count(cursor)) break;
            auto found = tree.find(cursor);
            if (found == tree.end() || found->second.pip == PipId()) return {};
            auto parent = ctx->getPipSrcWire(found->second.pip);
            if (!tree.count(parent)) return {};
            tree.erase(found);
            --children[parent];
            cursor = parent;
        }
    }
    for (auto target : targets) if (tree.count(target)) return {};
    for (auto sink : other_sinks) if (!tree.count(sink)) return {};
    std::vector<SavedRouting::Entry> result;
    for (const auto &wire : tree) result.push_back(wire.second);
    return result;
}

struct LutOutputForest
{
    std::set<WireId> roots;
    std::map<WireId, PipId> edges;
};

// Rebuild routing from the actual moved source to the original consumers.
// Retain any existing branches below an internal consumer; never retain the
// old driver. The native search validates the complete resulting tree.
std::optional<LutOutputForest> lut_output_forest(Context *ctx, NetInfo *net)
{
    const auto source = ctx->getNetinfoSourceWire(net);
    std::map<WireId, std::vector<WireId>> children;
    std::set<WireId> sinks;
    for (const auto &user : net->users)
        for (auto wire : ctx->getNetinfoSinkWires(net, user)) sinks.insert(wire);
    if (!net->wires.count(source) || sinks.empty() || sinks.count(source)) return {};
    for (const auto &wire : net->wires)
        if (wire.second.pip != PipId())
            children[ctx->getPipSrcWire(wire.second.pip)].push_back(wire.first);
    LutOutputForest result;
    std::vector<WireId> pending{source};
    std::set<WireId> seen;
    for (size_t i = 0; i < pending.size(); ++i) {
        auto wire = pending[i];
        if (!seen.insert(wire).second) return {};
        if (wire != source && sinks.count(wire)) {
            result.roots.insert(wire);
            continue;
        }
        for (auto child : children[wire]) pending.push_back(child);
    }
    if (result.roots.empty()) return {};
    for (const auto &wire : net->wires) {
        if (result.roots.count(wire.first)) continue;
        auto cursor = wire.first;
        std::set<WireId> path;
        while (cursor != source && !result.roots.count(cursor)) {
            auto parent = net->wires.find(cursor);
            if (parent == net->wires.end() || parent->second.pip == PipId() || !path.insert(cursor).second)
                return {};
            cursor = ctx->getPipSrcWire(parent->second.pip);
        }
        if (result.roots.count(cursor)) result.edges.emplace(wire.first, wire.second.pip);
    }
    for (auto sink : sinks) if (!result.roots.count(sink) && !result.edges.count(sink)) return {};
    return result;
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

bool Arch::analogue_candidate_pass(TimingAnalyser &tmg, float limit, bool improve, const GpuRouterCfg &repair_cfg,
                                   int candidate_count, bool preserve_delays, std::set<IdString> *visited_nets)
{
    Context *ctx = getCtx();
    NPNR_ASSERT(bitstream_configured && analogue_cache_valid);
    // Candidates per sink, sinks per pass, minimum worst-sink slack gain to
    // accept a change (ps), and the fanout above which a net is left alone
    // (every sink of the net is simulated per candidate)
    const int count = candidate_count > 0 ? candidate_count : ctx->setting<int>("gpurouter/analogueCandidates", 4);
    const int max_arcs = ctx->setting<int>("gpurouter/analogueCandidateArcs", 200);
    // Small per-net gains can add up across a path. The local stage still
    // requires the complete 20 ps WNS/TNS gain in its endpoint guard.
    const float min_gain = preserve_delays ? 0.0f : ctx->setting<float>("gpurouter/analogueCandidateGain", 20.0f);
    const int max_fanout = preserve_delays ? 128 : ctx->setting<int>("gpurouter/analogueCandidateFanout", 64);
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
        if (visited_nets && visited_nets->count(net.first)) continue;
        if (int(ni->users.entries()) > max_fanout)
            continue;
        if (!analogue_net_is_movable(ni))
            continue;
        if (preserve_delays) {
            int clocks = 0;
            auto driver = getPortTimingClass(ni->driver.cell, ni->driver.port, clocks);
            if (ni->clkconstr || (driver != TMG_COMB_OUTPUT && driver != TMG_REGISTER_OUTPUT))
                continue;
            bool data_only = true;
            for (const auto &user : ni->users) {
                auto sink = getPortTimingClass(user.cell, user.port, clocks);
                data_only &= sink == TMG_COMB_INPUT || sink == TMG_REGISTER_INPUT || sink == TMG_ENDPOINT;
            }
            if (!data_only)
                continue;
        }
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
    if (visited_nets) {
        items.resize(1);
        visited_nets->insert(items.front().ni->name);
    } else if (items.size() > size_t(max_arcs))
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
        // A local refinement cannot trade another sink's known delay or an
        // unobserved sink for an improvement of the selected critical sink.
        if (preserve_delays && std::find(counted.begin(), counted.end(), false) != counted.end())
            continue;
        generated += int(cands.size());
        tried++;
        const auto old = snapshot_net(ni);
        std::map<WireId, PipId> cell_connections;
        if (visited_nets) {
            std::vector<std::pair<WireId, PipId>> tree;
            for (const auto &wire : old) tree.emplace_back(wire.wire, wire.pip);
            cell_connections = analogue_cell_connections(ctx, tree);
        }
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
            if (visited_nets) {
                std::vector<std::pair<WireId, PipId>> tree;
                for (const auto &wire : entries) tree.emplace_back(wire.wire, wire.pip);
                if (analogue_cell_connections(ctx, tree) != cell_connections) {
                    ++rejected;
                    continue;
                }
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
                if (preserve_delays && d_new[i].maxDelay() > d_old[i])
                    ok_all = false;
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
    const int polish_candidates = settings.count(id("gpurouter/analoguePolishCandidates"))
                                          ? ctx->setting<int>("gpurouter/analoguePolishCandidates")
                                          : 0;
    if (polish_candidates < 0 || polish_candidates > 32)
        log_error("analoguePolishCandidates must be between 0 and 32 (0 disables).\n");
    const int lut_pin_trials = settings.count(id("gpurouter/analogueLutPinTrials"))
                                      ? ctx->setting<int>("gpurouter/analogueLutPinTrials")
                                      : 0;
    if (lut_pin_trials < 0 || lut_pin_trials > 16)
        log_error("analogueLutPinTrials must be between 0 and 16 (0 disables).\n");
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
            if (!analogue_net_is_movable(ni))
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
    // A separate, default-off local experiment starts from exactly the route
    // ordinary repair returns. Keep the signoff table fixed and compare every
    // native endpoint row, not only the worst clock. Incomplete external IO
    // remains incomplete in the normal final inventory and is not qualified
    // by this proposal guard.
    if (result && (polish_candidates > 0 || lut_pin_trials > 0 || post_route_lut_moves > 0 ||
                   post_route_private_lut_pins > 0 || post_route_net_trials > 0 ||
                   post_route_final_lut_pins > 0 || post_route_sole_lut_pins > 0 ||
                   post_route_lut_pairs > 0 || post_route_lut_input_nets > 0 ||
                   post_route_static_ff_lut_pins > 0) && !expired()) {
        const bool calibrated = pip_delay_calibrated;
        const auto observed = pip_delay_observed;
        const auto type_calibration = pip_type_calibration;
        auto signoff_table = [&]() {
            pip_delay_calibrated = calibrated;
            pip_delay_observed = observed;
            pip_type_calibration = type_calibration;
        };
        auto endpoint_rows = [&](TimingAnalyser &timing) {
            CriticalCohortEndpointRows rows;
            if (!timing.observations_complete())
                return rows;
            for (const auto &cell : cells)
                for (const auto &port : cell.second->ports) {
                    int clocks = 0;
                    auto klass = getPortTimingClass(cell.second.get(), port.first, clocks);
                    if (klass != TMG_REGISTER_INPUT && klass != TMG_ENDPOINT)
                        continue;
                    std::vector<EndpointClockPairTiming> values;
                    CellPortKey key(cell.first, port.first);
                    if (timing.get_endpoint_clock_pair_timings(key, values) && !values.empty())
                        rows.emplace(key, std::move(values));
                }
            // Reference-free extrema of unrelated clocks must come from an
            // analysis without clock skew, just as in the routed inventory.
            bool unrelated = false;
            for (const auto &entry : rows)
                for (const auto &row : entry.second)
                    unrelated |= !row.setup_timed;
            if (unrelated) {
                TimingAnalyser reference(ctx);
                reference.with_clock_skew = false;
                reference.setup_only = false;
                reference.setup(false, false, true);
                if (!reference.observations_complete())
                    return CriticalCohortEndpointRows();
                for (auto &entry : rows) {
                    std::vector<EndpointClockPairTiming> native;
                    if (!reference.get_endpoint_clock_pair_timings(entry.first, native))
                        return CriticalCohortEndpointRows();
                    for (auto &row : entry.second) {
                        if (row.setup_timed)
                            continue;
                        auto found = std::find_if(native.begin(), native.end(), [&](const auto &value) {
                            return value.launch == row.launch && value.capture == row.capture;
                        });
                        if (found == native.end())
                            return CriticalCohortEndpointRows();
                        row.max_path_delay = found->max_path_delay;
                        row.min_path_delay = found->min_path_delay;
                    }
                }
            }
            return rows;
        };
        std::string summary;
        current = time_routing(summary);
        auto before_rows = endpoint_rows(*tmg);
        AnalogueClockTiming before_clocks(tmg->get_timing_result());
        if (polish_candidates > 0)
            log_info("Analogue local refinement: up to 4 passes, %d candidates per sink, %zu native endpoint groups.\n",
                     polish_candidates, before_rows.size());
        for (int pass = 0; pass < 4 && polish_candidates > 0 && !before_rows.empty() && !expired(); ++pass) {
            SavedRouting kept = save_routing(ctx);
            compute_analogue_arcs(true);
            GpuRouterCfg cfg(ctx);
            cfg.stop_requested = expired;
            bool changed = false;
            try {
                changed = analogue_candidate_pass(*tmg, current + improve_band, true, cfg,
                                                  polish_candidates, true);
            } catch (const GpuRouterCancelled &) {
                restore_routing(ctx, kept);
                signoff_table();
                break;
            } catch (...) {
                restore_routing(ctx, kept);
                signoff_table();
                bitstream_configured = false;
                analogue_cache_valid = false;
                throw;
            }
            signoff_table();
            bitstream_configured = false;
            analogue_cache_valid = false;
            if (!changed)
                break;
            const float slack = time_routing(summary);
            auto after_rows = endpoint_rows(*tmg);
            AnalogueClockTiming after_clocks(tmg->get_timing_result());
            const bool safe = !after_clocks.worse_than(before_clocks) &&
                              critical_cohort_proposal_rows_safe(before_rows, after_rows);
            log_info("Analogue local refinement %d: %.3f -> %.3f ns, %zu endpoint groups, %s.\n",
                     pass + 1, current / 1000.0f, slack / 1000.0f, after_rows.size(),
                     safe ? "proposal guard passed" : "rejected; restoring baseline");
            if (!safe) {
                restore_routing(ctx, kept);
                break;
            }
            current = slack;
            before_rows = std::move(after_rows);
            before_clocks = std::move(after_clocks);
        }
        auto refine_lut_inputs = [&](int pin_trials, bool private_l5, bool sole_l5 = false,
                                     bool whole_input_nets = false, bool static_ff_bypass_l5 = false) {
            if (pin_trials <= 0 || expired()) return;
            // Start from a complete configured parent, including when the
            // preceding routing-only pass restored a rejected candidate.
            signoff_table();
            current = time_routing(summary);
            before_rows = endpoint_rows(*tmg);
            before_clocks = AnalogueClockTiming(tmg->get_timing_result());
            const auto cell_pips = static_ff_bypass_l5 ? analogue_parent_cell_pips(ctx) : pool<PipId>();
            std::map<IdString, decltype(CellInfo::pin_data)> whole_net_pins;
            if (whole_input_nets)
                for (const auto &entry : cells) whole_net_pins.emplace(entry.first, entry.second->pin_data);
            auto sole_policy = [&](const CellInfo *cell) {
                if (static_ff_bypass_l5) return false;
                if (!whole_input_nets || cell->type == id_MISTRAL_ALUT6) return sole_l5;
                const auto where = bel_data(cell->bel).lab_data;
                return getBoundBelCell(labs.at(where.lab).alms.at(where.alm).lut_bels.at(1 - where.idx)) == nullptr;
            };
            auto input_prefix = [&](NetInfo *net, PortRef user) -> std::optional<std::vector<SavedRouting::Entry>> {
                if (!whole_input_nets) return lut_input_prefix(ctx, net, user);
                std::vector<std::pair<WireId, PipId>> tree;
                for (const auto &wire : net->wires) tree.emplace_back(wire.first, wire.second.pip);
                auto prefix = analogue_source_prefix(ctx, ctx->getNetinfoSourceWire(net), tree);
                if (!prefix) return {};
                std::vector<SavedRouting::Entry> entries;
                for (const auto &wire : *prefix) entries.push_back({wire.first, wire.second, STRENGTH_PLACER});
                return entries;
            };
            struct Choice { CellInfo *cell; IdString first, second; float slack; delay_t gain; };
            std::vector<Choice> choices;
            const std::array<IdString, 6> ports{id_A, id_B, id_C, id_D, id_E, id_F};
            for (const auto &entry : cells) {
                auto *cell = entry.second.get();
                if (static_ff_bypass_l5 && cell->type == id_MISTRAL_ALUT6) continue;
                if (whole_input_nets) {
                    if (!cell->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                       id_MISTRAL_ALUT5, id_MISTRAL_ALUT6) || cell->bel == BelId()) continue;
                } else if (private_l5 ? !cell->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                                      id_MISTRAL_ALUT5) : cell->type != id_MISTRAL_ALUT6)
                    continue;
                for (auto first : ports) {
                    if (!cell->getPort(first)) continue;
                    const float slack = tmg->get_setup_slack({cell->name, first});
                    if (!std::isfinite(slack) || slack <= float(std::numeric_limits<delay_t>::lowest()) ||
                        slack >= float(std::numeric_limits<delay_t>::max()) ||
                        slack > current + improve_band)
                        continue;
                    for (auto second : ports) {
                        if (!analogue_lut_pin_swap_allowed(ctx, cell, first, second, sole_policy(cell),
                                                          static_ff_bypass_l5))
                            continue;
                        auto *a = cell->getPort(first), *b = cell->getPort(second);
                        // Private L5 trials release one terminal branch;
                        // all other sinks keep their parent routing. Bound
                        // their bookkeeping separately from LUT6 trials.
                        const auto fanout_limit = cell->type == id_MISTRAL_ALUT6 ? 128 : 256;
                        if (!analogue_net_is_movable(a) || !analogue_net_is_movable(b) ||
                            a->clkconstr || b->clkconstr || a->region || b->region ||
                            a->users.entries() > fanout_limit || b->users.entries() > fanout_limit)
                            continue;
                        int clocks = 0;
                        auto data_driver = [&](NetInfo *net) {
                            auto kind = getPortTimingClass(net->driver.cell, net->driver.port, clocks);
                            if (kind != TMG_COMB_OUTPUT && kind != TMG_REGISTER_OUTPUT) return false;
                            for (const auto &user : net->users) {
                                auto sink = getPortTimingClass(user.cell, user.port, clocks);
                                if (sink != TMG_COMB_INPUT && sink != TMG_REGISTER_INPUT && sink != TMG_ENDPOINT)
                                    return false;
                            }
                            return true;
                        };
                        if (!data_driver(a) || !data_driver(b))
                            continue;
                        DelayQuad old_delay, fast_delay;
                        if (!getCellDelay(cell, first, id_Q, old_delay) || !getCellDelay(cell, second, id_Q, fast_delay))
                            continue;
                        delay_t gain = old_delay.maxDelay() - fast_delay.maxDelay();
                        if (gain >= 20)
                            choices.push_back({cell, first, second, slack, gain});
                    }
                }
            }
            std::stable_sort(choices.begin(), choices.end(), [&](const Choice &a, const Choice &b) {
                if (a.slack != b.slack) return a.slack < b.slack;
                if (a.gain != b.gain) return a.gain > b.gain;
                if (a.cell != b.cell) return a.cell->name.str(ctx) < b.cell->name.str(ctx);
                if (a.first != b.first) return a.first.str(ctx) < b.first.str(ctx);
                return a.second.str(ctx) < b.second.str(ctx);
            });
            log_info("Analogue LUT input refinement: %zu eligible swaps, at most %d trials.\n",
                     choices.size(), pin_trials);
            if (whole_input_nets) log_info("    whole input nets; every parent cell connection remains fixed.\n");
            else if (sole_l5) log_info("    sole L5 occupants only; no peer LUT or FF may be present.\n");
            else if (private_l5) log_info("    private L5 inputs only; starting after existing routed refinements.\n");
            int trial = 0;
            for (const auto &choice : choices) {
                if (trial >= pin_trials || expired() || before_rows.empty()) break;
                auto *cell = choice.cell;
                if (!analogue_lut_pin_swap_allowed(ctx, cell, choice.first, choice.second, sole_policy(cell),
                                                  static_ff_bypass_l5)) continue;
                DelayQuad now_first, now_second;
                if (!getCellDelay(cell, choice.first, id_Q, now_first) ||
                    !getCellDelay(cell, choice.second, id_Q, now_second) ||
                    now_first.maxDelay() < now_second.maxDelay() + 20) continue;
                auto first_prefix = input_prefix(cell->getPort(choice.first), {cell, choice.first});
                auto second_prefix = input_prefix(cell->getPort(choice.second), {cell, choice.second});
                if (!first_prefix || !second_prefix) continue;
                ++trial;
                if (static_ff_bypass_l5)
                    log_info("Analogue static FF input choice %d: %s %s/%s; %zu parent cell pips permitted.\n",
                             trial, nameOf(cell), choice.first.c_str(ctx), choice.second.c_str(ctx), cell_pips.size());
                const auto pins = cell->pin_data;
                const auto rng = ctx->rngstate;
                const auto kept = save_routing(ctx, true);
                const std::set<IdString> changed{cell->getPort(choice.first)->name, cell->getPort(choice.second)->name};
                const std::map<IdString, std::vector<SavedRouting::Entry>> prefixes{
                    {cell->getPort(choice.first)->name, *first_prefix},
                    {cell->getPort(choice.second)->name, *second_prefix}};
                std::map<IdString, std::map<WireId, PipId>> fixed_cell_connections;
                if (sole_l5 || whole_input_nets) {
                    for (const auto &net : kept.nets) {
                        if (!changed.count(net.first)) continue;
                        std::vector<std::pair<WireId, PipId>> tree;
                        for (const auto &wire : net.second) tree.emplace_back(wire.wire, wire.pip);
                        fixed_cell_connections.emplace(net.first, analogue_cell_connections(ctx, tree));
                    }
                }
                auto rollback = [&]() {
                    cell->pin_data = pins;
                    if (whole_input_nets)
                        for (const auto &entry : whole_net_pins) cells.at(entry.first)->pin_data = entry.second;
                    restore_routing(ctx, kept);
                    ctx->rngstate = rng;
                    signoff_table();
                    bitstream_configured = false;
                    analogue_cache_valid = false;
                };
                try {
                    NPNR_ASSERT(analogue_swap_lut_pins(ctx, cell, choice.first, choice.second, sole_policy(cell),
                                                     static_ff_bypass_l5));
                    decltype(cell->pin_data) expected_pins;
                    if (whole_input_nets) expected_pins = cell->pin_data;
                    // Rebind verified parent prefixes without the old terminal
                    // suffixes. PLACER reserves the prefix for its owner while
                    // allowing the new branch to attach to it. Every other
                    // net remains an exact fixed obstacle.
                    for (const auto &net : kept.nets) {
                        auto *ni = nets.at(net.first).get();
                        if (changed.count(net.first)) ctx->ripupNet(net.first);
                        else for (auto &wire : ni->wires)
                            wire.second.strength = std::max(wire.second.strength, STRENGTH_LOCKED);
                    }
                    bool prefixes_bound = true;
                    size_t retained_wires = 0;
                    for (const auto &prefix : prefixes) {
                        prefixes_bound &= bind_net(ctx, nets.at(prefix.first).get(), prefix.second);
                        retained_wires += prefix.second.size();
                    }
                    if (!prefixes_bound) {
                        rollback();
                        log_info("Analogue LUT input trial %d: parent prefix binding refused.\n", trial);
                        current = time_routing(summary);
                        continue;
                    }
                    log_info("    retaining %zu parent wires across the two LUT input nets.\n", retained_wires);
                    bitstream_configured = false;
                    analogue_cache_valid = false;
                    GpuRouterCfg cfg(ctx);
                    cfg.telemetry_path.clear();
                    cfg.stop_requested = expired;
                    cfg.legality_timing_gate = false; // Full analogue setup/hold remains the final gate.
                    cfg.max_iter = std::min(cfg.max_iter, 64);
                    cfg.arch_retry_limit = std::min(cfg.arch_retry_limit, 2);
                    cfg.repair_rounds = 0;
                    cfg.repair_improve_rounds = 0;
                    cfg.repair_displace = false;
                    const bool routed = try_analogue_repair_reroute(trial, [&]() {
                        std::optional<AnalogueCellPipScope> scope;
                        if (static_ff_bypass_l5) scope.emplace(ctx, cell_pips);
                        return gpurouter(ctx, cfg, false);
                    });
                    if (whole_input_nets) for (const auto &prefix : prefixes) {
                        auto *net = nets.at(prefix.first).get();
                        std::vector<std::pair<WireId, PipId>> seed;
                        for (const auto &wire : prefix.second) seed.emplace_back(wire.wire, wire.pip);
                        auto retained = analogue_retain_source_prefix(ctx, net, ctx->getNetinfoSourceWire(net), seed);
                        if (!retained)
                            log_error("Whole input net route cannot retain its original source prefix; restoring parent.\n");
                        if (*retained) log_info("    retaining %zu unused parent source-egress wires after rerouting.\n", *retained);
                    }
                    bool untouched = true;
                    for (const auto &prefix : prefixes) {
                        auto *ni = nets.at(prefix.first).get();
                        for (const auto &wire : prefix.second) {
                            auto found = ni->wires.find(wire.wire);
                            if (found == ni->wires.end() || found->second.pip != wire.pip) untouched = false;
                        }
                    }
                    for (const auto &net : kept.nets) {
                        auto *ni = nets.at(net.first).get();
                        const bool selected = changed.count(net.first);
                        if (!selected && ni->wires.size() != net.second.size()) untouched = false;
                        dict<WireId, PlaceStrength> strengths;
                        for (const auto &wire : net.second) {
                            strengths[wire.wire] = wire.strength;
                            if (!selected) {
                                auto found = ni->wires.find(wire.wire);
                                if (found == ni->wires.end() || found->second.pip != wire.pip) untouched = false;
                            }
                        }
                        for (auto &wire : ni->wires) {
                            auto found = strengths.find(wire.first);
                            wire.second.strength = found == strengths.end() ? STRENGTH_WEAK : found->second;
                        }
                    }
                    ctx->rngstate = rng;
                    signoff_table();
                    if (!untouched) {
                        log_error("Local LUT input trial changed a protected route; restoring parent.\n");
                    }
                    // Keeping every parent prefix does not prevent the router
                    // from adding another LUT egress. Such a branch changes
                    // output selectors even when the neighboring FF is unused.
                    bool cell_connections_unchanged = true;
                    for (const auto &fixed : fixed_cell_connections) {
                        std::vector<std::pair<WireId, PipId>> tree;
                        for (const auto &wire : nets.at(fixed.first)->wires)
                            tree.emplace_back(wire.first, wire.second.pip);
                        cell_connections_unchanged &= analogue_cell_connections(ctx, tree) == fixed.second;
                    }
                    if (!cell_connections_unchanged) {
                        log_info("Analogue LUT input trial %d: cell connection changed; rejected.\n", trial);
                        rollback();
                        if (!expired()) current = time_routing(summary);
                        continue;
                    }
                    bool safe = false;
                    float slack = current;
                    CriticalCohortEndpointRows after_rows;
                    AnalogueClockTiming after_clocks;
                    if (routed) {
                        slack = time_routing(summary);
                        after_rows = endpoint_rows(*tmg);
                        after_clocks = AnalogueClockTiming(tmg->get_timing_result());
                        safe = !after_clocks.worse_than(before_clocks) &&
                               critical_cohort_proposal_rows_safe(before_rows, after_rows);
                    }
                    if (whole_input_nets) for (const auto &entry : whole_net_pins) {
                        const auto &expected = entry.first == cell->name ? expected_pins : entry.second;
                        const auto &actual = cells.at(entry.first)->pin_data;
                        if (actual.size() != expected.size())
                            log_error("Whole input net trial changed an undeclared pin map; restoring parent.\n");
                        for (const auto &pin : expected) {
                            auto found = actual.find(pin.first);
                            if (found == actual.end() || found->second.state != pin.second.state ||
                                found->second.bel_pins != pin.second.bel_pins)
                                log_error("Whole input net trial changed an undeclared pin map; restoring parent.\n");
                        }
                    }
                    log_info("Analogue LUT input trial %d: %s %s/%s, %.3f -> %.3f ns, %s.\n", trial,
                             nameOf(cell), choice.first.c_str(ctx), choice.second.c_str(ctx),
                             current / 1000.0f, slack / 1000.0f,
                             safe ? "proposal guard passed" : routed ? "rejected" : "routing failed");
                    if (safe) {
                        if (whole_input_nets) whole_net_pins[cell->name] = cell->pin_data;
                        current = slack;
                        before_rows = std::move(after_rows);
                        before_clocks = std::move(after_clocks);
                    } else {
                        rollback();
                        if (!expired()) current = time_routing(summary);
                    }
                } catch (const GpuRouterCancelled &) {
                    rollback();
                    break;
                } catch (...) {
                    rollback();
                    throw;
                }
            }
        };
        refine_lut_inputs(lut_pin_trials, false);
        if (post_route_lut_moves > 0 && !expired()) {
            GpuRouterCfg cfg(ctx);
            cfg.telemetry_path.clear();
            cfg.stop_requested = expired;
            cfg.legality_timing_gate = false; // The complete final analogue analysis is the gate.
            cfg.max_iter = std::min(cfg.max_iter, 64);
            cfg.arch_retry_limit = std::min(cfg.arch_retry_limit, 2);
            cfg.repair_rounds = 0;
            cfg.repair_improve_rounds = 0;
            cfg.repair_displace = false;
            signoff_table();
            current = time_routing(summary);
            before_rows = endpoint_rows(*tmg);
            before_clocks = AnalogueClockTiming(tmg->get_timing_result());
            const auto initial = routed_placement_snapshot(ctx);
            json11::Json::array moves;
            auto data_net = [&](NetInfo *net) {
                if (!net || !analogue_net_is_movable(net) || net->clkconstr || net->region ||
                    net->users.empty() || net->users.entries() > 128) return false;
                int clocks = 0;
                auto kind = getPortTimingClass(net->driver.cell, net->driver.port, clocks);
                if (kind != TMG_COMB_OUTPUT && kind != TMG_REGISTER_OUTPUT) return false;
                for (const auto &user : net->users) {
                    if (!user.cell || user.cell->bel == BelId()) return false;
                    auto sink = getPortTimingClass(user.cell, user.port, clocks);
                    if (sink != TMG_COMB_INPUT && sink != TMG_REGISTER_INPUT && sink != TMG_ENDPOINT) return false;
                }
                return true;
            };
            struct MoveSite { BelId bel; int64_t cost; };
            struct MoveGroup { CellInfo *cell; BelId home; float slack; std::vector<MoveSite> sites; };
            std::set<CellInfo *> moved;
            auto collect_groups = [&]() {
                std::vector<MoveGroup> groups;
                for (const auto &entry : cells) {
                    auto *cell = entry.second.get();
                    if (moved.count(cell) || !cell->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                       id_MISTRAL_ALUT5, id_MISTRAL_ALUT6) || cell->bel == BelId()) continue;
                    float slack = std::numeric_limits<float>::max();
                    bool eligible = data_net(cell->getPort(id_Q));
                    std::set<NetInfo *> distinct{cell->getPort(id_Q)};
                    for (const auto &port : cell->ports) if (port.second.type == PORT_IN) {
                        eligible &= data_net(port.second.net) && distinct.insert(port.second.net).second;
                        slack = std::min(slack, tmg->get_setup_slack({cell->name, port.first}));
                    }
                    if (!eligible || !std::isfinite(slack) || slack <= float(std::numeric_limits<delay_t>::lowest()) ||
                        slack > current + improve_band) continue;
                    MoveGroup group{cell, cell->bel, slack, {}};
                    auto cost = [&](BelId target) {
                        int64_t score = 0;
                        for (const auto &port : cell->ports) if (port.second.type == PORT_IN) {
                            auto *net = port.second.net;
                            const auto &src = getBelPinsForCellPin(net->driver.cell, net->driver.port);
                            const auto &dst = getBelPinsForCellPin(cell, port.first);
                            if (src.size() != 1 || dst.size() != 1) return std::numeric_limits<int64_t>::max();
                            int weight = tmg->get_setup_slack({cell->name, port.first}) <= current + improve_band ? 8 : 1;
                            score += int64_t(weight) * predictDelay(net->driver.cell->bel, src.front(), target, dst.front());
                        }
                        auto *out = cell->getPort(id_Q);
                        for (const auto &user : out->users) {
                            const auto &dst = getBelPinsForCellPin(user.cell, user.port);
                            if (dst.size() != 1) return std::numeric_limits<int64_t>::max();
                            int weight = tmg->get_setup_slack(CellPortKey(user)) <= current + improve_band ? 8 : 1;
                            score += int64_t(weight) * predictDelay(target, id_COMBOUT, user.cell->bel, dst.front());
                        }
                        return score;
                    };
                    const auto home = getBelLocation(cell->bel);
                    const auto old_cost = cost(cell->bel);
                    for (int dx = -8; dx <= 8; ++dx) for (int dy = -8; dy <= 8; ++dy) {
                        if (home.x + dx < 0 || home.y + dy < 0 ||
                            home.x + dx >= getGridDimX() || home.y + dy >= getGridDimY()) continue;
                        // Physical input routes differ between ALMs in one tile,
                        // although the placement predictor gives them equal cost.
                        // Inspect every eligible native site under the same total
                        // trial budget instead of collapsing them to the lowest z.
                        for (auto target : getBelsByTile(home.x + dx, home.y + dy)) {
                            if (!analogue_lut_move_allowed(ctx, cell, target)) continue;
                            const auto score = cost(target);
                            if (score >= old_cost) continue;
                            group.sites.push_back({target, score});
                        }
                    }
                    std::stable_sort(group.sites.begin(), group.sites.end(), [&](const MoveSite &a, const MoveSite &b) {
                        if (a.cost != b.cost) return a.cost < b.cost;
                        auto x = getBelLocation(a.bel), y = getBelLocation(b.bel);
                        return std::tie(x.x, x.y, x.z) < std::tie(y.x, y.y, y.z);
                    });
                    if (!group.sites.empty()) groups.push_back(std::move(group));
                }
                std::stable_sort(groups.begin(), groups.end(), [&](const MoveGroup &a, const MoveGroup &b) {
                    return a.slack != b.slack ? a.slack < b.slack : a.cell->name.str(ctx) < b.cell->name.str(ctx);
                });
                return groups;
            };
            std::unique_ptr<CriticalCohortRouteCosts> costs;
            int trial = 0, attempted = 0;
            // A kept move frees an ALM and changes native LAB input usage.
            // Re-screen its actual parent rather than reuse stale vacancies
            // or slacks. Total work and one move per cell stay bounded.
            for (int pass = 0; pass < 4 && trial < post_route_lut_moves && attempted < 128 && !expired(); ++pass) {
                const auto groups = collect_groups();
                log_info("Analogue LUT relocation pass %d: %zu eligible cells, at most %d total trials.\n",
                         pass + 1, groups.size(), post_route_lut_moves);
                bool progress = false;
                for (size_t option = 0; option < 128 && trial < post_route_lut_moves && attempted < 128 && !expired() && !progress; ++option)
                    for (const auto &group : groups) {
                        if (progress || trial >= post_route_lut_moves || attempted >= 128 || expired() || before_rows.empty()) break;
                        auto *cell = group.cell;
                        if (moved.count(cell) || option >= group.sites.size() || cell->bel != group.home) continue;
                        const auto target = group.sites[option].bel;
                        if (!analogue_lut_move_allowed(ctx, cell, target)) continue;
                        auto *output = cell->getPort(id_Q);
                        auto forest = lut_output_forest(ctx, output);
                        if (!forest) continue;
                        std::map<IdString, std::vector<SavedRouting::Entry>> prefixes;
                        bool complete = true;
                        for (const auto &port : cell->ports) if (port.second.type == PORT_IN) {
                            auto prefix = lut_input_prefix(ctx, port.second.net, {cell, port.first});
                            if (!prefix) { complete = false; break; }
                            prefixes.emplace(port.second.net->name, std::move(*prefix));
                        }
                        if (!complete) continue;
                        if (!costs) {
                            const auto parent = routed_placement_snapshot(ctx);
                            costs = std::make_unique<CriticalCohortRouteCosts>(ctx,
                                CriticalCohortRouteCosts::snapshot(ctx, parent), parent);
                        }
                        ++attempted;
                        const auto pins = cell->pin_data;
                        const auto labs_before = labs;
                        const auto rng = ctx->rngstate;
                        const auto kept = save_routing(ctx, true);
                        const auto home = cell->bel;
                        const auto strength = cell->belStrength;
                        auto rollback = [&]() {
                            if (cell->bel != home) {
                                if (cell->bel != BelId()) unbindBel(cell->bel);
                                bindBel(home, cell, strength);
                            }
                            labs = labs_before;
                            cell->pin_data = pins;
                            restore_routing(ctx, kept);
                            ctx->rngstate = rng;
                            signoff_table();
                            bitstream_configured = false;
                            analogue_cache_valid = false;
                        };
                        try {
                            if (!apply_critical_cohort_placement(ctx, {{cell, target}})) {
                                rollback();
                                continue;
                            }
                            ++trial;
                            const auto from = bel_data(home).lab_data, to = bel_data(target).lab_data;
                            assign_alm_lut_inputs(from.lab, from.alm);
                            assign_alm_lut_inputs(to.lab, to.alm);
                            cell->pin_data = pins; // Same half, no peer or FF shares these physical pins.
                            std::set<IdString> changed{output->name};
                            for (const auto &prefix : prefixes) changed.insert(prefix.first);
                            for (const auto &net : kept.nets) {
                                auto *ni = nets.at(net.first).get();
                                if (changed.count(net.first)) ctx->ripupNet(net.first);
                                else for (auto &wire : ni->wires)
                                    wire.second.strength = std::max(wire.second.strength, STRENGTH_LOCKED);
                            }
                            bool bound = true;
                            for (const auto &prefix : prefixes)
                                bound &= bind_net(ctx, nets.at(prefix.first).get(), prefix.second);
                            if (!bound) {
                                rollback();
                                log_info("Analogue LUT relocation trial %d: input prefix binding refused.\n", trial);
                                continue;
                            }
                            std::map<WireId, IdString> drivers;
                            for (const auto &net : nets) if (net.second->driver.cell) {
                                auto source = ctx->getNetinfoSourceWire(net.second.get());
                                if (source != WireId()) drivers.emplace(source, net.first);
                            }
                            int nodes = CriticalCohortRouteCosts::node_limit;
                            const auto tree = costs->reconnect_source(ctx, output, ctx->getNetinfoSourceWire(output),
                                forest->roots, forest->edges, changed, drivers, nodes);
                            std::vector<SavedRouting::Entry> output_tree;
                            for (const auto &wire : tree) output_tree.push_back({wire.first, wire.second, STRENGTH_PLACER});
                            bound = !tree.empty() && bind_net(ctx, output, output_tree);
                            if (!bound) {
                                rollback();
                                log_info("Analogue LUT relocation trial %d: source forest could not reconnect (%d nodes).\n",
                                         trial, CriticalCohortRouteCosts::node_limit - nodes);
                                continue;
                            }
                            bitstream_configured = false;
                            analogue_cache_valid = false;
                            const bool routed = try_analogue_repair_reroute(trial, [&]() { return gpurouter(ctx, cfg, false); });
                            bool untouched = true;
                            auto retains = [&](NetInfo *net, const std::vector<SavedRouting::Entry> &entries) {
                                for (const auto &wire : entries) {
                                    auto found = net->wires.find(wire.wire);
                                    if (found == net->wires.end() || found->second.pip != wire.pip) untouched = false;
                                }
                            };
                            retains(output, output_tree);
                            for (const auto &prefix : prefixes) retains(nets.at(prefix.first).get(), prefix.second);
                            for (const auto &net : kept.nets) {
                                auto *ni = nets.at(net.first).get();
                                if (!changed.count(net.first)) {
                                    if (ni->wires.size() != net.second.size()) untouched = false;
                                    retains(ni, net.second);
                                }
                                dict<WireId, PlaceStrength> strengths;
                                for (const auto &wire : net.second) strengths[wire.wire] = wire.strength;
                                for (auto &wire : ni->wires) {
                                    auto old = strengths.find(wire.first);
                                    wire.second.strength = old == strengths.end() ? STRENGTH_WEAK : old->second;
                                }
                            }
                            ctx->rngstate = rng;
                            signoff_table();
                            if (!untouched) log_error("Local LUT relocation changed a protected route; restoring parent.\n");
                            bool safe = false;
                            float slack = current;
                            CriticalCohortEndpointRows after_rows;
                            AnalogueClockTiming after_clocks;
                            if (routed) {
                                slack = time_routing(summary);
                                after_rows = endpoint_rows(*tmg);
                                after_clocks = AnalogueClockTiming(tmg->get_timing_result());
                                safe = !after_clocks.worse_than(before_clocks) &&
                                       critical_cohort_proposal_rows_safe(before_rows, after_rows);
                            }
                            log_info("Analogue LUT relocation trial %d: %s %s -> %s, %.3f -> %.3f ns, %s.\n",
                                     trial, nameOf(cell), getBelName(home).str(ctx).c_str(), getBelName(target).str(ctx).c_str(),
                                     current / 1000.0f, slack / 1000.0f,
                                     safe ? "proposal guard passed" : routed ? "rejected" : "routing failed");
                            if (safe) {
                                moves.push_back(json11::Json::object{{"cell", cell->name.str(ctx)},
                                    {"from_bel", getBelName(home).str(ctx)}, {"to_bel", getBelName(target).str(ctx)},
                                    {"strength", int(strength)}});
                                moved.insert(cell);
                                progress = true;
                                current = slack;
                                before_rows = std::move(after_rows);
                                before_clocks = std::move(after_clocks);
                                costs.reset();
                                ctx->check();
                            } else {
                                rollback();
                                if (!expired()) current = time_routing(summary);
                            }
                        } catch (const GpuRouterCancelled &) {
                            rollback();
                            break;
                        } catch (...) {
                            rollback();
                            throw;
                        }
                    }
                if (!progress) break;
            }
            log_info("Analogue LUT relocation: %d/128 placement attempts, %d/%d legal route trials, %zu retained.\n",
                     attempted, trial, post_route_lut_moves, moves.size());
            const json11::Json receipt = json11::Json::object{{"version", 1}, {"parent", initial},
                {"target", routed_placement_snapshot(ctx)}, {"moves", moves}};
            check_post_route_lut_moves(receipt, routed_placement_snapshot(ctx));
            post_route_lut_move_receipt = receipt.dump();
        }
        // Independent opt-in: its parent already contains every accepted
        // legacy pin refinement and LUT relocation. Each complete row and
        // every unrelated binding must retain that measured parent.
        refine_lut_inputs(post_route_private_lut_pins, true);
        if (post_route_net_trials > 0 && !expired()) {
            signoff_table();
            current = time_routing(summary);
            before_rows = endpoint_rows(*tmg);
            before_clocks = AnalogueClockTiming(tmg->get_timing_result());
            const auto initial = routed_placement_snapshot(ctx);
            std::set<IdString> visited;
            GpuRouterCfg cfg(ctx);
            cfg.telemetry_path.clear();
            cfg.stop_requested = expired;
            int accepted = 0;
            bool exhausted = false, cancelled = false;
            log_info("Analogue individual net refinement: up to %d trials, 16 candidates per net.\n",
                     post_route_net_trials);
            for (int trial = 1; trial <= post_route_net_trials && !expired() && !before_rows.empty(); ++trial) {
                const auto previous = visited;
                const auto kept = save_routing(ctx, true);
                const auto rng = ctx->rngstate;
                auto rollback = [&]() {
                    restore_routing(ctx, kept);
                    ctx->rngstate = rng;
                    signoff_table();
                    bitstream_configured = false;
                    analogue_cache_valid = false;
                };
                try {
                    compute_analogue_arcs(true);
                    const bool changed = analogue_candidate_pass(*tmg, current + improve_band, true, cfg,
                                                                  16, true, &visited);
                    ctx->rngstate = rng;
                    signoff_table();
                    bitstream_configured = false;
                    analogue_cache_valid = false;
                    if (visited == previous) {
                        exhausted = true;
                        break;
                    }
                    NPNR_ASSERT(visited.size() == previous.size() + 1);
                    IdString selected;
                    for (auto net : visited) if (!previous.count(net)) selected = net;
                    NPNR_ASSERT(selected != IdString());
                    for (const auto &net : kept.nets) {
                        if (net.first == selected) continue;
                        auto *ni = nets.at(net.first).get();
                        bool exact = ni->wires.size() == net.second.size();
                        for (const auto &wire : net.second) {
                            auto found = ni->wires.find(wire.wire);
                            exact &= found != ni->wires.end() && found->second.pip == wire.pip &&
                                     found->second.strength == wire.strength;
                        }
                        if (!exact) log_error("Individual net trial changed a protected route; restoring parent.\n");
                    }
                    float slack = current;
                    CriticalCohortEndpointRows after_rows;
                    AnalogueClockTiming after_clocks;
                    bool safe = false;
                    if (changed) {
                        slack = time_routing(summary);
                        after_rows = endpoint_rows(*tmg);
                        after_clocks = AnalogueClockTiming(tmg->get_timing_result());
                        safe = !after_clocks.worse_than(before_clocks) &&
                               critical_cohort_proposal_rows_safe(before_rows, after_rows);
                    }
                    log_info("Analogue net trial %d: %s, %.3f -> %.3f ns, %s.\n", trial, selected.c_str(ctx),
                             current / 1000.0f, slack / 1000.0f,
                             safe ? "proposal guard passed" : changed ? "rejected" : "no locally improving route");
                    if (safe) {
                        ++accepted;
                        current = slack;
                        before_rows = std::move(after_rows);
                        before_clocks = std::move(after_clocks);
                    } else {
                        rollback();
                        if (!expired()) current = time_routing(summary);
                    }
                } catch (const GpuRouterCancelled &) {
                    rollback();
                    cancelled = true;
                    break;
                } catch (...) {
                    rollback();
                    throw;
                }
            }
            if (routed_placement_snapshot(ctx) != initial)
                log_error("Individual net refinement changed the native design, placement, clocks or RNG.\n");
            log_info("Analogue individual net refinement: %zu nets inspected, %d retained.\n", visited.size(), accepted);
            const char *completion = cancelled ? "runtime budget exhausted" :
                                     before_rows.empty() ? "native timing rows unavailable" :
                                     exhausted ? "all eligible nets inspected" :
                                     visited.size() == size_t(post_route_net_trials) ? "requested trial budget reached" :
                                     "runtime budget exhausted";
            log_info("Analogue individual net completion: %s.\n", completion);
        }
        if (post_route_final_lut_pins > 0) {
            if (expired()) {
                log_info("Analogue final LUT input completion: runtime budget exhausted before stage.\n");
            } else {
                // Re-evaluate the same legal LUT6 swaps against the parent
                // produced by placement, private pins and per-net routing.
                // Earlier phases and every acceptance/footprint guard remain
                // unchanged; this phase has its own explicit trial budget.
                const auto initial = routed_placement_snapshot(ctx);
                log_info("Analogue final LUT input refinement: up to %d LUT6 trials after prior routed refinements.\n",
                         post_route_final_lut_pins);
                refine_lut_inputs(post_route_final_lut_pins, false);
                if (routed_placement_snapshot(ctx) != initial)
                    log_error("Final LUT input refinement changed the native design, placement, clocks or RNG.\n");
                log_info("Analogue final LUT input completion: %s.\n",
                         expired() ? "runtime budget exhausted" : before_rows.empty() ? "native timing rows unavailable" :
                         "bounded pass finished");
            }
        }
        if (post_route_sole_lut_pins > 0) {
            if (expired()) {
                log_info("Analogue sole L5 input completion: runtime budget exhausted before stage.\n");
            } else {
                const auto initial = routed_placement_snapshot(ctx);
                log_info("Analogue sole L5 input refinement: up to %d trials after prior routed refinements.\n",
                         post_route_sole_lut_pins);
                refine_lut_inputs(post_route_sole_lut_pins, true, true);
                if (routed_placement_snapshot(ctx) != initial)
                    log_error("Sole L5 input refinement changed the native design, placement, clocks or RNG.\n");
                log_info("Analogue sole L5 input completion: %s.\n",
                         expired() ? "runtime budget exhausted" : before_rows.empty() ? "native timing rows unavailable" :
                         "bounded pass finished");
            }
        }
        if (post_route_lut_pairs > 0 && !expired()) {
            using J = json11::Json;
            signoff_table();
            current = time_routing(summary);
            before_rows = endpoint_rows(*tmg);
            before_clocks = AnalogueClockTiming(tmg->get_timing_result());
            const auto initial = routed_placement_snapshot(ctx);
            auto receipt_parent = initial;
            std::map<IdString, decltype(CellInfo::pin_data)> preserved_pins;
            for (const auto &entry : cells) preserved_pins.emplace(entry.first, entry.second->pin_data);
            J::array moves;
            if (!post_route_lut_move_receipt.empty()) {
                std::string error;
                auto receipt = J::parse(post_route_lut_move_receipt, error);
                if (!error.empty()) log_error("Invalid preceding LUT move receipt.\n");
                check_post_route_lut_moves(receipt, initial);
                receipt_parent = receipt["parent"];
                moves = receipt["moves"].array_items();
            }
            std::set<IdString> moved;
            for (const auto &move : moves) moved.insert(ctx->id(move["cell"].string_value()));
            auto data_net = [&](NetInfo *net) {
                if (!net || !analogue_net_is_movable(net) || net->clkconstr || net->region ||
                    net->users.empty() || net->users.entries() > 128) return false;
                int clocks = 0;
                auto kind = getPortTimingClass(net->driver.cell, net->driver.port, clocks);
                if (kind != TMG_COMB_OUTPUT && kind != TMG_REGISTER_OUTPUT) return false;
                for (const auto &user : net->users) {
                    if (!user.cell || user.cell->bel == BelId()) return false;
                    kind = getPortTimingClass(user.cell, user.port, clocks);
                    if (kind != TMG_COMB_INPUT && kind != TMG_REGISTER_INPUT && kind != TMG_ENDPOINT) return false;
                }
                return true;
            };
            auto data_cell = [&](CellInfo *cell) {
                if (!cell || moved.count(cell->name) || !data_net(cell->getPort(id_Q))) return false;
                for (const auto &port : cell->ports)
                    if (port.second.type == PORT_IN && !data_net(port.second.net)) return false;
                return true;
            };
            auto cell_slack = [&](CellInfo *cell) {
                float slack = std::numeric_limits<float>::max();
                for (const auto &port : cell->ports) if (port.second.type == PORT_IN)
                    slack = std::min(slack, tmg->get_setup_slack({cell->name, port.first}));
                return slack;
            };
            struct PairOption { CellInfo *cold; BelId target; int distance; float slack; };
            struct PairGroup { CellInfo *cell, *peer; BelId target; float slack; std::vector<PairOption> options; };
            auto collect_pairs = [&]() {
                std::vector<PairGroup> groups;
                for (const auto &entry : cells) {
                    auto *cell = entry.second.get();
                    if (!analogue_sole_l5(ctx, cell) || !data_cell(cell)) continue;
                    const auto slack = cell_slack(cell);
                    if (!std::isfinite(slack) || slack <= float(std::numeric_limits<delay_t>::lowest()) ||
                        slack > current + improve_band) continue;
                    std::set<CellInfo *> peers;
                    for (const auto &user : cell->getPort(id_Q)->users) {
                        auto *peer = user.cell;
                        if (!peers.insert(peer).second || moved.count(peer->name)) continue;
                        const auto target = analogue_lut_pair_target(ctx, cell, peer);
                        if (target == BelId()) continue;
                        PairGroup group{cell, peer, target, slack, {}};
                        if (analogue_lut_pair_capacity_preview(ctx, cell, peer))
                            group.options.push_back({nullptr, BelId(), 0, 0});
                        else {
                            const auto where = bel_data(peer->bel).lab_data;
                            for (const auto &alm : labs.at(where.lab).alms) for (auto bel : alm.lut_bels) {
                                auto *cold = getBoundBelCell(bel);
                                if (!analogue_sole_l5(ctx, cold) || cold == cell || cold == peer ||
                                    !data_cell(cold)) continue;
                                const auto cold_slack = cell_slack(cold);
                                if (!std::isfinite(cold_slack) ||
                                    cold_slack <= float(std::numeric_limits<delay_t>::lowest()) ||
                                    cold_slack >= float(std::numeric_limits<delay_t>::max())) continue;
                                // Output forests retain their actual consumers. Neither
                                // moving output may itself terminate on the other move.
                                bool independent = true;
                                for (const auto &port : cold->ports) if (port.second.type == PORT_IN)
                                    independent &= port.second.net != cell->getPort(id_Q);
                                for (const auto &port : cell->ports) if (port.second.type == PORT_IN)
                                    independent &= port.second.net != cold->getPort(id_Q);
                                if (!independent) continue;
                                const auto home = getBelLocation(cold->bel);
                                for (int dx = -6; dx <= 6; ++dx) for (int dy = -6; dy <= 6; ++dy) {
                                    const int distance = std::abs(dx) + std::abs(dy);
                                    if (!distance || distance > 6) continue;
                                    for (auto site : getBelsByTile(home.x + dx, home.y + dy)) {
                                        if (!analogue_lut_move_allowed(ctx, cold, site) ||
                                            !analogue_lut_pair_capacity_preview(ctx, cell, peer, cold, site)) continue;
                                        group.options.push_back({cold, site, distance, cold_slack});
                                        break; // One legal empty ALM per tile bounds displacement search.
                                    }
                                }
                            }
                            std::stable_sort(group.options.begin(), group.options.end(), [&](const PairOption &a, const PairOption &b) {
                                if (a.slack != b.slack) return a.slack > b.slack;
                                if (a.distance != b.distance) return a.distance < b.distance;
                                if (a.cold != b.cold) return a.cold->name.str(ctx) < b.cold->name.str(ctx);
                                auto u = getBelLocation(a.target), v = getBelLocation(b.target);
                                return std::tie(u.x, u.y, u.z) < std::tie(v.x, v.y, v.z);
                            });
                            if (group.options.size() > 64) group.options.resize(64);
                        }
                        if (!group.options.empty()) groups.push_back(std::move(group));
                    }
                }
                std::stable_sort(groups.begin(), groups.end(), [&](const PairGroup &a, const PairGroup &b) {
                    if (a.slack != b.slack) return a.slack < b.slack;
                    if (a.cell != b.cell) return a.cell->name.str(ctx) < b.cell->name.str(ctx);
                    return a.peer->name.str(ctx) < b.peer->name.str(ctx);
                });
                return groups;
            };
            GpuRouterCfg cfg(ctx);
            cfg.telemetry_path.clear(); cfg.stop_requested = expired;
            cfg.legality_timing_gate = false; // Complete final analogue rows and clocks remain the acceptance gate.
            cfg.max_iter = std::min(cfg.max_iter, 64); cfg.arch_retry_limit = std::min(cfg.arch_retry_limit, 2);
            cfg.repair_rounds = 0; cfg.repair_improve_rounds = 0; cfg.repair_displace = false;
            int trial = 0, attempted = 0, accepted = 0;
            bool cancelled = false;
            log_info("Analogue LUT pair refinement: up to %d trials after all prior refinements.\n", post_route_lut_pairs);
            for (int pass = 0; pass < 4 && trial < post_route_lut_pairs && attempted < 128 && !expired(); ++pass) {
                const auto groups = collect_pairs();
                log_info("Analogue LUT pair pass %d: %zu eligible connected pairs.\n", pass + 1, groups.size());
                bool progress = false;
                for (size_t option = 0; option < 64 && !progress && !cancelled && attempted < 128 && trial < post_route_lut_pairs && !expired(); ++option)
                    for (const auto &group : groups) {
                        if (progress || attempted >= 128 || trial >= post_route_lut_pairs || expired() || before_rows.empty()) break;
                        if (option >= group.options.size()) continue;
                        auto *cell = group.cell;
                        const auto &choice = group.options[option];
                        if (moves.size() + (choice.cold ? 2 : 1) > 16) continue;
                        std::vector<std::pair<CellInfo *, BelId>> placement{{cell, group.target}}, original;
                        if (choice.cold) placement.emplace_back(choice.cold, choice.target);
                        std::map<IdString, std::vector<PortRef>> users;
                        std::map<IdString, LutOutputForest> forests;
                        std::map<IdString, std::vector<SavedRouting::Entry>> prefixes;
                        std::set<IdString> changed;
                        bool complete = true;
                        for (const auto &move : placement) {
                            original.emplace_back(move.first, move.first->bel);
                            auto *output = move.first->getPort(id_Q);
                            auto forest = lut_output_forest(ctx, output);
                            if (!forest) { complete = false; break; }
                            forests.emplace(output->name, std::move(*forest));
                            changed.insert(output->name);
                            for (const auto &port : move.first->ports) if (port.second.type == PORT_IN) {
                                users[port.second.net->name].push_back({move.first, port.first});
                                changed.insert(port.second.net->name);
                            }
                        }
                        for (const auto &net : users) {
                            if (forests.count(net.first)) { complete = false; break; }
                            auto prefix = lut_inputs_prefix(ctx, nets.at(net.first).get(), net.second);
                            if (!prefix) { complete = false; break; }
                            prefixes.emplace(net.first, std::move(*prefix));
                        }
                        if (!complete) continue;
                        const auto kept = save_routing(ctx, true);
                        const auto parent = routed_placement_snapshot(ctx);
                        const auto old_labs = labs;
                        const auto rng = ctx->rngstate;
                        std::map<CellInfo *, PlaceStrength> strengths;
                        for (const auto &move : original) {
                            strengths.emplace(move.first, move.first->belStrength);
                        }
                        auto rollback = [&]() {
                            for (const auto &move : original)
                                if (move.first->bel != BelId()) unbindBel(move.first->bel);
                            for (const auto &move : original) bindBel(move.second, move.first, strengths.at(move.first));
                            labs = old_labs;
                            for (const auto &entry : preserved_pins) cells.at(entry.first)->pin_data = entry.second;
                            restore_routing(ctx, kept); ctx->rngstate = rng;
                            signoff_table(); bitstream_configured = false; analogue_cache_valid = false;
                        };
                        ++attempted;
                        try {
                            CriticalCohortRouteCosts costs(ctx, CriticalCohortRouteCosts::snapshot(ctx, parent), parent);
                            if (!apply_critical_cohort_placement(ctx, placement) ||
                                !analogue_lut_pair_input_capacity(ctx, cell, group.peer)) { rollback(); continue; }
                            ++trial;
                            // Every physical pin map is retained: the destination
                            // has the same half and all shared pins have one owner.
                            for (const auto &net : kept.nets) {
                                auto *ni = nets.at(net.first).get();
                                if (changed.count(net.first)) ctx->ripupNet(net.first);
                                else for (auto &wire : ni->wires)
                                    wire.second.strength = std::max(wire.second.strength, STRENGTH_LOCKED);
                            }
                            bool bound = true;
                            for (const auto &prefix : prefixes) bound &= bind_net(ctx, nets.at(prefix.first).get(), prefix.second);
                            std::map<WireId, IdString> drivers;
                            for (const auto &net : nets) if (net.second->driver.cell)
                                drivers.emplace(ctx->getNetinfoSourceWire(net.second.get()), net.first);
                            std::map<IdString, std::vector<SavedRouting::Entry>> output_trees;
                            for (const auto &forest : forests) {
                                auto *net = nets.at(forest.first).get();
                                int nodes = CriticalCohortRouteCosts::node_limit;
                                const auto tree = costs.reconnect_source(ctx, net, ctx->getNetinfoSourceWire(net),
                                    forest.second.roots, forest.second.edges, changed, drivers, nodes);
                                auto &entries = output_trees[forest.first];
                                for (const auto &wire : tree) entries.push_back({wire.first, wire.second, STRENGTH_PLACER});
                                bound &= !tree.empty() && bind_net(ctx, net, entries);
                            }
                            if (!bound) {
                                rollback();
                                log_info("Analogue LUT pair trial %d: parent forest binding refused; restored.\n", trial);
                                continue;
                            }
                            std::map<IdString, std::map<WireId, PipId>> fixed;
                            for (const auto &net : kept.nets) if (changed.count(net.first)) {
                                std::vector<std::pair<WireId, PipId>> tree;
                                if (forests.count(net.first))
                                    for (const auto &wire : output_trees.at(net.first)) tree.emplace_back(wire.wire, wire.pip);
                                else for (const auto &wire : net.second) tree.emplace_back(wire.wire, wire.pip);
                                fixed.emplace(net.first, analogue_cell_connections(ctx, tree));
                            }
                            bitstream_configured = false; analogue_cache_valid = false;
                            const bool routed = try_analogue_repair_reroute(trial, [&]() { return gpurouter(ctx, cfg, false); });
                            bool untouched = true, cell_connections_unchanged = true;
                            auto retains = [&](IdString name, const std::vector<SavedRouting::Entry> &entries) {
                                const auto *net = nets.at(name).get();
                                for (const auto &wire : entries) {
                                    auto found = net->wires.find(wire.wire);
                                    if (found == net->wires.end() || found->second.pip != wire.pip) untouched = false;
                                }
                            };
                            for (const auto &prefix : prefixes) retains(prefix.first, prefix.second);
                            for (const auto &tree : output_trees) retains(tree.first, tree.second);
                            for (const auto &net : kept.nets) {
                                auto *ni = nets.at(net.first).get();
                                if (!changed.count(net.first)) {
                                    if (ni->wires.size() != net.second.size()) untouched = false;
                                    retains(net.first, net.second);
                                } else {
                                    std::vector<std::pair<WireId, PipId>> tree;
                                    for (const auto &wire : ni->wires) tree.emplace_back(wire.first, wire.second.pip);
                                    cell_connections_unchanged &= analogue_cell_connections(ctx, tree) == fixed.at(net.first);
                                }
                                dict<WireId, PlaceStrength> old;
                                for (const auto &wire : net.second) old[wire.wire] = wire.strength;
                                for (auto &wire : ni->wires) {
                                    auto found = old.find(wire.first);
                                    wire.second.strength = found == old.end() ? STRENGTH_WEAK : found->second;
                                }
                            }
                            ctx->rngstate = rng; signoff_table();
                            if (!untouched) log_error("LUT pair trial changed a protected route; restoring parent.\n");
                            auto expected = parent.object_items();
                            auto positions = parent["placement"].object_items();
                            for (const auto &move : placement)
                                positions[move.first->name.str(ctx)] = J::array{getBelName(move.second).str(ctx), int(strengths.at(move.first))};
                            expected["placement"] = positions;
                            if (routed_placement_snapshot(ctx) != J(expected))
                                log_error("LUT pair trial changed native fields outside declared placement; restoring parent.\n");
                            for (const auto &entry : preserved_pins) {
                                const auto &actual = cells.at(entry.first)->pin_data;
                                if (actual.size() != entry.second.size())
                                    log_error("LUT pair trial changed an undeclared physical pin map; restoring parent.\n");
                                for (const auto &pin : entry.second) {
                                    auto found = actual.find(pin.first);
                                    if (found == actual.end() || found->second.state != pin.second.state ||
                                        found->second.bel_pins != pin.second.bel_pins)
                                        log_error("LUT pair trial changed an undeclared physical pin map; restoring parent.\n");
                                }
                            }
                            float slack = current;
                            bool safe = false;
                            CriticalCohortEndpointRows after_rows;
                            AnalogueClockTiming after_clocks;
                            if (routed && cell_connections_unchanged) {
                                slack = time_routing(summary);
                                after_rows = endpoint_rows(*tmg);
                                after_clocks = AnalogueClockTiming(tmg->get_timing_result());
                                safe = !after_clocks.worse_than(before_clocks) &&
                                    critical_cohort_proposal_rows_safe(before_rows, after_rows);
                            }
                            log_info("Analogue LUT pair trial %d: %s beside %s, %zu move(s), %.3f -> %.3f ns, %s.\n",
                                trial, nameOf(cell), nameOf(group.peer), placement.size(), current / 1000.0f, slack / 1000.0f,
                                safe ? "proposal guard passed" : !cell_connections_unchanged ? "cell connection changed; rejected" : routed ? "rejected" : "routing failed");
                            if (safe) {
                                for (const auto &move : original) {
                                    moves.emplace_back(J::object{{"cell", move.first->name.str(ctx)},
                                        {"from_bel", getBelName(move.second).str(ctx)},
                                        {"to_bel", getBelName(move.first->bel).str(ctx)}, {"strength", int(strengths.at(move.first))}});
                                    moved.insert(move.first->name);
                                }
                                ++accepted; progress = true;
                                current = slack; before_rows = std::move(after_rows); before_clocks = std::move(after_clocks);
                                ctx->check();
                            } else { rollback(); if (!expired()) current = time_routing(summary); }
                        } catch (const GpuRouterCancelled &) { rollback(); cancelled = true; break; }
                        catch (...) { rollback(); throw; }
                    }
                if (!progress || cancelled) break;
            }
            if (accepted) {
                const J receipt = J::object{{"version", 1}, {"parent", receipt_parent},
                    {"target", routed_placement_snapshot(ctx)}, {"moves", moves}};
                check_post_route_lut_moves(receipt, routed_placement_snapshot(ctx));
                post_route_lut_move_receipt = receipt.dump();
            } else if (routed_placement_snapshot(ctx) != initial)
                log_error("Rejected LUT pair trials did not restore the complete native parent.\n");
            log_info("Analogue LUT pair completion: %d attempts, %d/%d legal trials, %d retained; %s.\n",
                attempted, trial, post_route_lut_pairs, accepted,
                cancelled || expired() ? "runtime budget exhausted" : before_rows.empty() ? "native timing rows unavailable" : "bounded pass finished");
        }
        if (post_route_lut_input_nets > 0) {
            if (expired()) {
                log_info("Analogue whole input net completion: runtime budget exhausted before stage.\n");
            } else {
                const auto initial = routed_placement_snapshot(ctx);
                log_info("Analogue whole input net refinement: up to %d LUT pin trials after prior refinements.\n",
                         post_route_lut_input_nets);
                refine_lut_inputs(post_route_lut_input_nets, false, false, true);
                if (routed_placement_snapshot(ctx) != initial)
                    log_error("Whole input net refinement changed native design, placement, clocks or RNG.\n");
                log_info("Analogue whole input net completion: %s.\n",
                         expired() ? "runtime budget exhausted" : before_rows.empty() ? "native timing rows unavailable" :
                         "bounded pass finished");
            }
        }
        if (post_route_static_ff_lut_pins > 0) {
            if (expired()) {
                log_info("Analogue static FF L5 input completion: runtime budget exhausted before stage.\n");
            } else {
                const auto initial = routed_placement_snapshot(ctx);
                log_info("Analogue static FF L5 input refinement: up to %d LUT pin trials after prior refinements.\n",
                         post_route_static_ff_lut_pins);
                refine_lut_inputs(post_route_static_ff_lut_pins, true, false, true, true);
                if (routed_placement_snapshot(ctx) != initial)
                    log_error("Static FF L5 input refinement changed native design, placement, clocks or RNG.\n");
                log_info("Analogue static FF L5 input completion: %s.\n",
                         expired() ? "runtime budget exhausted" : before_rows.empty() ? "native timing rows unavailable" :
                         "bounded pass finished");
            }
        }
        signoff_table();
        bitstream_configured = false;
        analogue_cache_valid = false;
    }
    return result;
}

NEXTPNR_NAMESPACE_END
