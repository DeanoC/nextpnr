/* Fixture-specific command-enable Boolean cut diagnostic.
 * SPDX-License-Identifier: ISC
 */
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include "command_cut_policy.h"
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
Hold command_hold(TimingAnalyser &timing)
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
void diagnostic_command_cut(Context *ctx, const char *prefix, bool require_clock_constraint)
{
    if (!prefix || !*prefix)
        return;
    auto root = ctx->cells.at(ctx->id("hps_ddr.port1.slot_free_MISTRAL_ALUT4_C")).get();
    auto keep = ctx->id("keep"), dont_touch = ctx->id("dont_touch");
    auto movable = [&](const CellInfo *c) {
        return c->bel != BelId() && c->belStrength == STRENGTH_WEAK && c->cluster == ClusterId() && !c->region &&
               !c->isPseudo() && !c->attrs.count(keep) && !c->attrs.count(dont_touch);
    };
    auto checked = [&](CellInfo *c, IdString type, uint64_t mask, int count) {
        NPNR_ASSERT(c && c->type == type && uint64_t(c->params.at(id_LUT).as_int64()) == mask &&
                    int(c->ports.size()) == count + 1);
        for (auto &p : c->ports)
            NPNR_ASSERT(p.second.net && c->get_pin_state(p.first) == PIN_SIG);
        return c;
    };
    auto source = [&](CellInfo *c, IdString p) {
        auto n = c->getPort(p);
        NPNR_ASSERT(n && n->driver.cell && n->driver.port == id_Q);
        return n->driver.cell;
    };
    checked(root, id_MISTRAL_ALUT4, 0xf0e0, 4);
    NPNR_ASSERT(movable(root));
    auto skid = root->getPort(id_A);
    auto er = checked(source(root, id_B), id_MISTRAL_ALUT5, 1ULL << 17, 5);
    auto slot = checked(source(root, id_C), id_MISTRAL_ALUT2, 0xb, 2);
    auto ew = checked(source(root, id_D), id_MISTRAL_ALUT2, 0xe, 2);
    auto valid = checked(source(slot, id_B), id_MISTRAL_ALUT2, 0xe, 2);
    auto filler = checked(source(ew, id_A), id_MISTRAL_ALUT3, 0x20, 3);
    auto aw = checked(source(ew, id_B), id_MISTRAL_ALUT5, 1ULL << 5, 5);
    auto fc = checked(ctx->cells.at(ctx->id("ready_cut_port1_from_core")).get(), id_MISTRAL_ALUT6,
                      ready_cut_policy::from_core_mask, 6);
    auto ready = slot->getPort(id_A);
    auto hps = ready->driver.cell;
    NPNR_ASSERT(hps && hps->type == ctx->id("cyclonev_hps_interface_fpga2sdram") &&
                ready->driver.port == ctx->id("cmd_ready_1"));
    NPNR_ASSERT(er->getPort(id_A) == aw->getPort(id_A) && er->getPort(id_B) == aw->getPort(id_B) &&
                er->getPort(id_C) == aw->getPort(id_D) && er->getPort(id_D) == aw->getPort(id_E));
    NPNR_ASSERT(skid == er->getPort(id_C) && filler->getPort(id_B) == skid &&
                filler->getPort(id_C) == er->getPort(id_D));
    std::vector<IdString> pins{id_A, id_B, id_C, id_D, id_E, id_F};
    std::vector<NetInfo *> expected_fc{er->getPort(id_A), er->getPort(id_B), er->getPort(id_C),
                                       er->getPort(id_D), er->getPort(id_E), aw->getPort(id_C)};
    for (size_t i = 0; i < expected_fc.size(); ++i)
        NPNR_ASSERT(fc->getPort(pins[i]) == expected_fc[i]);
    auto skid_root = checked(ctx->cells.at(ctx->id("hps_ddr.port1.slot_free_MISTRAL_ALUT3_B")).get(), id_MISTRAL_ALUT5,
                             ready_cut_policy::root_mask, 5);
    NPNR_ASSERT(skid_root->getPort(id_Q)->users.entries() == 107 && skid_root->getPort(id_C) == filler->getPort(id_Q) &&
                skid_root->getPort(id_D) == fc->getPort(id_Q) && skid_root->getPort(id_E) == ready);
    NPNR_ASSERT(valid->getPort(id_A) == skid_root->getPort(id_B) && valid->getPort(id_B) == skid_root->getPort(id_A));
    auto output = root->getPort(id_Q);
    NPNR_ASSERT(output->users.entries() == 109);
    std::vector<NetInfo *> new_inputs{valid->getPort(id_Q), ready, skid, filler->getPort(id_Q), fc->getPort(id_Q)};
    std::set<NetInfo *> seeds;
    std::map<NetInfo *, int> old_loads;
    for (auto &p : root->ports) {
        seeds.insert(p.second.net);
        if (p.second.type == PORT_IN)
            old_loads[p.second.net] = p.second.net->users.entries();
    }
    for (auto n : new_inputs) {
        seeds.insert(n);
        old_loads[n] = n->users.entries();
    }
    NPNR_ASSERT(seeds.size() == 9);
    auto ordinary = [](IdString t) {
        return t.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6);
    };
    auto closure = [&]() {
        std::set<CellPortKey> result;
        std::set<NetInfo *> active, done;
        std::function<void(NetInfo *)> walk = [&](NetInfo *n) {
            if (active.count(n))
                log_error("Cycle in command-cut fanout closure.\n");
            if (done.count(n))
                return;
            active.insert(n);
            for (auto user : n->users) {
                int count = 0;
                NPNR_ASSERT(ctx->getPortTimingClass(user.cell, user.port, count) != TMG_CLOCK_INPUT);
                if (ordinary(user.cell->type)) {
                    NPNR_ASSERT(user.cell->getPort(id_Q));
                    walk(user.cell->getPort(id_Q));
                } else {
                    NPNR_ASSERT(user.cell->type == id_MISTRAL_FF ||
                                (user.cell == hps && user.port == ctx->id("cmd_valid_1")));
                    result.emplace(user.cell->name, user.port);
                }
            }
            active.erase(n);
            done.insert(n);
        };
        for (auto n : seeds)
            walk(n);
        return result;
    };
    auto endpoint_set = closure();
    NPNR_ASSERT(endpoint_set.size() == 720);
    std::vector<CellPortKey> endpoints(endpoint_set.begin(), endpoint_set.end());
    std::sort(endpoints.begin(), endpoints.end(), [&](const CellPortKey &a, const CellPortKey &b) {
        return std::make_pair(a.cell.str(ctx), a.port.str(ctx)) < std::make_pair(b.cell.str(ctx), b.port.str(ctx));
    });
    std::set<CellPortKey> command_set;
    for (auto u : output->users) {
        NPNR_ASSERT(u.cell->type == id_MISTRAL_FF && u.port == id_ENA);
        command_set.emplace(u.cell->name, u.port);
    }
    std::vector<unsigned> command_indices;
    NetInfo *clock = nullptr;
    CellPinState clock_state = PIN_SIG;
    int ena = 0, data = 0, sclr = 0, hard = 0;
    for (unsigned i = 0; i < endpoints.size(); ++i) {
        auto ep = endpoints[i];
        auto c = ctx->cells.at(ep.cell).get();
        if (command_set.count(ep))
            command_indices.push_back(i);
        if (c->type == id_MISTRAL_FF) {
            auto clk = c->getPort(id_CLK);
            NPNR_ASSERT(clk && (!clock || (clk == clock && c->get_pin_state(id_CLK) == clock_state)));
            clock = clk;
            clock_state = c->get_pin_state(id_CLK);
            ena += ep.port == id_ENA;
            data += ep.port == id_DATAIN;
            sclr += ep.port == id_SCLR;
            NPNR_ASSERT(ep.port.in(id_ENA, id_DATAIN, id_SCLR));
        } else
            ++hard;
    }
    NPNR_ASSERT(command_indices.size() == 109 && ena == 497 && data == 197 && sclr == 25 && hard == 1);
    int hps_clock_count = 0;
    NPNR_ASSERT(ctx->getPortTimingClass(hps, ctx->id("cmd_valid_1"), hps_clock_count) == TMG_REGISTER_INPUT &&
                hps_clock_count == 1);
    auto hps_clock_info = ctx->getPortClockingInfo(hps, ctx->id("cmd_valid_1"), 0);
    NPNR_ASSERT(hps_clock_info.clock_port == ctx->id("cmd_port_clk_1") &&
                hps->getPort(hps_clock_info.clock_port) == clock && hps_clock_info.edge == RISING_EDGE &&
                hps->get_pin_state(hps_clock_info.clock_port) == PIN_SIG && clock_state == PIN_SIG);
    if (require_clock_constraint && !clock->clkconstr)
        log_error("Command cut requires fresh memory clock constraints.\n");
    std::set<Lab> protected_labs;
    for (auto &e : ctx->cells)
        if (e.second->bel != BelId() && (!movable(e.second.get()) || e.second->type == id_MISTRAL_MLAB)) {
            auto l = ctx->getBelLocation(e.second->bel);
            protected_labs.insert({l.x, l.y});
        }
    for (auto &e : ctx->nets)
        NPNR_ASSERT(e.second->wires.empty());
    auto original = root->bel;
    auto center = ctx->getBelLocation(original);
    auto strength = root->belStrength;
    auto snapshot = [&](const char *suffix) {
        ctx->archInfoToAttributes();
        std::string path = std::string(prefix) + suffix;
        std::ofstream file(path);
        if (!file || !write_json_file(file, path, ctx))
            log_error("Cannot write command cut snapshot.\n");
        file.close();
        if (!file)
            log_error("Cannot finish command cut snapshot.\n");
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
            log_error("Cannot finish command cut pin evidence.\n");
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

    std::ofstream seed_file(std::string(prefix) + ".closure-seeds.tsv"),
            labs(std::string(prefix) + ".protected-labs.tsv");
    seed_file << "source\tport\n";
    std::set<std::pair<std::string, std::string>> seed_names;
    for (auto n : seeds) {
        NPNR_ASSERT(n->driver.cell);
        seed_names.insert({n->driver.cell->name.str(ctx), n->driver.port.str(ctx)});
    }
    for (auto p : seed_names)
        seed_file << p.first << '\t' << p.second << '\n';
    seed_file.close();
    labs << "x\ty\n";
    for (auto l : protected_labs)
        labs << l.first << '\t' << l.second << '\n';
    labs.close();
    if (!seed_file || !labs)
        log_error("Cannot write command-cut closure evidence.\n");
    auto slacks = [&](TimingAnalyser &t) {
        std::vector<float> v;
        for (auto ep : endpoints) {
            float s = t.get_setup_slack(ep);
            if (!std::isfinite(s) || s >= float(std::numeric_limits<delay_t>::max()) ||
                s <= float(std::numeric_limits<delay_t>::lowest()))
                log_error("Untimed command-cut endpoint %s.%s; abort.\n", ep.cell.c_str(ctx), ep.port.c_str(ctx));
            v.push_back(s);
        }
        return v;
    };
    auto worst_command = [&](const std::vector<float> &v) {
        float result = std::numeric_limits<float>::infinity();
        for (auto i : command_indices)
            result = std::min(result, v[i]);
        return result;
    };
    TimingAnalyser before(ctx);
    before.setup(false, false, true);
    auto before_slacks = slacks(before);
    auto old_hold = command_hold(before);
    ctx->unbindBel(original);
    for (auto p : {id_A, id_B, id_C, id_D})
        root->disconnectPort(p);
    root->type = id_MISTRAL_ALUT5;
    root->params[id_LUT] = Property(command_cut_policy::root_mask, 32);
    root->addInput(id_E);
    for (size_t i = 0; i < new_inputs.size(); ++i)
        root->connectPort(pins[i], new_inputs[i]);
    ctx->assignArchInfo();
    ctx->bindBel(original, root, strength);
    bool original_legal = ctx->isBelLocationValid(original);
    NPNR_ASSERT(closure() == endpoint_set);
    TimingAnalyser timing(ctx);
    timing.setup();
    ctx->unbindBel(original);
    std::map<Lab, std::vector<BelId>> candidates;
    for (auto b : ctx->getBels()) {
        auto l = ctx->getBelLocation(b);
        if (std::abs(l.x - center.x) + std::abs(l.y - center.y) > 6 || protected_labs.count({l.x, l.y}) ||
            !ctx->checkBelAvail(b) || !ctx->isValidBelForCellType(root->type, b))
            continue;
        candidates[{l.x, l.y}].push_back(b);
    }
    BelId best;
    std::vector<float> best_slacks;
    int sta_runs = 0;
    std::ofstream trials(std::string(prefix) + ".trials.tsv");
    trials << "bel\timproves\tcommand_worst_ps\tguard_min_delta_ps\n";
    for (auto &g : candidates) {
        std::sort(g.second.begin(), g.second.end(),
                  [&](BelId a, BelId b) { return std::string(ctx->nameOfBel(a)) < std::string(ctx->nameOfBel(b)); });
        for (auto b : g.second) {
            ctx->bindBel(b, root, strength);
            if (!ctx->isBelLocationValid(b)) {
                ctx->unbindBel(b);
                continue;
            }
            timing.run(true);
            ++sta_runs;
            auto candidate = slacks(timing);
            bool improves = command_cut_policy::improves(before_slacks, candidate, command_indices);
            float min_delta = std::numeric_limits<float>::infinity();
            for (size_t i = 0; i < candidate.size(); ++i)
                min_delta = std::min(min_delta, candidate[i] - before_slacks[i]);
            trials << ctx->nameOfBel(b) << '\t' << improves << '\t' << worst_command(candidate) << '\t' << min_delta
                   << '\n';
            if (improves && (best == BelId() || worst_command(candidate) > worst_command(best_slacks) ||
                             (worst_command(candidate) == worst_command(best_slacks) &&
                              std::string(ctx->nameOfBel(b)) < std::string(ctx->nameOfBel(best))))) {
                best = b;
                best_slacks = std::move(candidate);
            }
            ctx->unbindBel(b);
            break;
        }
    }
    trials.close();
    if (!trials)
        log_error("Cannot write command-cut trials.\n");
    if (best == BelId())
        log_error("No legal command-cut site improves command timing without regressing any closure endpoint; abort "
                  "before routing.\n");
    ctx->bindBel(best, root, strength);
    for (auto &e : ctx->cells)
        if (e.second->bel != BelId() && !ctx->isBelLocationValid(e.second->bel))
            log_error("Command cut leaves illegal cell %s.\n", e.first.c_str(ctx));
    TimingAnalyser after(ctx);
    after.setup(false, false, true);
    auto final_slacks = slacks(after);
    auto new_hold = command_hold(after);
    NPNR_ASSERT(final_slacks == best_slacks &&
                command_cut_policy::improves(before_slacks, final_slacks, command_indices));
    if (!enable_replication_policy::hold_nonregressing(old_hold, new_hold))
        log_error("Command cut predicted hold regression; abort before routing.\n");
    NPNR_ASSERT(ctx->cells.size() == saved.size() && root->getPort(id_Q) == output && output->users.entries() == 109);
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
    eps << "cell\tport\tcell_type\tcommand\tbefore_slack_ps\tafter_slack_ps\n";
    for (size_t i = 0; i < endpoints.size(); ++i) {
        auto ep = endpoints[i];
        eps << ep.cell.str(ctx) << '\t' << ep.port.str(ctx) << '\t' << ctx->cells.at(ep.cell)->type.str(ctx) << '\t'
            << command_set.count(ep) << '\t' << before_slacks[i] << '\t' << final_slacks[i] << '\n';
    }
    eps.close();
    if (!eps)
        log_error("Cannot write command endpoints.\n");
    std::ofstream loads(std::string(prefix) + ".loads.tsv");
    loads << "source\tport\tbefore_users\tafter_users\n";
    for (auto &p : old_loads)
        loads << p.first->driver.cell->name.str(ctx) << '\t' << p.first->driver.port.str(ctx) << '\t' << p.second
              << '\t' << p.first->users.entries() << '\n';
    loads.close();
    if (!loads)
        log_error("Cannot write command loads.\n");
    std::ofstream audit(std::string(prefix) + ".audit.json");
    audit << "{\"original_root_bel\":" << std::quoted(ctx->nameOfBel(original))
          << ",\"original_root_site_legal\":" << (original_legal ? "true" : "false")
          << ",\"root_final_bel\":" << std::quoted(ctx->nameOfBel(best)) << ",\"radius\":6,\"sta_runs\":" << sta_runs
          << ",\"closure_seed_count\":9,\"endpoint_count\":720,\"command_count\":109,\"FF_ENA\":497,\"FF_DATAIN\":197,"
             "\"FF_SCLR\":25,\"HPS_cmd_valid\":1,\"before_hold_violations\":"
          << old_hold.size() << ",\"after_hold_violations\":" << new_hold.size()
          << ",\"predicted_hold_nonregressing\":true,\"memory_clock_constraint_present\":"
          << (clock->clkconstr ? "true" : "false")
          << ",\"hps_capture_clock_port\":\"cmd_port_clk_1\",\"hps_capture_clock_matches_memory\":true,\"fresh_clock_"
             "required\":"
          << (require_clock_constraint ? "true" : "false") << ",\"intentional_extra_hps_ready_load\":1}\n";
    audit.close();
    if (!audit)
        log_error("Cannot write command audit.\n");
    log_info("Command cut: original site legal %d, %d STA runs, command worst setup %.0f -> %.0f ps; 720 guarded "
             "endpoints.\n",
             original_legal, sta_runs, worst_command(before_slacks), worst_command(final_slacks));
}
NEXTPNR_NAMESPACE_END
