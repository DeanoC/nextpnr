/* Bounded, transactional placement repair. SPDX-License-Identifier: ISC */
#include "critical_cohort.h"
#include "critical_cohort_model.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>
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
                NPNR_ASSERT(ctx->get_lut_pin_delay(half ? id_D : id_C, false, logic));
                delay_t incoming = ctx->predictDelay(net->driver.cell->bel, pins.front(), lut, half ? id_D : id_C);
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
                        if (model->active() && sink_end && seed.size() == 1 && cell->type == id_MISTRAL_FF)
                            focus.emplace_back(cell->name, hops[i].port);
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
NEXTPNR_NAMESPACE_END
