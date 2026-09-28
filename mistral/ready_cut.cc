/* Fixture-specific Quartus-like ready-enable Boolean cut diagnostic.
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
#include "ready_cut_policy.h"
#include "timeout_refine_policy.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Lab = std::pair<int, int>;
using Hold = std::map<std::string, int>;
Hold cut_hold(TimingAnalyser &timing)
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
void diagnostic_ready_cut(Context *ctx, const char *prefix, bool require_clock_constraint)
{
    if (!prefix || !*prefix)
        return;
    const char *root_name = "hps_ddr.port1.slot_free_MISTRAL_ALUT3_B";
    auto root = ctx->cells.at(ctx->id(root_name)).get();
    auto keep = ctx->id("keep"), dont_touch = ctx->id("dont_touch");
    auto movable = [&](const CellInfo *c) {
        return c->bel != BelId() && c->belStrength <= STRENGTH_WEAK && c->cluster == ClusterId() && !c->region &&
               !c->isPseudo() && !c->attrs.count(keep) && !c->attrs.count(dont_touch);
    };
    auto checked = [&](CellInfo *c, IdString type, uint64_t mask, int count) {
        NPNR_ASSERT(c && c->type == type && uint64_t(c->params.at(id_LUT).as_int64()) == mask &&
                    int(c->ports.size()) == count + 1);
        for (auto &p : c->ports)
            NPNR_ASSERT(p.second.net && c->get_pin_state(p.first) == PIN_SIG);
        return c;
    };
    auto source = [&](CellInfo *c, IdString pin) {
        auto n = c->getPort(pin);
        NPNR_ASSERT(n && n->driver.cell && n->driver.port == id_Q);
        return n->driver.cell;
    };
    checked(root, id_MISTRAL_ALUT3, 0x32, 3);
    auto er = checked(source(root, id_A), id_MISTRAL_ALUT5, 1ULL << 17, 5);
    auto slot = checked(source(root, id_B), id_MISTRAL_ALUT2, 0xb, 2);
    auto ew = checked(source(root, id_C), id_MISTRAL_ALUT2, 0xe, 2);
    auto valid = checked(source(slot, id_B), id_MISTRAL_ALUT2, 0xe, 2);
    auto filler = checked(source(ew, id_A), id_MISTRAL_ALUT3, 0x20, 3);
    auto aw = checked(source(ew, id_B), id_MISTRAL_ALUT5, 1ULL << 5, 5);
    NPNR_ASSERT(er->getPort(id_A) == aw->getPort(id_A) && er->getPort(id_B) == aw->getPort(id_B) &&
                er->getPort(id_C) == aw->getPort(id_D) && er->getPort(id_D) == aw->getPort(id_E));
    NPNR_ASSERT(filler->getPort(id_B) == er->getPort(id_C) && filler->getPort(id_C) == er->getPort(id_D));
    auto ready = slot->getPort(id_A);
    NPNR_ASSERT(ready->driver.cell && ready->driver.cell->type == ctx->id("cyclonev_hps_interface_fpga2sdram") &&
                ready->driver.port == ctx->id("cmd_ready_1"));
    std::vector<NetInfo *> fc_inputs{er->getPort(id_A), er->getPort(id_B), er->getPort(id_C),
                                     er->getPort(id_D), er->getPort(id_E), aw->getPort(id_C)};
    std::vector<NetInfo *> root_inputs{valid->getPort(id_B), valid->getPort(id_A), filler->getPort(id_Q), nullptr,
                                       ready};
    for (auto n : fc_inputs)
        NPNR_ASSERT(n->driver.cell && n->driver.cell->type == id_MISTRAL_FF && n->driver.port == id_Q &&
                    n->driver.cell->get_pin_state(id_Q) == PIN_SIG);
    for (auto n : {root_inputs[0], root_inputs[1]})
        NPNR_ASSERT(n->driver.cell && n->driver.cell->type == id_MISTRAL_FF && n->driver.port == id_Q &&
                    n->driver.cell->get_pin_state(id_Q) == PIN_SIG);
    NPNR_ASSERT(movable(root) && movable(er));
    auto original = root->bel, fc_center = er->bel;
    auto strength = root->belStrength;
    auto output = root->getPort(id_Q);
    NPNR_ASSERT(output->users.entries() == 107);
    auto replica = ctx->cells.at(ctx->id(std::string(root_name) + "$enable_replica")).get();
    NPNR_ASSERT(replica->type == id_MISTRAL_ALUT3 && replica->getPort(id_Q)->users.entries() == 4);
    for (auto p : {id_A, id_B, id_C})
        NPNR_ASSERT(replica->getPort(p) == root->getPort(p));
    std::vector<CellPortKey> endpoints;
    NetInfo *clock = nullptr;
    CellPinState clock_state = PIN_SIG;
    for (auto user : output->users) {
        NPNR_ASSERT(user.cell->type == id_MISTRAL_FF && user.port == id_ENA && user.cell->bel != BelId());
        auto clk = user.cell->getPort(id_CLK);
        NPNR_ASSERT(clk && (!clock || (clk == clock && user.cell->get_pin_state(id_CLK) == clock_state)));
        clock = clk;
        clock_state = user.cell->get_pin_state(id_CLK);
        endpoints.emplace_back(user.cell->name, user.port);
    }
    std::sort(endpoints.begin(), endpoints.end(),
              [&](const CellPortKey &a, const CellPortKey &b) { return a.cell.str(ctx) < b.cell.str(ctx); });
    if (require_clock_constraint && !clock->clkconstr)
        log_error("Ready cut requires fresh memory clock constraints.\n");
    std::set<Lab> protected_labs;
    for (auto &e : ctx->cells)
        if (e.second->bel != BelId() && (!movable(e.second.get()) || e.second->type == id_MISTRAL_MLAB)) {
            auto l = ctx->getBelLocation(e.second->bel);
            protected_labs.insert({l.x, l.y});
        }
    for (auto &e : ctx->nets)
        NPNR_ASSERT(e.second->wires.empty());
    auto clone_id = ctx->id("ready_cut_port1_from_core"), net_id = ctx->id("ready_cut_port1_from_core$Q");
    NPNR_ASSERT(!ctx->cells.count(clone_id) && !ctx->nets.count(net_id) && !ctx->net_aliases.count(net_id));
    auto snapshot = [&](const char *suffix) {
        ctx->archInfoToAttributes();
        std::string path = std::string(prefix) + suffix;
        std::ofstream file(path);
        if (!file || !write_json_file(file, path, ctx))
            log_error("Cannot write ready cut snapshot.\n");
        file.close();
        if (!file)
            log_error("Cannot finish ready cut snapshot.\n");
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
            log_error("Cannot finish ready cut pin evidence.\n");
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
    std::map<NetInfo *, int> old_loads;
    for (auto n : fc_inputs)
        old_loads[n] = n->users.entries();
    for (auto n : root_inputs)
        if (n)
            old_loads[n] = n->users.entries();
    for (auto p : {id_A, id_B, id_C})
        old_loads[root->getPort(p)] = root->getPort(p)->users.entries();
    TimingAnalyser before(ctx);
    before.setup(false, false, true);
    auto old_hold = cut_hold(before);
    auto slacks = [&](TimingAnalyser &t) {
        std::vector<float> v;
        for (auto ep : endpoints) {
            auto s = t.get_setup_slack(ep);
            NPNR_ASSERT(std::isfinite(s));
            v.push_back(s);
        }
        return v;
    };
    auto initial_slacks = slacks(before);
    auto worst = [](const std::vector<float> &v) { return *std::min_element(v.begin(), v.end()); };
    auto fc = ctx->createCell(clone_id, id_MISTRAL_ALUT6);
    fc->params[id_LUT] = Property(ready_cut_policy::from_core_mask, 64);
    std::vector<IdString> pins{id_A, id_B, id_C, id_D, id_E, id_F};
    for (size_t i = 0; i < fc_inputs.size(); ++i) {
        fc->addInput(pins[i]);
        fc->connectPort(pins[i], fc_inputs[i]);
    }
    auto fc_net = ctx->createNet(net_id);
    fc->addOutput(id_Q);
    fc->connectPort(id_Q, fc_net);
    root_inputs[3] = fc_net;
    ctx->unbindBel(original);
    for (auto p : {id_A, id_B, id_C})
        root->disconnectPort(p);
    root->type = id_MISTRAL_ALUT5;
    root->params[id_LUT] = Property(ready_cut_policy::root_mask, 32);
    root->addInput(id_D);
    root->addInput(id_E);
    for (size_t i = 0; i < root_inputs.size(); ++i)
        root->connectPort(pins[i], root_inputs[i]);
    ctx->assignArchInfo();
    ctx->bindBel(original, root, strength);
    bool original_legal = ctx->isBelLocationValid(original);
    ctx->unbindBel(original);
    std::ofstream labs(std::string(prefix) + ".protected-labs.tsv");
    labs << "x\ty\n";
    for (auto l : protected_labs)
        labs << l.first << '\t' << l.second << '\n';
    labs.close();
    if (!labs)
        log_error("Cannot write protected LABs.\n");
    auto domain = [&](CellInfo *c, BelId center) {
        auto p = ctx->getBelLocation(center);
        std::map<Lab, std::vector<BelId>> result;
        for (auto b : ctx->getBels()) {
            auto l = ctx->getBelLocation(b);
            if (std::abs(l.x - p.x) + std::abs(l.y - p.y) > 6 || protected_labs.count({l.x, l.y}) ||
                !ctx->checkBelAvail(b) || !ctx->isValidBelForCellType(c->type, b))
                continue;
            result[{l.x, l.y}].push_back(b);
        }
        for (auto &g : result)
            std::sort(g.second.begin(), g.second.end(), [&](BelId a, BelId b) {
                return std::string(ctx->nameOfBel(a)) < std::string(ctx->nameOfBel(b));
            });
        return result;
    };
    std::ofstream geometry(std::string(prefix) + ".geometry.tsv");
    geometry << "cell\tbel\tcenter_x\tcenter_y\tscore_ps\n";
    auto initialize = [&](CellInfo *c, BelId center, bool anchor) {
        BelId best;
        int best_score = std::numeric_limits<int>::max();
        for (auto &g : domain(c, center))
            for (auto b : g.second) {
                ctx->bindBel(b, c, STRENGTH_WEAK);
                if (!ctx->isBelLocationValid(b)) {
                    ctx->unbindBel(b);
                    continue;
                }
                int in = 0, out = 0;
                for (auto &p : c->ports)
                    if (p.second.type == PORT_IN)
                        in = std::max(in, int(ctx->predictArcDelay(p.second.net, {c, p.first})));
                if (anchor)
                    out = ctx->predictDelay(b, id_COMBOUT, original, id_D);
                else
                    for (auto user : c->getPort(id_Q)->users)
                        out = std::max(out, int(ctx->predictArcDelay(c->getPort(id_Q), user)));
                int score = in + out;
                if (best == BelId() || score < best_score ||
                    (score == best_score && std::string(ctx->nameOfBel(b)) < std::string(ctx->nameOfBel(best)))) {
                    best = b;
                    best_score = score;
                }
                ctx->unbindBel(b);
                break;
            }
        if (best == BelId())
            log_error("No legal ready-cut initialization site for %s; abort before routing.\n", c->name.c_str(ctx));
        ctx->bindBel(best, c, STRENGTH_WEAK);
        auto l = ctx->getBelLocation(center);
        geometry << c->name.str(ctx) << '\t' << ctx->nameOfBel(best) << '\t' << l.x << '\t' << l.y << '\t' << best_score
                 << '\n';
    };
    initialize(fc, fc_center, true);
    initialize(root, original, false);
    geometry.close();
    if (!geometry)
        log_error("Cannot write initialization evidence.\n");
    TimingAnalyser timing(ctx);
    timing.setup();
    auto current_slacks = slacks(timing);
    int sta_runs = 0, accepted_moves = 0;
    std::ofstream trials(std::string(prefix) + ".trials.tsv"), moves(std::string(prefix) + ".moves.tsv");
    trials << "phase\tpass\tcell\tbel\timproves\tworst_ps\n";
    moves << "pass\tcell\told_bel\tnew_bel\tbefore_worst_ps\tafter_worst_ps\n";
    std::vector<CellInfo *> targets{root, fc};
    std::vector<BelId> centers{original, fc_center};
    for (int pass = 0; pass < 2; ++pass) {
        bool changed = false;
        for (size_t i = 0; i < targets.size(); ++i) {
            auto c = targets[i];
            auto previous = c->bel;
            auto best = previous;
            auto best_slacks = current_slacks;
            ctx->unbindBel(previous);
            for (auto &g : domain(c, centers[i]))
                for (auto b : g.second) {
                    ctx->bindBel(b, c, STRENGTH_WEAK);
                    if (!ctx->isBelLocationValid(b)) {
                        ctx->unbindBel(b);
                        continue;
                    }
                    timing.run(true);
                    ++sta_runs;
                    auto trial_slacks = slacks(timing);
                    bool improves = timeout_refine_policy::improves(current_slacks, trial_slacks);
                    trials << "refine\t" << pass << '\t' << c->name.str(ctx) << '\t' << ctx->nameOfBel(b) << '\t'
                           << improves << '\t' << worst(trial_slacks) << '\n';
                    if (improves && (worst(trial_slacks) > worst(best_slacks) ||
                                     (worst(trial_slacks) == worst(best_slacks) &&
                                      std::string(ctx->nameOfBel(b)) < std::string(ctx->nameOfBel(best))))) {
                        best = b;
                        best_slacks = std::move(trial_slacks);
                    }
                    ctx->unbindBel(b);
                    break;
                }
            ctx->bindBel(best, c, STRENGTH_WEAK);
            timing.run(true);
            ++sta_runs;
            NPNR_ASSERT(slacks(timing) == best_slacks);
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
    moves.close();
    trials.close();
    if (!moves || !trials)
        log_error("Cannot write ready-cut search evidence.\n");
    if (!timeout_refine_policy::improves(initial_slacks, current_slacks))
        log_error("Ready cut lacks strict nonregressing setup improvement over original; abort before routing.\n");
    for (auto &e : ctx->cells)
        if (e.second->bel != BelId() && !ctx->isBelLocationValid(e.second->bel))
            log_error("Ready cut leaves illegal cell %s.\n", e.first.c_str(ctx));
    TimingAnalyser after(ctx);
    after.setup(false, false, true);
    auto new_hold = cut_hold(after);
    if (!enable_replication_policy::hold_nonregressing(old_hold, new_hold))
        log_error("Ready cut predicted hold regression; abort before routing.\n");
    NPNR_ASSERT(slacks(after) == current_slacks && ctx->cells.size() == saved.size() + 1 &&
                root->getPort(id_Q) == output && output->users.entries() == 107);
    for (auto &e : saved) {
        auto c = ctx->cells.at(e.first).get();
        auto &s = e.second;
        NPNR_ASSERT(c->attrs.size() == s.attrs.size() && c->belStrength == s.strength);
        for (auto &a : s.attrs)
            NPNR_ASSERT(c->attrs.at(a.first) == a.second);
        if (c != root) {
            NPNR_ASSERT(c->bel == s.bel && c->type == s.type && c->params.size() == s.params.size() &&
                        c->ports.size() == s.ports.size() && c->pin_data.size() == s.pins.size());
            for (auto &p : s.params)
                NPNR_ASSERT(c->params.at(p.first) == p.second);
        }
        for (auto &p : s.ports) {
            if (c == root && p.first != id_Q)
                continue;
            auto &a = c->ports.at(p.first);
            NPNR_ASSERT(std::make_tuple(a.net, a.type, c->get_pin_state(p.first), a.user_idx.idx()) == p.second);
        }
        for (auto &p : s.pins)
            NPNR_ASSERT(c->get_pin_state(p.first) == p.second);
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
        log_error("Cannot write ready-cut endpoint evidence.\n");
    std::ofstream loads(std::string(prefix) + ".loads.tsv");
    loads << "source\tport\tbefore_users\tafter_users\n";
    for (auto &p : old_loads)
        loads << p.first->driver.cell->name.str(ctx) << '\t' << p.first->driver.port.str(ctx) << '\t' << p.second
              << '\t' << p.first->users.entries() << '\n';
    loads << fc->name.str(ctx) << "\tQ\t0\t1\n";
    loads.close();
    if (!loads)
        log_error("Cannot write ready-cut load evidence.\n");
    std::ofstream audit(std::string(prefix) + ".audit.json");
    audit << "{\"original_root_bel\":" << std::quoted(ctx->nameOfBel(original))
          << ",\"original_root_site_legal\":" << (original_legal ? "true" : "false")
          << ",\"root_final_bel\":" << std::quoted(ctx->nameOfBel(root->bel))
          << ",\"from_core_bel\":" << std::quoted(ctx->nameOfBel(fc->bel))
          << ",\"from_core_center_bel\":" << std::quoted(ctx->nameOfBel(fc_center))
          << ",\"radius\":6,\"max_passes\":2,\"sta_runs\":" << sta_runs << ",\"accepted_moves\":" << accepted_moves
          << ",\"before_hold_violations\":" << old_hold.size() << ",\"after_hold_violations\":" << new_hold.size()
          << ",\"predicted_hold_nonregressing\":true,\"memory_clock_constraint_present\":"
          << (clock->clkconstr ? "true" : "false")
          << ",\"fresh_clock_required\":" << (require_clock_constraint ? "true" : "false")
          << ",\"intentional_extra_hps_ready_load\":1}\n";
    audit.close();
    if (!audit)
        log_error("Cannot write ready-cut audit.\n");
    log_info("Ready cut: original site legal %d, %d moves, %d STA runs, worst setup %.0f -> %.0f ps.\n", original_legal,
             accepted_moves, sta_runs, worst(initial_slacks), worst(current_slacks));
}
NEXTPNR_NAMESPACE_END
