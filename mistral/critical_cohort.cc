/* Bounded, transactional placement repair. SPDX-License-Identifier: ISC */
#include "critical_cohort.h"
#include "critical_cohort_model.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include "io_delay.h"
#include "json11.hpp"
#include "log.h"
#include "place_common.h"
#include "remap_clock_guard.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
namespace guard = mistral_remap_clock_guard;
constexpr int kMaxCells = 128;
constexpr int kMaxDestinations = 32;
constexpr int kMaxHops = 8;
constexpr int kSearchNodes = 10000;
constexpr int kRadius = 2;

bool allowed(Context *ctx, CellInfo *cell)
{
    if (!cell || cell->isPseudo() || cell->bel == BelId() || cell->region ||
        cell->belStrength > (cell->cluster == ClusterId() ? STRENGTH_WEAK : STRENGTH_STRONG))
        return false;
    if (!(ctx->is_comb_cell(cell->type) || cell->type == id_MISTRAL_FF) ||
        (cell->type == id_MISTRAL_ALUT_ARITH && cell->cluster == ClusterId()))
        return false;
    for (const char *attr : {"keep", "dont_touch", "BEL", "FES_SLOT"})
        if (cell->attrs.count(ctx->id(attr)))
            return false;
    return true;
}

bool close_clusters(Context *ctx, std::set<CellInfo *> &cells)
{
    std::vector<CellInfo *> pending(cells.begin(), cells.end());
    for (size_t i = 0; i < pending.size(); ++i) {
        CellInfo *cell = pending[i];
        if (!allowed(ctx, cell) || pending.size() > kMaxCells)
            return false;
        if (cell->cluster == ClusterId())
            continue;
        CellInfo *root = ctx->getClusterRootCell(cell->cluster);
        std::vector<std::pair<CellInfo *, BelId>> members;
        if (root->bel == BelId() || !ctx->getClusterPlacement(cell->cluster, root->bel, members))
            return false;
        for (const auto &member : members) {
            // A malformed starting cluster is not a repair candidate.
            if (member.first->bel != member.second)
                return false;
            if (cells.insert(member.first).second)
                pending.push_back(member.first);
        }
    }
    return cells.size() <= kMaxCells;
}

std::vector<CellInfo *> occupants(Context *ctx, int x, int y)
{
    std::vector<CellInfo *> result;
    for (BelId bel : ctx->getBelsByTile(x, y))
        if (auto *cell = ctx->getBoundBelCell(bel))
            result.push_back(cell);
    return result;
}

struct SavedBinding
{
    CellInfo *cell;
    BelId bel;
    PlaceStrength strength;
};

class Transaction
{
  public:
    Transaction(Context *ctx, const std::set<CellInfo *> &cells) : ctx(ctx), labs(ctx->labs)
    {
        for (CellInfo *cell : cells)
            bindings.push_back({cell, cell->bel, cell->belStrength});
        std::sort(bindings.begin(), bindings.end(),
                  [](const SavedBinding &a, const SavedBinding &b) { return a.cell->name < b.cell->name; });
    }
    ~Transaction()
    {
        if (!kept)
            restore();
    }
    void unbind()
    {
        for (const auto &saved : bindings)
            if (saved.cell->bel != BelId())
                ctx->unbindBel(saved.cell->bel);
    }
    void restore()
    {
        unbind();
        for (const auto &saved : bindings)
            ctx->bindBel(saved.bel, saved.cell, saved.strength);
        ctx->labs = labs;
    }
    void commit() { kept = true; }
    Context *ctx;
    std::vector<SavedBinding> bindings;
    std::vector<LABInfo> labs;
    bool kept = false;
};

using Placement = std::vector<std::pair<CellInfo *, BelId>>;

bool translated(Context *ctx, const std::set<CellInfo *> &cells, int dx, int dy, Placement &placement, int dz = 0)
{
    if (dz &&
        (cells.size() != 1 || (*cells.begin())->type != id_MISTRAL_FF || (*cells.begin())->cluster != ClusterId()))
        return false;
    for (CellInfo *cell : cells) {
        Loc old = ctx->getBelLocation(cell->bel);
        BelId target = ctx->getBelByLocation(Loc(old.x + dx, old.y + dy, old.z + dz));
        if (target == BelId() || !ctx->isValidBelForCellType(cell->type, target) || !cell->testRegion(target) ||
            !ctx->fes_placement_allowed(target, cell))
            return false;
        placement.emplace_back(cell, target);
    }
    std::sort(placement.begin(), placement.end(),
              [](const auto &a, const auto &b) { return a.first->name < b.first->name; });
    return true;
}

// Check every occupied BEL: placing a LUT can invalidate an unmoved FF in
// the same ALM, and removing a clock user can alter the shared LAB controls.
bool legal_labs(Context *ctx, const std::set<uint32_t> &labs)
{
    for (uint32_t lab : labs)
        for (const auto &alm : ctx->labs.at(lab).alms)
            for (BelId bel :
                 {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2], alm.ff_bels[3]})
                if (ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel))
                    return false;
    return true;
}

struct Unit
{
    CellInfo *root;
    BelId home;
    std::vector<CellInfo *> cells;
    PlaceStrength strength;
};

// Re-place complete displaced units. Only original participants may move;
// a third occupant discovered during the search is an obstruction.
class Replacer
{
  public:
    Context *ctx;
    const Transaction &saved;
    std::vector<Unit> units;
    std::vector<BelId> vacancies;
    std::set<uint32_t> labs;
    int dx, dy, budget;
    CriticalCohortStats &stats;

    std::vector<BelId> candidates(const Unit &unit)
    {
        std::vector<BelId> result;
        auto add = [&](BelId bel) {
            if (bel != BelId() && unit.root->cluster != ClusterId() && unit.root->constr_abs_z) {
                Loc loc = ctx->getBelLocation(bel);
                bel = ctx->getBelByLocation(Loc(loc.x, loc.y, unit.root->constr_z));
            }
            if (bel != BelId() && ctx->isValidBelForCellType(unit.root->type, bel) &&
                std::find(result.begin(), result.end(), bel) == result.end())
                result.push_back(bel);
        };
        Loc home = ctx->getBelLocation(unit.home);
        // Keep non-colliding guests at home before trying an exchange. A small
        // cohort should not shuffle unrelated occupants of its destination.
        add(unit.home);
        for (BelId bel : ctx->getBelsByTile(home.x, home.y))
            add(bel);
        for (int radius = 1; radius <= kRadius; ++radius)
            for (int y = home.y - radius; y <= home.y + radius; ++y)
                for (int x = home.x - radius; x <= home.x + radius; ++x) {
                    if (std::max(std::abs(x - home.x), std::abs(y - home.y)) != radius || x < 0 || y < 0 ||
                        x >= ctx->getGridDimX() || y >= ctx->getGridDimY())
                        continue;
                    add(ctx->getBelByLocation(Loc(x, y, home.z)));
                }
        add(ctx->getBelByLocation(Loc(home.x - dx, home.y - dy, home.z)));
        for (BelId bel : vacancies)
            add(bel);
        for (Loc origin : {Loc(home.x - dx, home.y - dy, home.z), home})
            for (int radius = 0; radius <= kRadius; ++radius)
                for (int y = origin.y - radius; y <= origin.y + radius; ++y)
                    for (int x = origin.x - radius; x <= origin.x + radius; ++x) {
                        if (std::max(std::abs(x - origin.x), std::abs(y - origin.y)) != radius || x < 0 || y < 0 ||
                            x >= ctx->getGridDimX() || y >= ctx->getGridDimY())
                            continue;
                        for (BelId bel : ctx->getBelsByTile(x, y))
                            add(bel);
                    }
        return result;
    }

    bool run(size_t index)
    {
        if (index == units.size())
            return legal_labs(ctx, labs);
        const Unit &unit = units[index];
        for (BelId bel : candidates(unit)) {
            if (stats.nodes >= budget)
                return false;
            ++stats.nodes;
            Placement placement;
            if (unit.root->cluster != ClusterId()) {
                if (!ctx->getClusterPlacement(unit.root->cluster, bel, placement))
                    continue;
            } else {
                placement.emplace_back(unit.root, bel);
            }
            std::set<BelId> distinct;
            bool available = placement.size() == unit.cells.size();
            for (const auto &member : placement) {
                available &= member.second != BelId() && distinct.insert(member.second).second &&
                             ctx->checkBelAvail(member.second) && member.first->testRegion(member.second) &&
                             ctx->fes_placement_allowed(member.second, member.first);
            }
            if (!available)
                continue;
            auto previous_labs = labs;
            for (const auto &member : placement) {
                auto strength = unit.strength;
                for (const auto &binding : saved.bindings)
                    if (binding.cell == member.first)
                        strength = binding.strength;
                ctx->bindBel(member.second, member.first, strength);
                labs.insert(ctx->bel_data(member.second).lab_data.lab);
            }
            if (legal_labs(ctx, labs) && run(index + 1))
                return true;
            for (const auto &member : placement)
                ctx->unbindBel(member.second);
            labs = std::move(previous_labs);
        }
        return false;
    }
};

bool data_net(const NetInfo *net)
{
    return net && !net->is_global && !net->clkconstr && net->driver.cell && net->driver.cell->bel != BelId() &&
           !net->driver.cell->type.in(id_MISTRAL_CONST, id_GND, id_VCC);
}

struct Hop
{
    CellInfo *driver, *sink;
    delay_t delay;
    IdString port;
};

// Score the complete cohort boundary, including side loads. Prediction only
// orders trials; fresh STA decides whether the complete placement survives.
int64_t score(Context *ctx, TimingAnalyser &timing, const std::set<CellInfo *> &cohort, const Placement &future,
              delay_t band)
{
    std::map<CellInfo *, BelId> target;
    for (const auto &member : future)
        target[member.first] = member.second;
    int64_t total = 0;
    std::set<std::pair<NetInfo *, CellPortKey>> seen;
    auto arc = [&](NetInfo *net, const PortRef &user) {
        if (!data_net(net) || !user.cell || user.cell->bel == BelId() || !seen.emplace(net, CellPortKey(user)).second)
            return;
        auto *driver = net->driver.cell;
        BelId src = target.count(driver) ? target.at(driver) : driver->bel;
        BelId dst = target.count(user.cell) ? target.at(user.cell) : user.cell->bel;
        auto src_pins = ctx->getBelPinsForCellPin(driver, net->driver.port);
        auto dst_pins = ctx->getBelPinsForCellPin(user.cell, user.port);
        if (src_pins.empty() || dst_pins.empty())
            return;
        int weight = timing.get_setup_slack(CellPortKey(user)) <= band ? 8 : 1;
        total += int64_t(weight) * ctx->predictDelay(src, src_pins.front(), dst, dst_pins.front());
    };
    for (CellInfo *cell : cohort)
        for (const auto &port : cell->ports) {
            NetInfo *net = port.second.net;
            if (port.second.type == PORT_IN)
                arc(net, {cell, port.first});
            else if (data_net(net))
                for (const auto &user : net->users)
                    arc(net, user);
        }
    return total;
}

using EndpointRows = std::map<CellPortKey, guard::Rows>;
EndpointRows endpoint_rows(Context *ctx, TimingAnalyser &timing)
{
    EndpointRows result;
    for (const auto &entry : ctx->cells)
        for (const auto &port : entry.second->ports) {
            int count = 0;
            auto klass = ctx->getPortTimingClass(entry.second.get(), port.first, count);
            if (klass != TMG_REGISTER_INPUT && klass != TMG_ENDPOINT)
                continue;
            guard::Rows rows;
            if (timing.get_endpoint_clock_pair_timings({entry.first, port.first}, rows) && !rows.empty())
                result.emplace(CellPortKey(entry.first, port.first), std::move(rows));
        }
    return result;
}

bool safe_endpoints(const EndpointRows &before, const EndpointRows &after)
{
    if (before.size() != after.size())
        return false;
    for (const auto &entry : before) {
        auto found = after.find(entry.first);
        if (found == after.end() || !guard::rows_match(entry.second, found->second))
            return false;
        for (size_t i = 0; i < entry.second.size(); ++i) {
            const auto &old = entry.second[i], &now = found->second[i];
            if (old.setup_timed) {
                if (!old.setup_margin || !now.setup_margin)
                    return false;
                // Preserve each endpoint, including those already failing.
                delay_t floor = std::min(delay_t(0), *old.setup_margin);
                if (*now.setup_margin < floor)
                    return false;
            }
            if (old.hold_related &&
                (!old.hold_margin || !now.hold_margin || *now.hold_margin < std::min(delay_t(0), *old.hold_margin)))
                return false;
            if (!old.setup_timed &&
                (now.max_path_delay > old.max_path_delay || now.min_path_delay < old.min_path_delay))
                return false;
        }
    }
    return true;
}

delay_t worst_slack(TimingAnalyser &timing)
{
    delay_t worst = std::numeric_limits<delay_t>::max();
    for (const auto &entry : timing.get_timing_result().clock_setup_slack)
        worst = std::min(worst, entry.second);
    return worst;
}

struct TimingGuard
{
    bool gain = false, clocks = false, holds = false, endpoints = false;
    bool accepts() const { return gain && clocks && holds && endpoints; }
};

TimingGuard timing_guard(Context *ctx, TimingAnalyser &before, TimingAnalyser &after,
                         const std::vector<CellPortKey> &focus = {}, const EndpointRows *saved_before = nullptr)
{
    TimingGuard result;
    if (before.have_loops || after.have_loops)
        return result;
    result.gain = worst_slack(before) < 0 && worst_slack(after) >= int64_t(worst_slack(before)) + 20;
    if (!focus.empty()) {
        float old_focus = std::numeric_limits<float>::max(), new_focus = old_focus;
        for (const auto &port : focus) {
            float old = before.get_setup_slack(port), now = after.get_setup_slack(port);
            if (!guard::timed(old) || !guard::timed(now))
                return {};
            old_focus = std::min(old_focus, old);
            new_focus = std::min(new_focus, now);
        }
        result.gain =
                worst_slack(before) < 0 && worst_slack(after) >= worst_slack(before) && new_focus >= old_focus + 20;
    }
    result.clocks = guard::clocks_nonregressing(before, after);
    result.holds = guard::holds_nonregressing(guard::holds(before), guard::holds(after));
    result.endpoints =
            safe_endpoints(saved_before ? *saved_before : endpoint_rows(ctx, before), endpoint_rows(ctx, after));
    return result;
}

struct Guidance
{
    std::vector<Hop> hops;
    std::vector<CellPortKey> endpoints;
};

PortRef guidance_sink(Context *ctx, const json11::Json::array &segments, size_t index)
{
    const auto &pin = segments.at(index)["to"];
    const auto name = pin["cell"].string_value();
    auto cell = ctx->cells.find(ctx->id(name));
    if (cell != ctx->cells.end())
        return {cell->second.get(), ctx->id(pin["port"].string_value())};
    // A route-through inserted after placement is not in the input graph.
    // Resolve only the exact buffer pattern generated by reassign_alm_inputs,
    // ending at a known FF. The caller still validates its original driver.
    if (index + 2 >= segments.size() || pin["port"].string_value() != "A")
        return {};
    const auto &logic = segments.at(index + 1), &local = segments.at(index + 2);
    auto matches = [&](const json11::Json &p, const char *port) {
        return p["cell"].string_value() == name && p["port"].string_value() == port;
    };
    if (logic["type"].string_value() != "logic" || !matches(logic["from"], "A") || !matches(logic["to"], "Q") ||
        local["type"].string_value() != "routing" || !matches(local["from"], "Q") ||
        local["to"]["port"].string_value() != "DATAIN")
        return {};
    auto ff = ctx->cells.find(ctx->id(local["to"]["cell"].string_value()));
    if (ff == ctx->cells.end() || ff->second->type != id_MISTRAL_FF || name != ff->second->name.str(ctx) + "$ROUTETHRU")
        return {};
    return {ff->second.get(), id_DATAIN};
}

Guidance routed_guidance(Context *ctx, TimingAnalyser &timing)
{
    Guidance result;
    if (ctx->critical_cohort_report.empty())
        return result;
    std::string error;
    auto report = json11::Json::parse(ctx->critical_cohort_report, error);
    if (!error.empty() || !report["critical_paths"].is_array() ||
        !report["timing_summary"]["final_analogue_model"].bool_value())
        log_error("Critical cohort guidance requires a final analogue timing report.\n");
    for (const auto &path : report["critical_paths"].array_items()) {
        const auto domain = path["from"].string_value();
        if (domain != path["to"].string_value() || domain.size() <= 8 ||
            (domain.substr(0, 8) != "posedge " && domain.substr(0, 8) != "negedge "))
            continue;
        const std::string clock = domain.substr(8);
        const auto &old_clock = report["fmax"][clock];
        if (!old_clock["achieved"].is_number() || !old_clock["constraint"].is_number() ||
            old_clock["achieved"].number_value() >= old_clock["constraint"].number_value())
            continue;
        const auto &clocks = timing.get_timing_result().clock_fmax;
        auto found = clocks.find(ctx->id(clock));
        if (found == clocks.end() || std::abs(found->second.constraint - old_clock["constraint"].number_value()) > 1e-4)
            log_error("Critical cohort guidance clock '%s' does not match this design.\n", clock.c_str());
        const auto &segments = path["path"].array_items();
        if (segments.empty() || segments.back()["type"].string_value() != "setup")
            continue;
        CellPortKey endpoint(ctx->id(segments.back()["to"]["cell"].string_value()),
                             ctx->id(segments.back()["to"]["port"].string_value()));
        auto cell = ctx->cells.find(endpoint.cell);
        int count = 0;
        if (cell == ctx->cells.end() || !cell->second->getPort(endpoint.port) ||
            ctx->getPortTimingClass(cell->second.get(), endpoint.port, count) != TMG_REGISTER_INPUT)
            log_error("Critical cohort guidance endpoint does not match this design.\n");
        result.endpoints.push_back(endpoint);
        for (size_t i = 0; i < segments.size(); ++i) {
            const auto &segment = segments.at(i);
            if (segment["type"].string_value() != "routing" || !segment["delay"].is_number())
                continue;
            auto driver = ctx->cells.find(ctx->id(segment["from"]["cell"].string_value()));
            PortRef sink = guidance_sink(ctx, segments, i);
            if (driver == ctx->cells.end() || !sink.cell)
                continue;
            NetInfo *net = sink.cell->getPort(sink.port);
            if (!data_net(net) || net->driver.cell != driver->second.get() ||
                net->driver.port != ctx->id(segment["from"]["port"].string_value()))
                continue;
            double ps = segment["delay"].number_value() * 1000;
            if (!std::isfinite(ps) || ps <= 0 || ps >= double(std::numeric_limits<delay_t>::max()))
                continue;
            result.hops.push_back({driver->second.get(), sink.cell, delay_t(ps), sink.port});
        }
    }
    if (result.hops.empty() || result.endpoints.empty())
        log_error("Critical cohort guidance has no matching hops on a failing clock.\n");
    log_info("Critical cohort: routed guidance has %zu hops and %zu setup endpoints.\n", result.hops.size(),
             result.endpoints.size());
    return result;
}
bool frozen_lab(Context *ctx, uint32_t lab)
{
    for (const auto &alm : ctx->labs.at(lab).alms)
        for (BelId bel :
             {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2], alm.ff_bels[3]})
            if (auto *cell = ctx->getBoundBelCell(bel))
                if (cell->belStrength == STRENGTH_LOCKED)
                    return true;
    return false;
}
} // namespace

void critical_cohort_pin_preview(Context *ctx, const std::function<void()> &inspect)
{
    struct Restore
    {
        Context *ctx;
        std::vector<LABInfo> labs;
        std::vector<std::pair<CellInfo *, dict<IdString, ArchPinInfo>>> pins;
        explicit Restore(Context *ctx) : ctx(ctx), labs(ctx->labs)
        {
            for (const auto &entry : ctx->cells) {
                auto *cell = entry.second.get();
                if (ctx->is_comb_cell(cell->type) || cell->type.in(id_MISTRAL_MLAB, id_MISTRAL_BUF))
                    pins.emplace_back(cell, cell->pin_data);
            }
        }
        ~Restore()
        {
            for (auto &entry : pins)
                entry.first->pin_data = std::move(entry.second);
            ctx->labs = std::move(labs);
        }
    } restore(ctx);
    for (uint32_t lab = 0; lab < ctx->labs.size(); ++lab) {
        if (frozen_lab(ctx, lab))
            continue;
        for (uint8_t alm = 0; alm < 10; ++alm)
            ctx->assign_alm_lut_inputs(lab, alm);
    }
    inspect();
}

void critical_cohort_setup_timing(Context *ctx, TimingAnalyser &timing)
{
    timing.setup(false, false, true);
    bool adjusted = false;
    for (uint32_t lab = 0; lab < ctx->labs.size(); ++lab) {
        if (frozen_lab(ctx, lab))
            continue;
        for (uint8_t alm = 0; alm < 10; ++alm)
            for (uint8_t half = 0; half < 2; ++half) {
                auto *ff = ctx->get_alm_route_through_ff(lab, alm, half);
                if (!ff)
                    continue;
                auto *net = ff->getPort(id_DATAIN);
                if (!net || !net->driver.cell || net->driver.cell->bel == BelId())
                    continue;
                const auto pins = ctx->getBelPinsForCellPin(net->driver.cell, net->driver.port);
                if (pins.empty())
                    continue;
                BelId lut = ctx->labs.at(lab).alms.at(alm).lut_bels.at(half);
                DelayQuad logic;
                const IdString rt_pin = ctx->alm_route_through_pin(lab, alm, half);
                NPNR_ASSERT(ctx->get_lut_pin_delay(rt_pin, false, logic));
                delay_t incoming = ctx->predictDelay(net->driver.cell->bel, pins.front(), lut, rt_pin);
                delay_t local = ctx->predictDelay(lut, id_COMBOUT, ff->bel, id_DATAIN);
                // Fold the buffer's two route arcs and logic into DATAIN.
                // This preserves native early/late analysis without adding
                // cells, nets or backpointers to a temporary graph.
                timing.set_route_delay({ff->name, id_DATAIN}, DelayPair(incoming + local) + logic.delayPair());
                adjusted = true;
            }
    }
    if (adjusted)
        timing.run(false, false, false, true);
}

std::vector<CellInfo *> critical_cohort_neighborhood(Context *ctx, CellInfo *cell)
{
    if (!allowed(ctx, cell))
        return {};
    Loc home = ctx->getBelLocation(cell->bel);
    std::set<CellInfo *> cohort{cell};
    auto include = [&](CellInfo *neighbor) {
        if (!allowed(ctx, neighbor))
            return;
        Loc loc = ctx->getBelLocation(neighbor->bel);
        if (loc.x == home.x && loc.y == home.y)
            cohort.insert(neighbor);
    };
    for (const auto &port : cell->ports) {
        NetInfo *net = port.second.net;
        if (!data_net(net))
            continue;
        if (port.second.type == PORT_IN)
            include(net->driver.cell);
        else
            for (const auto &user : net->users)
                include(user.cell);
    }
    if (!close_clusters(ctx, cohort))
        return {};
    std::vector<CellInfo *> result(cohort.begin(), cohort.end());
    std::sort(result.begin(), result.end(), [](CellInfo *a, CellInfo *b) { return a->name < b->name; });
    return result;
}

static bool critical_cohort_trial_impl(Context *ctx, const std::vector<CellInfo *> &seed, int dx, int dy,
                                       int node_budget, const std::function<bool()> &accept, CriticalCohortStats &stats,
                                       bool full_lab, int dz)
{
    stats = {};
    if (seed.empty() || (!dx && !dy && !dz) || node_budget <= 0)
        return false;
    std::set<CellInfo *> cohort(seed.begin(), seed.end());
    if (!close_clusters(ctx, cohort))
        return false;
    Placement target;
    if (!translated(ctx, cohort, dx, dy, target, dz))
        return false;
    std::set<std::pair<int, int>> target_tiles;
    std::set<std::pair<uint32_t, uint8_t>> target_alms;
    for (const auto &member : target) {
        Loc loc = ctx->getBelLocation(member.second);
        target_tiles.emplace(loc.x, loc.y);
        const auto &data = ctx->bel_data(member.second).lab_data;
        target_alms.emplace(data.lab, data.alm);
    }
    std::set<CellInfo *> guests;
    for (auto tile : target_tiles)
        for (CellInfo *cell : occupants(ctx, tile.first, tile.second)) {
            if (cohort.count(cell) || guests.count(cell))
                continue;
            const auto &data = ctx->bel_data(cell->bel).lab_data;
            if (!full_lab && !target_alms.count({data.lab, data.alm}))
                continue;
            std::set<CellInfo *> unit{cell};
            // Protected occupants remain fixed. A direct collision with one
            // fails below, while other sites in its LAB remain usable.
            if (close_clusters(ctx, unit))
                guests.insert(unit.begin(), unit.end());
        }
    if (!close_clusters(ctx, guests))
        return false;
    for (CellInfo *cell : cohort)
        guests.erase(cell);
    std::set<CellInfo *> participants = cohort;
    participants.insert(guests.begin(), guests.end());
    if (participants.size() > kMaxCells)
        return false;
    stats.cells = int(cohort.size());
    stats.displaced = int(guests.size());
    Transaction transaction(ctx, participants);
    Replacer replacer{ctx, transaction, {}, {}, {}, dx, dy, node_budget, stats};
    std::set<ClusterId> clusters;
    for (const auto &binding : transaction.bindings) {
        replacer.labs.insert(ctx->bel_data(binding.bel).lab_data.lab);
        if (cohort.count(binding.cell))
            replacer.vacancies.push_back(binding.bel);
        if (!guests.count(binding.cell))
            continue;
        CellInfo *root = binding.cell;
        std::vector<CellInfo *> members{root};
        if (root->cluster != ClusterId()) {
            if (!clusters.insert(root->cluster).second)
                continue;
            root = ctx->getClusterRootCell(root->cluster);
            members.clear();
            Placement cluster;
            if (!ctx->getClusterPlacement(root->cluster, root->bel, cluster))
                return false;
            for (const auto &member : cluster)
                members.push_back(member.first);
        }
        replacer.units.push_back({root, root->bel, members, root->belStrength});
    }
    std::sort(replacer.vacancies.begin(), replacer.vacancies.end(), [&](BelId a, BelId b) {
        Loc x = ctx->getBelLocation(a), y = ctx->getBelLocation(b);
        return std::tie(x.x, x.y, x.z) < std::tie(y.x, y.y, y.z);
    });
    std::sort(replacer.units.begin(), replacer.units.end(), [](const Unit &a, const Unit &b) {
        if (a.cells.size() != b.cells.size())
            return a.cells.size() > b.cells.size();
        bool a_ff = a.root->type == id_MISTRAL_FF, b_ff = b.root->type == id_MISTRAL_FF;
        return a_ff != b_ff ? !a_ff : a.root->name < b.root->name;
    });
    transaction.unbind();
    for (const auto &member : target) {
        if (!ctx->checkBelAvail(member.second))
            return false;
        PlaceStrength strength = STRENGTH_WEAK;
        for (const auto &binding : transaction.bindings)
            if (binding.cell == member.first)
                strength = binding.strength;
        ctx->bindBel(member.second, member.first, strength);
        replacer.labs.insert(ctx->bel_data(member.second).lab_data.lab);
    }
    if (!legal_labs(ctx, replacer.labs) || !replacer.run(0))
        return false;
    for (const auto &binding : transaction.bindings)
        if (binding.cell->bel == BelId() || get_constraints_distance(ctx, binding.cell) != 0)
            return false;
    stats.timed = true;
    if (!accept())
        return false;
    ctx->check();
    transaction.commit();
    return true;
}

bool critical_cohort_trial(Context *ctx, const std::vector<CellInfo *> &seed, int dx, int dy, int node_budget,
                           const std::function<bool()> &accept, CriticalCohortStats &stats, int dz)
{
    // Start with colliding ALMs, retaining other LAB occupants. Evacuating the
    // full LAB is a fallback for shared input/control legality, not the first
    // arrangement tested. Both searches share the caller's node budget.
    if (critical_cohort_trial_impl(ctx, seed, dx, dy, node_budget, accept, stats, false, dz))
        return true;
    if (stats.timed || stats.nodes >= node_budget)
        return false;
    int spent = stats.nodes;
    bool kept = critical_cohort_trial_impl(ctx, seed, dx, dy, node_budget - spent, accept, stats, true, dz);
    stats.nodes += spent;
    return kept;
}

namespace {
bool swap_movable(Context *ctx, CellInfo *cell)
{
    return allowed(ctx, cell) && cell->cluster == ClusterId() && cell->constr_children.empty() &&
           cell->constr_x == 0 && cell->constr_y == 0 && cell->constr_z == 0 && !cell->constr_abs_z &&
           cell->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5,
                         id_MISTRAL_ALUT6);
}
}

bool critical_cohort_swap_trial(Context *ctx, CellInfo *cell, BelId target, const std::function<bool()> &accept)
{
    if (!swap_movable(ctx, cell) || target == BelId() || cell->bel == target)
        return false;
    auto *guest = ctx->getBoundBelCell(target);
    if (guest && !swap_movable(ctx, guest))
        return false;
    BelId home = cell->bel;
    auto compatible = [&](CellInfo *c, BelId b) {
        if (!ctx->isValidBelForCellType(c->type, b) || !c->testRegion(b) || !ctx->fes_placement_allowed(b, c))
            return false;
        for (const auto &pin : c->pin_data)
            for (IdString physical : pin.second.bel_pins)
                if (ctx->getBelPinWire(b, physical) == WireId())
                    return false;
        return true;
    };
    if (!compatible(cell, target) || (guest && !compatible(guest, home)))
        return false;
    std::set<CellInfo *> participants{cell};
    if (guest) participants.insert(guest);
    Transaction transaction(ctx, participants);
    auto strength = cell->belStrength;
    auto guest_strength = guest ? guest->belStrength : STRENGTH_WEAK;
    transaction.unbind();
    ctx->bindBel(target, cell, strength);
    if (guest) ctx->bindBel(home, guest, guest_strength);
    if (!legal_labs(ctx, {ctx->bel_data(home).lab_data.lab, ctx->bel_data(target).lab_data.lab}) || !accept())
        return false;
    ctx->check();
    transaction.commit();
    return true;
}

void repair_critical_swaps(Context *ctx, int timing_budget)
{
    if (!timing_budget) return;
    if (timing_budget < 0 || timing_budget > 64 || ctx->fes_any_slot_region_active)
        log_error("Critical swaps require an ordinary placed design and a budget of 1..64.\n");
    for (const auto &entry : ctx->nets)
        if (!entry.second->wires.empty()) log_error("Critical swaps require an unrouted design.\n");
    int timed = 0, attempted = 0, kept = 0;
    const int max_attempts = timing_budget * 128;
    while (timed < timing_budget && attempted < max_attempts) {
        TimingAnalyser before(ctx);
        before.with_clock_skew = false;
        EndpointRows rows;
        Guidance guidance;
        critical_cohort_pin_preview(ctx, [&]() {
            critical_cohort_setup_timing(ctx, before);
            rows = endpoint_rows(ctx, before);
            guidance = routed_guidance(ctx, before);
        });
        if (before.have_loops || worst_slack(before) >= 0 || rows.empty()) break;
        if (guidance.hops.empty()) log_error("Critical swaps require routed report guidance.\n");
        struct Candidate { CellInfo *cell; BelId target; int64_t delta; };
        std::vector<std::vector<Candidate>> proposals;
        std::set<IdString> seen;
        // Rank long reported hops deterministically, rather than spending the
        // source budget on the prefix of just the first reported path.
        auto hops = guidance.hops;
        std::sort(hops.begin(), hops.end(), [&](const Hop &a, const Hop &b) {
            // A carry chain cannot be freely translated, but its ordinary
            // result mux can. Reserve discovery priority for that frontier.
            bool ac = a.driver->type == id_MISTRAL_ALUT_ARITH && swap_movable(ctx, a.sink);
            bool bc = b.driver->type == id_MISTRAL_ALUT_ARITH && swap_movable(ctx, b.sink);
            if (ac != bc) return ac > bc;
            if (a.delay != b.delay) return a.delay > b.delay;
            return std::tie(a.sink->name, a.driver->name, a.port) <
                   std::tie(b.sink->name, b.driver->name, b.port);
        });
        for (const auto &hop : hops) {
            for (auto *cell : {hop.sink, hop.driver}) {
                if (!swap_movable(ctx, cell) || !seen.insert(cell->name).second) continue;
                auto *anchor = cell == hop.sink ? hop.driver : hop.sink;
                Loc center = ctx->getBelLocation(anchor->bel);
                std::vector<Candidate> candidates;
                for (int x = center.x - 3; x <= center.x + 3; ++x)
                    for (int y = center.y - 3; y <= center.y + 3; ++y) {
                        if (std::abs(x - center.x) + std::abs(y - center.y) > 3) continue;
                        for (BelId target : ctx->getBelsByTile(x, y)) {
                            if (target == cell->bel || !ctx->isValidBelForCellType(cell->type, target)) continue;
                            auto *guest = ctx->getBoundBelCell(target);
                            if (guest && !swap_movable(ctx, guest)) continue;
                            std::set<CellInfo *> members{cell};
                            Placement future{{cell, target}}, original{{cell, cell->bel}};
                            if (guest) {
                                members.insert(guest);
                                future.emplace_back(guest, cell->bel);
                                original.emplace_back(guest, target);
                            }
                            // Rank every incoming and outgoing connection of
                            // both exchanged cells, rather than the chosen hop.
                            auto delta = score(ctx, before, members, future, worst_slack(before) + 150) -
                                         score(ctx, before, members, original, worst_slack(before) + 150);
                            candidates.push_back({cell, target, delta});
                        }
                    }
                std::sort(candidates.begin(), candidates.end(), [&](const Candidate &a, const Candidate &b) {
                    Loc x = ctx->getBelLocation(a.target), y = ctx->getBelLocation(b.target);
                    return std::tie(a.delta, x.x, x.y, x.z) < std::tie(b.delta, y.x, y.y, y.z);
                });
                std::vector<Candidate> diverse;
                std::map<std::pair<int, int>, int> per_tile;
                int checked = 0;
                for (const auto &candidate : candidates) {
                    auto at = ctx->getBelLocation(candidate.target);
                    if (per_tile[{at.x, at.y}] >= 4) continue;
                    if (checked++ == 256 || attempted >= max_attempts) break;
                    ++attempted;
                    bool legal = false;
                    critical_cohort_swap_trial(ctx, cell, candidate.target, [&]() { legal = true; return false; });
                    if (!legal) continue;
                    ++per_tile[{at.x, at.y}];
                    diverse.push_back(candidate);
                    if (diverse.size() == 32) break;
                }
                proposals.push_back(std::move(diverse));
                if (proposals.size() == 8) break;
            }
            if (proposals.size() == 8) break;
        }
        bool improved = false;
        for (int rank = 0; rank < 32 && !improved && timed < timing_budget && attempted < max_attempts; ++rank)
            for (const auto &proposal : proposals) {
                if (timed >= timing_budget || attempted >= max_attempts) break;
                if (size_t(rank) >= proposal.size()) continue;
                const auto &candidate = proposal[rank];
                auto home = candidate.cell->bel;
                ++attempted;
                bool accepted = critical_cohort_swap_trial(ctx, candidate.cell, candidate.target, [&]() {
                    ++timed;
                    TimingAnalyser after(ctx);
                    after.with_clock_skew = false;
                    bool safe = false;
                    critical_cohort_pin_preview(ctx, [&]() {
                        critical_cohort_setup_timing(ctx, after);
                        // Permit progress on tied failing endpoints, retaining
                        // the cohort's full-design endpoint/clock/hold guards.
                        for (const auto &endpoint : guidance.endpoints) {
                            if (after.get_setup_slack(endpoint) < before.get_setup_slack(endpoint) + 20) continue;
                            auto checks = timing_guard(ctx, before, after, {endpoint}, &rows);
                            log_info("Critical swap guards: gain=%d clocks=%d holds=%d endpoints=%d.\n",
                                     int(checks.gain), int(checks.clocks), int(checks.holds), int(checks.endpoints));
                            safe = checks.accepts();
                            break;
                        }
                        log_info("Critical swap trial %d: cell=%s target=%s worst=%ld->%ldps accepted=%d.\n",
                                 timed, ctx->nameOf(candidate.cell), ctx->nameOfBel(candidate.target),
                                 long(worst_slack(before)), long(worst_slack(after)), int(safe));
                    });
                    return safe;
                });
                if (accepted) {
                    log_info("Critical swap kept: cell=%s from=%s to=%s.\n", ctx->nameOf(candidate.cell),
                             ctx->nameOfBel(home), ctx->nameOfBel(candidate.target));
                    ++kept; improved = true; break;
                }
            }
        if (!improved) break;
    }
    log_info("Critical swaps: attempted=%d/%d timed=%d/%d kept=%d; predicted only, full routing required.\n",
             attempted, max_attempts, timed, timing_budget, kept);
}

void repair_critical_cohorts(Context *ctx, int timing_budget)
{
    if (timing_budget <= 0 || !ctx->setting<bool>("timing_driven"))
        return;
    int timed = 0, attempted = 0, kept = 0, nodes = 0;
    // Deterministic work limits also bound runs with no legal STA candidate.
    const int max_attempts = std::min(128, timing_budget * 8);
    const int max_nodes = 100000;
    auto remaining = [&]() { return timed < timing_budget && attempted < max_attempts && nodes < max_nodes; };
    std::unique_ptr<CriticalCohortRouteModel> model;
    while (remaining()) {
        TimingAnalyser before(ctx);
        // Global clock insertion is not a distance-proportional data route.
        // Before routing, its predicted skew can dominate candidate selection;
        // use ideal clocks here, retaining phases and related hold analysis.
        before.with_clock_skew = false;
        EndpointRows before_rows;
        critical_cohort_pin_preview(ctx, [&]() {
            critical_cohort_setup_timing(ctx, before);
            if (!model)
                model = std::make_unique<CriticalCohortRouteModel>(ctx, ctx->critical_cohort_report);
            model->apply(ctx, before);
            before_rows = endpoint_rows(ctx, before);
        });
        delay_t old_slack = worst_slack(before);
        if (before.have_loops || old_slack >= 0)
            break;
        struct Destination
        {
            int x, y, z;
            int64_t cost;
        };
        struct Cohort
        {
            std::vector<CellInfo *> seed;
            Loc home;
            std::vector<Destination> destinations;
            std::vector<CellPortKey> focus;
        };
        std::vector<Cohort> proposals;
        Guidance guidance;
        // Rank arcs using STA's physical pin map, then restore it before
        // changing any bindings for a placement trial.
        critical_cohort_pin_preview(ctx, [&]() {
            guidance = routed_guidance(ctx, before);
            // A complete calibrated model can follow the current worst paths
            // after each move, rather than repeatedly selecting stale report arcs.
            if (model->active())
                guidance = {};
            std::vector<Hop> hops = guidance.hops;
            for (const auto &entry : ctx->cells) {
                if (!guidance.hops.empty())
                    break;
                CellInfo *sink = entry.second.get();
                if (sink->isPseudo() || sink->bel == BelId())
                    continue;
                for (const auto &port : sink->ports) {
                    auto *net = port.second.net;
                    if (port.second.type != PORT_IN || !data_net(net) || net->driver.cell == sink ||
                        before.get_setup_slack({sink->name, port.first}) > old_slack + 150)
                        continue;
                    delay_t delay = model->active() ? model->route_delay(ctx, {sink, port.first})
                                                    : ctx->predictArcDelay(net, {sink, port.first});
                    if (delay >= 700)
                        hops.push_back({net->driver.cell, sink, delay, port.first});
                }
            }
            std::sort(hops.begin(), hops.end(), [](const Hop &a, const Hop &b) {
                if (a.delay != b.delay)
                    return a.delay > b.delay;
                return std::tie(a.sink->name, a.driver->name) < std::tie(b.sink->name, b.driver->name);
            });
            std::set<std::vector<IdString>> visited;
            if (model->active()) {
                std::vector<CellInfo *> registers;
                for (const auto &entry : ctx->cells) {
                    auto *ff = entry.second.get();
                    if (!allowed(ctx, ff) || ff->type != id_MISTRAL_FF || ff->cluster != ClusterId() ||
                        before.get_setup_slack({ff->name, id_DATAIN}) > old_slack + 150)
                        continue;
                    const auto &data = ctx->bel_data(ff->bel).lab_data;
                    auto *net = ff->getPort(id_DATAIN);
                    if (!data_net(net) || !ctx->is_comb_cell(net->driver.cell->type))
                        continue;
                    const auto &driver = ctx->bel_data(net->driver.cell->bel).lab_data;
                    auto pins = ctx->getBelPinsForCellPin(net->driver.cell, net->driver.port);
                    if (pins.empty() || pins.front() != id_COMBOUT ||
                        (data.lab == driver.lab && data.alm == driver.alm && data.idx / 2 == driver.idx))
                        continue;
                    registers.push_back(ff);
                }
                std::sort(registers.begin(), registers.end(), [&](CellInfo *a, CellInfo *b) {
                    auto sa = before.get_setup_slack({a->name, id_DATAIN});
                    auto sb = before.get_setup_slack({b->name, id_DATAIN});
                    return sa != sb ? sa < sb : a->name < b->name;
                });
                for (size_t i = 0; i < std::min(registers.size(), size_t(kMaxHops)); ++i) {
                    auto *ff = registers[i];
                    auto *driver = ff->getPort(id_DATAIN)->driver.cell;
                    const auto &data = ctx->bel_data(driver->bel).lab_data;
                    const auto &alm = ctx->labs.at(data.lab).alms.at(data.alm);
                    Loc home = ctx->getBelLocation(ff->bel);
                    std::vector<Destination> destinations;
                    for (int j = 0; j < 2; ++j) {
                        if (j && (!ctx->lab_ff4 || ctx->labs.at(data.lab).is_mlab))
                            continue;
                        Loc target = ctx->getBelLocation(alm.ff_bels.at(data.idx * 2 + j));
                        destinations.push_back({target.x, target.y, target.z, 0});
                    }
                    visited.insert({ff->name});
                    proposals.push_back({{ff}, home, std::move(destinations), {{ff->name, id_DATAIN}}});
                }
                log_info("Critical cohort: %zu direct LUT/register packing proposals.\n", proposals.size());
            }
            for (size_t i = 0; i < std::min(hops.size(), size_t(kMaxHops)); ++i) {
                for (bool sink_end : {true, false}) {
                    CellInfo *cell = sink_end ? hops[i].sink : hops[i].driver;
                    CellInfo *anchor = sink_end ? hops[i].driver : hops[i].sink;
                    Loc home = ctx->getBelLocation(cell->bel), other = ctx->getBelLocation(anchor->bel);
                    if (home.x == other.x && home.y == other.y)
                        continue;
                    auto neighborhood = critical_cohort_neighborhood(ctx, cell);
                    std::vector<std::vector<CellInfo *>> seeds{neighborhood};
                    if (model->active() && cell->type == id_MISTRAL_FF && cell->cluster == ClusterId() &&
                        neighborhood.size() > 1)
                        seeds.insert(seeds.begin(), {cell});
                    for (auto seed : seeds) {
                        std::vector<IdString> names;
                        for (CellInfo *member : seed)
                            names.push_back(member->name);
                        if (seed.empty() || !visited.insert(names).second)
                            continue;
                        std::set<CellInfo *> cohort(seed.begin(), seed.end());
                        if (!close_clusters(ctx, cohort))
                            continue;
                        std::vector<Destination> destinations;
                        for (int y = 0; y < ctx->getGridDimY(); ++y)
                            for (int x = 0; x < ctx->getGridDimX(); ++x) {
                                int distance = std::abs(x - other.x) + 2 * std::abs(y - other.y);
                                int old_distance = std::abs(home.x - other.x) + 2 * std::abs(home.y - other.y);
                                if (distance >= old_distance)
                                    continue;
                                std::vector<int> slots{home.z};
                                // A register by itself has no cohort arrangement
                                // to preserve. Let it reach its driver's ALM rather
                                // than keeping an arbitrary old register slot.
                                if (model->active() && cohort.size() == 1 && cell->type == id_MISTRAL_FF &&
                                    cell->cluster == ClusterId()) {
                                    slots.clear();
                                    for (BelId bel : ctx->getBelsByTile(x, y)) {
                                        if (ctx->getBelType(bel) != id_MISTRAL_FF)
                                            continue;
                                        const auto &data = ctx->bel_data(bel).lab_data;
                                        if (data.idx % 2 && (!ctx->lab_ff4 || ctx->labs.at(data.lab).is_mlab))
                                            continue;
                                        slots.push_back(ctx->getBelLocation(bel).z);
                                    }
                                }
                                for (int z : slots) {
                                    Placement future;
                                    if (translated(ctx, cohort, x - home.x, y - home.y, future, z - home.z))
                                        destinations.push_back(
                                                {x, y, z, score(ctx, before, cohort, future, old_slack + 150)});
                                }
                            }
                        std::sort(destinations.begin(), destinations.end(),
                                  [](const Destination &a, const Destination &b) {
                                      return std::tie(a.cost, a.y, a.x, a.z) < std::tie(b.cost, b.y, b.x, b.z);
                                  });
                        log_info("Critical cohort: neighborhood (%d,%d), %zu cells including complete clusters, %zu "
                                 "closer "
                                 "destinations.\n",
                                 home.x, home.y, cohort.size(), destinations.size());
                        if (destinations.size() > kMaxDestinations)
                            destinations.resize(kMaxDestinations);
                        std::vector<CellPortKey> focus;
                        int clock_info_count = 0;
                        if (model->active() &&
                            ctx->getPortTimingClass(hops[i].sink, hops[i].port, clock_info_count) == TMG_REGISTER_INPUT)
                            // Tied clocked endpoints need individual progress:
                            // shortening one memory address wire can leave the
                            // clock's worst margin unchanged. All endpoint and
                            // clock guards still apply to either moved end.
                            focus.emplace_back(hops[i].sink->name, hops[i].port);
                        proposals.push_back({std::move(seed), home, std::move(destinations), std::move(focus)});
                    }
                }
            }
        });
        bool improved = false;
        // Give both ends and other critical hops a trial before spending the
        // entire STA budget on another destination of the first hop.
        for (int d = 0; d < kMaxDestinations && !improved && remaining(); ++d) {
            for (const auto &proposal : proposals) {
                if (!remaining())
                    break;
                if (size_t(d) >= proposal.destinations.size())
                    continue;
                const auto &dest = proposal.destinations[d];
                const Loc home = proposal.home;
                CriticalCohortStats stats;
                ++attempted;
                delay_t next_slack = old_slack;
                bool accepted = critical_cohort_trial(
                        ctx, proposal.seed, dest.x - home.x, dest.y - home.y, std::min(kSearchNodes, max_nodes - nodes),
                        [&]() {
                            ++timed;
                            TimingAnalyser after(ctx);
                            after.with_clock_skew = false;
                            bool safe = false;
                            critical_cohort_pin_preview(ctx, [&]() {
                                critical_cohort_setup_timing(ctx, after);
                                model->apply(ctx, after);
                                next_slack = worst_slack(after);
                                auto checks = timing_guard(ctx, before, after,
                                                           proposal.focus.empty() ? guidance.endpoints : proposal.focus,
                                                           &before_rows);
                                log_info("Critical cohort guards: gain=%d clocks=%d holds=%d endpoints=%d.\n",
                                         int(checks.gain), int(checks.clocks), int(checks.holds),
                                         int(checks.endpoints));
                                safe = checks.accepts();
                            });
                            return safe;
                        },
                        stats, dest.z - home.z);
                nodes += stats.nodes;
                log_info("Critical cohort: (%d,%d)->(%d,%d) z=%d->%d cells=%d guests=%d nodes=%d timed=%d "
                         "slack=%ld->%ldps "
                         "kept=%d.\n",
                         home.x, home.y, dest.x, dest.y, home.z, dest.z, stats.cells, stats.displaced, stats.nodes,
                         int(stats.timed), long(old_slack), long(next_slack), int(accepted));
                if (accepted) {
                    ++kept;
                    improved = true;
                    break;
                }
            }
        }
        if (!improved)
            break;
    }
    log_info("Critical cohort: attempted=%d/%d nodes=%d/%d timed=%d/%d kept=%d; routing and final signoff still "
             "required.\n",
             attempted, max_attempts, nodes, max_nodes, timed, timing_budget, kept);
}

bool critical_cohort_timing_safe(Context *ctx, TimingAnalyser &before, TimingAnalyser &after,
                                 const std::vector<CellPortKey> &focus)
{
    return timing_guard(ctx, before, after, focus).accepts();
}
// A static source has no launch transition and therefore no setup/hold pair.
// Prove only direct constants and identity route-throughs; do not infer constants
// from LUT truth tables, absent tags, disabled clocks or matching net names.
static json11::Json static_endpoint_proof(Context *ctx, CellInfo *sink, IdString port)
{
    using J = json11::Json;
    auto describe = [&](CellInfo *cell) {
        J::object params, pins;
        for (const auto &p : cell->params) params[p.first.str(ctx)] = p.second.to_string();
        for (const auto &p : cell->pin_data) pins[p.first.str(ctx)] = int(p.second.state);
        return J::object{{"cell", cell->name.str(ctx)}, {"type", cell->type.str(ctx)},
                         {"params", params}, {"pins", pins}};
    };
    J::array chain, captures;
    int count = 0;
    if (ctx->getPortTimingClass(sink, port, count) != TMG_REGISTER_INPUT || count <= 0)
        return J(); // Do not exempt uncharacterized IO/control endpoints.
    for (int i = 0; i < count; ++i) {
        auto info = ctx->getPortClockingInfo(sink, port, i);
        auto *clock = sink->getPort(info.clock_port);
        if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay() <= 0 ||
            (info.edge != RISING_EDGE && info.edge != FALLING_EDGE))
            return J();
        captures.push_back(J::array{info.clock_port.str(ctx), clock->name.str(ctx), int(info.edge)});
    }
    auto *net = sink->getPort(port);
    pool<IdString> seen;
    while (net && net->driver.cell && seen.insert(net->name).second) {
        auto *cell = net->driver.cell;
        if (net->driver.port != id_Q || cell->getPort(id_Q) != net)
            return J();
        auto node = describe(cell);
        node["net"] = net->name.str(ctx);
        chain.push_back(node);
        if (cell->type == id_MISTRAL_CONST) {
            auto value = cell->params.find(id_LUT);
            if (cell->ports.size() != 1 || cell->ports.at(id_Q).type != PORT_OUT ||
                cell->params.size() != 1 || value == cell->params.end() || value->second.is_string ||
                value->second.str.find_first_not_of("01") != std::string::npos ||
                value->second.str.size() > 64 ||
                (value->second.as_int64() != 0 && value->second.as_int64() != 1))
                return J();
            return J::object{{"reason", "constant-no-launch-transition"}, {"value", int(value->second.as_int64())},
                             {"sink", describe(sink)}, {"capture", captures}, {"chain", chain}};
        }
        if (cell->type != id_MISTRAL_BUF || cell->ports.size() != 2 || !cell->params.empty() ||
            !cell->ports.count(id_A) || cell->ports.at(id_A).type != PORT_IN ||
            cell->ports.at(id_Q).type != PORT_OUT)
            return J();
        auto pin = cell->pin_data.find(id_A);
        if (pin != cell->pin_data.end() && pin->second.state != PIN_SIG)
            return J();
        net = cell->getPort(id_A);
    }
    return J(); // Undriven nets and cycles are not constants.
}

// Contracts preserve topology and intent, never invent timing arcs. In particular
// structural reset validation cannot discharge recovery/removal or MTBF.
static json11::Json interface_cell(Context *ctx, const CellInfo *cell)
{
    using J = json11::Json;
    J::object ports, params, attrs, pins;
    for (const auto &p : cell->ports)
        ports[p.first.str(ctx)] = J::array{int(p.second.type), p.second.net ? p.second.net->name.str(ctx) : ""};
    for (const auto &p : cell->params) params[p.first.str(ctx)] = p.second.to_string();
    for (const auto &p : cell->attrs) attrs[p.first.str(ctx)] = p.second.to_string();
    for (const auto &p : cell->pin_data) pins[p.first.str(ctx)] = int(p.second.state);
    return J::object{{"type", cell->type.str(ctx)}, {"ports", ports}, {"params", params},
                     {"attrs", attrs}, {"pins", pins}};
}

// Reuse the deliberately narrow constant/identity proof without pretending that
// an unregistered IO data input is a register capture. Only low data on an
// unregistered open-drain pad qualifies; OE remains an independent endpoint.
static json11::Json constant_open_drain(Context *ctx, CellInfo *cell, IdString port)
{
    using J = json11::Json;
    if (cell->type != id_MISTRAL_IO || port != id_I || !cell->params.empty() ||
        cell->get_pin_state(id_I) != PIN_SIG || cell->get_pin_state(id_OE) != PIN_SIG ||
        !cell->getPort(id_PAD) || !cell->getPort(id_OE) || !cell->getPort(id_OE)->driver.cell)
        return J();
    auto *net = cell->getPort(id_I);
    J::object chain;
    pool<IdString> seen;
    while (net && net->driver.cell && seen.insert(net->name).second) {
        auto *driver = net->driver.cell;
        if (net->driver.port != id_Q || driver->getPort(id_Q) != net) return J();
        chain[driver->name.str(ctx)] = interface_cell(ctx, driver);
        if (driver->type == id_MISTRAL_CONST) {
            auto v = driver->params.find(id_LUT);
            if (driver->ports.size() != 1 || driver->ports.at(id_Q).type != PORT_OUT ||
                driver->params.size() != 1 || v == driver->params.end() || v->second.is_string ||
                v->second.str.empty() || v->second.str.size() > 64 ||
                v->second.str.find_first_not_of("0") != std::string::npos)
                return J();
            if (cell->getPort(id_OE)->driver.cell->type == id_MISTRAL_CONST) return J();
            return J::object{{"reason", "constant-open-drain-data"}, {"value", 0},
                {"scope", "I-only; OE and input feedback remain independent"},
                {"cell", interface_cell(ctx, cell)}, {"chain", chain},
                {"oe_endpoint", J::array{cell->name.str(ctx), "OE"}}};
        }
        if (driver->type != id_MISTRAL_BUF || driver->ports.size() != 2 || !driver->params.empty() ||
            !driver->ports.count(id_A) || driver->ports.at(id_A).type != PORT_IN ||
            driver->ports.at(id_Q).type != PORT_OUT || driver->get_pin_state(id_A) != PIN_SIG)
            return J();
        net = driver->getPort(id_A);
    }
    return J();
}

static json11::Json reset_interface_contracts(Context *ctx, pool<IdString> &stages)
{
    using J = json11::Json;
    const auto setting = ctx->settings.find(ctx->id("timing/reset_contracts"));
    if (setting == ctx->settings.end()) return J::object{};
    std::string error;
    J declarations = J::parse(setting->second.as_string(), error);
    auto fail = [](const char *reason) { log_error("Invalid reset interface contract: %s.\n", reason); };
    if (!error.empty() || !declarations.is_array()) fail("expected array of ordered stage declarations");
    J::object result;
    for (const auto &declaration : declarations.array_items()) {
        if (!declaration.is_object() || declaration.object_items().size() != 1 ||
            !declaration["stages"].is_array() || declaration["stages"].array_items().size() < 2)
            fail("each declaration requires at least two stages and no other fields");
        std::vector<CellInfo *> chain;
        for (const auto &name : declaration["stages"].array_items()) {
            if (!name.is_string() || name.string_value().empty()) fail("invalid stage name");
            auto found = ctx->cells.find(ctx->id(name.string_value()));
            if (found == ctx->cells.end() || !stages.insert(found->first).second) fail("missing or repeated stage");
            chain.push_back(found->second.get());
        }
        auto *clock = chain.front()->getPort(id_CLK);
        auto *reset = chain.front()->getPort(id_ACLR);
        auto clock_state = chain.front()->get_pin_state(id_CLK);
        auto reset_state = chain.front()->get_pin_state(id_ACLR);
        if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay() <= 0 ||
            !reset || !reset->driver.cell || (clock_state != PIN_SIG && clock_state != PIN_INV) ||
            (reset_state != PIN_SIG && reset_state != PIN_INV)) fail("missing clock waveform or raw reset driver/polarity");
        J::object cells, cone;
        for (size_t i = 0; i < chain.size(); ++i) {
            auto *ff = chain[i];
            auto attr = ff->attrs.find(ctx->id("async_reg"));
            if (ff->type != id_MISTRAL_FF || !ff->params.empty() || attr == ff->attrs.end() ||
                !((!attr->second.is_string && attr->second.as_int64() == 1) ||
                  (attr->second.is_string && attr->second.as_string() == "true")))
                fail("stage requires native FF and explicit async_reg intent");
            if (ff->getPort(id_CLK) != clock || ff->getPort(id_ACLR) != reset ||
                ff->get_pin_state(id_CLK) != clock_state || ff->get_pin_state(id_ACLR) != reset_state ||
                ff->get_pin_state(id_DATAIN) != PIN_SIG || ff->get_pin_state(id_Q) != PIN_SIG)
                fail("stage clock, edge, reset or data polarity mismatch");
            for (auto pin : {id_ENA, id_SCLR, id_SLOAD})
                if (ff->getPort(pin) || ff->get_pin_state(pin) != (pin == id_ENA ? PIN_1 : PIN_0))
                    fail("stage override is not tied inactive");
            if (ff->getPort(id_SDATA)) fail("stage has connected synchronous load data");
            if (!i) {
                auto proof = static_endpoint_proof(ctx, ff, id_DATAIN);
                if (proof.is_null() || proof["value"].int_value() != 1)
                    fail("first D must be constant one opposite native FF reset zero");
                cone["constant_source"] = proof;
            } else if (ff->getPort(id_DATAIN) != chain[i-1]->getPort(id_Q))
                fail("stage data does not follow previous Q");
            auto *q = ff->getPort(id_Q);
            if (!q || q->driver.cell != ff || q->driver.port != id_Q || q->users.empty()) fail("missing stage Q fanout");
            if (i + 1 < chain.size() && (q->users.entries() != 1 ||
                q->users.begin()->cell != chain[i+1] || q->users.begin()->port != id_DATAIN))
                fail("intermediate Q must have sole next-stage D fanout");
            // A top-level alias would escape the users list.
            for (const auto &p : ctx->ports) if (p.second.net == q) fail("stage Q escapes to top level");
            cells[ff->name.str(ctx)] = interface_cell(ctx, ff);
        }
        J::array sinks, raw_fanout;
        pool<IdString> visiting, visited;
        std::function<bool(NetInfo *)> walk = [&](NetInfo *net) {
            if (!net || visiting.count(net->name)) return false;
            if (visited.count(net->name)) return true;
            visiting.insert(net->name);
            for (const auto &p : ctx->ports) if (p.second.net == net) return false;
            if (net->users.empty()) return false;
            for (const auto &user : net->users) {
                auto *cell = user.cell;
                int count = 0;
                auto kind = ctx->getPortTimingClass(cell, user.port, count);
                cone[cell->name.str(ctx)] = interface_cell(ctx, cell);
                if (kind == TMG_REGISTER_INPUT) {
                    if ((cell->type != id_MISTRAL_FF && !(cell->type.in(id_MISTRAL_SDROUT, id_MISTRAL_SDRIO) && user.port.in(id_I, id_CEOUT))) || user.port == id_ACLR || count != 1 ||
                        std::find(chain.begin(), chain.end(), cell) != chain.end()) return false;
                    auto info = ctx->getPortClockingInfo(cell, user.port, 0);
                    if (cell->getPort(info.clock_port) != clock ||
                        info.edge != (clock_state == PIN_INV ? FALLING_EDGE : RISING_EDGE)) return false;
                    sinks.push_back(J::array{cell->name.str(ctx), user.port.str(ctx)});
                } else if (kind == TMG_COMB_INPUT && (ctx->is_comb_cell(cell->type) || cell->type.in(id_MISTRAL_BUF, id_MISTRAL_NOT))) {
                    bool output = false;
                    for (const auto &p : cell->ports) if (p.second.type == PORT_OUT && p.second.net) {
                        int unused = 0;
                        if (ctx->getPortTimingClass(cell, p.first, unused) != TMG_COMB_OUTPUT || !walk(p.second.net)) return false;
                        output = true;
                    }
                    if (!output) return false;
                } else return false;
            }
            visiting.erase(net->name); visited.insert(net->name);
            return true;
        };
        if (!walk(chain.back()->getPort(id_Q)) || sinks.empty()) fail("final Q has unproved synchronous same-domain fanout");
        for (const auto &user : reset->users) raw_fanout.push_back(J::array{user.cell->name.str(ctx), user.port.str(ctx)});
        std::sort(raw_fanout.begin(), raw_fanout.end(), [](const J &a, const J &b) { return a.dump() < b.dump(); });
        std::sort(sinks.begin(), sinks.end(), [](const J &a, const J &b) { return a.dump() < b.dump(); });
        result[chain.front()->name.str(ctx)] = J::object{{"declaration", declaration}, {"stages", cells}, {"cone", cone},
            {"clock", clock->name.str(ctx)}, {"edge", int(clock_state == PIN_INV ? FALLING_EDGE : RISING_EDGE)},
            {"raw_reset", reset->name.str(ctx)}, {"raw_reset_driver", interface_cell(ctx, reset->driver.cell)},
            {"raw_reset_fanout", raw_fanout}, {"synchronous_sinks", sinks},
            {"reset_active", reset_state == PIN_INV ? 1 : 0}, {"reset_value", 0},
            {"status", "structurally-validated"}, {"physically_qualified", false}};
    }
    for (const auto &entry : result)
        for (const auto &user : entry.second["raw_reset_fanout"].array_items())
            if (user[1].string_value() != "ACLR" || !stages.count(ctx->id(user[0].string_value())))
                fail("raw reset fanout escapes declared chains");
    return result;
}

// Unlike placement guidance, a routed retention decision must account for every
// connected endpoint. Unsupported IO/constant/unknown-clock paths fail closed.
json11::Json critical_swap_endpoint_snapshot(Context *ctx, TimingAnalyser &timing, TimingAnalyser &unskewed)
{
    using J = json11::Json;
    J::object endpoints, clocks, static_endpoints, constant_outputs;
    pool<IdString> reset_stages;
    auto reset_contracts = reset_interface_contracts(ctx, reset_stages);
    J::array physical_obligations;
    if (!reset_stages.empty())
        physical_obligations.push_back("reset: minimum assertion pulse; PLL lock stability/relock; recovery/removal containment; metastability/MTBF");
    J::array unsupported;
    auto clock_key = [&](const ClockDomainKey &key) {
        return J::array{key.clock.str(ctx), int(key.edge)};
    };
    auto optional = [](const std::optional<delay_t> &v) { return v ? J(double(*v)) : J(); };
    for (const auto &net : ctx->nets) {
        if (!net.second->clkconstr) continue;
        const auto &c = *net.second->clkconstr;
        clocks[net.first.str(ctx)] = J::array{double(c.period.minDelay()), double(c.period.maxDelay()),
                double(c.high.minDelay()), double(c.high.maxDelay()), double(c.low.minDelay()),
                double(c.low.maxDelay()), double(c.phase_shift), c.phase_group.str(ctx)};
    }
    auto export_endpoint = [&](CellPortKey endpoint, bool external, bool registered_io) {
        std::vector<EndpointClockPairTiming> rows, reference;
        if (!timing.get_endpoint_clock_pair_timings(endpoint, rows, external, external, registered_io) || rows.empty() ||
            !unskewed.get_endpoint_clock_pair_timings(endpoint, reference, external, external, registered_io) ||
            !guard::rows_match(rows, reference)) return false;
        J::array values;
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto &row = rows[i];
            // Related setup/hold use signoff's actual clock skew. Untimed
            // extrema need the separate reference-free analyser frame.
            values.push_back(J::object{{"launch", clock_key(row.launch)}, {"capture", clock_key(row.capture)},
                {"setup_timed", row.setup_timed}, {"hold_related", row.hold_related},
                {"setup_window", optional(row.setup_window)}, {"setup", optional(row.setup_margin)},
                {"hold", optional(row.hold_margin)}, {"max_path", double(reference[i].max_path_delay)},
                {"min_path", double(reference[i].min_path_delay)}});
        }
        endpoints[J(J::array{endpoint.cell.str(ctx), endpoint.port.str(ctx)}).dump()] = values;
        return true;
    };
    J::object registered_contracts, primitive_clocks;
    for (const auto &entry : timing.get_primitive_clock_timings()) {
        const auto &r = entry.requirement;
        J key = J::array{entry.port.cell.str(ctx), entry.port.port.str(ctx)};
        primitive_clocks[key.dump()] = J::object{{"clock", entry.clock.str(ctx)},
            {"limits", J::array{double(r.min_period), double(r.min_high), double(r.min_low)}},
            {"route", J::array{double(entry.route.minDelay()), double(entry.route.maxDelay())}},
            {"margins", J::array{entry.margins[0], entry.margins[1], entry.margins[2]}}};
    }
    for (const auto &cell : ctx->cells) {
        for (const auto &port : cell.second->ports) {
            if (!port.second.net) continue;
            int count = 0;
            auto kind = ctx->getPortTimingClass(cell.second.get(), port.first, count);
            // Inventory physical output boundaries even when the architecture's
            // ordinary unconstrained timing classification ignores them. Fabric
            // register inputs never stand in for a private clock-to-pad capture.
            const auto type = cell.second->type;
            const bool unregistered_output = (type == id_MISTRAL_OB && port.first == id_I) ||
                (type == id_MISTRAL_IO && port.first.in(id_I, id_OE));
            const bool registered_pad = port.first == id_PAD && type.in(id_MISTRAL_SDROUT,
                id_MISTRAL_DDROUT, id_MISTRAL_SDRIO, id_MISTRAL_DDRBIDIR, id_MISTRAL_SDRIN, id_MISTRAL_DDRIN);
            if (kind != TMG_REGISTER_INPUT && kind != TMG_ENDPOINT &&
                !unregistered_output && !registered_pad) continue;
            J key = J::array{cell.first.str(ctx), port.first.str(ctx)};
            if (registered_pad) {
                for (bool input : {false, true}) {
                    auto models = ctx->getRegisteredIoTiming(cell.second.get(), port.first, input);
                    const bool required = input ? (type.in(id_MISTRAL_SDRIN, id_MISTRAL_DDRIN) ||
                        (type == id_MISTRAL_SDRIO && cell.second->params.count(ctx->id("IOREG_IN")) &&
                         cell.second->params.at(ctx->id("IOREG_IN")).as_int64())) :
                        type.in(id_MISTRAL_SDROUT, id_MISTRAL_DDROUT, id_MISTRAL_SDRIO, id_MISTRAL_DDRBIDIR);
                    if (models.empty()) {
                        if (required) unsupported.push_back(key);
                        continue;
                    }
                    J::object channels;
                    bool complete = true;
                    for (const auto &model : models) {
                        const TimingAnalyser::RegisteredIoEndpoint *boundary = nullptr;
                        for (const auto &entry : timing.get_registered_io_endpoints())
                            if (entry.pad == CellPortKey(cell.first, port.first) && entry.input == input &&
                                entry.model.name == model.name) boundary = &entry;
                        J clock_identity = J::array{cell.first.str(ctx), model.clocking.clock_port.str(ctx)};
                        if (!boundary || !primitive_clocks.count(clock_identity.dump()) ||
                            !export_endpoint(boundary->endpoint, true, true)) { complete = false; continue; }
                        const auto &info = model.clocking;
                        auto pair = [](DelayPair d) { return J::array{double(d.minDelay()), double(d.maxDelay())}; };
                        channels[model.name.str(ctx)] = J::object{
                            {"endpoint", J::array{boundary->endpoint.cell.str(ctx), boundary->endpoint.port.str(ctx)}},
                            {"clock_port", info.clock_port.str(ctx)}, {"edge", int(info.edge)},
                            {"setup", pair(info.setup)}, {"hold", pair(info.hold)},
                            {"clock_to_q_rise", pair(info.clockToQ.rise)}, {"clock_to_q_fall", pair(info.clockToQ.fall)}};
                    }
                    if (complete && !channels.empty()) {
                        J::object params, pins, attrs, connections;
                        for (const auto &p : cell.second->params) params[p.first.str(ctx)] = p.second.to_string();
                        for (const auto &p : cell.second->pin_data) pins[p.first.str(ctx)] = int(p.second.state);
                        for (const auto &p : cell.second->attrs) attrs[p.first.str(ctx)] = p.second.to_string();
                        for (const auto &p : cell.second->ports) connections[p.first.str(ctx)] = J::array{
                            int(p.second.type), p.second.net ? p.second.net->name.str(ctx) : ""};
                        std::string top_port;
                        for (const auto &constraint : read_io_delays(ctx)) {
                            auto root = ctx->ports.find(ctx->id(constraint["port"].string_value()));
                            if (constraint["input"].bool_value() == input && root != ctx->ports.end() &&
                                root->second.net == port.second.net) top_port = constraint["port"].string_value();
                        }
                        if (top_port.empty()) { unsupported.push_back(key); continue; }
                        J identity = J::array{cell.first.str(ctx), port.first.str(ctx), input};
                        registered_contracts[identity.dump()] = J::object{{"net", port.second.net->name.str(ctx)}, {"port", top_port},
                            {"type", cell.second->type.str(ctx)}, {"bel", cell.second->bel == BelId() ? "" : ctx->getBelName(cell.second->bel).str(ctx)},
                            {"params", params}, {"pins", pins}, {"attrs", attrs}, {"ports", connections}, {"channels", channels}};
                    } else unsupported.push_back(key);
                }
                continue;
            }
            if (port.first == id_ACLR && reset_stages.count(cell.first)) continue;
            if (unregistered_output) {
                auto proof = constant_open_drain(ctx, cell.second.get(), port.first);
                if (!proof.is_null()) { constant_outputs[key.dump()] = proof; continue; }
            }
            // FF ACLR's legacy synchronous approximation is not recovery/removal.
            const bool external = !(cell.second->type == id_MISTRAL_FF && port.first == id_ACLR);
            if (!export_endpoint({cell.first, port.first}, external, false)) {
                auto proof = static_endpoint_proof(ctx, cell.second.get(), port.first);
                if (!proof.is_null()) static_endpoints[key.dump()] = proof;
                else unsupported.push_back(key);
            }
        }
    }
    for (const auto &contract : reset_contracts.object_items())
        for (const auto &sink : contract.second["synchronous_sinks"].array_items())
            if (!endpoints.count(sink.dump()))
                log_error("Reset contract final consumer lacks native setup/hold endpoint: %s.\n", sink.dump().c_str());
    // Preserve the complete explicit boundary contract, including min/max and
    // launch clock/edge. A changed budget must never masquerade as route gain.
    auto io_constraints = read_io_delays(ctx);
    // Retain the physical/logical output binding as well as the SDC numbers.
    // In particular, data and OE share a pad contract but are separate endpoints.
    J::object output_contracts;
    for (const auto &constraint : io_constraints) {
        if (constraint["input"].bool_value()) continue;
        auto root = ctx->ports.find(ctx->id(constraint["port"].string_value()));
        if (root == ctx->ports.end() || !root->second.net) continue;
        auto *net = root->second.net;
        pool<IdString> cells;
        if (net->driver.cell) cells.insert(net->driver.cell->name);
        for (const auto &user : net->users) cells.insert(user.cell->name);
        J::object bindings;
        for (auto name : cells) {
            auto *cell = ctx->cells.at(name).get();
            J::object ports, params, pins, attrs;
            for (const auto &pin : cell->ports)
                ports[pin.first.str(ctx)] = J::array{int(pin.second.type),
                    pin.second.net ? pin.second.net->name.str(ctx) : ""};
            for (const auto &param : cell->params) params[param.first.str(ctx)] = param.second.to_string();
            for (const auto &pin : cell->pin_data) pins[pin.first.str(ctx)] = int(pin.second.state);
            for (const auto &attr : cell->attrs) attrs[attr.first.str(ctx)] = attr.second.to_string();
            bindings[name.str(ctx)] = J::object{{"type", cell->type.str(ctx)}, {"ports", ports},
                {"params", params}, {"pins", pins}, {"attrs", attrs}};
        }
        output_contracts[constraint["port"].string_value()] = J::object{
            {"net", net->name.str(ctx)}, {"direction", int(root->second.type)}, {"cells", bindings}};
    }
    std::sort(io_constraints.begin(), io_constraints.end(), [](const J &a, const J &b) { return a.dump() < b.dump(); });
    std::sort(unsupported.begin(), unsupported.end(), [](const J &a, const J &b) { return a.dump() < b.dump(); });
    unsupported.erase(std::unique(unsupported.begin(), unsupported.end()), unsupported.end());
    return J::object{{"schema", 6}, {"constant_outputs", constant_outputs}, {"reset_contracts", reset_contracts}, {"physical_obligations", physical_obligations}, {"registered_contracts", registered_contracts}, {"primitive_clocks", primitive_clocks}, {"complete", unsupported.empty() && physical_obligations.empty() && !endpoints.empty()},
        {"io_constraints", io_constraints}, {"output_contracts", output_contracts}, {"unsupported", unsupported}, {"static_endpoints", static_endpoints},
        {"clocks", clocks}, {"endpoints", endpoints}};
}
NEXTPNR_NAMESPACE_END
