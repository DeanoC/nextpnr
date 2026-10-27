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
        (cells.size() != 1 || (*cells.begin())->cluster != ClusterId() ||
         !(*cells.begin())->type.in(id_MISTRAL_FF, id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                    id_MISTRAL_ALUT5, id_MISTRAL_ALUT6, id_MISTRAL_NOT)))
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

using EndpointRows = CriticalCohortEndpointRows;
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

Guidance routed_guidance(Context *ctx, TimingAnalyser &timing, const std::string &document)
{
    Guidance result;
    if (document.empty())
        return result;
    std::string error;
    auto report = json11::Json::parse(document, error);
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

bool critical_cohort_proposal_rows_safe(const CriticalCohortEndpointRows &before,
                                       const CriticalCohortEndpointRows &after)
{
    if (before.empty() || before.size() != after.size())
        return false;
    int64_t old_worst = std::numeric_limits<int64_t>::max(), new_worst = old_worst;
    int64_t old_negative = 0, new_negative = 0;
    for (const auto &entry : before) {
        auto found = after.find(entry.first);
        if (found == after.end() || entry.second.empty() || !guard::rows_match(entry.second, found->second))
            return false;
        for (size_t i = 0; i < entry.second.size(); ++i) {
            const auto &old = entry.second[i], &now = found->second[i];
            if (old.setup_timed) {
                if (!old.setup_window || *old.setup_window <= 0 || !old.setup_margin || !now.setup_margin ||
                    *now.setup_margin < *old.setup_margin)
                    return false;
                old_worst = std::min(old_worst, int64_t(*old.setup_margin));
                new_worst = std::min(new_worst, int64_t(*now.setup_margin));
                old_negative += std::min(int64_t(0), int64_t(*old.setup_margin));
                new_negative += std::min(int64_t(0), int64_t(*now.setup_margin));
            } else if (now.max_path_delay > old.max_path_delay || now.min_path_delay < old.min_path_delay) {
                return false;
            }
            if (old.hold_related &&
                (!old.hold_margin || !now.hold_margin ||
                 (*old.hold_margin > 0 ? *now.hold_margin <= 0 : *now.hold_margin < *old.hold_margin)))
                return false;
        }
    }
    return old_worst != std::numeric_limits<int64_t>::max() &&
           (new_worst >= old_worst + 20 || (new_worst == old_worst && new_negative >= old_negative + 20));
}

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
                // The temporary LUT input ends at GOUT. Its block-input mux
                // is charged on the cell side just like an inserted BUF.
                CellInfo route_through(ctx, ctx->id("$critical_cohort_route_through"), id_MISTRAL_BUF);
                logic += ctx->get_lut_input_delay(&route_through, id_A);
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
                                       bool full_lab, int dz, bool keep_registers)
{
    stats = {};
    if (seed.empty() || (!dx && !dy && !dz) || node_budget <= 0)
        return false;
    std::set<CellInfo *> cohort(seed.begin(), seed.end());
    if (!close_clusters(ctx, cohort))
        return false;
    auto has_register = [](const std::set<CellInfo *> &cells) {
        return std::any_of(cells.begin(), cells.end(), [](CellInfo *c) { return c->type == id_MISTRAL_FF; });
    };
    if (keep_registers && has_register(cohort))
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
            if (close_clusters(ctx, unit) && (!keep_registers || !has_register(unit)))
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
                           const std::function<bool()> &accept, CriticalCohortStats &stats, int dz, bool keep_registers)
{
    // Start with colliding ALMs, retaining other LAB occupants. Evacuating the
    // full LAB is a fallback for shared input/control legality, not the first
    // arrangement tested. Both searches share the caller's node budget.
    if (critical_cohort_trial_impl(ctx, seed, dx, dy, node_budget, accept, stats, false, dz, keep_registers))
        return true;
    if (stats.timed || stats.nodes >= node_budget)
        return false;
    int spent = stats.nodes;
    bool kept = critical_cohort_trial_impl(ctx, seed, dx, dy, node_budget - spent, accept, stats, true, dz, keep_registers);
    stats.nodes += spent;
    return kept;
}

static void run_critical_cohorts(Context *ctx, int timing_budget, const std::string &model_report,
                                 const std::function<bool(const CriticalCohortPrediction &)> &emit, int candidates)
{
    if (timing_budget <= 0 || !ctx->setting<bool>("timing_driven"))
        return;
    int timed = 0, attempted = 0, kept = 0, nodes = 0, emitted = 0;
    std::vector<SavedBinding> fixed_registers;
    if (emit)
        for (const auto &entry : ctx->cells)
            if (entry.second->type == id_MISTRAL_FF)
                fixed_registers.push_back({entry.second.get(), entry.second->bel, entry.second->belStrength});
    // Deterministic work limits also bound runs with no legal STA candidate.
    const int max_attempts = std::min(128, timing_budget * 8);
    const int max_nodes = 100000;
    auto remaining = [&]() {
        return timed < timing_budget && attempted < max_attempts && nodes < max_nodes &&
               (!emit || emitted < candidates);
    };
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
                model = std::make_unique<CriticalCohortRouteModel>(ctx, model_report);
            if (emit && !model->active())
                log_error("Independent cohort proposals require a complete measured route model.\n");
            if (!model->apply(ctx, before))
                log_error("Parent routing-cost prediction is incomplete.\n");
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
            bool direct_packing = false;
            bool keep_registers = false;
        };
        std::vector<Cohort> proposals;
        std::vector<Hop> route_hops;
        Guidance guidance;
        // Rank arcs using STA's physical pin map, then restore it before
        // changing any bindings for a placement trial.
        critical_cohort_pin_preview(ctx, [&]() {
            guidance = routed_guidance(ctx, before, ctx->critical_cohort_report);
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
                    if (port.second.type != PORT_IN || !data_net(net) || net->driver.cell == sink)
                        continue;
                    auto slack = before.get_setup_slack({sink->name, port.first});
                    bool near_worst = slack <= old_slack + 150;
                    bool route_candidate = emit && ctx->routed_placement_preserve_routes && guard::timed(slack) && slack < 0;
                    if (!near_worst && !route_candidate) continue;
                    delay_t delay = model->active() ? model->route_delay(ctx, {sink, port.first})
                                                    : ctx->predictArcDelay(net, {sink, port.first});
                    if (delay >= 700) {
                        Hop hop{net->driver.cell, sink, delay, port.first};
                        if (near_worst) hops.push_back(hop);
                        if (route_candidate) route_hops.push_back(hop);
                    }
                }
            }
            std::sort(hops.begin(), hops.end(), [](const Hop &a, const Hop &b) {
                if (a.delay != b.delay)
                    return a.delay > b.delay;
                return std::tie(a.sink->name, a.driver->name) < std::tie(b.sink->name, b.driver->name);
            });
            if (emit && ctx->routed_placement_preserve_routes) {
                auto choices = std::move(route_hops);
                route_hops.clear();
                std::sort(choices.begin(), choices.end(), [&](const Hop &a, const Hop &b) {
                    auto sa = before.get_setup_slack({a.sink->name, a.port});
                    auto sb = before.get_setup_slack({b.sink->name, b.port});
                    if (sa != sb) return sa < sb;
                    if (a.delay != b.delay) return a.delay > b.delay;
                    return std::tie(a.sink->name, a.port) < std::tie(b.sink->name, b.port);
                });
                std::set<IdString> seen_nets;
                for (const auto &hop : choices) {
                    auto *net = critical_cohort_logical_net(ctx, hop.sink, hop.port);
                    if (net && seen_nets.insert(net->name).second) route_hops.push_back(hop);
                    if (route_hops.size() == size_t(std::min(8, std::max(1, timing_budget / 4)))) break;
                }
            }
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
                    proposals.push_back({{ff}, home, std::move(destinations), {{ff->name, id_DATAIN}}, true});
                    // The reverse packing choice leaves every register and
                    // its clock sink at the measured parent location.
                    if (emit && allowed(ctx, driver) && driver->cluster == ClusterId() &&
                        driver->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                         id_MISTRAL_ALUT5, id_MISTRAL_ALUT6, id_MISTRAL_NOT)) {
                        const auto &ff_data = ctx->bel_data(ff->bel).lab_data;
                        const auto &ff_alm = ctx->labs.at(ff_data.lab).alms.at(ff_data.alm);
                        Loc target = ctx->getBelLocation(ff_alm.lut_bels.at(ff_data.idx / 2));
                        proposals.push_back({{driver}, ctx->getBelLocation(driver->bel),
                                             {{target.x, target.y, target.z, 0}}, {{ff->name, id_DATAIN}}, false, true});
                    }
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
        if (emit) {
            // Keep the existing caller's order untouched. Independent routed
            // candidates alternate fixed-register LUT packing, FF packing,
            // and neighborhood families under the same total budget.
            std::vector<Cohort> logic, packing, neighborhood;
            for (auto &proposal : proposals) {
                if (!proposal.keep_registers && !proposal.direct_packing) {
                    auto fixed = proposal;
                    fixed.seed.erase(std::remove_if(fixed.seed.begin(), fixed.seed.end(), [&](CellInfo *cell) {
                        return !ctx->is_comb_cell(cell->type) || cell->type == id_MISTRAL_CONST;
                    }), fixed.seed.end());
                    fixed.keep_registers = true;
                    if (!fixed.seed.empty()) logic.push_back(std::move(fixed));
                }
                auto &family = proposal.keep_registers ? logic : proposal.direct_packing ? packing : neighborhood;
                family.push_back(std::move(proposal));
            }
            proposals.clear();
            if (ctx->routed_placement_preserve_routes) {
                // Captured native clock/control reservations are valid only
                // while every FF stays at its parent BEL. Spend this lane's
                // budget on supported logic moves with fixed registers.
                proposals = std::move(logic);
            } else
                for (size_t i = 0; i < std::max({logic.size(), packing.size(), neighborhood.size()}); ++i) {
                    if (i < logic.size()) proposals.push_back(std::move(logic[i]));
                    if (i < packing.size()) proposals.push_back(std::move(packing[i]));
                    if (i < neighborhood.size()) proposals.push_back(std::move(neighborhood[i]));
                }
        }
        std::vector<std::vector<CriticalCohortBranchRepair>> branch_choices;
        for (const auto &hop : route_hops)
            branch_choices.push_back(model->branch_repairs(ctx, {hop.sink, hop.port}));
        int branch_trials = 0;
        const int branch_limit = std::min(24, std::max(1, timing_budget / 2));
        // Visit distinct nets before their second and third alternatives. All
        // branch trials share the existing total STA and candidate limits.
        for (size_t variant = 0; variant < 3 && remaining() && branch_trials < branch_limit; ++variant)
          for (const auto &choices : branch_choices) {
            if (!remaining() || branch_trials >= branch_limit) break;
            if (variant >= choices.size()) continue;
            const auto &branch = choices[variant];
            ++branch_trials;
            ++attempted;
            ++timed;
            TimingAnalyser after(ctx);
            after.with_clock_skew = false;
            EndpointRows after_rows;
            bool complete = false;
            TimingGuard checks;
            critical_cohort_pin_preview(ctx, [&]() {
                critical_cohort_setup_timing(ctx, after);
                complete = model->apply(ctx, after, &branch);
                after_rows = endpoint_rows(ctx, after);
                checks = timing_guard(ctx, before, after, {}, &before_rows);
            });
            // The exact retention score also admits tied WNS with a 20 ps
            // negative-slack-sum gain. All endpoint, clock and hold guards
            // remain mandatory for this new routing-only proposal family.
            bool eligible = complete && checks.clocks && checks.holds && checks.endpoints &&
                            critical_cohort_proposal_rows_safe(before_rows, after_rows);
            log_info("Critical cohort branch repair: net=%s target=%s avoid=%s complete=%d eligible=%d nodes=%d.\n",
                     branch.net.c_str(ctx), ctx->getWireName(branch.target).str(ctx).c_str(),
                     ctx->getPipName(branch.avoid).str(ctx).c_str(), int(complete), int(eligible), model->cost_nodes());
            if (emit({before_rows, after_rows, old_slack, worst_slack(after), checks.accepts(), eligible,
                      complete, model->cost_nodes(), model->routing_trace()})) {
                if (!eligible) log_error("Cannot emit an ineligible routing-only proposal.\n");
                ++emitted;
            }
        }
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
                            bool cost_complete = false;
                            EndpointRows after_rows;
                            critical_cohort_pin_preview(ctx, [&]() {
                                critical_cohort_setup_timing(ctx, after);
                                cost_complete = model->apply(ctx, after);
                                next_slack = worst_slack(after);
                                auto checks = timing_guard(ctx, before, after,
                                                           proposal.focus.empty() ? guidance.endpoints : proposal.focus,
                                                           &before_rows);
                                log_info("Critical cohort guards: gain=%d clocks=%d holds=%d endpoints=%d.\n",
                                         int(checks.gain), int(checks.clocks), int(checks.holds),
                                         int(checks.endpoints));
                                safe = checks.accepts();
                                if (emit)
                                    after_rows = endpoint_rows(ctx, after);
                            });
                            if (proposal.keep_registers) {
                                const bool unchanged = std::all_of(fixed_registers.begin(), fixed_registers.end(),
                                        [](const SavedBinding &b) {
                                            return b.cell->bel == b.bel && b.cell->belStrength == b.strength;
                                        });
                                log_info("Critical cohort: LUT-only trial fixed registers=%d.\n", int(unchanged));
                                safe &= unchanged;
                            }
                            if (emit) {
                                bool eligible = cost_complete && safe &&
                                                critical_cohort_proposal_rows_safe(before_rows, after_rows);
                                log_info("Critical cohort: routed proposal guards legacy=%d strict=%d costs=%d nodes=%d.\n",
                                         int(safe), int(eligible), int(cost_complete), model->cost_nodes());
                                if (emit({before_rows, after_rows, old_slack, next_slack, safe, eligible,
                                          cost_complete, model->cost_nodes(), model->routing_trace()})) {
                                    if (!eligible)
                                        log_error("Cannot emit an ineligible routed placement proposal.\n");
                                    ++emitted;
                                }
                                return false;
                            }
                            return safe;
                        },
                        stats, dest.z - home.z, proposal.keep_registers);
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
    if (emit)
        log_info("Critical cohort: emitted %d independent placement proposals.\n", emitted);
}

void repair_critical_cohorts(Context *ctx, int timing_budget)
{
    run_critical_cohorts(ctx, timing_budget, ctx->critical_cohort_report, {}, 0);
}

void propose_critical_cohorts(Context *ctx, const std::string &model, int timing_budget, int candidates,
                             const std::function<bool(const CriticalCohortPrediction &)> &emit)
{
    NPNR_ASSERT(timing_budget >= 1 && timing_budget <= 64 && candidates >= 1 && candidates <= 4 && emit);
    run_critical_cohorts(ctx, timing_budget, model, emit, candidates);
}

void propose_critical_cohort_groups(Context *ctx, const std::string &report, int trial_limit,
                                   const std::function<void(const CriticalCohortGroupTrial &)> &inspect)
{
    NPNR_ASSERT(trial_limit >= 1 && trial_limit <= 64 && inspect);
    TimingAnalyser before(ctx);
    before.with_clock_skew = false;
    Guidance guidance;
    critical_cohort_pin_preview(ctx, [&]() {
        critical_cohort_setup_timing(ctx, before);
        CriticalCohortRouteModel model(ctx, report);
        if (!model.active() || !model.apply(ctx, before) || before.have_loops)
            log_error("Group placement requires a complete matching parent model.\n");
        guidance = routed_guidance(ctx, before, report);
    });
    std::sort(guidance.hops.begin(), guidance.hops.end(), [](const Hop &a, const Hop &b) {
        if (a.delay != b.delay) return a.delay > b.delay;
        return std::tie(a.sink->name, a.port, a.driver->name) < std::tie(b.sink->name, b.port, b.driver->name);
    });
    std::map<CellInfo *, BelId> parent;
    for (const auto &cell : ctx->cells) parent[cell.second.get()] = cell.second->bel;
    struct Destination { int dx, dy; int64_t cost; };
    struct Group { std::vector<CellInfo *> seed; std::vector<Destination> destinations; };
    std::vector<Group> groups;
    std::set<std::vector<IdString>> visited;
    const auto band = worst_slack(before) + 150;
    for (size_t index = 0; index < std::min(guidance.hops.size(), size_t(kMaxHops)); ++index) {
        const auto &hop = guidance.hops[index];
        for (auto *anchor : {hop.sink, hop.driver}) {
            auto neighbors = critical_cohort_neighborhood(ctx, anchor);
            if (neighbors.empty()) continue;
            std::set<CellInfo *> cohort(neighbors.begin(), neighbors.end());
            // Carry clusters may span LABs. Include their directly connected
            // registers even when those registers occupy another LAB.
            for (auto *cell : neighbors)
                for (const auto &port : cell->ports) {
                    auto *net = port.second.net;
                    if (!data_net(net)) continue;
                    if (port.second.type == PORT_OUT)
                        for (const auto &user : net->users)
                            if (user.cell && user.cell->type == id_MISTRAL_FF && user.port == id_DATAIN &&
                                allowed(ctx, user.cell)) cohort.insert(user.cell);
                    if (cell->type == id_MISTRAL_FF && port.first == id_DATAIN && allowed(ctx, net->driver.cell))
                        cohort.insert(net->driver.cell);
                }
            if (!close_clusters(ctx, cohort) || std::none_of(cohort.begin(), cohort.end(),
                    [](CellInfo *cell) { return cell->type == id_MISTRAL_FF; })) continue;
            std::vector<CellInfo *> seed(cohort.begin(), cohort.end());
            std::sort(seed.begin(), seed.end(), [](CellInfo *a, CellInfo *b) { return a->name < b->name; });
            std::vector<IdString> key;
            for (auto *cell : seed) key.push_back(cell->name);
            if (!visited.insert(key).second) continue;
            Loc home = ctx->getBelLocation(anchor->bel);
            auto *other = anchor == hop.sink ? hop.driver : hop.sink;
            Loc opposite = ctx->getBelLocation(other->bel);
            int64_t sx = 0, sy = 0, weights = 0;
            auto boundary = [&](NetInfo *net, const PortRef &user) {
                if (!data_net(net) || !user.cell || user.cell->bel == BelId() ||
                    cohort.count(net->driver.cell) == cohort.count(user.cell)) return;
                auto from = ctx->getBelLocation(net->driver.cell->bel), to = ctx->getBelLocation(user.cell->bel);
                int weight = before.get_setup_slack(CellPortKey(user)) <= band ? 8 : 1;
                int sign = cohort.count(net->driver.cell) ? 1 : -1;
                sx += int64_t(weight) * sign * (to.x - from.x);
                sy += int64_t(weight) * sign * (to.y - from.y);
                weights += weight;
            };
            std::set<std::pair<NetInfo *, CellPortKey>> seen_arcs;
            for (auto *cell : cohort)
                for (const auto &port : cell->ports) {
                    auto *net = port.second.net;
                    if (port.second.type == PORT_IN) {
                        if (seen_arcs.emplace(net, CellPortKey(cell->name, port.first)).second)
                            boundary(net, {cell, port.first});
                    } else if (data_net(net))
                        for (const auto &user : net->users)
                            if (seen_arcs.emplace(net, CellPortKey(user)).second) boundary(net, user);
                }
            std::vector<std::pair<int, int>> centers{{home.x, home.y}, {opposite.x, opposite.y}};
            if (weights) centers.emplace_back(home.x + int(sx / weights), home.y + int(sy / weights));
            Placement old;
            for (auto *cell : cohort) old.emplace_back(cell, cell->bel);
            const auto original_cost = score(ctx, before, cohort, old, band);
            Group group{seed, {}};
            std::set<std::pair<int, int>> destinations;
            for (auto center : centers)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int mx = center.first + dx - home.x, my = center.second + dy - home.y;
                        if ((!mx && !my) || !destinations.emplace(mx, my).second) continue;
                        Placement future;
                        if (!translated(ctx, cohort, mx, my, future)) continue;
                        auto cost = score(ctx, before, cohort, future, band);
                        if (cost < original_cost) group.destinations.push_back({mx, my, cost});
                    }
            std::sort(group.destinations.begin(), group.destinations.end(), [](const Destination &a, const Destination &b) {
                return std::tie(a.cost, a.dy, a.dx) < std::tie(b.cost, b.dy, b.dx);
            });
            if (!group.destinations.empty()) groups.push_back(std::move(group));
        }
    }
    int attempted = 0, trials = 0, nodes = 0;
    for (size_t d = 0; d < kMaxDestinations && trials < trial_limit && attempted < 128 && nodes < 100000; ++d)
        for (const auto &group : groups) {
            if (trials >= trial_limit || attempted >= 128 || nodes >= 100000) break;
            if (d >= group.destinations.size()) continue;
            const auto &dest = group.destinations[d];
            ++attempted;
            CriticalCohortStats stats;
            critical_cohort_trial(ctx, group.seed, dest.dx, dest.dy, std::min(kSearchNodes, 100000 - nodes), [&]() {
                ++trials;
                std::set<CellInfo *> changed;
                Placement old, now;
                int registers = 0;
                for (const auto &binding : parent)
                    if (binding.first->bel != binding.second) {
                        auto *cell = binding.first;
                        changed.insert(cell); old.emplace_back(cell, binding.second); now.emplace_back(cell, cell->bel);
                        registers += cell->type == id_MISTRAL_FF;
                    }
                CriticalCohortGroupTrial trial{{}, int(changed.size()), registers,
                    score(ctx, before, changed, old, band), score(ctx, before, changed, now, band)};
                for (auto *cell : group.seed) trial.seed.push_back(cell->name);
                inspect(trial);
                return false;
            }, stats);
            nodes += stats.nodes;
        }
    for (const auto &binding : parent)
        NPNR_ASSERT(binding.first->bel == binding.second);
    log_info("Group placement: groups=%zu attempted=%d/128 legal=%d/%d nodes=%d/100000; full routing required.\n",
             groups.size(), attempted, trials, trial_limit, nodes);
}

namespace {
bool ordinary_movable_lut(Context *ctx, CellInfo *cell)
{
    return allowed(ctx, cell) && cell->cluster == ClusterId() &&
        cell->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                      id_MISTRAL_ALUT5, id_MISTRAL_ALUT6, id_MISTRAL_NOT);
}
} // namespace

std::vector<CellInfo *> critical_cohort_displaced_luts(Context *ctx, const json11::Json &origin)
{
    using Alm = std::pair<uint32_t, uint8_t>;
    std::map<Alm, std::set<CellInfo *>> arrivals;
    std::map<CellInfo *, BelId> homes;
    for (const auto &entry : ctx->cells) {
        auto *cell = entry.second.get();
        auto row = origin["placement"][cell->name.str(ctx)];
        if (!row.is_array() || !row[0].is_string() || cell->bel == BelId()) continue;
        BelId old = ctx->getBelByName(IdStringList::parse(ctx, row[0].string_value()));
        if (old == BelId() || old == cell->bel) continue;
        homes[cell] = old;
        if (ctx->is_comb_cell(cell->type) || cell->type == id_MISTRAL_FF) {
            const auto &site = ctx->bel_data(cell->bel).lab_data;
            arrivals[{site.lab, site.alm}].insert(cell);
        }
    }
    std::vector<CellInfo *> result;
    for (const auto &entry : homes) {
        auto *cell = entry.first;
        if (!ordinary_movable_lut(ctx, cell)) continue;
        const auto &site = ctx->bel_data(entry.second).lab_data;
        auto found = arrivals.find({site.lab, site.alm});
        if (found != arrivals.end() && std::any_of(found->second.begin(), found->second.end(),
                [&](CellInfo *other) { return other != cell; })) result.push_back(cell);
    }
    std::sort(result.begin(), result.end(), [](CellInfo *a, CellInfo *b) { return a->name < b->name; });
    return result;
}

bool inspect_critical_cohort_lut_site(Context *ctx, CellInfo *cell, BelId target,
                                     const std::function<void()> &inspect)
{
    if (!ordinary_movable_lut(ctx, cell) || target == BelId() || target == cell->bel ||
        !ctx->checkBelAvail(target)) return false;
    Transaction transaction(ctx, {cell});
    if (!apply_critical_cohort_placement(ctx, {{cell, target}})) return false;
    inspect();
    return true;
}

void propose_critical_cohort_displaced_luts(Context *ctx, const json11::Json &origin,
    const std::string &report, int trial_limit, const std::function<void(const CriticalCohortLutTrial &)> &inspect,
    CriticalCohortLutSearch &stats)
{
    NPNR_ASSERT(trial_limit >= 1 && trial_limit <= 64 && inspect);
    stats = {};
    TimingAnalyser before(ctx);
    before.with_clock_skew = false;
    critical_cohort_pin_preview(ctx, [&]() {
        critical_cohort_setup_timing(ctx, before);
        CriticalCohortRouteModel model(ctx, report);
        if (!model.active() || !model.apply(ctx, before) || before.have_loops ||
            before.get_timing_result().clock_setup_slack.empty())
            log_error("Displaced LUT search requires a complete matching parent model.\n");
    });
    struct Option { BelId bel; int64_t cost; int occupants; };
    struct Group {
        CellInfo *cell;
        BelId home, from;
        int64_t cost;
        std::vector<Option> sites;
        size_t cursor[2] = {0, 0};
        std::set<std::pair<int, int>> legal_tiles;
        int legal_sites = 0;
    };
    std::vector<Group> groups;
    const auto band = worst_slack(before) + 150;
    auto order = [&](BelId a, BelId b) {
        auto x = ctx->getBelLocation(a), y = ctx->getBelLocation(b);
        return std::tie(x.x, x.y, x.z) < std::tie(y.x, y.y, y.z);
    };
    for (auto *cell : critical_cohort_displaced_luts(ctx, origin)) {
        BelId home = ctx->getBelByName(IdStringList::parse(ctx, origin["placement"][cell->name.str(ctx)][0].string_value()));
        Loc old = ctx->getBelLocation(home), now = ctx->getBelLocation(cell->bel);
        Group group{cell, home, cell->bel, score(ctx, before, {cell}, {{cell, cell->bel}}, band), {}};
        int64_t sx = 0, sy = 0, count = 0;
        std::set<std::pair<NetInfo *, CellPortKey>> arcs;
        auto neighbor = [&](NetInfo *net, PortRef user) {
            if (!data_net(net) || !user.cell || !arcs.emplace(net, CellPortKey(user)).second) return;
            auto *other = net->driver.cell == cell ? user.cell : net->driver.cell;
            if (other == cell || other->bel == BelId()) return;
            Loc location = ctx->getBelLocation(other->bel);
            int weight = before.get_setup_slack(CellPortKey(user)) <= band ? 8 : 1;
            sx += int64_t(weight) * location.x; sy += int64_t(weight) * location.y; count += weight;
        };
        for (const auto &port : cell->ports) {
            auto *net = port.second.net;
            if (port.second.type == PORT_IN) neighbor(net, {cell, port.first});
            else if (data_net(net)) for (const auto &user : net->users) neighbor(net, user);
        }
        std::vector<std::pair<int, int>> centers{{old.x, old.y}, {now.x, now.y}};
        if (count) centers.emplace_back(int(sx/count), int(sy/count));
        std::set<BelId> visited;
        auto add_site = [&](BelId bel) {
            if (bel == BelId() || !visited.insert(bel).second || bel == cell->bel || !ctx->checkBelAvail(bel) ||
                !ctx->isValidBelForCellType(cell->type, bel) || !cell->testRegion(bel) ||
                !ctx->fes_placement_allowed(bel, cell)) return;
            const auto &info = ctx->bel_data(bel).lab_data;
            const auto &alm = ctx->labs.at(info.lab).alms.at(info.alm);
            int occupants = 0;
            for (auto site : {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1],
                              alm.ff_bels[2], alm.ff_bels[3]}) {
                auto *occupant = ctx->getBoundBelCell(site);
                occupants += occupant && occupant != cell;
            }
            group.sites.push_back({bel, score(ctx, before, {cell}, {{cell, bel}}, band), occupants});
        };
        for (auto center : centers)
            for (int y = center.second - 2; y <= center.second + 2; ++y)
                for (int x = center.first - 2; x <= center.first + 2; ++x) {
                    if (x < 0 || y < 0 || x >= ctx->getGridDimX() || y >= ctx->getGridDimY()) continue;
                    for (auto bel : ctx->getBelsByTile(x, y)) add_site(bel);
                }
        // The group also vacated known native sites, which need not lie in
        // the displaced cell's immediate neighborhood.
        for (const auto &entry : origin["placement"].object_items())
            if (entry.second[0].is_string() && !entry.second[0].string_value().empty())
                add_site(ctx->getBelByName(IdStringList::parse(ctx, entry.second[0].string_value())));
        std::sort(group.sites.begin(), group.sites.end(), [&](const Option &a, const Option &b) {
            if (a.cost != b.cost) return a.cost < b.cost;
            return a.occupants != b.occupants ? a.occupants < b.occupants : order(a.bel, b.bel);
        });
        if (!group.sites.empty()) groups.push_back(std::move(group));
    }
    stats.cells = int(groups.size());
    for (int phase = 0; phase < 2; ++phase) {
        const int attempt_limit = phase == 0 ? 64 : 128;
        while (stats.legal < trial_limit && stats.attempted < attempt_limit) {
            bool pending = false;
            for (auto &group : groups) {
                if (stats.legal >= trial_limit || stats.attempted >= attempt_limit) break;
                if (phase == 1 && group.legal_sites) continue;
                // Reserve the second lane for equal/higher estimates on LUTs
                // with no legal improving site. Electrical timing is decided
                // only by complete routing, never this geometric estimate.
                auto &cursor = group.cursor[phase];
                while (cursor < group.sites.size()) {
                    const auto &site = group.sites[cursor];
                    Loc loc = ctx->getBelLocation(site.bel);
                    const bool improving = site.cost < group.cost;
                    if (!group.legal_tiles.count({loc.x, loc.y}) && (phase == 0 ? improving : !improving)) break;
                    ++cursor;
                }
                if (cursor == group.sites.size()) continue;
                pending = true; ++stats.attempted;
                const auto &site = group.sites[cursor++];
                // An illegal first slot must not discard an otherwise usable
                // tile. Only successful inspections consume its diversity slot.
                if (inspect_critical_cohort_lut_site(ctx, group.cell, site.bel, [&]() {
                    ++stats.legal;
                    inspect({group.cell->name, group.home, group.from, site.bel, group.cost, site.cost});
                })) {
                    Loc loc = ctx->getBelLocation(site.bel);
                    group.legal_tiles.emplace(loc.x, loc.y);
                    ++group.legal_sites;
                }
            }
            if (!pending) break;
        }
    }
    log_info("Displaced LUT search: cells=%d attempted=%d/128 legal=%d/%d; full routing required.\n",
             stats.cells, stats.attempted, stats.legal, trial_limit);
}

bool apply_critical_cohort_placement(Context *ctx, const std::vector<std::pair<CellInfo *, BelId>> &moves)
{
    // At most two retained 128-cell compound moves in this experiment.
    if (moves.empty()) return true;
    if (moves.size() > 2 * kMaxCells) return false;
    std::set<CellInfo *> cells;
    std::set<BelId> targets;
    std::set<uint32_t> labs;
    for (const auto &move : moves) {
        if (!allowed(ctx, move.first) || move.second == BelId() ||
            !ctx->isValidBelForCellType(move.first->type, move.second) ||
            !move.first->testRegion(move.second) || !ctx->fes_placement_allowed(move.second, move.first) ||
            !cells.insert(move.first).second || !targets.insert(move.second).second)
            return false;
        labs.insert(ctx->bel_data(move.first->bel).lab_data.lab);
        labs.insert(ctx->bel_data(move.second).lab_data.lab);
    }
    for (const auto &move : moves) {
        auto *occupant = ctx->getBoundBelCell(move.second);
        if (occupant && !cells.count(occupant)) return false;
    }
    Transaction transaction(ctx, cells);
    transaction.unbind();
    for (const auto &move : moves) {
        auto saved = std::find_if(transaction.bindings.begin(), transaction.bindings.end(),
                                  [&](const SavedBinding &b) { return b.cell == move.first; });
        NPNR_ASSERT(saved != transaction.bindings.end());
        ctx->bindBel(move.second, move.first, saved->strength);
    }
    if (!legal_labs(ctx, labs)) return false;
    for (auto *cell : cells) {
        if (get_constraints_distance(ctx, cell) != 0) return false;
        if (cell->cluster != ClusterId()) {
            auto *root = ctx->getClusterRootCell(cell->cluster);
            Placement members;
            if (!ctx->getClusterPlacement(cell->cluster, root->bel, members)) return false;
            for (const auto &member : members)
                if (member.first->bel != member.second) return false;
        }
    }
    ctx->check();
    transaction.commit();
    return true;
}

bool critical_cohort_timing_safe(Context *ctx, TimingAnalyser &before, TimingAnalyser &after,
                                 const std::vector<CellPortKey> &focus)
{
    return timing_guard(ctx, before, after, focus).accepts();
}
NEXTPNR_NAMESPACE_END
