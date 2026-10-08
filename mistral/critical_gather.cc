/* Post-place gather for long failing setup hops. SPDX-License-Identifier: ISC */
#include "critical_gather.h"

#include "log.h"
#include "nextpnr.h"
#include "timing.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

NEXTPNR_NAMESPACE_BEGIN

namespace {

constexpr long kMinSlackGainPs = 20;
// predictDelay charges about 100 ps per row. A legal site is often only one
// row closer than the current BEL, so the predictor has to accept that step.
constexpr delay_t kMinArcGainPs = 80;
constexpr delay_t kLongArcPs = 700;
constexpr delay_t kSlackBandPs = 150;
constexpr int kMaxCommits = 32;
constexpr int kCellsPerRound = 32;
// One LUT moving toward its driver lengthens that LUT's other critical
// loads. Slide a few ends of the same cone, then time them together.
constexpr int kBatch = 4;
constexpr int kWindowRadius = 2;
// A 5x5 LAB window holds far more BELs than we can legally try. Keep the
// long jump, the halfway tile, and a short slide as separate budgets so the
// sites next to the driver cannot crowd out a step from the current BEL.
constexpr int kAnchorTrials = 32;
constexpr int kStepTrials = 40;
struct Measure
{
    delay_t worst = std::numeric_limits<delay_t>::max();
    std::vector<std::pair<IdString, delay_t>> clocks;
};

struct Watch
{
    NetInfo *net = nullptr;
    CellInfo *sink = nullptr;
    IdString port;
    bool band = false;
};

struct Binding
{
    CellInfo *cell = nullptr;
    BelId bel;
    PlaceStrength strength = STRENGTH_NONE;
};

struct AppliedMove
{
    bool ok = false;
    Binding mover;
    Binding guest;
};

bool critical_gather_disabled()
{
    const char *env = std::getenv("NEXTPNR_MISTRAL_CRITICAL_GATHER");
    return env != nullptr && env[0] == '0' && env[1] == '\0';
}

// Mistral clock buffers do not implement getBelGlobalBuf, so a global or
// constrained clock net has to be rejected by the net itself.
bool data_arc(const NetInfo *net)
{
    return net != nullptr && net->driver.cell != nullptr && net->driver.cell->bel != BelId() && !net->is_global &&
           !net->clkconstr;
}

// Null when the cell may slide. Carry stays put: pack clusters a chain, and a
// loose arithmetic LUT has no general route for CI/CO.
const char *stuck_reason(Context *ctx, const CellInfo *cell)
{
    if (cell == nullptr || cell->isPseudo() || cell->bel == BelId())
        return "unplaced";
    if (cell->belStrength > STRENGTH_WEAK)
        return "fixed";
    if (cell->cluster != ClusterId())
        return "cluster";
    if (cell->region != nullptr)
        return "region";
    if (cell->type == id_MISTRAL_ALUT_ARITH)
        return "carry";
    if (!(ctx->is_comb_cell(cell->type) || cell->type == id_MISTRAL_FF))
        return "type";
    for (const char *name : {"keep", "dont_touch", "BEL", "FES_SLOT"}) {
        if (cell->attrs.count(ctx->id(name)))
            return name;
    }
    return nullptr;
}

bool movable(Context *ctx, const CellInfo *cell) { return stuck_reason(ctx, cell) == nullptr; }

delay_t port_slack(TimingAnalyser &tmg, const CellInfo *cell, IdString port)
{
    float slack = tmg.get_setup_slack(CellPortKey(cell->name, port));
    if (!(slack < 1.0e8f))
        return std::numeric_limits<delay_t>::max();
    return delay_t(slack);
}

Measure measure(TimingAnalyser &tmg)
{
    tmg.setup_only = false;
    tmg.with_clock_skew = true;
    tmg.setup(false, false, true);
    Measure result;
    if (tmg.have_loops)
        return result;
    for (const auto &clock : tmg.get_timing_result().clock_setup_slack) {
        result.clocks.push_back(clock);
        result.worst = std::min(result.worst, clock.second);
    }
    std::sort(result.clocks.begin(), result.clocks.end(),
              [](const std::pair<IdString, delay_t> &a, const std::pair<IdString, delay_t> &b) {
                  return a.first < b.first;
              });
    return result;
}

void align_clocks(const Measure &before, const Measure &after, std::vector<GatherClockSlack> &before_rows,
                  std::vector<GatherClockSlack> &after_rows)
{
    std::vector<IdString> keys;
    for (const auto &clock : before.clocks)
        keys.push_back(clock.first);
    for (const auto &clock : after.clocks)
        keys.push_back(clock.first);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    auto find = [](const Measure &sample, IdString key) -> GatherClockSlack {
        for (const auto &clock : sample.clocks)
            if (clock.first == key)
                return {true, long(clock.second)};
        return {};
    };
    before_rows.clear();
    after_rows.clear();
    for (IdString key : keys) {
        before_rows.push_back(find(before, key));
        after_rows.push_back(find(after, key));
    }
}

Binding capture(CellInfo *cell)
{
    Binding saved;
    saved.cell = cell;
    saved.bel = cell->bel;
    saved.strength = cell->belStrength;
    return saved;
}

// Both cells are currently bound. Put them back on the BELs they occupied
// when `capture` ran. `guest.cell` is null when the landing site was empty.
void restore(Context *ctx, const Binding &mover, const Binding &guest)
{
    if (mover.cell->bel != BelId())
        ctx->unbindBel(mover.cell->bel);
    if (guest.cell != nullptr && guest.cell->bel != BelId())
        ctx->unbindBel(guest.cell->bel);
    ctx->bindBel(mover.bel, mover.cell, mover.strength);
    if (guest.cell != nullptr)
        ctx->bindBel(guest.bel, guest.cell, guest.strength);
}

void collect_watches(Context *ctx, TimingAnalyser &tmg, CellInfo *cell, delay_t band, std::vector<Watch> &watches)
{
    for (auto &port : cell->ports) {
        NetInfo *net = port.second.net;
        if (!data_arc(net))
            continue;
        if (port.second.type == PORT_IN) {
            watches.push_back({net, cell, port.first, port_slack(tmg, cell, port.first) <= band});
            continue;
        }
        for (auto user : net->users) {
            if (user.cell == nullptr || user.cell->bel == BelId())
                continue;
            watches.push_back({net, user.cell, user.port, port_slack(tmg, user.cell, user.port) <= band});
        }
    }
}

struct Cost
{
    delay_t band = 0;
    delay_t max_other = 0;
};

Cost cost_of(Context *ctx, const std::vector<Watch> &watches)
{
    Cost cost;
    for (const auto &watch : watches) {
        if (watch.sink->bel == BelId() || watch.net->driver.cell == nullptr || watch.net->driver.cell->bel == BelId())
            continue;
        delay_t delay = ctx->predictArcDelay(watch.net, PortRef{watch.sink, watch.port});
        if (watch.band)
            cost.band += delay;
        else
            cost.max_other = std::max(cost.max_other, delay);
    }
    return cost;
}

struct TrialBel
{
    BelId bel;
    int dist = 0;
    bool occupied = false;
    int x = 0, y = 0, z = 0;
};

bool better_trial(const TrialBel &a, const TrialBel &b)
{
    if (a.occupied != b.occupied)
        return !a.occupied;
    if (a.dist != b.dist)
        return a.dist < b.dist;
    if (a.x != b.x)
        return a.x < b.x;
    if (a.y != b.y)
        return a.y < b.y;
    return a.z < b.z;
}

// `limit` is applied after the empty-then-closer sort, and a BEL already
// chosen by an earlier window is left there. `empty_only` skips swaps:
// trading places with a cell at the far end of a long hop dumps that cell
// onto the mover's old site.
void collect_tiles(Context *ctx, CellInfo *cell, const std::vector<std::pair<int, int>> &tiles, Loc anchor, int limit,
                   bool empty_only, std::vector<TrialBel> &trials)
{
    std::vector<TrialBel> window;
    for (const auto &tile : tiles) {
        int x = tile.first, y = tile.second;
        if (x < 0 || y < 0 || x >= ctx->getGridDimX() || y >= ctx->getGridDimY())
            continue;
        for (BelId bel : ctx->getBelsByTile(x, y)) {
            if (!ctx->isValidBelForCellType(cell->type, bel) || bel == cell->bel)
                continue;
            if (cell->region != nullptr && !cell->testRegion(bel))
                continue;
            CellInfo *guest = ctx->getBoundBelCell(bel);
            if (guest != nullptr && (empty_only || !movable(ctx, guest)))
                continue;
            if (guest != nullptr && !ctx->isValidBelForCellType(guest->type, cell->bel))
                continue;
            if (guest != nullptr && guest->region != nullptr && !guest->testRegion(cell->bel))
                continue;
            bool seen = false;
            for (const auto &already : trials)
                if (already.bel == bel)
                    seen = true;
            if (seen)
                continue;
            Loc loc = ctx->getBelLocation(bel);
            window.push_back({bel, std::abs(loc.x - anchor.x) + std::abs(loc.y - anchor.y), guest != nullptr, loc.x,
                              loc.y, loc.z});
        }
    }
    std::sort(window.begin(), window.end(), better_trial);
    if (int(window.size()) > limit)
        window.resize(limit);
    trials.insert(trials.end(), window.begin(), window.end());
}

void collect_window(Context *ctx, CellInfo *cell, int origin_x, int origin_y, int radius, Loc anchor, int limit,
                    std::vector<TrialBel> &trials)
{
    std::vector<std::pair<int, int>> tiles;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
            tiles.push_back({origin_x + dx, origin_y + dy});
    collect_tiles(ctx, cell, tiles, anchor, limit, false, trials);
}

// On success the mover sits on the chosen BEL and any swapped cell sits on the
// mover's old BEL. `result.mover` / `result.guest` still describe the old bindings.
AppliedMove attempt_cell(Context *ctx, TimingAnalyser &tmg, CellInfo *cell, Loc anchor, delay_t band, delay_t min_arc)
{
    AppliedMove result;
    std::vector<Watch> own;
    collect_watches(ctx, tmg, cell, band, own);
    Cost old_own = cost_of(ctx, own);
    if (old_own.band < min_arc)
        return result;

    Loc here = ctx->getBelLocation(cell->bel);
    std::vector<TrialBel> trials;
    collect_window(ctx, cell, anchor.x, anchor.y, kWindowRadius, anchor, kAnchorTrials, trials);
    collect_window(ctx, cell, (anchor.x + here.x) / 2, (anchor.y + here.y) / 2, 2, anchor, kStepTrials, trials);
    collect_window(ctx, cell, here.x, here.y, 2, anchor, kStepTrials, trials);

    Binding home = capture(cell);
    BelId best = BelId();
    Binding best_guest;
    Cost best_cost;
    best_cost.band = old_own.band;
    bool found = false;
    for (const auto &trial : trials) {
        CellInfo *guest = ctx->getBoundBelCell(trial.bel);
        Binding guest_home = guest != nullptr ? capture(guest) : Binding{};
        std::vector<Watch> guest_watches;
        if (guest != nullptr)
            collect_watches(ctx, tmg, guest, band, guest_watches);
        Cost old_guest = cost_of(ctx, guest_watches);

        ctx->unbindBel(home.bel);
        if (guest != nullptr)
            ctx->unbindBel(guest_home.bel);
        ctx->bindBel(trial.bel, cell, STRENGTH_WEAK);
        if (guest != nullptr)
            ctx->bindBel(home.bel, guest, STRENGTH_WEAK);
        bool legal = ctx->isBelLocationValid(trial.bel) && ctx->isBelLocationValid(home.bel);
        Cost next_own, next_guest;
        if (legal) {
            next_own = cost_of(ctx, own);
            next_guest = cost_of(ctx, guest_watches);
        }
        // Side loads may grow. The batch is accepted only when static timing
        // says worst setup slack improved and passing clocks held.
        bool better = false;
        if (legal) {
            delay_t band_gain = (old_own.band - next_own.band) + (old_guest.band - next_guest.band);
            better = band_gain >= kMinArcGainPs && (!found || next_own.band < best_cost.band);
        }
        if (better) {
            found = true;
            best = trial.bel;
            best_guest = guest_home;
            best_cost = next_own;
        }
        restore(ctx, home, guest_home);
    }
    if (!found)
        return result;

    ctx->unbindBel(home.bel);
    if (best_guest.cell != nullptr)
        ctx->unbindBel(best_guest.bel);
    ctx->bindBel(best, cell, STRENGTH_WEAK);
    if (best_guest.cell != nullptr)
        ctx->bindBel(home.bel, best_guest.cell, STRENGTH_WEAK);
    if (!ctx->isBelLocationValid(best) || !ctx->isBelLocationValid(home.bel)) {
        restore(ctx, home, best_guest);
        return result;
    }
    result.ok = true;
    result.mover = home;
    result.guest = best_guest;
    return result;
}

struct Hop
{
    CellInfo *cell = nullptr;
    Loc anchor;
    delay_t arc = 0;
};

void log_worst_path(TimingAnalyser &tmg)
{
    TimingResult &result = tmg.get_timing_result();
    const CriticalPath *worst = nullptr;
    delay_t worst_slack = std::numeric_limits<delay_t>::max();
    auto consider = [&](const CriticalPath &path) {
        delay_t sum = 0;
        for (const auto &segment : path.segments)
            sum += segment.delay;
        delay_t path_slack = path.max_delay - sum;
        if (worst == nullptr || path_slack < worst_slack) {
            worst = &path;
            worst_slack = path_slack;
        }
    };
    for (auto &entry : result.clock_paths)
        consider(entry.second);
    for (auto &path : result.xclock_paths)
        consider(path);
    if (worst == nullptr)
        return;
    delay_t logic = 0, route = 0, longest = 0;
    int hops = 0;
    for (const auto &segment : worst->segments) {
        using Type = CriticalPath::Segment::Type;
        if (segment.type == Type::ROUTING) {
            route += segment.delay;
            longest = std::max(longest, segment.delay);
            ++hops;
        } else if (segment.type == Type::LOGIC || segment.type == Type::CLK_TO_Q || segment.type == Type::SETUP ||
                   segment.type == Type::SOURCE) {
            logic += segment.delay;
        }
    }
    log_info("Critical-path gather: worst path %ld ps logic, %ld ps routing over %d hops, longest hop %ld ps.\n",
             long(logic), long(route), hops, long(longest));
}

void hold_move(const AppliedMove &move)
{
    move.mover.cell->belStrength = STRENGTH_STRONG;
    if (move.guest.cell != nullptr)
        move.guest.cell->belStrength = STRENGTH_STRONG;
}

void thaw_moves(const std::vector<AppliedMove> &moves)
{
    for (const auto &move : moves) {
        move.mover.cell->belStrength = move.mover.strength;
        if (move.guest.cell != nullptr)
            move.guest.cell->belStrength = move.guest.strength;
    }
}

void undo_moves(Context *ctx, const std::vector<AppliedMove> &moves)
{
    for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
        restore(ctx, it->mover, it->guest);
        NPNR_ASSERT(it->mover.cell->bel == it->mover.bel);
        if (it->guest.cell != nullptr)
            NPNR_ASSERT(it->guest.cell->bel == it->guest.bel);
    }
}


} // namespace

bool critical_gather_accepts(long before_worst_ps, const std::vector<GatherClockSlack> &before, long after_worst_ps,
                             const std::vector<GatherClockSlack> &after, long min_gain_ps)
{
    if (before.size() != after.size() || min_gain_ps < 0)
        return false;
    if (after_worst_ps < before_worst_ps + min_gain_ps)
        return false;
    for (size_t i = 0; i < before.size(); ++i) {
        if (!before[i].known)
            continue;
        if (!after[i].known)
            return false;
        if (before[i].slack_ps >= 0 && after[i].slack_ps < 0)
            return false;
    }
    return true;
}

void gather_critical_paths(Context *ctx)
{
    if (critical_gather_disabled() || !ctx->setting<bool>("timing_driven"))
        return;

    TimingAnalyser initial(ctx);
    Measure before = measure(initial);
    if (initial.have_loops || before.clocks.empty() || before.worst >= 0)
        return;

    const auto started = std::chrono::steady_clock::now();
    const auto budget = std::chrono::seconds(60);
    int commits = 0;
    delay_t slack = before.worst;
    log_info("Critical-path gather: worst setup slack %ld ps. Sliding long hops the 3-tile refine could not close.\n",
             long(slack));
    log_worst_path(initial);

    auto commit_some = [&](TimingAnalyser &tmg, const Measure &now, const std::vector<Hop> &unique,
                           delay_t band) -> bool {
        auto timed_ok = [&](const Measure &after, bool loops) {
            std::vector<GatherClockSlack> before_rows, after_rows;
            align_clocks(now, after, before_rows, after_rows);
            return !loops && critical_gather_accepts(long(now.worst), before_rows, long(after.worst), after_rows,
                                                     kMinSlackGainPs);
        };
        auto describe = [&](const std::vector<AppliedMove> &moves, const std::vector<Hop> &hops_kept, delay_t after_ps) {
            for (size_t i = 0; i < moves.size(); ++i) {
                Loc src = ctx->getBelLocation(moves[i].mover.bel);
                Loc dest = ctx->getBelLocation(hops_kept[i].cell->bel);
                log_info("Critical-path gather: slack %ld -> %ld ps by moving %s from (%d,%d) to (%d,%d).\n",
                         long(now.worst), long(after_ps), hops_kept[i].cell->name.c_str(ctx), src.x, src.y, dest.x,
                         dest.y);
            }
        };

        // A rejected batch used to end the pass. Later hops on the same
        // critical set can still be legal, so keep scanning until one
        // group or one solo slide improves slack.
        bool kept = false;
        size_t hop_i = 0;
        while (!kept && hop_i < unique.size() && std::chrono::steady_clock::now() - started < budget) {
            std::vector<AppliedMove> pending;
            std::vector<Hop> pending_hops;
            while (hop_i < unique.size() && int(pending.size()) < kBatch &&
                   std::chrono::steady_clock::now() - started < budget) {
                const Hop &hop = unique[hop_i++];
                if (!movable(ctx, hop.cell))
                    continue;
                AppliedMove move = attempt_cell(ctx, tmg, hop.cell, hop.anchor, band, hop.arc);
                if (!move.ok)
                    continue;
                hold_move(move);
                pending.push_back(std::move(move));
                pending_hops.push_back(hop);
            }
            if (pending.empty())
                break;

            TimingAnalyser check(ctx);
            Measure after = measure(check);
            if (timed_ok(after, check.have_loops)) {
                thaw_moves(pending);
                describe(pending, pending_hops, after.worst);
                slack = after.worst;
                commits += int(pending.size());
                kept = true;
            } else if (pending.size() == 1) {
                undo_moves(ctx, pending);
            } else {
                std::vector<Hop> alone = pending_hops;
                undo_moves(ctx, pending);
                for (const auto &hop : alone) {
                    if (std::chrono::steady_clock::now() - started >= budget)
                        break;
                    AppliedMove again = attempt_cell(ctx, tmg, hop.cell, hop.anchor, band, hop.arc);
                    if (!again.ok)
                        continue;
                    TimingAnalyser one_check(ctx);
                    Measure one_after = measure(one_check);
                    if (timed_ok(one_after, one_check.have_loops)) {
                        thaw_moves({again});
                        describe({again}, {hop}, one_after.worst);
                        slack = one_after.worst;
                        ++commits;
                        kept = true;
                        break;
                    }
                    undo_moves(ctx, {again});
                }
            }
        }
        return kept;
    };

    while (commits < kMaxCommits && slack < 0 && std::chrono::steady_clock::now() - started < budget) {
        TimingAnalyser tmg(ctx);
        Measure now = measure(tmg);
        if (tmg.have_loops || now.worst >= 0)
            break;
        delay_t band = now.worst + kSlackBandPs;
        std::vector<Hop> hops;
        for (auto &entry : ctx->cells) {
            CellInfo *sink = entry.second.get();
            if (sink->isPseudo())
                continue;
            for (auto &port : sink->ports) {
                if (port.second.type != PORT_IN || port.second.net == nullptr)
                    continue;
                NetInfo *net = port.second.net;
                if (!data_arc(net) || net->driver.cell == sink)
                    continue;
                CellInfo *driver = net->driver.cell;
                delay_t slack_ps = port_slack(tmg, sink, port.first);
                if (slack_ps > band)
                    continue;
                delay_t arc = ctx->predictArcDelay(net, PortRef{sink, port.first});
                if (arc < kLongArcPs)
                    continue;
                if (movable(ctx, sink))
                    hops.push_back({sink, ctx->getBelLocation(driver->bel), arc});
                else if (movable(ctx, driver))
                    hops.push_back({driver, ctx->getBelLocation(sink->bel), arc});
            }
        }
        std::sort(hops.begin(), hops.end(), [](const Hop &a, const Hop &b) {
            if (a.arc != b.arc)
                return a.arc > b.arc;
            return a.cell->name < b.cell->name;
        });
        std::vector<Hop> unique;
        for (const auto &hop : hops) {
            bool seen = false;
            for (const auto &kept : unique)
                if (kept.cell == hop.cell)
                    seen = true;
            if (!seen)
                unique.push_back(hop);
            if (int(unique.size()) >= kCellsPerRound)
                break;
        }
        if (unique.empty() || !commit_some(tmg, now, unique, band))
            break;
    }

    if (commits == 0) {
        log_info("Critical-path gather: no legal move improved worst setup slack (%ld ps).\n", long(before.worst));
        return;
    }
    TimingAnalyser kept_tmg(ctx);
    measure(kept_tmg);
    log_worst_path(kept_tmg);
    log_info("Critical-path gather: kept %d move%s, worst setup slack %ld -> %ld ps.\n", commits,
             commits == 1 ? "" : "s", long(before.worst), long(slack));
    timing_analysis(ctx, false, true, false, false);
}

NEXTPNR_NAMESPACE_END
