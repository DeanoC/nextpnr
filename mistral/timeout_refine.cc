/* Bounded two-cell timeout placement refinement diagnostic.
 * SPDX-License-Identifier: ISC
 */
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include "enable_replication_policy.h"
#include "jsonwrite.h"
#include "log.h"
#include "nextpnr.h"
#include "timeout_refine_policy.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Lab = std::pair<int, int>;
using Hold = std::map<std::string, int>;
Hold refine_hold(TimingAnalyser &timing)
{
    Hold result;
    for (const auto &path : timing.get_timing_result().min_delay_violations) {
        if (path.segments.empty())
            continue;
        int slack = 0;
        for (const auto &s : path.segments)
            slack += s.delay;
        auto ep = path.segments.back().to;
        std::string key = std::to_string(path.clock_pair.start.clock.index) + ":" +
                          std::to_string(int(path.clock_pair.start.edge)) + ":" +
                          std::to_string(path.clock_pair.end.clock.index) + ":" +
                          std::to_string(int(path.clock_pair.end.edge)) + ":" + std::to_string(ep.first.index) + ":" +
                          std::to_string(ep.second.index);
        auto it = result.find(key);
        if (it == result.end())
            result.emplace(key, slack);
        else
            it->second = std::min(it->second, slack);
    }
    return result;
}
} // namespace
// Fixture-specific two-cell coordinate descent; placement predictor experiment only.
void diagnostic_timeout_refine(Context *ctx, const char *prefix, bool require_clock_constraint)
{
    if (!prefix || !*prefix)
        return;
    const char *names[] = {"timeout_partition_ch1_control9", "timeout_partition_ch1_control0"};
    std::vector<CellInfo *> targets;
    std::vector<BelId> origins;
    auto keep = ctx->id("keep"), dont_touch = ctx->id("dont_touch");
    auto movable = [&](const CellInfo *c) {
        return c->bel != BelId() && c->belStrength <= STRENGTH_WEAK && c->cluster == ClusterId() && !c->region &&
               !c->isPseudo() && !c->attrs.count(keep) && !c->attrs.count(dont_touch);
    };
    std::set<Lab> protected_labs;
    for (auto &e : ctx->cells) {
        auto c = e.second.get();
        if (c->bel != BelId() && (!movable(c) || c->type == id_MISTRAL_MLAB)) {
            auto l = ctx->getBelLocation(c->bel);
            protected_labs.insert({l.x, l.y});
        }
    }
    for (auto name : names) {
        auto c = ctx->cells.at(ctx->id(name)).get();
        NPNR_ASSERT(c->type == id_MISTRAL_ALUT6 && c->ports.size() == 7 && movable(c));
        auto l = ctx->getBelLocation(c->bel);
        NPNR_ASSERT(!protected_labs.count({l.x, l.y}));
        for (auto &p : c->ports) {
            NPNR_ASSERT(p.second.net && c->get_pin_state(p.first) == PIN_SIG);
            auto mapped = ctx->getBelPinsForCellPin(c, p.first);
            NPNR_ASSERT(mapped.begin() != mapped.end());
            if (p.second.type == PORT_IN)
                NPNR_ASSERT(p.second.net->driver.cell && p.second.net->driver.cell->bel != BelId());
        }
        targets.push_back(c);
        origins.push_back(c->bel);
    }
    NPNR_ASSERT(targets[0]->getPort(id_Q)->users.entries() == 1 &&
                targets[1]->getPort(id_E) == targets[0]->getPort(id_Q));
    std::vector<CellPortKey> endpoints;
    NetInfo *clock = nullptr;
    CellPinState clock_state = PIN_SIG;
    for (auto user : targets[1]->getPort(id_Q)->users) {
        NPNR_ASSERT(user.cell->type == id_MISTRAL_FF && user.port == id_ENA && user.cell->bel != BelId());
        auto clk = user.cell->getPort(id_CLK);
        NPNR_ASSERT(clk && (!clock || (clk == clock && user.cell->get_pin_state(id_CLK) == clock_state)));
        clock = clk;
        clock_state = user.cell->get_pin_state(id_CLK);
        endpoints.emplace_back(user.cell->name, user.port);
    }
    NPNR_ASSERT(endpoints.size() == 29);
    std::sort(endpoints.begin(), endpoints.end(),
              [&](const CellPortKey &a, const CellPortKey &b) { return a.cell.str(ctx) < b.cell.str(ctx); });
    NPNR_ASSERT(ctx->cells.count(ctx->id("hps_ddr.port1.slot_free_MISTRAL_ALUT3_B$enable_replica")));
    NPNR_ASSERT(ctx->cells.count(ctx->id(
            "ddr1_nack_MISTRAL_FF_Q_ENA_MISTRAL_ALUT2_Q_A_MISTRAL_ALUT2_B_Q_MISTRAL_ALUT4_D$retained_enable_replica")));
    for (auto &e : ctx->nets)
        NPNR_ASSERT(e.second->wires.empty());
    if (require_clock_constraint && !clock->clkconstr)
        log_error("Timeout refinement requires fresh memory clock constraints.\n");
    auto snapshot = [&](const char *suffix) {
        ctx->archInfoToAttributes();
        std::string path = std::string(prefix) + suffix;
        std::ofstream file(path);
        if (!file || !write_json_file(file, path, ctx))
            log_error("Cannot write timeout refinement snapshot.\n");
        file.close();
        if (!file)
            log_error("Cannot finish timeout refinement snapshot.\n");
        std::map<std::pair<std::string, std::string>, int> pins;
        for (auto &e : ctx->cells) {
            auto c = e.second.get();
            for (auto &p : c->ports)
                pins[{c->name.str(ctx), p.first.str(ctx)}] = int(c->get_pin_state(p.first));
            for (auto &p : c->pin_data)
                pins[{c->name.str(ctx), p.first.str(ctx)}] = int(c->get_pin_state(p.first));
        }
        std::ofstream sidecar(path + ".pins.tsv");
        sidecar << "cell\tport\tstate\n";
        for (auto &p : pins)
            sidecar << p.first.first << '\t' << p.first.second << '\t' << p.second << '\n';
        sidecar.close();
        if (!sidecar)
            log_error("Cannot finish timeout refinement pin evidence.\n");
    };
    snapshot(".before.json");
    struct Saved
    {
        IdString type;
        BelId bel;
        PlaceStrength strength;
        dict<IdString, Property> params, attrs;
        std::map<IdString, std::tuple<NetInfo *, PortType, CellPinState, int>> ports;
        std::map<IdString, CellPinState> pins;
    };
    std::map<IdString, Saved> saved;
    for (auto &e : ctx->cells) {
        auto c = e.second.get();
        auto &s = saved[e.first];
        s.type = c->type;
        s.bel = c->bel;
        s.strength = c->belStrength;
        s.params = c->params;
        s.attrs = c->attrs;
        for (auto &p : c->ports)
            s.ports[p.first] = {p.second.net, p.second.type, c->get_pin_state(p.first), p.second.user_idx.idx()};
        for (auto &p : c->pin_data)
            s.pins[p.first] = c->get_pin_state(p.first);
    }
    std::ofstream labs(std::string(prefix) + ".protected-labs.tsv");
    labs << "x\ty\n";
    for (auto l : protected_labs)
        labs << l.first << '\t' << l.second << '\n';
    labs.close();
    if (!labs)
        log_error("Cannot write protected LABs.\n");
    TimingAnalyser timing(ctx);
    timing.setup(false, false, true);
    auto initial_hold = refine_hold(timing);
    auto slacks = [&]() {
        std::vector<float> s;
        for (auto ep : endpoints) {
            auto v = timing.get_setup_slack(ep);
            NPNR_ASSERT(std::isfinite(v));
            s.push_back(v);
        }
        return s;
    };
    auto initial_slacks = slacks(), current_slacks = initial_slacks;
    auto worst = [](const std::vector<float> &v) { return *std::min_element(v.begin(), v.end()); };
    int sta_runs = 0, accepted_moves = 0;
    std::ofstream trials(std::string(prefix) + ".trials.tsv"), moves(std::string(prefix) + ".moves.tsv");
    trials << "pass\tcell\tbel\tlegal\timproves\tworst_ps\n";
    moves << "pass\tcell\told_bel\tnew_bel\tbefore_worst_ps\tafter_worst_ps\n";
    for (int pass = 0; pass < 2; ++pass) {
        bool changed = false;
        for (size_t i = 0; i < targets.size(); ++i) {
            auto c = targets[i];
            auto previous = c->bel;
            auto strength = c->belStrength;
            auto center = ctx->getBelLocation(origins[i]);
            std::map<Lab, std::vector<BelId>> candidates;
            for (auto b : ctx->getBels()) {
                auto l = ctx->getBelLocation(b);
                if (std::abs(l.x - center.x) + std::abs(l.y - center.y) > 6 || protected_labs.count({l.x, l.y}) ||
                    !ctx->isValidBelForCellType(c->type, b) || (b != previous && !ctx->checkBelAvail(b)))
                    continue;
                candidates[{l.x, l.y}].push_back(b);
            }
            auto best = previous;
            auto best_slacks = current_slacks;
            ctx->unbindBel(previous);
            for (auto &group : candidates) {
                std::sort(group.second.begin(), group.second.end(), [&](BelId a, BelId b) {
                    return std::string(ctx->nameOfBel(a)) < std::string(ctx->nameOfBel(b));
                });
                for (auto b : group.second) {
                    ctx->bindBel(b, c, strength);
                    if (!ctx->isBelLocationValid(b)) {
                        ctx->unbindBel(b);
                        continue;
                    }
                    timing.run(true);
                    ++sta_runs;
                    auto trial_slacks = slacks();
                    bool improves = timeout_refine_policy::improves(current_slacks, trial_slacks);
                    trials << pass << '\t' << c->name.str(ctx) << '\t' << ctx->nameOfBel(b) << "\t1\t" << improves
                           << '\t' << worst(trial_slacks) << '\n';
                    if (improves && (worst(trial_slacks) > worst(best_slacks) ||
                                     (worst(trial_slacks) == worst(best_slacks) &&
                                      std::string(ctx->nameOfBel(b)) < std::string(ctx->nameOfBel(best))))) {
                        best = b;
                        best_slacks = std::move(trial_slacks);
                    }
                    ctx->unbindBel(b);
                    break; // All legal slots in a LAB have identical ALUT-input / FF-ENA predictor delays.
                }
            }
            ctx->bindBel(best, c, strength);
            timing.run(true);
            ++sta_runs;
            NPNR_ASSERT(slacks() == best_slacks);
            if (best != previous) {
                moves << pass << '\t' << c->name.str(ctx) << '\t' << ctx->nameOfBel(previous) << '\t'
                      << ctx->nameOfBel(best) << '\t' << worst(current_slacks) << '\t' << worst(best_slacks) << '\n';
                current_slacks = best_slacks;
                ++accepted_moves;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    trials.close();
    moves.close();
    if (!trials || !moves)
        log_error("Cannot write refinement search evidence.\n");
    for (auto &e : ctx->cells)
        if (e.second->bel != BelId() && !ctx->isBelLocationValid(e.second->bel))
            log_error("Refinement leaves illegal cell %s.\n", e.first.c_str(ctx));
    TimingAnalyser after(ctx);
    after.setup(false, false, true);
    auto final_hold = refine_hold(after);
    if (!enable_replication_policy::hold_nonregressing(initial_hold, final_hold))
        log_error("Refinement predicted hold regression; abort before routing.\n");
    for (size_t i = 0; i < endpoints.size(); ++i)
        NPNR_ASSERT(after.get_setup_slack(endpoints[i]) == current_slacks[i] && current_slacks[i] >= initial_slacks[i]);
    NPNR_ASSERT(ctx->cells.size() == saved.size());
    for (auto &e : saved) {
        auto c = ctx->cells.at(e.first).get();
        auto &s = e.second;
        bool target = c == targets[0] || c == targets[1];
        NPNR_ASSERT((target || c->bel == s.bel) && c->type == s.type && c->belStrength == s.strength &&
                    c->params.size() == s.params.size() && c->attrs.size() == s.attrs.size() &&
                    c->ports.size() == s.ports.size() && c->pin_data.size() == s.pins.size());
        for (auto &p : s.params)
            NPNR_ASSERT(c->params.at(p.first) == p.second);
        for (auto &p : s.attrs)
            NPNR_ASSERT(c->attrs.at(p.first) == p.second);
        for (auto &p : s.pins)
            NPNR_ASSERT(c->get_pin_state(p.first) == p.second);
        for (auto &p : s.ports) {
            auto &a = c->ports.at(p.first);
            NPNR_ASSERT(std::make_tuple(a.net, a.type, c->get_pin_state(p.first), a.user_idx.idx()) == p.second);
        }
    }
    ctx->check();
    snapshot(".after.json");
    std::ofstream eps(std::string(prefix) + ".endpoint-slacks.tsv");
    eps << "cell\tport\tbefore_slack_ps\tafter_slack_ps\n";
    for (size_t i = 0; i < endpoints.size(); ++i)
        eps << endpoints[i].cell.str(ctx) << '\t' << endpoints[i].port.str(ctx) << '\t' << initial_slacks[i] << '\t'
            << current_slacks[i] << '\n';
    eps.close();
    if (!eps)
        log_error("Cannot write endpoint evidence.\n");
    std::ofstream audit(std::string(prefix) + ".audit.json");
    audit << "{\"radius\":6,\"max_passes\":2,\"sta_runs\":" << sta_runs << ",\"accepted_moves\":" << accepted_moves
          << ",\"before_hold_violations\":" << initial_hold.size() << ",\"after_hold_violations\":" << final_hold.size()
          << ",\"predicted_hold_nonregressing\":true,\"memory_clock_constraint_present\":"
          << (clock->clkconstr ? "true" : "false")
          << ",\"fresh_clock_required\":" << (require_clock_constraint ? "true" : "false") << ",\"targets\":[";
    for (size_t i = 0; i < targets.size(); ++i) {
        if (i)
            audit << ',';
        audit << "{\"cell\":" << std::quoted(targets[i]->name.str(ctx))
              << ",\"original_bel\":" << std::quoted(ctx->nameOfBel(origins[i]))
              << ",\"final_bel\":" << std::quoted(ctx->nameOfBel(targets[i]->bel)) << '}';
    }
    audit << "]}\n";
    audit.close();
    if (!audit)
        log_error("Cannot write refinement audit.\n");
    log_info("Timeout refinement: %d moves, %d STA trials, predicted worst setup %.0f -> %.0f ps.\n", accepted_moves,
             sta_runs, worst(initial_slacks), worst(current_slacks));
}
NEXTPNR_NAMESPACE_END
