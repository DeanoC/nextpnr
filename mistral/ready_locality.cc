/* Fixture-scoped read-only rigid translations. No routing or retained moves.
 * SPDX-License-Identifier: ISC */
#include "ready_locality.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include "jsonwrite.h"
#include "timing.h"
NEXTPNR_NAMESPACE_BEGIN
bool ready_locality_trial(Context *ctx, const std::vector<std::pair<CellInfo *, BelId>> &moves,
                          const std::function<void(bool)> &measure)
{
    std::set<CellInfo *> cells;
    std::set<BelId> destinations;
    std::vector<std::tuple<CellInfo *, BelId, PlaceStrength>> saved;
    for (auto &m : moves) {
        if (!m.first || m.first->bel == BelId() || m.second == BelId() || !cells.insert(m.first).second ||
            !destinations.insert(m.second).second || ctx->getBoundBelCell(m.second) ||
            !ctx->isValidBelForCellType(m.first->type, m.second))
            throw std::runtime_error("invalid/occupied locality destination");
        saved.emplace_back(m.first, m.first->bel, m.first->belStrength);
    }
    auto restore = [&]() {
        for (auto &s : saved)
            if (std::get<0>(s)->bel != BelId())
                ctx->unbindBel(std::get<0>(s)->bel);
        for (auto &s : saved)
            ctx->bindBel(std::get<1>(s), std::get<0>(s), std::get<2>(s));
    };
    bool legal = true;
    try {
        for (auto &s : saved)
            ctx->unbindBel(std::get<1>(s));
        for (size_t i = 0; i < moves.size(); ++i)
            ctx->bindBel(moves[i].second, moves[i].first, std::get<2>(saved[i]));
        for (auto &e : ctx->cells)
            if (e.second->bel != BelId())
                legal &= ctx->isBelLocationValid(e.second->bel);
        measure(legal);
    } catch (...) {
        restore();
        throw;
    }
    restore();
    return legal;
}
namespace {
#include "ready_locality_payload.inc"
using Key = std::pair<IdString, IdString>;
using Pins = std::map<Key, std::pair<CellPinState, std::vector<IdString>>>;
Pins pins(Context *ctx)
{
    Pins result;
    for (auto &e : ctx->cells) {
        for (auto &p : e.second->ports)
            result[{e.first, p.first}] = {e.second->get_pin_state(p.first), {}};
        for (auto &p : e.second->pin_data)
            result[{e.first, p.first}] = {p.second.state, {p.second.bel_pins.begin(), p.second.bel_pins.end()}};
    }
    return result;
}
void snapshot(Context *ctx, std::string path)
{
    ctx->archInfoToAttributes();
    std::ofstream f(path);
    NPNR_ASSERT(f && write_json_file(f, path, ctx));
    f.close();
    NPNR_ASSERT(f);
    std::ofstream p(path + ".pins.tsv");
    p << "cell\tport\tstate\n";
    std::map<std::pair<std::string, std::string>, int> rows;
    for (auto &e : pins(ctx))
        rows[{e.first.first.str(ctx), e.first.second.str(ctx)}] = int(e.second.first);
    for (auto &e : rows)
        p << e.first.first << '\t' << e.first.second << '\t' << e.second << '\n';
    p.close();
    NPNR_ASSERT(p);
}
std::string json(Context *ctx)
{
    std::ostringstream s;
    std::string filename = "ready-locality-invariant";
    NPNR_ASSERT(write_json_file(s, filename, ctx));
    return s.str();
}
bool comb(IdString t)
{
    return t.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6,
                id_MISTRAL_ALUT_ARITH, id_MISTRAL_NOT, id_MISTRAL_BUF);
}
struct Scope
{
    std::vector<CellPortKey> endpoints;
    size_t seeds = 0, exclusions = 0;
};
Scope scope(Context *ctx, const std::vector<CellInfo *> &group, const std::string &stem)
{
    Scope result;
    std::set<NetInfo *> seeds, visited, active;
    std::set<CellPortKey> ends;
    std::ofstream f(stem + ".seeds.tsv");
    f << "cell\tport\tnet\texcluded_reason\n";
    for (auto c : group)
        for (auto &p : c->ports)
            if (p.second.net) {
                auto n = p.second.net;
                std::string reason;
                if (p.first == id_CLK || (n->driver.cell && n->driver.cell->type == id_MISTRAL_CLKBUF))
                    reason = "clock";
                else if (n->driver.cell && n->driver.cell->type == id_MISTRAL_CONST)
                    reason = "constant";
                f << c->name.str(ctx) << '\t' << p.first.str(ctx) << '\t' << n->name.str(ctx) << '\t' << reason << '\n';
                if (reason.empty())
                    seeds.insert(n);
                else
                    ++result.exclusions;
            }
    f.close();
    NPNR_ASSERT(f);
    result.seeds = seeds.size();
    std::function<void(NetInfo *)> walk = [&](NetInfo *n) {
        NPNR_ASSERT(!active.count(n));
        if (!visited.insert(n).second)
            return;
        active.insert(n);
        for (auto &u : n->users) {
            NPNR_ASSERT(u.cell);
            if (comb(u.cell->type)) {
                for (auto &p : u.cell->ports)
                    if (p.second.type == PORT_OUT && p.second.net)
                        walk(p.second.net);
            } else
                ends.insert(CellPortKey(u));
        }
        active.erase(n);
    };
    for (auto n : seeds)
        walk(n);
    for (auto c : group)
        if (c->type == id_MISTRAL_FF)
            for (auto &p : c->ports)
                if (p.second.type == PORT_IN && p.second.net && p.first != id_CLK)
                    ends.insert({c->name, p.first});
    result.endpoints.assign(ends.begin(), ends.end());
    std::sort(result.endpoints.begin(), result.endpoints.end(), [&](auto &a, auto &b) {
        return std::make_pair(a.cell.str(ctx), a.port.str(ctx)) < std::make_pair(b.cell.str(ctx), b.port.str(ctx));
    });
    return result;
}
using DomainKey = std::tuple<IdString, IdString, IdString, int, IdString, int>;
struct Value
{
    delay_t setup, hold;
    bool timed;
};
struct Evaluation
{
    std::map<DomainKey, Value> domains;
    std::map<IdString, ClockFmax> clocks;
    std::map<IdString, CriticalPath> paths;
    std::map<Key, std::pair<int, int>> endpoint_info;
};
bool finite(delay_t x)
{
    return x != std::numeric_limits<delay_t>::max() && x != std::numeric_limits<delay_t>::lowest();
}
Evaluation evaluate(Context *ctx, const Scope &s)
{
    TimingAnalyser t(ctx);
    t.setup_only = false;
    t.with_clock_skew = true;
    t.setup(true, true, true);
    Evaluation out;
    for (auto &ep : s.endpoints) {
        int count = 0;
        auto cl = ctx->getPortTimingClass(ctx->cells.at(ep.cell).get(), ep.port, count);
        // All boundary entries remain visible even when the timing model ignores them.
        std::vector<TimingAnalyser::EndpointDomainSlack> domains;
        if (cl != TMG_IGNORE && cl != TMG_CLOCK_INPUT)
            domains = t.get_endpoint_domain_slacks(ep);
        out.endpoint_info[{ep.cell, ep.port}] = {int(cl), int(domains.size())};
        for (auto &d : domains) {
            DomainKey key{ep.cell, ep.port, d.launch.clock, int(d.launch.edge), d.capture.clock, int(d.capture.edge)};
            NPNR_ASSERT(out.domains.emplace(key, Value{d.setup, d.hold, d.timed}).second);
        }
    }
    for (auto &e : t.get_timing_result().clock_fmax) {
        NPNR_ASSERT(std::isfinite(e.second.achieved) && std::isfinite(e.second.constraint));
        out.clocks[e.first] = e.second;
    }
    for (auto &e : t.get_timing_result().clock_paths)
        out.paths[e.first] = e.second;
    if (out.clocks.size() != 3) {
        std::string names;
        for (auto &c : out.clocks)
            names += c.first.str(ctx) + ",";
        throw std::runtime_error("Locality expected three timed clocks; actual: " + names);
    }
    return out;
}
struct Metrics
{
    size_t rows = 0, timed_finite = 0, untimed = 0, nonfinite = 0, setup_regressions = 0, hold_regressions = 0,
           clock_regressions = 0;
};
Metrics report(Context *ctx, const Scope &s, const Evaluation &before, const Evaluation *trial, const std::string &stem)
{
    Metrics m;
    std::ofstream paths(stem + ".paths.tsv");
    paths << "phase\tclock\tsegment\ttype\tfrom_cell\tfrom_port\tto_cell\tto_port\tdelay_ps\n";
    auto write_paths = [&](const Evaluation &ev, const char *phase) {
        for (auto &entry : ev.paths) {
            size_t index = 0;
            for (auto &s : entry.second.segments)
                paths << phase << '\t' << entry.first.str(ctx) << '\t' << index++ << '\t'
                      << CriticalPath::Segment::type_to_str(s.type) << '\t' << s.from.first.str(ctx) << '\t'
                      << s.from.second.str(ctx) << '\t' << s.to.first.str(ctx) << '\t' << s.to.second.str(ctx) << '\t'
                      << s.delay << '\n';
        }
    };
    write_paths(before, "before");
    if (trial)
        write_paths(*trial, "trial");
    paths.close();
    NPNR_ASSERT(paths);
    std::ofstream e(stem + ".endpoints.tsv");
    e << "cell\tport\ttiming_class\tdomain_count\texclusion\n";
    for (auto &ep : s.endpoints) {
        auto i = before.endpoint_info.at({ep.cell, ep.port});
        std::string reason = i.first == TMG_IGNORE        ? "timing_ignore"
                             : i.first == TMG_CLOCK_INPUT ? "clock_input"
                             : i.second == 0              ? "no_domain_pairs"
                                                          : "";
        e << ep.cell.str(ctx) << '\t' << ep.port.str(ctx) << '\t' << i.first << '\t' << i.second << '\t' << reason
          << '\n';
    }
    e.close();
    NPNR_ASSERT(e);
    if (!trial)
        return m;
    NPNR_ASSERT(trial->endpoint_info == before.endpoint_info && trial->domains.size() == before.domains.size());
    std::ofstream d(stem + ".domains.tsv");
    d << "cell\tport\tlaunch_clock\tlaunch_edge\tcapture_clock\tcapture_edge\ttimed\tbefore_finite\ttrial_"
         "finite\tbefore_setup_ps\ttrial_setup_ps\tbefore_hold_ps\ttrial_hold_ps\n";
    for (auto &entry : before.domains) {
        auto &k = entry.first;
        auto &a = entry.second;
        auto &b = trial->domains.at(k);
        NPNR_ASSERT(a.timed == b.timed);
        bool af = finite(a.setup) && finite(a.hold), bf = finite(b.setup) && finite(b.hold);
        ++m.rows;
        if (!a.timed)
            ++m.untimed;
        else if (!af || !bf)
            ++m.nonfinite;
        else {
            ++m.timed_finite;
            m.setup_regressions += b.setup < a.setup;
            m.hold_regressions += b.hold < std::min(a.hold, delay_t(0));
        }
        d << std::get<0>(k).str(ctx) << '\t' << std::get<1>(k).str(ctx) << '\t' << std::get<2>(k).str(ctx) << '\t'
          << std::get<3>(k) << '\t' << std::get<4>(k).str(ctx) << '\t' << std::get<5>(k) << '\t' << a.timed << '\t'
          << af << '\t' << bf << '\t' << a.setup << '\t' << b.setup << '\t' << a.hold << '\t' << b.hold << '\n';
    }
    d.close();
    NPNR_ASSERT(d);
    NPNR_ASSERT(before.clocks.size() == trial->clocks.size());
    std::ofstream c(stem + ".clocks.tsv");
    c << "clock\tbefore_mhz\ttrial_mhz\tconstraint_mhz\n"
      << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (auto &e : before.clocks) {
        auto &b = trial->clocks.at(e.first);
        NPNR_ASSERT(e.second.constraint == b.constraint);
        m.clock_regressions += b.achieved < e.second.achieved;
        c << e.first.str(ctx) << '\t' << e.second.achieved << '\t' << b.achieved << '\t' << b.constraint << '\n';
    }
    c.close();
    NPNR_ASSERT(c);
    return m;
}
} // namespace
void diagnostic_ready_locality(Context *ctx, const char *prefix, bool imported_context)
{
    if (!prefix || !*prefix)
        return;
    // Fixed scope: previous reuse/cut/refinement experiments must be off.
    auto source = ctx->cells.at(ctx->id("hps_ddr.port1.slot_free_MISTRAL_ALUT3_B")).get();
    auto replica = ctx->cells.at(ctx->id("hps_ddr.port1.slot_free_MISTRAL_ALUT3_B$enable_replica")).get();
    NPNR_ASSERT(source->getPort(id_Q)->users.entries() == 107 && replica->getPort(id_Q)->users.entries() == 4);
    for (auto name : {"ram_clock.clocks[0]", "ram_clock.clocks[1]", "display.pixel_clk"}) {
        auto n = ctx->nets.at(ctx->id(name)).get();
        NPNR_ASSERT(n->clkconstr);
    }
    std::vector<CellInfo *> small, large;
    auto keep = ctx->id("keep"), dont_touch = ctx->id("dont_touch");
    for (auto &m : locality_members) {
        auto c = ctx->cells.at(ctx->id(m.name)).get();
        NPNR_ASSERT(c->type == ctx->id(m.type) && c->bel != BelId());
        auto l = ctx->getBelLocation(c->bel);
        NPNR_ASSERT(l.x == m.x && l.y == m.y && l.z == m.z && c->belStrength == STRENGTH_WEAK &&
                    c->cluster == ClusterId() && !c->region && !c->isPseudo() && !c->attrs.count(keep) &&
                    !c->attrs.count(dont_touch));
        NPNR_ASSERT(c->type == id_MISTRAL_FF || (comb(c->type) && c->type != id_MISTRAL_ALUT_ARITH));
        large.push_back(c);
        if (m.small)
            small.push_back(c);
    }
    NPNR_ASSERT(small.size() == 387 && large.size() == 655);
    // Pure placement trials require unrouted inputs; clock routing is global and unchanged.
    for (auto &e : ctx->nets)
        NPNR_ASSERT(e.second->wires.empty() || e.second->is_global);
    snapshot(ctx, std::string(prefix) + ".before.json");
    auto original_json = json(ctx);
    auto original_pins = pins(ctx);
    std::map<CellInfo *, std::pair<BelId, PlaceStrength>> positions;
    std::map<Key, int> indices;
    using Constraints = std::tuple<ClusterId, Region *, int, int, int, bool, std::vector<CellInfo *>>;
    std::map<CellInfo *, Constraints> constraints;
    auto constraint_state = [](CellInfo *c) {
        return Constraints(c->cluster, c->region, c->constr_x, c->constr_y, c->constr_z, c->constr_abs_z,
                           c->constr_children);
    };
    using ClockState = std::tuple<int, int, int, int, int, int, IdString, int>;
    auto clock_states = [&]() {
        std::map<IdString, ClockState> states;
        for (auto &e : ctx->nets)
            if (e.second->clkconstr) {
                auto &c = *e.second->clkconstr;
                states[e.first] = {c.period.minDelay(), c.period.maxDelay(), c.high.minDelay(), c.high.maxDelay(),
                                   c.low.minDelay(),    c.low.maxDelay(),    c.phase_group,     c.phase_shift};
            }
        return states;
    };
    auto original_clocks = clock_states();
    // Establish which generated/buffered nets the timing model actually uses.
    std::map<IdString, size_t> clock_references;
    for (auto &e : ctx->cells)
        for (auto &p : e.second->ports) {
            int count = 0;
            auto cl = ctx->getPortTimingClass(e.second.get(), p.first, count);
            if (cl != TMG_REGISTER_INPUT && cl != TMG_REGISTER_OUTPUT)
                continue;
            for (int i = 0; i < count; ++i) {
                auto ci = ctx->getPortClockingInfo(e.second.get(), p.first, i);
                auto n = e.second->getPort(ci.clock_port);
                NPNR_ASSERT(n && n->clkconstr);
                ++clock_references[n->name];
            }
        }
    NPNR_ASSERT(clock_references.size() == 3);
    std::ofstream refs(std::string(prefix) + ".clock-references.tsv");
    refs << "clock\tregistered_port_references\n";
    for (auto &e : clock_references) {
        NPNR_ASSERT(original_clocks.count(e.first));
        refs << e.first.str(ctx) << '\t' << e.second << '\n';
    }
    refs.close();
    NPNR_ASSERT(refs);
    std::ofstream clock_file(std::string(prefix) + ".clock-constraints.tsv");
    clock_file << "clock\tperiod_min_ps\tperiod_max_ps\thigh_min_ps\thigh_max_ps\tlow_min_ps\tlow_max_ps\tphase_"
                  "group\tphase_shift_ps\n";
    for (auto &e : original_clocks) {
        auto &c = e.second;
        clock_file << e.first.str(ctx) << '\t' << std::get<0>(c) << '\t' << std::get<1>(c) << '\t' << std::get<2>(c)
                   << '\t' << std::get<3>(c) << '\t' << std::get<4>(c) << '\t' << std::get<5>(c) << '\t'
                   << std::get<6>(c).str(ctx) << '\t' << std::get<7>(c) << '\n';
    }
    clock_file.close();
    NPNR_ASSERT(clock_file);
    for (auto &e : ctx->cells) {
        positions[e.second.get()] = {e.second->bel, e.second->belStrength};
        constraints[e.second.get()] = constraint_state(e.second.get());
        if (e.second->bel != BelId())
            NPNR_ASSERT(ctx->isBelLocationValid(e.second->bel));
        for (auto &p : e.second->ports)
            indices[{e.first, p.first}] = p.second.user_idx.idx();
    }
    std::vector<int> input_counts;
    for (auto &lab : ctx->labs)
        for (auto &a : lab.alms)
            input_counts.push_back(a.unique_input_count);
    std::ofstream audit(std::string(prefix) + ".audit.json");
    audit << "{\"grouping_sha256\":\"" << grouping_sha256
          << "\",\"imported_context\":" << (imported_context ? "true" : "false")
          << ",\"predictions_only\":true,\"trials\":[";
    for (int trial = 0; trial < 2; ++trial) {
        auto &group = trial ? large : small;
        int dy = trial ? 43 : 41;
        std::string label = trial ? "group655-dy43" : "group387-dy41", stem = std::string(prefix) + "." + label;
        std::vector<std::pair<CellInfo *, BelId>> moves;
        std::set<std::pair<int, int>> dest_labs;
        std::ofstream mv(stem + ".moves.tsv");
        mv << "cell\told_bel\tnew_bel\n";
        for (auto c : group) {
            auto l = ctx->getBelLocation(c->bel);
            auto dst = ctx->getBelByLocation(Loc(l.x, l.y + dy, l.z));
            NPNR_ASSERT(dst != BelId() && ctx->isValidBelForCellType(c->type, dst));
            dest_labs.insert({l.x, l.y + dy});
            moves.emplace_back(c, dst);
            mv << c->name.str(ctx) << '\t' << ctx->getBelName(c->bel).str(ctx) << '\t' << ctx->getBelName(dst).str(ctx)
               << '\n';
        }
        mv.close();
        NPNR_ASSERT(mv);
        for (auto &l : dest_labs)
            for (auto b : ctx->getBelsByTile(l.first, l.second))
                NPNR_ASSERT(!ctx->getBoundBelCell(b));
        auto s = scope(ctx, group, stem);
        NPNR_ASSERT(s.endpoints.size() == size_t(trial ? 2095 : 1715));
        auto before = evaluate(ctx, s);
        Metrics metrics;
        size_t invalid = 0;
        bool legal = ready_locality_trial(ctx, moves, [&](bool valid) {
            std::set<CellInfo *> moved(group.begin(), group.end());
            for (auto &e : positions) {
                auto l = ctx->getBelLocation(e.second.first);
                auto want = moved.count(e.first) ? ctx->getBelByLocation(Loc(l.x, l.y + dy, l.z)) : e.second.first;
                NPNR_ASSERT(e.first->bel == want && e.first->belStrength == e.second.second);
            }
            NPNR_ASSERT(pins(ctx) == original_pins);
            std::ofstream legality(stem + ".legality.tsv");
            legality << "cell\tbel\tbefore_valid\ttrial_valid\n";
            for (auto &e : ctx->cells)
                if (e.second->bel != BelId()) {
                    bool v = ctx->isBelLocationValid(e.second->bel);
                    invalid += !v;
                    legality << e.first.str(ctx) << '\t' << ctx->getBelName(e.second->bel).str(ctx) << "\t1\t" << v
                             << '\n';
                }
            legality.close();
            NPNR_ASSERT(legality && valid == (invalid == 0));
            if (valid) {
                auto after = evaluate(ctx, s);
                metrics = report(ctx, s, before, &after, stem);
            } else
                metrics = report(ctx, s, before, nullptr, stem);
            NPNR_ASSERT(json(ctx) == original_json); // BEL attributes are refreshed only for the snapshot.
            snapshot(ctx, stem + ".json");
        });
        snapshot(ctx, std::string(prefix) + ".restored-" + label + ".json");
        NPNR_ASSERT(json(ctx) == original_json && pins(ctx) == original_pins);
        for (auto &e : positions)
            NPNR_ASSERT(e.first->bel == e.second.first && e.first->belStrength == e.second.second);
        for (auto &e : ctx->cells)
            for (auto &p : e.second->ports)
                NPNR_ASSERT(p.second.user_idx.idx() == indices.at({e.first, p.first}));
        size_t n = 0;
        for (auto &lab : ctx->labs)
            for (auto &a : lab.alms)
                NPNR_ASSERT(a.unique_input_count == input_counts.at(n++));
        for (auto &e : ctx->cells)
            if (e.second->bel != BelId())
                NPNR_ASSERT(ctx->isBelLocationValid(e.second->bel));
        for (auto &e : constraints)
            NPNR_ASSERT(constraint_state(e.first) == e.second);
        NPNR_ASSERT(clock_states() == original_clocks);
        ctx->check();
        if (trial)
            audit << ',';
        audit << "{\"label\":\"" << label << "\",\"group_size\":" << group.size() << ",\"dx\":0,\"dy\":" << dy
              << ",\"destination_labs\":" << dest_labs.size()
              << ",\"destinations_empty\":true,\"legal\":" << (legal ? "true" : "false")
              << ",\"invalid_cells\":" << invalid << ",\"endpoints\":" << s.endpoints.size()
              << ",\"seed_nets\":" << s.seeds << ",\"excluded_incident_ports\":" << s.exclusions
              << ",\"domain_rows\":" << metrics.rows << ",\"timed_finite_rows\":" << metrics.timed_finite
              << ",\"untimed_rows\":" << metrics.untimed << ",\"nonfinite_rows\":" << metrics.nonfinite
              << ",\"setup_regressions\":" << metrics.setup_regressions
              << ",\"hold_regressions\":" << metrics.hold_regressions
              << ",\"clock_regressions\":" << metrics.clock_regressions << ",\"restored\":true}";
        audit.flush();
    }
    audit << "],\"all_restored\":true,\"pins_and_indexed_users_preserved\":true,\"cached_lab_input_counts_restored\":"
             "true}\n";
    audit.close();
    NPNR_ASSERT(audit);
}
NEXTPNR_NAMESPACE_END
