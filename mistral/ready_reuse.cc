/* Fixture-scoped reuse of one existing equivalent enable replica.
 * SPDX-License-Identifier: ISC */
#include "ready_reuse.h"
#include <algorithm>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include "jsonwrite.h"
#include "ready_fallback_policy.h"
#include "timing.h"
NEXTPNR_NAMESPACE_BEGIN
bool ready_reuse_equivalent(const CellInfo *a, const CellInfo *b)
{
    if (!a || !b || a == b || a->type != id_MISTRAL_ALUT3 || b->type != a->type || a->ports.size() != 4 ||
        b->ports.size() != 4 || !a->params.count(id_LUT) || !b->params.count(id_LUT) ||
        a->params.at(id_LUT).as_int64() != 0x32 || b->params.at(id_LUT).as_int64() != 0x32)
        return false;
    for (auto pin : {id_A, id_B, id_C, id_Q})
        if (!a->getPort(pin) || !b->getPort(pin) || a->get_pin_state(pin) != PIN_SIG ||
            b->get_pin_state(pin) != PIN_SIG || (pin != id_Q && a->getPort(pin) != b->getPort(pin)))
            return false;
    return a->getPort(id_Q) != b->getPort(id_Q);
}
bool ready_reuse_clock_valid(const CellInfo *ff, const NetInfo *clock)
{
    return ff && ff->type == id_MISTRAL_FF && clock && clock->clkconstr && ff->getPort(id_CLK) == clock &&
           ff->get_pin_state(id_CLK) == PIN_SIG && ff->get_pin_state(id_ENA) == PIN_SIG;
}
void ready_reuse_move(Context *ctx, CellInfo *ff, NetInfo *replica_net)
{
    NPNR_ASSERT(ff->type == id_MISTRAL_FF && ff->bel != BelId() && ff->get_pin_state(id_ENA) == PIN_SIG);
    ff->disconnectPort(id_ENA);
    ff->connectPort(id_ENA, replica_net);
    ctx->assign_ff_info(ff);
    ctx->update_bel(ff->bel);
}
namespace {
const char *original_name = "hps_ddr.port1.slot_free_MISTRAL_ALUT3_B";
const char *replica_name = "hps_ddr.port1.slot_free_MISTRAL_ALUT3_B$enable_replica";
const char *sink_name = "hps_ddr.port1.skid_burstcount_MISTRAL_FF_Q_5";
using Key = std::pair<IdString, IdString>;
using Pins = std::map<Key, std::pair<CellPinState, std::vector<IdString>>>;
using Tree = std::map<WireId, std::pair<PipId, PlaceStrength>>;
using Quad = std::tuple<delay_t, delay_t, delay_t, delay_t>;
Quad quad(const DelayQuad &q) { return {q.minRiseDelay(), q.maxRiseDelay(), q.minFallDelay(), q.maxFallDelay()}; }
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
std::string json(Context *ctx)
{
    std::ostringstream s;
    std::string filename = "ready-reuse-invariant";
    NPNR_ASSERT(write_json_file(s, filename, ctx));
    return s.str();
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
struct Scope
{
    NetInfo *ready = nullptr, *clock = nullptr;
    std::vector<CellPortKey> endpoints;
};
Scope scope(Context *ctx)
{
    Scope result;
    for (auto &n : ctx->nets)
        if (n.second->driver.cell && n.second->driver.cell->type == id_cyclonev_hps_interface_fpga2sdram &&
            n.second->driver.port.str(ctx) == "cmd_ready_1") {
            NPNR_ASSERT(!result.ready);
            result.ready = n.second.get();
        }
    NPNR_ASSERT(result.ready && result.ready->users.entries() == 1);
    auto cp = ctx->id("cmd_port_clk_1");
    result.clock = result.ready->driver.cell->getPort(cp);
    NPNR_ASSERT(result.clock && result.clock->clkconstr && result.ready->driver.cell->get_pin_state(cp) == PIN_SIG);
    std::set<CellPortKey> ends;
    std::set<NetInfo *> active, visited;
    std::set<CellInfo *> interiors;
    std::function<void(NetInfo *)> walk = [&](NetInfo *n) {
        NPNR_ASSERT(!active.count(n));
        if (!visited.insert(n).second)
            return;
        active.insert(n);
        for (auto &u : n->users) {
            auto c = u.cell;
            NPNR_ASSERT(c);
            if (c->type == id_MISTRAL_FF) {
                NPNR_ASSERT(u.port.in(id_ENA, id_SCLR) && c->getPort(id_CLK) == result.clock &&
                            c->get_pin_state(id_CLK) == PIN_SIG);
                int clocks = 0;
                NPNR_ASSERT(ctx->getPortTimingClass(c, u.port, clocks) == TMG_REGISTER_INPUT && clocks == 1);
                NPNR_ASSERT(ctx->getPortClockingInfo(c, u.port, 0).edge == RISING_EDGE);
                ends.insert(CellPortKey(u));
            } else {
                NPNR_ASSERT(c->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5,
                                       id_MISTRAL_ALUT6) &&
                            u.port != id_Q);
                interiors.insert(c);
                auto q = c->getPort(id_Q);
                NPNR_ASSERT(q);
                walk(q);
            }
        }
        active.erase(n);
    };
    walk(result.ready);
    NPNR_ASSERT(ends.size() == 224 && interiors.size() == 5);
    result.endpoints.assign(ends.begin(), ends.end());
    std::sort(result.endpoints.begin(), result.endpoints.end(), [&](auto &a, auto &b) {
        return std::make_pair(a.cell.str(ctx), a.port.str(ctx)) < std::make_pair(b.cell.str(ctx), b.port.str(ctx));
    });
    return result;
}
struct Evaluation
{
    std::vector<TimingAnalyser::EndpointDomainSlack> endpoints;
    std::map<IdString, ClockFmax> clocks;
};
Evaluation evaluate(Context *ctx, const Scope &s, const std::string &prefix)
{
    TimingAnalyser t(ctx);
    t.setup_only = false;
    t.with_clock_skew = true;
    t.setup(true, true, true);
    Evaluation result;
    std::ofstream f(prefix + "-endpoints.tsv");
    f << "cell\tport\tlaunch_clock\tlaunch_edge\tcapture_clock\tcapture_edge\tsetup_ps\thold_ps\n";
    for (auto &ep : s.endpoints) {
        auto d = t.get_endpoint_domain_slacks(ep);
        NPNR_ASSERT(d.size() == 1 && d[0].timed && d[0].launch.clock == s.clock->name &&
                    d[0].capture.clock == s.clock->name && d[0].launch.edge == RISING_EDGE &&
                    d[0].capture.edge == RISING_EDGE);
        NPNR_ASSERT(ready_fallback_policy::finite({d[0].setup, d[0].hold}) && t.get_setup_slack(ep) == d[0].setup);
        result.endpoints.push_back(d[0]);
        f << ep.cell.str(ctx) << '\t' << ep.port.str(ctx) << '\t' << d[0].launch.clock.str(ctx) << '\t'
          << int(d[0].launch.edge) << '\t' << d[0].capture.clock.str(ctx) << '\t' << int(d[0].capture.edge) << '\t'
          << d[0].setup << '\t' << d[0].hold << '\n';
    }
    f.close();
    NPNR_ASSERT(f);
    std::ofstream c(prefix + "-clocks.tsv");
    c << "clock\tachieved_mhz\tconstraint_mhz\n" << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (auto &e : t.get_timing_result().clock_fmax) {
        result.clocks[e.first] = e.second;
        c << e.first.str(ctx) << '\t' << e.second.achieved << '\t' << e.second.constraint << '\n';
    }
    c.close();
    NPNR_ASSERT(c && !result.clocks.empty());
    return result;
}
} // namespace
void diagnostic_ready_reuse(Context *ctx, const char *prefix)
{
    if (!prefix || !*prefix)
        return;
    auto original = ctx->cells.at(ctx->id(original_name)).get(), replica = ctx->cells.at(ctx->id(replica_name)).get(),
         sink = ctx->cells.at(ctx->id(sink_name)).get();
    NPNR_ASSERT(ready_reuse_equivalent(original, replica));
    auto old = original->getPort(id_Q), copy = replica->getPort(id_Q);
    NPNR_ASSERT(old->users.entries() == 107 && copy->users.entries() == 4 && sink->getPort(id_ENA) == old);
    auto s = scope(ctx);
    NPNR_ASSERT(ready_reuse_clock_valid(sink, s.clock));
    auto keep = ctx->id("keep"), dont_touch = ctx->id("dont_touch");
    auto ordinary = [&](const CellInfo *c) {
        return c->bel != BelId() && c->belStrength <= STRENGTH_WEAK && c->cluster == ClusterId() && !c->region &&
               !c->isPseudo() && !c->attrs.count(keep) && !c->attrs.count(dont_touch);
    };
    for (auto c : {original, replica, sink})
        NPNR_ASSERT(ordinary(c));
    auto loc = ctx->getBelLocation(sink->bel), a = ctx->getBelLocation(original->bel),
         b = ctx->getBelLocation(replica->bel);
    NPNR_ASSERT(loc.x == 34 && loc.y == 22 && loc.z == 14 && a.x == 24 && a.y == 20 && b.x == 37 && b.y == 25);
    int group = 0;
    for (auto n : {old, copy}) {
        NPNR_ASSERT(n->wires.empty() && !n->is_global && !n->clkconstr && !n->region &&
                    n->constant_value == IdString() && !n->attrs.count(keep) && !n->attrs.count(dont_touch));
        for (auto &u : n->users) {
            NPNR_ASSERT(u.port == id_ENA && ready_reuse_clock_valid(u.cell, s.clock) && ordinary(u.cell));
            auto l = ctx->getBelLocation(u.cell->bel);
            if (n == old && l.x == loc.x && l.y == loc.y) {
                ++group;
                NPNR_ASSERT(u.cell == sink);
            }
        }
    }
    NPNR_ASSERT(group == 1);
    for (auto &p : ctx->ports)
        NPNR_ASSERT(p.second.net != old && p.second.net != copy);
    for (auto &e : ctx->cells)
        if (e.second->bel != BelId()) {
            auto l = ctx->getBelLocation(e.second->bel);
            if (l.x == loc.x && l.y == loc.y)
                NPNR_ASSERT(ordinary(e.second.get()) && e.second->type != id_MISTRAL_MLAB);
            NPNR_ASSERT(ctx->isBelLocationValid(e.second->bel));
        }
    snapshot(ctx, std::string(prefix) + ".before.json");
    auto before = evaluate(ctx, s, std::string(prefix) + ".before");
    auto pin_before = pins(ctx);
    struct CellState
    {
        IdString type;
        BelId bel;
        PlaceStrength strength;
        dict<IdString, Property> params, attrs;
        std::map<IdString, std::tuple<NetInfo *, PortType, int>> ports;
    };
    std::map<IdString, CellState> saved;
    for (auto &e : ctx->cells) {
        auto c = e.second.get();
        CellState state{c->type, c->bel, c->belStrength, c->params, c->attrs, {}};
        for (auto &p : c->ports)
            state.ports[p.first] = {p.second.net, p.second.type, p.second.user_idx.idx()};
        saved.emplace(e.first, std::move(state));
    }
    std::map<NetInfo *, size_t> fanouts;
    for (auto &e : ctx->nets)
        fanouts[e.second.get()] = e.second->users.entries();
    ready_reuse_move(ctx, sink, copy);
    NPNR_ASSERT(old->users.entries() == 106 && copy->users.entries() == 5 && pins(ctx) == pin_before);
    std::ofstream legal(std::string(prefix) + ".legality.tsv");
    legal << "cell\tbel\tbefore_valid\tafter_valid\n";
    for (auto &e : ctx->cells)
        if (e.second->bel != BelId()) {
            NPNR_ASSERT(ctx->isBelLocationValid(e.second->bel));
            legal << e.first.str(ctx) << '\t' << ctx->getBelName(e.second->bel).str(ctx) << "\t1\t1\n";
        }
    legal.close();
    NPNR_ASSERT(legal);
    NPNR_ASSERT(ctx->cells.size() == saved.size() && ctx->nets.size() == fanouts.size());
    for (auto &e : ctx->cells) {
        auto c = e.second.get();
        auto &state = saved.at(e.first);
        NPNR_ASSERT(c->type == state.type && c->bel == state.bel && c->belStrength == state.strength &&
                    c->params == state.params && c->attrs == state.attrs && c->ports.size() == state.ports.size());
        for (auto &p : c->ports) {
            auto oldp = state.ports.at(p.first);
            if (c == sink && p.first == id_ENA)
                NPNR_ASSERT(p.second.net == copy && p.second.type == std::get<1>(oldp));
            else
                NPNR_ASSERT(std::make_tuple(p.second.net, p.second.type, p.second.user_idx.idx()) == oldp);
        }
    }
    for (auto &e : ctx->nets) {
        auto n = e.second.get();
        NPNR_ASSERT(n->users.entries() == fanouts.at(n) + (n == copy ? 1 : 0) - (n == old ? 1 : 0));
    }
    auto after_scope = scope(ctx);
    NPNR_ASSERT(after_scope.endpoints == s.endpoints);
    auto after = evaluate(ctx, after_scope, std::string(prefix) + ".after");
    for (size_t i = 0; i < before.endpoints.size(); ++i)
        NPNR_ASSERT(after.endpoints[i].hold >= std::min(before.endpoints[i].hold, delay_t(0)));
    ctx->check();
    snapshot(ctx, std::string(prefix) + ".after.json");
    std::ofstream audit(std::string(prefix) + ".audit.json");
    audit << "{\"original\":\"" << original_name << "\",\"replica\":\"" << replica_name << "\",\"sink\":\"" << sink_name
          << "\",\"old_fanout_before\":107,\"old_fanout_after\":106,\"replica_fanout_before\":4,\"replica_fanout_"
             "after\":5,\"lab_group_size\":1,\"endpoints\":224,\"memory_clock\":\""
          << s.clock->name.str(ctx)
          << "\",\"clock_constraint_present\":true,\"placements_preserved\":true,\"pins_preserved\":true,\"input_loads_"
             "preserved\":true,\"other_connections_preserved\":true,\"indexed_users_preserved\":true,\"predicted_hold_"
             "nonregressing\":true}\n";
    audit.close();
    NPNR_ASSERT(audit);
}
void diagnostic_ready_reuse_timing(Context *ctx, const char *prefix)
{
    if (!prefix || !*prefix)
        return;
    NPNR_ASSERT(ctx->bitstream_configured && ctx->analogue_cache_valid && ctx->pip_delay_calibrated);
    NPNR_ASSERT(ctx->cells.at(ctx->id(original_name))->getPort(id_Q)->users.entries() == 106 &&
                ctx->cells.at(ctx->id(replica_name))->getPort(id_Q)->users.entries() == 5);
    auto graph = json(ctx);
    auto pin_before = pins(ctx);
    auto observed = ctx->pip_delay_observed;
    auto types = ctx->pip_type_calibration;
    auto prior = ctx->pip_delay_prior;
    auto settings = ctx->settings;
    std::map<IdString, Tree> routes;
    std::map<IdString, int> udata;
    for (auto &e : ctx->nets) {
        udata[e.first] = e.second->udata;
        for (auto &w : e.second->wires)
            routes[e.first][w.first] = {w.second.pip, w.second.strength};
    }
    std::map<const PortRef *, std::pair<bool, Quad>> cache;
    for (auto &e : ctx->analogue_arc_cache)
        cache[e.first] = {e.second.ok, quad(e.second.delay)};
    auto s = scope(ctx);
    evaluate(ctx, s, std::string(prefix) + ".final");
    NPNR_ASSERT(json(ctx) == graph && pins(ctx) == pin_before && ctx->settings == settings &&
                ctx->pip_delay_observed == observed && ctx->pip_delay_prior == prior && ctx->pip_delay_calibrated &&
                ctx->analogue_cache_valid && ctx->bitstream_configured);
    for (size_t i = 0; i < types.size(); ++i)
        NPNR_ASSERT(types[i].hops == ctx->pip_type_calibration[i].hops &&
                    types[i].table_ps == ctx->pip_type_calibration[i].table_ps &&
                    types[i].analogue_ps == ctx->pip_type_calibration[i].analogue_ps);
    for (auto &e : ctx->nets) {
        Tree t;
        for (auto &w : e.second->wires)
            t[w.first] = {w.second.pip, w.second.strength};
        NPNR_ASSERT(t == routes[e.first] && e.second->udata == udata.at(e.first));
    }
    NPNR_ASSERT(cache.size() == ctx->analogue_arc_cache.size());
    for (auto &e : ctx->analogue_arc_cache)
        NPNR_ASSERT(cache.at(e.first) == std::make_pair(e.second.ok, quad(e.second.delay)));
    std::ofstream f(std::string(prefix) + ".final-audit.json");
    f << "{\"read_only\":true,\"endpoints\":224,\"graph_preserved\":true,\"pins_preserved\":true,\"routes_preserved\":"
         "true,\"cache_preserved\":true,\"calibration_preserved\":true,\"clock_constraint_present\":true}\n";
    f.close();
    NPNR_ASSERT(f);
}
NEXTPNR_NAMESPACE_END
