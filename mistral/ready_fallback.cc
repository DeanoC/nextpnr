/* Bounded final ready-net fallback route diagnostic. SPDX-License-Identifier: ISC */
#include <algorithm>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include "gpurouter.h"
#include "jsonwrite.h"
#include "log.h"
#include "nextpnr.h"
#include "ready_fallback_policy.h"
#include "ready_shortest_backend.h"
#include "timing.h"
NEXTPNR_NAMESPACE_BEGIN
namespace {
using Tree = std::map<WireId, std::pair<PipId, PlaceStrength>>;
using Quad = std::tuple<delay_t, delay_t, delay_t, delay_t>;
Quad quad(const DelayQuad &q) { return {q.minRiseDelay(), q.maxRiseDelay(), q.minFallDelay(), q.maxFallDelay()}; }
Tree tree(NetInfo *net)
{
    Tree t;
    for (auto &w : net->wires)
        t[w.first] = {w.second.pip, w.second.strength};
    return t;
}
bool bind_tree(Context *ctx, NetInfo *net, const Tree &t)
{
    ctx->ripupNet(net->name);
    for (auto &e : t)
        if (e.second.first == PipId()) {
            if (!ctx->checkWireAvail(e.first))
                return false;
            ctx->bindWire(e.first, net, e.second.second);
        }
    for (auto &e : t)
        if (e.second.first != PipId()) {
            if (!ctx->checkWireAvail(e.first) || !ctx->checkPipAvailForNet(e.second.first, net))
                return false;
            ctx->bindPip(e.second.first, net, e.second.second);
        }
    return true;
}
void complete(Context *ctx, NetInfo *net)
{
    auto src = ctx->getNetinfoSourceWire(net);
    NPNR_ASSERT(src != WireId() && net->wires.count(src));
    for (auto &usr : net->users)
        for (auto sink : ctx->getNetinfoSinkWires(net, usr)) {
            std::set<WireId> seen;
            auto w = sink;
            while (w != src) {
                NPNR_ASSERT(seen.insert(w).second && net->wires.count(w));
                auto p = net->wires.at(w).pip;
                NPNR_ASSERT(p != PipId() && ctx->getPipDstWire(p) == w);
                w = ctx->getPipSrcWire(p);
            }
        }
}
std::string json(Context *ctx)
{
    std::ostringstream f;
    std::string name = "ready-fallback";
    NPNR_ASSERT(write_json_file(f, name, ctx));
    return f.str();
}
using Pins = std::map<std::pair<IdString, IdString>, std::pair<CellPinState, std::vector<IdString>>>;
Pins pins(Context *ctx)
{
    Pins p;
    for (auto &e : ctx->cells) {
        for (auto &v : e.second->ports)
            p[{e.first, v.first}] = {e.second->get_pin_state(v.first), {}};
        for (auto &v : e.second->pin_data)
            p[{e.first, v.first}] = {v.second.state, v.second.bel_pins};
    }
    return p;
}
struct Evaluation
{
    std::vector<TimingAnalyser::EndpointDomainSlack> endpoints;
    std::map<IdString, ClockFmax> clocks;
    int worst = std::numeric_limits<int>::max();
    bool finite = true;
};
} // namespace
void Arch::ready_fallback_pass(const char *prefix)
{
    if (!prefix || !*prefix)
        return;
    const char *shortest_env = std::getenv("NEXTPNR_MISTRAL_READY_SHORTEST");
    const bool exact = shortest_env && *shortest_env;
    NPNR_ASSERT(!exact || std::string(shortest_env) == "1");
    auto ctx = getCtx();
    NPNR_ASSERT(bitstream_configured && analogue_cache_valid && pip_delay_calibrated);
    NetInfo *target = nullptr;
    for (auto &e : nets) {
        auto n = e.second.get();
        if (n->driver.cell && n->driver.cell->type == id_cyclonev_hps_interface_fpga2sdram &&
            n->driver.port.str(ctx) == "cmd_ready_1") {
            NPNR_ASSERT(!target);
            target = n;
        }
    }
    NPNR_ASSERT(target && target->users.entries() == 1 && !target->is_global && !target->wires.empty());
    for (auto &w : target->wires)
        NPNR_ASSERT(w.second.strength == STRENGTH_WEAK);
    complete(ctx, target);
    auto original = tree(target);
    auto source_wire = ctx->getNetinfoSourceWire(target);
    auto sink_wires = ctx->getNetinfoSinkWires(target, *target->users.begin());
    NPNR_ASSERT(sink_wires.size() == 1);
    auto source_clock = target->driver.cell->getPort(id("cmd_port_clk_1"));
    NPNR_ASSERT(source_clock && source_clock->clkconstr);
    NPNR_ASSERT(target->driver.cell->get_pin_state(id("cmd_port_clk_1")) == PIN_SIG);
    std::set<CellPortKey> endpoint_set;
    std::set<NetInfo *> active, visited;
    int interiors = 0;
    std::function<void(NetInfo *)> walk = [&](NetInfo *n) {
        NPNR_ASSERT(!active.count(n));
        if (!visited.insert(n).second)
            return;
        active.insert(n);
        for (auto &u : n->users) {
            auto c = u.cell;
            NPNR_ASSERT(c);
            if (c->type == id_MISTRAL_FF) {
                NPNR_ASSERT(u.port.in(id_ENA, id_SCLR) && c->getPort(id_CLK) == source_clock &&
                            c->get_pin_state(id_CLK) == PIN_SIG);
                int clocks = 0;
                NPNR_ASSERT(ctx->getPortTimingClass(c, u.port, clocks) == TMG_REGISTER_INPUT && clocks == 1);
                auto info = ctx->getPortClockingInfo(c, u.port, 0);
                NPNR_ASSERT(info.edge == RISING_EDGE);
                endpoint_set.insert(CellPortKey(u));
            } else {
                NPNR_ASSERT(c->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5,
                                       id_MISTRAL_ALUT6) &&
                            u.port != id_Q);
                auto q = c->getPort(id_Q);
                NPNR_ASSERT(q);
                if (!visited.count(q))
                    ++interiors;
                walk(q);
            }
        }
        active.erase(n);
    };
    walk(target);
    NPNR_ASSERT(endpoint_set.size() == 224 && interiors == 5);
    std::vector<CellPortKey> endpoints(endpoint_set.begin(), endpoint_set.end());
    std::sort(endpoints.begin(), endpoints.end(), [&](auto &a, auto &b) {
        return std::make_pair(a.cell.str(ctx), a.port.str(ctx)) < std::make_pair(b.cell.str(ctx), b.port.str(ctx));
    });
    auto observed = pip_delay_observed;
    auto types = pip_type_calibration;
    auto prior = pip_delay_prior;
    auto calibrated = pip_delay_calibrated;
    std::map<IdString, Tree> routes;
    std::map<IdString, std::pair<BelId, PlaceStrength>> bels;
    std::map<IdString, int32_t> udata;
    for (auto &e : nets) {
        routes[e.first] = tree(e.second.get());
        udata[e.first] = e.second->udata;
    }
    for (auto &e : cells)
        bels[e.first] = {e.second->bel, e.second->belStrength};
    archInfoToAttributes();
    auto graph = json(ctx);
    auto original_pins = pins(ctx);
    // This is a routed context. LAB route-through insertion rewires FF.DATAIN
    // without refreshing placement's cached ffInfo.datain. Preserve and audit
    // the baseline placement-check vector; do not repair that cache here.
    using Validity = std::map<IdString, std::tuple<BelId, bool, std::string>>;
    auto placement_validity = [&]() {
        Validity result;
        for (const auto &entry : cells) {
            auto cell = entry.second.get();
            if (cell->bel == BelId())
                continue;
            bool valid = isBelLocationValid(cell->bel, false);
            std::string reason;
            const auto &data = bel_data(cell->bel);
            if (!valid && data.type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF)) {
                if (!is_alm_legal(data.lab_data.lab, data.lab_data.alm))
                    reason += "alm;";
                if (!check_lab_input_count(data.lab_data.lab))
                    reason += "lab_input_count;";
                if (data.type == id_MISTRAL_FF && !is_lab_ctrlset_legal(data.lab_data.lab))
                    reason += "lab_control_set;";
                if (!check_mlab_groups(data.lab_data.lab))
                    reason += "mlab_group;";
            }
            if (reason.empty())
                reason = valid ? "valid" : "other";
            result.emplace(entry.first, std::make_tuple(cell->bel, valid, reason));
        }
        return result;
    };
    const auto baseline_validity = placement_validity();
    size_t baseline_invalid = 0;
    for (const auto &entry : baseline_validity)
        baseline_invalid += !std::get<1>(entry.second);
    auto validity_evidence = [&](const char *suffix) {
        auto current = placement_validity();
        NPNR_ASSERT(current == baseline_validity);
        std::ofstream f(std::string(prefix) + suffix);
        f << "cell\tbel\tbefore_valid\tafter_valid\tbefore_reason\tafter_reason\n";
        for (const auto &entry : baseline_validity) {
            const auto &after = current.at(entry.first);
            f << entry.first.str(ctx) << '\t' << getBelName(std::get<0>(entry.second)).str(ctx) << '\t'
              << std::get<1>(entry.second) << '\t' << std::get<1>(after) << '\t' << std::get<2>(entry.second) << '\t'
              << std::get<2>(after) << '\n';
        }
        f.close();
        NPNR_ASSERT(f);
    };
    validity_evidence(".before.placement-validity.tsv");
    log_info("Ready fallback baseline placement-check census: %zu of %zu bound cells report invalid in routed phase.\n",
             baseline_invalid, baseline_validity.size());
    std::map<const PortRef *, std::pair<bool, Quad>> old_cache;
    for (auto &e : analogue_arc_cache)
        old_cache[e.first] = {e.second.ok, quad(e.second.delay)};
    auto invariant = [&]() {
        NPNR_ASSERT(pip_delay_prior == prior && pip_delay_calibrated == calibrated && pip_delay_observed == observed &&
                    analogue_cache_valid && bitstream_configured);
        for (size_t i = 0; i < types.size(); ++i)
            NPNR_ASSERT(types[i].hops == pip_type_calibration[i].hops &&
                        types[i].table_ps == pip_type_calibration[i].table_ps &&
                        types[i].analogue_ps == pip_type_calibration[i].analogue_ps);
        NPNR_ASSERT(cells.size() == bels.size() && nets.size() == routes.size());
        for (auto &e : cells)
            NPNR_ASSERT(std::make_pair(e.second->bel, e.second->belStrength) == bels.at(e.first));
        for (auto &e : nets)
            if (e.second.get() != target)
                NPNR_ASSERT(tree(e.second.get()) == routes.at(e.first));
        NPNR_ASSERT(pins(ctx) == original_pins && json(ctx) == graph);
        NPNR_ASSERT(placement_validity() == baseline_validity);
        NPNR_ASSERT(analogue_arc_cache.size() == old_cache.size());
        for (auto &e : analogue_arc_cache) {
            if (e.first->cell->getPort(e.first->port) == target)
                NPNR_ASSERT(!e.second.ok);
            else
                NPNR_ASSERT(std::make_pair(e.second.ok, quad(e.second.delay)) == old_cache.at(e.first));
        }
    };
    auto evaluate = [&]() {
        TimingAnalyser t(ctx);
        t.setup_only = false;
        t.with_clock_skew = true;
        t.setup(true, true, true);
        Evaluation e;
        for (auto &p : endpoints) {
            auto d = t.get_endpoint_domain_slacks(p);
            NPNR_ASSERT(d.size() == 1 && d[0].timed && d[0].launch.clock == source_clock->name &&
                        d[0].capture.clock == source_clock->name && d[0].launch.edge == RISING_EDGE &&
                        d[0].capture.edge == RISING_EDGE);
            e.endpoints.push_back(d[0]);
            e.finite &= ready_fallback_policy::finite({d[0].setup, d[0].hold});
            e.worst = std::min(e.worst, int(d[0].setup));
            NPNR_ASSERT(t.get_setup_slack(p) == d[0].setup);
        }
        for (auto &c : t.get_timing_result().clock_fmax)
            e.clocks[c.first] = c.second;
        NPNR_ASSERT(!e.clocks.empty());
        return e;
    };
    auto snapshot = [&](const std::string &suffix) {
        std::string path = std::string(prefix) + suffix;
        std::ofstream f(path);
        NPNR_ASSERT(f && write_json_file(f, path, ctx));
        f.close();
        NPNR_ASSERT(f);
        std::ofstream p(path + ".pins.tsv");
        p << "cell\tport\tstate\n";
        std::map<std::pair<std::string, std::string>, int> sorted;
        for (auto &e : pins(ctx))
            sorted[{e.first.first.str(ctx), e.first.second.str(ctx)}] = int(e.second.first);
        for (auto &e : sorted)
            p << e.first.first << '\t' << e.first.second << '\t' << e.second << '\n';
        p.close();
        NPNR_ASSERT(p);
    };
    auto rbf = [&](const char *suffix) {
        std::vector<uint8_t> bytes;
        cyclonev->rbf_save(bytes);
        std::ofstream f(std::string(prefix) + suffix, std::ios::binary);
        f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        f.close();
        NPNR_ASSERT(f);
    };
    const auto before = evaluate();
    NPNR_ASSERT(before.finite);
    invariant();
    snapshot(".before.json");
    rbf(".before.rbf");
    dump_ready_timing((std::string(prefix) + ".before.trace").c_str());
    auto guards = [&](const std::string &suffix, const Evaluation &e) {
        std::ofstream f(std::string(prefix) + suffix);
        f << "cell\tport\tlaunch_clock\tlaunch_edge\tcapture_clock\tcapture_edge\tbefore_setup_ps\tafter_setup_"
             "ps\tbefore_hold_ps\tafter_hold_ps\n";
        bool ok = e.finite;
        for (size_t i = 0; i < endpoints.size(); ++i) {
            auto &a = before.endpoints[i];
            auto &b = e.endpoints[i];
            NPNR_ASSERT(a.launch == b.launch && a.capture == b.capture);
            f << endpoints[i].cell.str(ctx) << '\t' << endpoints[i].port.str(ctx) << '\t' << b.launch.clock.str(ctx)
              << '\t' << int(b.launch.edge) << '\t' << b.capture.clock.str(ctx) << '\t' << int(b.capture.edge) << '\t'
              << a.setup << '\t' << b.setup << '\t' << a.hold << '\t' << b.hold << '\n';
            ok &= ready_fallback_policy::endpoint_ok({a.setup, a.hold}, {b.setup, b.hold});
        }
        f.close();
        NPNR_ASSERT(f);
        return ok;
    };
    std::ofstream clocks(std::string(prefix) + ".clocks.tsv");
    clocks << "trial\tclock\tbefore_mhz\tafter_mhz\tconstraint_mhz\n"
           << std::setprecision(std::numeric_limits<float>::max_digits10);
    auto clock_guard = [&](int trial, const Evaluation &e) {
        NPNR_ASSERT(e.clocks.size() == before.clocks.size());
        bool ok = true;
        for (auto &c : before.clocks) {
            auto now = e.clocks.at(c.first);
            NPNR_ASSERT(now.constraint == c.second.constraint);
            clocks << trial << '\t' << c.first.str(ctx) << '\t' << c.second.achieved << '\t' << now.achieved << '\t'
                   << now.constraint << '\n';
            ok &= ready_fallback_policy::clock_ok(c.second.achieved, now.achieved);
        }
        clocks.flush();
        NPNR_ASSERT(clocks);
        return ok;
    };
    std::ofstream trials(std::string(prefix) + ".candidates.tsv");
    trials << "trial\tvariant\tbound\teligible\tcache_ok\tfinite\tclock_nonregressing\tworst_before_ps\tworst_after_"
              "ps\tgain_ps\treason\n";
    const auto original_settings = ctx->settings;
    std::unique_ptr<GpuRouterCfg> cfg;
    if (!exact) {
        cfg.reset(new GpuRouterCfg(ctx));
        ctx->settings = original_settings;
    }
    const int count = exact ? 1 : 8;
    NPNR_ASSERT(prior == 1.25f);
    int chosen = -1, best_worst = before.worst;
    Tree best = original;
    size_t generated = 0;
    {
        std::unique_ptr<GpuCandidateRouter> router;
        std::vector<GpuRouteTree> candidates;
        if (exact) {
            const auto original_scalar = ctx->getNetinfoRouteDelay(target, *target->users.begin());
            candidates = ready_shortest_candidate(ctx, target, source_wire, sink_wires[0], prefix);
            // The currently bound route is an available path in this graph.
            NPNR_ASSERT(candidates.size() == 1 && candidates[0].route_delay <= original_scalar);
        } else {
            router.reset(new GpuCandidateRouter(ctx, *cfg));
            ctx->settings = original_settings;
            auto user = target->users.enumerate().begin();
            auto generated_sinks = router->candidates({{target, (*user).index}}, count);
            ctx->settings = original_settings;
            NPNR_ASSERT(generated_sinks.size() == 1);
            candidates = std::move(generated_sinks[0]);
        }
        generated = candidates.size();
        invariant();
        for (size_t i = 0; i < generated; ++i) {
            auto &candidate = candidates[i];
            Tree proposed;
            for (auto &w : candidate.wires)
                NPNR_ASSERT(proposed.emplace(w.first, std::make_pair(w.second, STRENGTH_WEAK)).second);
            std::string stem = ".candidate-" + std::to_string(i);
            std::ofstream tf(std::string(prefix) + stem + ".tree.tsv");
            tf << "wire\tpip_src\tstrength\n";
            for (auto &w : proposed)
                tf << nameOfWire(w.first) << '\t'
                   << (w.second.first == PipId() ? "" : nameOfWire(getPipSrcWire(w.second.first))) << '\t'
                   << int(w.second.second) << '\n';
            tf.close();
            NPNR_ASSERT(tf);
            bool bound = bind_tree(ctx, target, proposed);
            if (!bound) {
                NPNR_ASSERT(bind_tree(ctx, target, original));
                configure_bitstream(false);
                invariant();
                trials << i << '\t' << candidate.variant << "\t0\t0\t0\t0\t0\t" << before.worst
                       << "\t0\t0\tarch_unavailable\n";
                trials.flush();
                NPNR_ASSERT(trials);
                continue;
            }
            complete(ctx, target);
            configure_bitstream(false);
            invariant();
            if (exact)
                NPNR_ASSERT(ctx->getNetinfoRouteDelay(target, *target->users.begin()) == candidate.route_delay);
            auto now = evaluate();
            bool guard = guards(stem + ".guards.tsv", now);
            bool clock_ok = clock_guard(i, now);
            bool gain = ready_fallback_policy::gain_ok(before.worst, now.worst);
            bool eligible = guard && clock_ok && gain;
            trials << i << '\t' << candidate.variant << "\t1\t" << eligible << "\t0\t" << now.finite << '\t' << clock_ok
                   << '\t' << before.worst << '\t' << now.worst << '\t' << now.worst - before.worst << '\t'
                   << (eligible    ? "eligible"
                       : !guard    ? "endpoint_guard"
                       : !clock_ok ? "clock_regression"
                                   : "insufficient_gain")
                   << '\n';
            trials.flush();
            NPNR_ASSERT(trials);
            if (eligible && now.worst > best_worst) {
                best_worst = now.worst;
                chosen = i;
                best = proposed;
            }
            NPNR_ASSERT(bind_tree(ctx, target, original));
            configure_bitstream(false);
            invariant();
        }
    }
    for (auto &e : nets)
        e.second->udata = udata.at(e.first);
    NPNR_ASSERT(bind_tree(ctx, target, best));
    complete(ctx, target);
    configure_bitstream(false);
    invariant();
    auto final = evaluate();
    NPNR_ASSERT(guards(".guards.tsv", final) && clock_guard(-1, final));
    NPNR_ASSERT(final.worst == best_worst && (chosen < 0 || ready_fallback_policy::gain_ok(before.worst, final.worst)));
    ctx->check();
    validity_evidence(".placement-validity.tsv");
    // ROUTING is serialized from attributes, so refresh only after all invariants
    // compared against the original graph. Final --write must describe this tree.
    archInfoToAttributes();
    snapshot(".after.json");
    rbf(".after.rbf");
    dump_ready_timing((std::string(prefix) + ".after.trace").c_str());
    trials.close();
    clocks.close();
    NPNR_ASSERT(trials && clocks);
    std::ofstream audit(std::string(prefix) + ".audit.json");
    audit << "{\"selected_trial\":" << chosen << ",\"max_candidates\":" << count << ",\"search_mode\":\""
          << (exact ? "exact_dijkstra" : "gpu_candidates") << "\",\"generated\":" << generated
          << ",\"endpoints\":224,\"interior_luts\":5,\"gain_threshold_ps\":20,\"worst_before_ps\":" << before.worst
          << ",\"worst_after_ps\":" << final.worst << ",\"source_wire\":\"" << nameOfWire(source_wire)
          << "\",\"sink_wire\":\"" << nameOfWire(sink_wires[0]) << "\",\"prior\":" << prior
          << ",\"candidate_margin\":" << (cfg ? cfg->candidate_margin : 0)
          << ",\"candidate_unbounded\":" << (cfg && cfg->candidate_unbounded ? "true" : "false")
          << ",\"candidate_expand_k\":" << (cfg ? cfg->candidate_expand_k : 0)
          << ",\"before_invalid_bels\":" << baseline_invalid << ",\"after_invalid_bels\":" << baseline_invalid
          << ",\"placement_check_vector_preserved\":true"
          << ",\"calibration_unchanged\":true,\"graph_preserved\":true,\"placements_preserved\":true,\"unrelated_"
             "routes_preserved\":true,\"unrelated_cache_preserved\":true,\"ready_cache_ok\":false,\"net_udata_"
             "restored\":true}\n";
    audit.close();
    NPNR_ASSERT(audit);
    log_info("Ready fallback: selected candidate %d of %zu; affected worst slack %.3f -> %.3f ns.\n", chosen, generated,
             before.worst / 1000.0, final.worst / 1000.0);
}
NEXTPNR_NAMESPACE_END
