/* Optional, bounded post-placement replication of ordinary LUT enable drivers.
 * Predictions admit candidates; routed setup/hold analysis remains authoritative.
 * SPDX-License-Identifier: ISC
 */
#include "nextpnr.h"
#include "log.h"
#include "enable_replication_policy.h"
#include "jsonwrite.h"
#include "timing.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Lab = std::pair<int, int>;
using Hold = std::map<std::string, int>;

Hold hold_violations(TimingAnalyser &timing)
{
    Hold result;
    for (const auto &path : timing.get_timing_result().min_delay_violations) {
        if (path.segments.empty()) continue;
        int slack = 0;
        for (const auto &segment : path.segments) slack += segment.delay;
        const auto &ep = path.segments.back().to;
        std::string key = std::to_string(path.clock_pair.start.clock.index) + ":" +
                std::to_string(int(path.clock_pair.start.edge)) + ":" +
                std::to_string(path.clock_pair.end.clock.index) + ":" +
                std::to_string(int(path.clock_pair.end.edge)) + ":" +
                std::to_string(ep.first.index) + ":" + std::to_string(ep.second.index);
        auto found = result.find(key);
        if (found == result.end()) result.emplace(key, slack);
        else found->second = std::min(found->second, slack);
    }
    return result;
}
}

void Arch::replicate_enables(int budget)
{
    namespace policy = enable_replication_policy;
    NPNR_ASSERT(policy::valid_budget(budget));
    if (!budget) return;
    Context *ctx = getCtx();
    settings[id("mistral/replicateEnables")] = budget;
    const char *dump_prefix = std::getenv("NEXTPNR_MISTRAL_ENABLE_REPLICATION_DUMP");
    auto snapshot = [&](const char *suffix) {
        if (!dump_prefix || !*dump_prefix) return;
        archInfoToAttributes();
        std::string filename = std::string(dump_prefix) + suffix;
        std::ofstream stream(filename);
        if (!stream || !write_json_file(stream, filename, ctx))
            log_error("Cannot write enable replication snapshot '%s'.\n", filename.c_str());
        stream.close();
        if (!stream) log_error("Cannot finish enable replication snapshot.\n");
        std::map<std::pair<std::string, std::string>, int> pins;
        for (const auto &entry : cells) {
            const CellInfo *cell = entry.second.get();
            for (const auto &port : cell->ports)
                pins[{cell->name.str(ctx), port.first.str(ctx)}] = int(cell->get_pin_state(port.first));
            for (const auto &pin : cell->pin_data)
                pins[{cell->name.str(ctx), pin.first.str(ctx)}] = int(cell->get_pin_state(pin.first));
        }
        std::ofstream sidecar(filename + ".pins.tsv");
        sidecar << "cell\tport\tstate\n";
        for (const auto &pin : pins)
            sidecar << pin.first.first << '\t' << pin.first.second << '\t' << pin.second << '\n';
        sidecar.close();
        if (!sidecar) log_error("Cannot write enable replication pin states.\n");
    };
    snapshot(".before.json");
    std::ofstream manifest;
    if (dump_prefix && *dump_prefix) {
        manifest.open(std::string(dump_prefix) + ".replicas.tsv");
        if (!manifest) log_error("Cannot write enable replication manifest.\n");
        manifest << "source\tclone\tbel\tmin_gain_ps\tsink\tport\tx\ty\n";
    }
    auto ordinary_lut = [](IdString type) {
        return type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6);
    };
    auto movable = [&](const CellInfo *cell) {
        return cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK && cell->cluster == ClusterId() &&
                cell->region == nullptr && !cell->isPseudo() && !cell->attrs.count(id("keep")) &&
                !cell->attrs.count(id("dont_touch"));
    };
    auto lab_of = [&](const CellInfo *cell) {
        Loc loc = getBelLocation(cell->bel);
        return Lab(loc.x, loc.y);
    };
    // Fixed boundary/clock nets must not acquire new loads or users.
    std::set<const NetInfo *> boundary;
    for (const auto &port : ctx->ports) if (port.second.net) boundary.insert(port.second.net);
    auto ordinary_net = [&](const NetInfo *net) {
        return net && !net->is_global && !net->clkconstr && !net->region && net->wires.empty() &&
                net->constant_value == IdString() && !boundary.count(net) &&
                !net->attrs.count(id("keep")) && !net->attrs.count(id("dont_touch"));
    };
    auto mapped = [&](const CellInfo *cell, IdString pin) {
        for (auto physical : getBelPinsForCellPin(cell, pin)) {
            (void)physical;
            return true;
        }
        return false;
    };
    std::set<Lab> protected_labs;
    for (const auto &entry : cells) {
        const CellInfo *cell = entry.second.get();
        if (cell->bel != BelId() && (!movable(cell) || cell->type == id_MISTRAL_MLAB))
            protected_labs.insert(lab_of(cell));
    }
    auto ordinary_logic_lab = [&](BelId bel) {
        IdString type = getBelType(bel);
        if (!type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF)) return false;
        auto loc = getBelLocation(bel);
        // MCOMB is an MLAB-capable tile, not an occupied memory. Ordinary
        // LUT/FF use is legal there; actual MISTRAL_MLAB cells protect the LAB.
        return !protected_labs.count({loc.x, loc.y});
    };
    std::map<IdString, std::pair<BelId, PlaceStrength>> original_placements;
    std::map<std::pair<IdString, IdString>, CellPinState> original_pins;
    for (const auto &entry : cells) {
        const CellInfo *cell = entry.second.get();
        original_placements[cell->name] = {cell->bel, cell->belStrength};
        for (const auto &pin : cell->pin_data) original_pins[{cell->name, pin.first}] = pin.second.state;
        for (const auto &port : cell->ports) original_pins[{cell->name, port.first}] = cell->get_pin_state(port.first);
    }
    struct Candidate { CellInfo *cell; float slack, criticality; };
    std::vector<Candidate> candidates;
    {
        TimingAnalyser timing(ctx);
        timing.setup();
        for (const auto &entry : cells) {
            CellInfo *driver = entry.second.get();
            if (!ordinary_lut(driver->type) || !movable(driver) || !ordinary_logic_lab(driver->bel)) continue;
            NetInfo *net = driver->getPort(id_Q);
            if (!ordinary_net(net) || net->users.entries() < 2) continue;
            bool eligible = true;
            NetInfo *clock = nullptr;
            CellPinState clock_state = PIN_SIG;
            float slack = std::numeric_limits<float>::max(), criticality = 0;
            std::set<Lab> groups;
            for (auto user : net->users) {
                if (user.cell->type != id_MISTRAL_FF || user.port != id_ENA || !movable(user.cell) ||
                    !ordinary_logic_lab(user.cell->bel) || !mapped(user.cell, user.port)) { eligible = false; break; }
                NetInfo *user_clock = user.cell->getPort(id_CLK);
                if (!user_clock || (clock && (clock != user_clock || clock_state != user.cell->get_pin_state(id_CLK)))) {
                    eligible = false; break;
                }
                clock = user_clock;
                clock_state = user.cell->get_pin_state(id_CLK);
                groups.insert(lab_of(user.cell));
                slack = std::min(slack, timing.get_setup_slack(CellPortKey(user)));
                criticality = std::max(criticality, timing.get_criticality(CellPortKey(user)));
            }
            if (eligible && groups.size() >= 2 && policy::critical_failing(criticality, slack))
                candidates.push_back({driver, slack, criticality});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](const Candidate &a, const Candidate &b) {
        if (a.slack != b.slack) return a.slack < b.slack;
        if (a.criticality != b.criticality) return a.criticality > b.criticality;
        return a.cell->name.str(ctx) < b.cell->name.str(ctx);
    });
    // Adding a second sink on any upstream net in this pass exceeds our load budget.
    std::set<const NetInfo *> loaded_inputs;
    int accepted = 0, examined = 0;
    const int candidate_limit = 32;
    for (const auto &candidate : candidates) {
        if (accepted == budget || examined++ == candidate_limit) break;
        CellInfo *driver = candidate.cell;
        NetInfo *old_net = driver->getPort(id_Q);
        std::vector<IdString> input_pins;
        std::vector<int> old_input_delays;
        std::set<const NetInfo *> unique_inputs;
        bool eligible = true;
        for (IdString pin : {id_A, id_B, id_C, id_D, id_E, id_F}) {
            if (!driver->ports.count(pin)) continue;
            CellPinState state = driver->get_pin_state(pin);
            NetInfo *net = driver->getPort(pin);
            if (state == PIN_0 || state == PIN_1) {
                if (net) { eligible = false; break; } // accept only already folded constants
                continue;
            }
            if (!ordinary_net(net) || !net->driver.cell || net->driver.cell == driver ||
                loaded_inputs.count(net) || unique_inputs.count(net)) { eligible = false; break; }
            // A fabric-driven net can still be a boundary or clock signal if
            // another user is a hard block, I/O, or a clock input. Do not add
            // loading to such nets merely because their driver is ordinary.
            for (auto user : net->users) {
                int clock_count = 0;
                if (!(ordinary_lut(user.cell->type) || user.cell->type == id_MISTRAL_FF) ||
                    getPortTimingClass(user.cell, user.port, clock_count) == TMG_CLOCK_INPUT) {
                    eligible = false;
                    break;
                }
            }
            if (!eligible) break;
            CellInfo *source = net->driver.cell;
            if (!(ordinary_lut(source->type) || (source->type == id_MISTRAL_FF && net->driver.port == id_Q)) ||
                !movable(source) || !ordinary_logic_lab(source->bel) || !mapped(source, net->driver.port) || !mapped(driver, pin)) {
                eligible = false; break;
            }
            unique_inputs.insert(net);
            input_pins.push_back(pin);
            old_input_delays.push_back(ctx->predictArcDelay(net, {driver, pin}));
        }
        if (!eligible || input_pins.empty()) continue;
        TimingAnalyser before(ctx);
        before.setup(false, false, true);
        Hold old_hold = hold_violations(before);
        std::map<Lab, std::vector<PortRef>> groups;
        std::vector<std::pair<float, Lab>> critical_groups;
        for (auto user : old_net->users) groups[lab_of(user.cell)].push_back(user);
        for (auto &group : groups) {
            std::sort(group.second.begin(), group.second.end(), [&](const PortRef &a, const PortRef &b) {
                return a.cell->name.str(ctx) < b.cell->name.str(ctx);
            });
            float slack = std::numeric_limits<float>::max();
            bool critical = false;
            for (auto user : group.second) {
                float s = before.get_setup_slack(CellPortKey(user));
                slack = std::min(slack, s);
                critical |= policy::critical_failing(before.get_criticality(CellPortKey(user)), s);
            }
            if (critical) critical_groups.emplace_back(slack, group.first);
        }
        std::sort(critical_groups.begin(), critical_groups.end());
        if (critical_groups.empty()) continue;
        if (critical_groups.size() > 4) critical_groups.resize(4);
        std::set<Lab> search_tiles;
        for (const auto &group : critical_groups)
            for (int dx = -3; dx <= 3; ++dx)
                for (int dy = -3; dy <= 3; ++dy)
                    if (std::abs(dx) + std::abs(dy) <= 3)
                        search_tiles.emplace(group.second.first + dx, group.second.second + dy);
        IdString clone_name = idf("%s$enable_replica", nameOf(driver));
        IdString output_name = idf("%s$Q", clone_name.c_str(ctx));
        if (cells.count(clone_name) || nets.count(output_name) || net_aliases.count(output_name)) continue;
        log_info("Enable replication considering '%s': %zu LAB groups, %zu signal inputs.\n",
                 nameOf(driver), groups.size(), input_pins.size());
        CellInfo *clone = createCell(clone_name, driver->type);
        clone->params = driver->params;
        clone->attrs = driver->attrs;
        for (const char *attr : {"NEXTPNR_BEL", "BEL_STRENGTH", "FES_PINMAP_V1"}) clone->attrs.erase(id(attr));
        clone->pin_data = driver->pin_data;
        for (const auto &port : driver->ports)
            if (port.second.type == PORT_IN) driver->copyPortTo(port.first, clone, port.first);
        NetInfo *new_net = createNet(output_name);
        clone->addOutput(id_Q);
        clone->connectPort(id_Q, new_net);
        assignArchInfo();
        BelId best;
        int best_gain = 0;
        std::vector<PortRef> best_users;
        for (Lab tile : search_tiles) {
            for (BelId bel : getBelsByTile(tile.first, tile.second)) {
                if (!checkBelAvail(bel) || !isValidBelForCellType(clone->type, bel) || !ordinary_logic_lab(bel)) continue;
                bindBel(bel, clone, STRENGTH_WEAK);
                if (isBelLocationValid(bel)) {
                    std::vector<int> new_inputs;
                    for (IdString pin : input_pins) new_inputs.push_back(ctx->predictArcDelay(clone->getPort(pin), {clone, pin}));
                    if (policy::inputs_nonregressing(old_input_delays, new_inputs)) {
                        std::vector<PortRef> selected;
                        int gain = std::numeric_limits<int>::max();
                        bool critical = false;
                        for (const auto &group : groups) {
                            std::vector<int> old_outputs, new_outputs;
                            for (auto user : group.second) {
                                old_outputs.push_back(ctx->predictArcDelay(old_net, user));
                                new_outputs.push_back(ctx->predictArcDelay(new_net, user));
                            }
                            int group_gain = policy::group_gain(old_outputs, new_outputs);
                            if (!group_gain) continue;
                            gain = std::min(gain, group_gain);
                            for (auto user : group.second) {
                                selected.push_back(user);
                                critical |= policy::critical_failing(before.get_criticality(CellPortKey(user)),
                                                                     before.get_setup_slack(CellPortKey(user)));
                            }
                        }
                        if (critical && !selected.empty() && selected.size() < old_net->users.entries() && gain > best_gain) {
                            best = bel; best_gain = gain; best_users = std::move(selected);
                        }
                    }
                }
                unbindBel(bel);
            }
        }
        bool commit = best != BelId();
        if (commit) {
            for (auto user : best_users) { user.cell->disconnectPort(id_ENA); user.cell->connectPort(id_ENA, new_net); }
            bindBel(best, clone, STRENGTH_WEAK);
            assignArchInfo();
            for (const auto &entry : original_placements)
                if (!isBelLocationValid(entry.second.first)) { commit = false; break; }
            commit &= isBelLocationValid(best);
            if (commit) {
                TimingAnalyser after(ctx);
                after.setup(false, false, true);
                commit = policy::hold_nonregressing(old_hold, hold_violations(after));
                if (!commit) log_info("Enable replication rejected '%s': predicted hold regression.\n", nameOf(driver));
            }
        }
        if (!commit) {
            for (auto user : best_users)
                if (user.cell->getPort(id_ENA) == new_net) { user.cell->disconnectPort(id_ENA); user.cell->connectPort(id_ENA, old_net); }
            if (clone->bel != BelId()) unbindBel(clone->bel);
            std::vector<IdString> ports;
            for (const auto &port : clone->ports) ports.push_back(port.first);
            for (IdString port : ports) clone->disconnectPort(port);
            cells.erase(clone_name); nets.erase(output_name); net_aliases.erase(output_name);
            assignArchInfo();
            continue;
        }
        for (const NetInfo *net : unique_inputs) loaded_inputs.insert(net);
        ++accepted;
        log_info("Enable replica source=%s clone=%s bel=%s sinks=%zu gain=%dps slack=%.0fps criticality=%.3f\n",
                 nameOf(driver), nameOf(clone), nameOfBel(best), best_users.size(), best_gain,
                 candidate.slack, candidate.criticality);
        for (IdString pin : input_pins)
            log_info("  input %s.%s: original=%dps replica=%dps\n", nameOf(driver), pin.c_str(ctx),
                     int(ctx->predictArcDelay(driver->getPort(pin), {driver, pin})),
                     int(ctx->predictArcDelay(clone->getPort(pin), {clone, pin})));
        for (auto user : best_users) {
            Lab lab = lab_of(user.cell);
            if (manifest.is_open()) manifest << nameOf(driver) << '\t' << nameOf(clone) << '\t' << nameOfBel(best)
                    << '\t' << best_gain << '\t' << nameOf(user.cell) << "\tENA\t" << lab.first << '\t' << lab.second << '\n';
        }
    }
    for (const auto &entry : original_placements) {
        const CellInfo *cell = cells.at(entry.first).get();
        NPNR_ASSERT(cell->bel == entry.second.first && cell->belStrength == entry.second.second);
        NPNR_ASSERT(isBelLocationValid(cell->bel));
    }
    for (const auto &entry : original_pins)
        NPNR_ASSERT(cells.at(entry.first.first)->get_pin_state(entry.first.second) == entry.second);
    ctx->check();
    snapshot(".after.json");
    if (manifest.is_open()) { manifest.close(); if (!manifest) log_error("Cannot finish enable replication manifest.\n"); }
    log_info("Enable replication: %d replicas, budget %d; examined at most %d of %zu ranked candidates.\n",
             accepted, budget, candidate_limit, candidates.size());
}
NEXTPNR_NAMESPACE_END
