#include "heap_hard_blocks.h"
#include "heap_assignment.h"
#include "heap_hard_block_cost.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
bool protected_cell(const Context *ctx, const CellInfo *cell)
{
    if (cell->isPseudo() || cell->cluster != ClusterId() || cell->region || cell->belStrength > STRENGTH_WEAK)
        return true;
    for (const auto &attr : cell->attrs) {
        const auto name = attr.first.str(ctx);
        if (name == "keep" || name == "dont_touch" || name == "BEL" || name == "LOC" ||
            name == "FES_SLOT" || name == "FES_SLOT_REGION" || name == "REGION") return true;
    }
    return false;
}

struct Planner {
    Context *ctx;
    const PlacerHeapCfg &cfg;
    const TimingAnalyser &timing;
    const dict<IdString, HeapHardBlockLocation> &locations;
    std::map<std::pair<CellPortKey, bool>, int> cone_state;
    size_t work = 0;
    std::string reason;

    bool fail(const std::string &message) { reason = message; return false; }
    bool static_input(CellInfo *cell, IdString port) const { return cfg.hardBlockStaticInput(cell, port); }
    bool clocked(CellInfo *cell, IdString port, TimingPortClass expected) const
    {
        int count = 0;
        if (ctx->getPortTimingClass(cell, port, count) != expected || count <= 0) return false;
        // A native registered port can also have combinational arcs (e.g.
        // registered DSP modes). Do not stop at that boundary and hide a cone.
        for (const auto &other : cell->ports) {
            if (!other.second.net || other.second.type != (expected == TMG_REGISTER_INPUT ? PORT_OUT : PORT_IN))
                continue;
            DelayQuad delay;
            if (ctx->getCellDelay(cell, expected == TMG_REGISTER_INPUT ? port : other.first,
                                 expected == TMG_REGISTER_INPUT ? other.first : port, delay)) return false;
        }
        for (int i = 0; i < count; ++i) {
            const auto model = ctx->getPortClockingInfo(cell, port, i);
            auto *net = cell->getPort(model.clock_port);
            if (!net || !net->clkconstr || net->clkconstr->period.minDelay() <= 0) return false;
        }
        return true;
    }
    // The analyser's propagated tags do not reveal a branch that ends at an
    // unmodelled primitive. Check the complete connected native combinational
    // cone in both directions, stopping at real register boundaries/constants.
    bool cone(CellInfo *cell, IdString pin, bool forward, unsigned depth = 0)
    {
        if (depth > 256) return fail("cone depth bound");
        if (++work > 1000000) return fail("cone work bound");
        const auto key = std::make_pair(CellPortKey(cell->name, pin), forward);
        auto found = cone_state.find(key);
        if (found != cone_state.end()) return found->second == 2;
        cone_state[key] = 1;
        auto *net = cell->getPort(pin);
        if (!net) return fail("disconnected cone port");
        int count = 0;
        auto cls = ctx->getPortTimingClass(cell, pin, count);
        if (forward && cls == TMG_REGISTER_INPUT) {
            if (!clocked(cell, pin, TMG_REGISTER_INPUT)) return fail("unknown capture clock");
        } else if (!forward && cls == TMG_REGISTER_OUTPUT) {
            if (!clocked(cell, pin, TMG_REGISTER_OUTPUT)) return fail("unknown launch clock");
        } else if (forward && cell->ports.at(pin).type == PORT_OUT) {
            if (cls != TMG_COMB_OUTPUT && cls != TMG_REGISTER_OUTPUT) return fail("unsupported output branch");
            if (net->users.empty()) return fail("dead output branch");
            for (const auto &user : net->users)
                if (!cone(user.cell, user.port, true, depth + 1)) return false;
        } else if (!forward && cell->ports.at(pin).type == PORT_IN) {
            if (!static_input(cell, pin)) {
                if (!net->driver.cell || !cone(net->driver.cell, net->driver.port, false, depth + 1)) return false;
            }
        } else {
            if (cls != (forward ? TMG_COMB_INPUT : TMG_COMB_OUTPUT)) return fail("unsupported combinational branch");
            bool arc_found = false;
            for (const auto &port : cell->ports) {
                if (!port.second.net || port.second.type != (forward ? PORT_OUT : PORT_IN)) continue;
                DelayQuad delay;
                if (!ctx->getCellDelay(cell, forward ? pin : port.first, forward ? port.first : pin, delay)) continue;
                arc_found = true;
                if (!std::isfinite(double(delay.minDelay())) || !std::isfinite(double(delay.maxDelay())) ||
                    !cone(cell, port.first, forward, depth + 1)) return false;
            }
            if (!arc_found) return fail("no native combinational arc");
        }
        cone_state[key] = 2;
        return true;
    }
    bool weight(CellInfo *cell, IdString pin, int64_t &value)
    {
        std::vector<TimingAnalyser::SetupWindowHint> rows;
        if (!timing.get_setup_window_hints(CellPortKey(cell->name, pin), rows))
            return fail("incomplete setup-window tags at " + cell->name.str(ctx) + "." + pin.str(ctx));
        double minimum_ns = std::numeric_limits<double>::infinity();
        for (const auto &row : rows) {
            // This first policy requires every branch to be timed. Known cuts
            // are represented correctly by the API, but are not silently dropped.
            if (!row.window) return fail("untimed incident domain pair");
            minimum_ns = std::min(minimum_ns, double(ctx->getDelayNS(*row.window)));
        }
        if (!std::isfinite(minimum_ns) || minimum_ns <= 0) return fail("invalid setup window");
        // Fixed point: 1ns / window, with 2^16 units. The lower bound rejects
        // loss of resolution rather than flattening slow clocks into one weight.
        const double scaled = 65536.0 / minimum_ns;
        if (scaled < 1 || scaled > 1048576) return fail("window weight outside numerical bounds");
        value = int64_t(std::llround(scaled));
        return true;
    }
};
} // namespace

bool plan_heap_hard_blocks(Context *ctx, const PlacerHeapCfg &cfg, const TimingAnalyser &timing,
                          const dict<IdString, HeapHardBlockLocation> &locations,
                          std::vector<HeapHardBlockAssignment> &assignment, std::string &reason)
{
    assignment.clear(); reason.clear();
    if (cfg.hardBlockTiming && !cfg.hardBlockWindows) {
        reason = "timing weighting requires window assignment"; return false;
    }
    if (!cfg.hardBlockWindows || cfg.hardBlockBucket == BelBucketId() || !cfg.hardBlockStaticInput || !cfg.hardBlockCellEligible) {
        reason = "architecture has not declared an independent hard-block bucket"; return false;
    }
    for (const auto &net : ctx->nets)
        if (!net.second->wires.empty() || net.second->region) { reason = "existing routing or routing region"; return false; }
    Planner p{ctx, cfg, timing, locations, {}, 0, {}};
    std::vector<CellInfo *> cells;
    pool<IdString> selected;
    for (const auto &entry : ctx->cells) {
        auto *cell = entry.second.get();
        if (ctx->getBelBucketForCellType(cell->type) != cfg.hardBlockBucket) continue;
        auto loc = locations.find(cell->name);
        if (protected_cell(ctx, cell) || !cfg.hardBlockCellEligible(cell) || loc == locations.end() || !loc->second.movable) {
            reason = "protected, clustered, constrained or fixed bucket member"; return false;
        }
        cells.push_back(cell); selected.insert(cell->name);
    }
    if (cells.empty() || cells.size() > 1024) { reason = "bucket size outside bound"; return false; }
    std::sort(cells.begin(), cells.end(), [&](CellInfo *a, CellInfo *b) { return a->name.str(ctx) < b->name.str(ctx); });
    std::vector<BelId> bels;
    for (auto bel : ctx->getBels()) {
        if (ctx->getBelBucketForBel(bel) != cfg.hardBlockBucket) continue;
        auto *occupant = ctx->getBoundBelCell(bel);
        if (occupant && !selected.count(occupant->name)) continue;
        bels.push_back(bel);
    }
    if (bels.size() < cells.size() || bels.size() > 4096 || bels.size() * cells.size() > 1048576) {
        reason = "site count or matrix outside bound"; return false;
    }
    std::sort(bels.begin(), bels.end(), [&](BelId a, BelId b) { return ctx->nameOfBel(a) < std::string(ctx->nameOfBel(b)); });
    std::vector<std::vector<int64_t>> costs;
    size_t inputs = 0, outputs = 0, constants = 0, cost_work = 0;
    for (auto *cell : cells) {
        struct Edge { double x, y, weight; };
        std::vector<Edge> edges;
        auto edge = [&](CellInfo *anchor, CellInfo *sink, IdString pin) {
            if (!anchor || selected.count(anchor->name)) return p.fail("direct movable hard-block interaction");
            auto loc = locations.find(anchor->name);
            if (loc == locations.end() || !std::isfinite(loc->second.x) || !std::isfinite(loc->second.y))
                return p.fail("unknown incident anchor");
            int64_t weight;
            if (!p.weight(sink, pin, weight)) return false;
            // Inspect the exact analyser state already used by this HeAP
            // iteration. Logging neither reruns timing nor changes this cost.
            if (ctx->debug)
                log_info("HeAP hard-block cost edge: cell='%s' anchor='%s' sink='%s' pin='%s' "
                         "window_weight=%lld fanout=%zu criticality=%.17g anchor_xy=(%.17g,%.17g) "
                         "timing_weight=%.17g criticality_exponent=%.17g\n",
                         ctx->nameOf(cell), ctx->nameOf(anchor), ctx->nameOf(sink), pin.c_str(ctx),
                         static_cast<long long>(weight), size_t(sink->getPort(pin)->users.entries()),
                         double(timing.get_criticality(CellPortKey(sink->name, pin))), loc->second.x, loc->second.y,
                         double(cfg.timingWeight), double(cfg.criticalityExponent));
            double effective_weight = double(weight);
            if (cfg.hardBlockTiming &&
                !heap_hard_block_detail::timing_weight(weight, sink->getPort(pin)->users.entries(),
                                                       timing.get_criticality(CellPortKey(sink->name, pin)),
                                                       cfg.timingWeight, cfg.criticalityExponent, effective_weight))
                return p.fail("invalid native timing/fanout weight");
            if (ctx->debug && cfg.hardBlockTiming)
                log_info("HeAP hard-block timing cost: cell='%s' sink='%s' pin='%s' weight=%.17g\n",
                         ctx->nameOf(cell), ctx->nameOf(sink), pin.c_str(ctx), effective_weight);
            edges.push_back({loc->second.x, loc->second.y, effective_weight});
            return true;
        };
        for (const auto &port : cell->ports) {
            auto *net = port.second.net;
            if (!net) continue;
            int count = 0;
            auto cls = ctx->getPortTimingClass(cell, port.first, count);
            if (cls == TMG_CLOCK_INPUT) continue;
            if (port.second.type == PORT_IN) {
                if (p.static_input(cell, port.first)) { ++constants; continue; }
                if (cls != TMG_REGISTER_INPUT || !p.clocked(cell, port.first, TMG_REGISTER_INPUT) ||
                    !p.cone(cell, port.first, false) || !edge(net->driver.cell, cell, port.first)) {
                    reason = p.reason.empty() ? "unsupported active hard-block input" : p.reason; return false;
                }
                ++inputs;
            } else if (port.second.type == PORT_OUT) {
                if (net->users.empty()) continue;
                if (cls != TMG_REGISTER_OUTPUT || !p.clocked(cell, port.first, TMG_REGISTER_OUTPUT)) {
                    reason = "unsupported active hard-block output"; return false;
                }
                for (const auto &user : net->users) {
                    if (!p.cone(user.cell, user.port, true) || !edge(user.cell, user.cell, user.port)) {
                        reason = p.reason; return false;
                    }
                    ++outputs;
                }
            } else { reason = "hard-block inout"; return false; }
        }
        if (edges.empty()) { reason = "no timed hard-block edges"; return false; }
        if (edges.size() > 65536 || edges.size() * bels.size() > 16777216 - cost_work) {
            reason = "incident cost work bound"; return false;
        }
        cost_work += edges.size() * bels.size();
        const auto &raw = locations.at(cell->name);
        if (ctx->debug)
            log_info("HeAP hard-block cost target: cell='%s' raw=(%.17g,%.17g) scale=(%d,%d)\n",
                     ctx->nameOf(cell), raw.x, raw.y, cfg.hpwl_scale_x, cfg.hpwl_scale_y);
        std::vector<int64_t> row;
        for (auto bel : bels) {
            if (!cell->testRegion(bel) || !ctx->isValidBelForCellType(cell->type, bel)) { row.push_back(-1); continue; }
            const auto loc = ctx->getBelLocation(bel);
            // One unit of raw-target displacement is a deterministic weak
            // regularizer; every actual timed edge carries its native weight.
            double cost = std::abs(raw.x - loc.x) * cfg.hpwl_scale_x + std::abs(raw.y - loc.y) * cfg.hpwl_scale_y;
            for (const auto &e : edges)
                cost += double(e.weight) * (std::abs(e.x - loc.x) * cfg.hpwl_scale_x +
                                           std::abs(e.y - loc.y) * cfg.hpwl_scale_y);
            if (!std::isfinite(cost) || cost < 0 || cost >= double(int64_t(1) << 40)) {
                reason = "assignment cost outside numerical bound"; return false;
            }
            row.push_back(int64_t(std::llround(cost)));
        }
        costs.push_back(std::move(row));
    }
    std::vector<int> result;
    if (!heap_assignment_detail::solve(costs, result)) { reason = "no complete bounded matching"; return false; }
    for (size_t i = 0; i < cells.size(); ++i) assignment.push_back({cells[i], bels.at(result[i])});
    log_info("HeAP hard-block windows: assigned %zu cells to %zu sites; %zu input edges, %zu output branches, "
             "%zu static inputs.\n", cells.size(), bels.size(), inputs, outputs, constants);
    return true;
}

bool bind_heap_hard_blocks(Context *ctx, const PlacerHeapCfg &cfg,
                           const std::vector<HeapHardBlockAssignment> &assignment, std::string &reason)
{
    reason.clear();
    if (assignment.empty() || !cfg.hardBlockWindows || !cfg.hardBlockCellEligible) {
        reason = "undeclared or empty assignment"; return false;
    }
    pool<BelId> bels;
    pool<IdString> cells;
    for (const auto &entry : assignment) {
        if (!entry.cell || entry.bel == BelId() || entry.cell->bel != BelId() || entry.cell->belStrength != STRENGTH_NONE ||
            protected_cell(ctx, entry.cell) || !cfg.hardBlockCellEligible(entry.cell) ||
            ctx->getBelBucketForCellType(entry.cell->type) != cfg.hardBlockBucket || !bels.insert(entry.bel).second || !cells.insert(entry.cell->name).second ||
            !ctx->checkBelAvail(entry.bel) || !entry.cell->testRegion(entry.bel) ||
            !ctx->isValidBelForCellType(entry.cell->type, entry.bel)) {
            reason = "native assignment precondition"; return false;
        }
    }
    struct Rollback {
        Context *ctx;
        std::vector<BelId> bound;
        bool committed = false;
        ~Rollback() { if (!committed) for (auto it = bound.rbegin(); it != bound.rend(); ++it) ctx->unbindBel(*it); }
    } transaction{ctx, {}, false};
    transaction.bound.reserve(assignment.size());
    for (const auto &entry : assignment) {
        ctx->bindBel(entry.bel, entry.cell, STRENGTH_WEAK);
        transaction.bound.push_back(entry.bel);
    }
    for (const auto &entry : assignment)
        if (!ctx->isBelLocationValid(entry.bel)) { reason = "native assignment legality"; return false; }
    transaction.committed = true;
    return true;
}
NEXTPNR_NAMESPACE_END
