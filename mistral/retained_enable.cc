/* Single measured-critical-source diagnostic, not an iterative replication pass.
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
#include "jsonwrite.h"
#include "log.h"
#include "nextpnr.h"
#include "retained_enable_policy.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Lab = std::pair<int, int>;
using Hold = std::map<std::string, int>;
const char *target_name = "ddr1_nack_MISTRAL_FF_Q_ENA_MISTRAL_ALUT2_Q_A_MISTRAL_ALUT2_B_Q_MISTRAL_ALUT4_D";
const char *critical_name = "ddr1_test.burst_end_MISTRAL_FF_Q";
const char *prior_name = "hps_ddr.port1.slot_free_MISTRAL_ALUT3_B$enable_replica";
Hold retained_hold(TimingAnalyser &timing)
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
void diagnostic_retained_enable(Context *ctx, const char *prefix, bool require_clock_constraint)
{
    if (!prefix || !*prefix)
        return;
    auto ordinary = [](IdString type) {
        return type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6);
    };
    auto keep = ctx->id("keep"), dont_touch = ctx->id("dont_touch");
    auto movable = [&](const CellInfo *c) {
        return c->bel != BelId() && c->belStrength <= STRENGTH_WEAK && c->cluster == ClusterId() && !c->region &&
               !c->isPseudo() && !c->attrs.count(keep) && !c->attrs.count(dont_touch);
    };
    auto lab = [&](const CellInfo *c) {
        auto l = ctx->getBelLocation(c->bel);
        return Lab(l.x, l.y);
    };
    std::set<Lab> protected_labs;
    for (const auto &e : ctx->cells) {
        auto c = e.second.get();
        if (c->bel != BelId() && (!movable(c) || c->type == id_MISTRAL_MLAB))
            protected_labs.insert(lab(c));
    }
    auto ordinary_lab = [&](BelId bel) {
        auto t = ctx->getBelType(bel);
        auto l = ctx->getBelLocation(bel);
        return t.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF) && !protected_labs.count({l.x, l.y});
    };
    std::set<const NetInfo *> boundary;
    for (auto &p : ctx->ports)
        if (p.second.net)
            boundary.insert(p.second.net);
    auto ordinary_net = [&](const NetInfo *net) {
        return net && !net->is_global && !net->clkconstr && !net->region && net->wires.empty() &&
               net->constant_value == IdString() && !boundary.count(net) && !net->attrs.count(keep) &&
               !net->attrs.count(dont_touch);
    };
    auto mapped = [&](const CellInfo *c, IdString pin) {
        auto p = ctx->getBelPinsForCellPin(c, pin);
        return p.begin() != p.end();
    };
    CellInfo *driver = ctx->cells.at(ctx->id(target_name)).get(),
             *critical = ctx->cells.at(ctx->id(critical_name)).get();
    NPNR_ASSERT(driver->type == id_MISTRAL_ALUT4 && driver->params.at(id_LUT).as_int64() == 0xe400 && movable(driver) &&
                ordinary_lab(driver->bel));
    NPNR_ASSERT(driver->ports.size() == 5);
    auto old_net = driver->getPort(id_Q);
    NPNR_ASSERT(ordinary_net(old_net) && old_net->users.entries() == 106);
    NPNR_ASSERT(critical->type == id_MISTRAL_FF && critical->getPort(id_ENA) == old_net && movable(critical) &&
                ordinary_lab(critical->bel));
    // Require the previously qualified timeout mapping to be present.
    int timeout_cells = 0;
    for (auto &e : ctx->cells)
        if (e.first.str(ctx).find("timeout_partition_ch") == 0)
            ++timeout_cells;
    NPNR_ASSERT(timeout_cells == 41);
    std::map<Lab, std::vector<PortRef>> groups;
    NetInfo *clock = nullptr;
    CellPinState clock_state = PIN_SIG;
    int protected_users = 0;
    for (auto user : old_net->users) {
        NPNR_ASSERT(user.cell->type == id_MISTRAL_FF && user.port == id_ENA && movable(user.cell) &&
                    mapped(user.cell, user.port));
        auto clk = user.cell->getPort(id_CLK);
        NPNR_ASSERT(clk && (!clock || (clock == clk && clock_state == user.cell->get_pin_state(id_CLK))));
        clock = clk;
        clock_state = user.cell->get_pin_state(id_CLK);
        groups[lab(user.cell)].push_back(user);
        if (protected_labs.count(lab(user.cell)))
            ++protected_users;
    }
    NPNR_ASSERT(groups.size() == 21 && protected_users == 28);
    if (require_clock_constraint && !clock->clkconstr)
        log_error("Retained enable diagnostic requires fresh memory clock constraints.\n");
    std::set<const NetInfo *> prior_inputs;
    int existing_copies = 0;
    for (auto &e : ctx->cells)
        if (e.first.str(ctx).find("$enable_replica") != std::string::npos) {
            ++existing_copies;
            NPNR_ASSERT(e.first.str(ctx) == prior_name && ordinary(e.second->type));
            NPNR_ASSERT(e.second->getPort(id_Q)->users.entries() == 4);
            for (auto &p : e.second->ports)
                if (p.second.type == PORT_IN && p.second.net)
                    prior_inputs.insert(p.second.net);
        }
    NPNR_ASSERT(existing_copies == 1);
    int configured_budget = ctx->setting<int>("mistral/replicateEnables", 0);
    NPNR_ASSERT(retained_enable_policy::budget_available(configured_budget, existing_copies));
    std::vector<IdString> input_pins{id_A, id_B, id_C, id_D};
    std::vector<int> old_inputs;
    std::set<const NetInfo *> inputs;
    std::map<IdString, int> input_user_counts;
    for (auto pin : input_pins) {
        auto net = driver->getPort(pin);
        NPNR_ASSERT(driver->get_pin_state(pin) == PIN_SIG && ordinary_net(net) && net->driver.cell &&
                    net->driver.cell != driver && mapped(driver, pin));
        NPNR_ASSERT(inputs.insert(net).second);
        auto source = net->driver.cell;
        NPNR_ASSERT((ordinary(source->type) || (source->type == id_MISTRAL_FF && net->driver.port == id_Q)) &&
                    movable(source) && ordinary_lab(source->bel) && mapped(source, net->driver.port));
        for (auto user : net->users) {
            int count = 0;
            NPNR_ASSERT((ordinary(user.cell->type) || user.cell->type == id_MISTRAL_FF) &&
                        ctx->getPortTimingClass(user.cell, user.port, count) != TMG_CLOCK_INPUT);
        }
        old_inputs.push_back(ctx->predictArcDelay(net, {driver, pin}));
        input_user_counts[pin] = net->users.entries();
    }
    NPNR_ASSERT(retained_enable_policy::disjoint(inputs, prior_inputs));
    auto clone_id = ctx->idf("%s$retained_enable_replica", target_name), net_id = ctx->idf("%s$Q", clone_id.c_str(ctx));
    NPNR_ASSERT(!ctx->cells.count(clone_id) && !ctx->nets.count(net_id) && !ctx->net_aliases.count(net_id));
    for (auto &e : ctx->nets)
        NPNR_ASSERT(e.second->wires.empty());
    auto snapshot = [&](const char *suffix) {
        ctx->archInfoToAttributes();
        std::string path = std::string(prefix) + suffix;
        std::ofstream file(path);
        if (!file || !write_json_file(file, path, ctx))
            log_error("Cannot write retained enable snapshot.\n");
        file.close();
        if (!file)
            log_error("Cannot finish retained enable snapshot.\n");
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
            log_error("Cannot finish retained enable pin evidence.\n");
    };
    snapshot(".before.json");
    std::ofstream labs(std::string(prefix) + ".protected-labs.tsv");
    labs << "x\ty\n";
    for (auto p : protected_labs)
        labs << p.first << '\t' << p.second << '\n';
    labs.close();
    if (!labs)
        log_error("Cannot write protected LAB evidence.\n");
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
    TimingAnalyser before(ctx);
    before.setup(false, false, true);
    auto old_hold = retained_hold(before);
    float critical_slack = before.get_setup_slack(CellPortKey(critical->name, id_ENA));
    float criticality = before.get_criticality(CellPortKey(critical->name, id_ENA));
    auto clone = ctx->createCell(clone_id, driver->type);
    clone->params = driver->params;
    clone->attrs = driver->attrs;
    for (auto name : {"NEXTPNR_BEL", "BEL_STRENGTH", "FES_PINMAP_V1"})
        clone->attrs.erase(ctx->id(name));
    clone->pin_data = driver->pin_data;
    for (auto &p : driver->ports)
        if (p.second.type == PORT_IN)
            driver->copyPortTo(p.first, clone, p.first);
    auto new_net = ctx->createNet(net_id);
    clone->addOutput(id_Q);
    clone->connectPort(id_Q, new_net);
    ctx->assignArchInfo();
    auto center = lab(critical);
    BelId best;
    int best_gain = 0;
    std::vector<PortRef> moved;
    for (auto trial : ctx->getBels()) {
        auto l = ctx->getBelLocation(trial);
        if (std::abs(l.x - center.first) + std::abs(l.y - center.second) > 3 || !ctx->checkBelAvail(trial) ||
            !ctx->isValidBelForCellType(clone->type, trial) || !ordinary_lab(trial))
            continue;
        ctx->bindBel(trial, clone, STRENGTH_WEAK);
        if (ctx->isBelLocationValid(trial)) {
            std::vector<int> delays;
            for (auto pin : input_pins)
                delays.push_back(ctx->predictArcDelay(clone->getPort(pin), {clone, pin}));
            if (enable_replication_policy::inputs_nonregressing(old_inputs, delays)) {
                std::vector<PortRef> selected;
                int gain = std::numeric_limits<int>::max();
                bool includes_critical = false;
                for (auto &group : groups) {
                    std::vector<int> old_outputs, new_outputs;
                    for (auto user : group.second) {
                        old_outputs.push_back(ctx->predictArcDelay(old_net, user));
                        new_outputs.push_back(ctx->predictArcDelay(new_net, user));
                    }
                    int group_gain = retained_enable_policy::group_gain(protected_labs.count(group.first), old_outputs,
                                                                        new_outputs);
                    if (!group_gain)
                        continue;
                    gain = std::min(gain, group_gain);
                    for (auto user : group.second) {
                        selected.push_back(user);
                        includes_critical |= user.cell == critical;
                    }
                }
                if (includes_critical && !selected.empty() && selected.size() < old_net->users.entries() &&
                    (gain > best_gain ||
                     (gain == best_gain && std::string(ctx->nameOfBel(trial)) < std::string(ctx->nameOfBel(best))))) {
                    best = trial;
                    best_gain = gain;
                    moved = std::move(selected);
                }
            }
        }
        ctx->unbindBel(trial);
    }
    if (best == BelId())
        log_error("No legal nonregressing retained-enable replica site; abort before routing.\n");
    std::set<IdString> moved_names;
    for (auto user : moved) {
        NPNR_ASSERT(!protected_labs.count(lab(user.cell)));
        moved_names.insert(user.cell->name);
        user.cell->disconnectPort(id_ENA);
        user.cell->connectPort(id_ENA, new_net);
    }
    ctx->bindBel(best, clone, STRENGTH_WEAK);
    ctx->assignArchInfo();
    for (auto &e : ctx->cells)
        if (e.second->bel != BelId() && !ctx->isBelLocationValid(e.second->bel))
            log_error("Retained-enable diagnostic leaves illegal cell %s.\n", e.first.c_str(ctx));
    TimingAnalyser after(ctx);
    after.setup(false, false, true);
    auto new_hold = retained_hold(after);
    if (!enable_replication_policy::hold_nonregressing(old_hold, new_hold))
        log_error("Retained-enable diagnostic predicted hold regression; abort before routing.\n");
    for (auto &e : saved) {
        auto c = ctx->cells.at(e.first).get();
        auto &s = e.second;
        NPNR_ASSERT(c->type == s.type && c->bel == s.bel && c->belStrength == s.strength &&
                    c->params.size() == s.params.size() && c->attrs.size() == s.attrs.size() &&
                    c->ports.size() == s.ports.size());
        for (auto &p : s.params)
            NPNR_ASSERT(c->params.at(p.first) == p.second);
        for (auto &a : s.attrs)
            NPNR_ASSERT(c->attrs.at(a.first) == a.second);
        for (auto &p : s.ports) {
            auto &actual = c->ports.at(p.first);
            if (p.first == id_ENA && moved_names.count(e.first)) {
                NPNR_ASSERT(actual.net == new_net && actual.type == std::get<1>(p.second) &&
                            c->get_pin_state(p.first) == std::get<2>(p.second));
            } else
                NPNR_ASSERT(std::make_tuple(actual.net, actual.type, c->get_pin_state(p.first),
                                            actual.user_idx.idx()) == p.second);
        }
        for (auto &p : s.pins)
            NPNR_ASSERT(c->get_pin_state(p.first) == p.second);
    }
    for (auto pin : input_pins)
        NPNR_ASSERT(driver->getPort(pin)->users.entries() == input_user_counts.at(pin) + 1);
    NPNR_ASSERT(moved_names.count(critical->name) && old_net->users.entries() + new_net->users.entries() == 106);
    ctx->check();
    snapshot(".after.json");
    std::ofstream replicas(std::string(prefix) + ".replicas.tsv");
    replicas << "source\tclone\tbel\tmin_gain_ps\tsink\tport\tx\ty\n";
    for (auto user : moved) {
        auto p = lab(user.cell);
        replicas << target_name << '\t' << clone_id.str(ctx) << '\t' << ctx->nameOfBel(best) << '\t' << best_gain
                 << '\t' << user.cell->name.str(ctx) << "\tENA\t" << p.first << '\t' << p.second << '\n';
    }
    replicas.close();
    if (!replicas)
        log_error("Cannot write retained replica evidence.\n");
    std::ofstream input_file(std::string(prefix) + ".inputs.tsv");
    input_file << "pin\tsource\tsource_port\told_delay_ps\tnew_delay_ps\toriginal_users\tnew_users\n";
    for (size_t i = 0; i < input_pins.size(); ++i) {
        auto p = input_pins[i];
        auto n = driver->getPort(p);
        input_file << p.str(ctx) << '\t' << n->driver.cell->name.str(ctx) << '\t' << n->driver.port.str(ctx) << '\t'
                   << old_inputs[i] << '\t' << ctx->predictArcDelay(n, {clone, p}) << '\t' << input_user_counts.at(p)
                   << '\t' << n->users.entries() << '\n';
    }
    input_file.close();
    if (!input_file)
        log_error("Cannot write retained input evidence.\n");
    std::ofstream outputs(std::string(prefix) + ".outputs.tsv");
    outputs << "sink\tport\tx\ty\tprotected\tmoved\told_delay_ps\tnew_delay_ps\n";
    for (auto &group : groups)
        for (auto user : group.second)
            outputs << user.cell->name.str(ctx) << "\tENA\t" << group.first.first << '\t' << group.first.second << '\t'
                    << protected_labs.count(group.first) << '\t' << moved_names.count(user.cell->name) << '\t'
                    << ctx->predictArcDelay(old_net, user) << '\t' << ctx->predictArcDelay(new_net, user) << '\n';
    outputs.close();
    if (!outputs)
        log_error("Cannot write retained output evidence.\n");
    auto number = [](float v) { return std::isfinite(v) ? std::to_string(v) : std::string("null"); };
    std::ofstream audit(std::string(prefix) + ".audit.json");
    audit << "{\"source\":" << std::quoted(target_name) << ",\"clone\":" << std::quoted(clone_id.str(ctx))
          << ",\"critical\":" << std::quoted(critical_name)
          << ",\"selection\":\"measured routed critical endpoint; not predicted-failing "
             "qualification\",\"original_users\":106,\"protected_users\":"
          << protected_users << ",\"moved_users\":" << moved.size() << ",\"configured_budget\":" << configured_budget
          << ",\"prior_copies\":" << existing_copies
          << ",\"prior_input_loads_disjoint\":true,\"predicted_hold_nonregressing\":true,\"before_hold_violations\":"
          << old_hold.size() << ",\"after_hold_violations\":" << new_hold.size()
          << ",\"before_critical_slack_ps\":" << number(critical_slack)
          << ",\"before_criticality\":" << number(criticality)
          << ",\"after_critical_slack_ps\":" << number(after.get_setup_slack(CellPortKey(critical->name, id_ENA)))
          << ",\"memory_clock_constraint_present\":" << (clock->clkconstr ? "true" : "false")
          << ",\"fresh_clock_required\":" << (require_clock_constraint ? "true" : "false") << ",\"memory_period_ps\":"
          << (clock->clkconstr ? std::to_string(clock->clkconstr->period.maxDelay()) : "null") << "}\n";
    audit.close();
    if (!audit)
        log_error("Cannot write retained audit.\n");
    log_info("Retained enable diagnostic: %zu users copied, %d protected users retained, gain %dps.\n", moved.size(),
             protected_users, best_gain);
}
NEXTPNR_NAMESPACE_END
