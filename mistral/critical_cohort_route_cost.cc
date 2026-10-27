/* Bounded native routing-cost estimates for placement proposals. SPDX-License-Identifier: ISC */
#include "critical_cohort_route_cost.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include "log.h"
#include "routed_placement.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
using J = json11::Json;
void require(bool ok, const char *message)
{
    if (!ok) log_error("Invalid cohort routing-cost source: %s.\n", message);
}
struct CalibrationScope
{
    Context *ctx;
    bool calibrated;
    float prior;
    dict<PipId, delay_t> observed;
    std::array<Arch::TypeCalibration, 256> types;
    CalibrationScope(Context *ctx, bool enabled, float factor, const dict<PipId, delay_t> &values,
                     const std::array<Arch::TypeCalibration, 256> &calibration)
            : ctx(ctx), calibrated(ctx->pip_delay_calibrated), prior(ctx->pip_delay_prior),
              observed(values), types(ctx->pip_type_calibration)
    {
        // Allocate the replacement before modifying the Context. The native
        // calibration calculation is reused without retaining its mutable state.
        std::swap(observed, ctx->pip_delay_observed);
        ctx->pip_delay_calibrated = enabled;
        ctx->pip_delay_prior = factor;
        ctx->pip_type_calibration = calibration;
    }
    ~CalibrationScope()
    {
        std::swap(observed, ctx->pip_delay_observed);
        ctx->pip_delay_calibrated = calibrated;
        ctx->pip_delay_prior = prior;
        ctx->pip_type_calibration = types;
    }
};
}

json11::Json CriticalCohortRouteCosts::snapshot(Context *ctx, const json11::Json &parent)
{
    J::object measured;
    J::object background;
    J::object reservations;
    for (const auto &entry : ctx->wires) {
        const auto &wire = entry.second;
        if ((wire.flags & WireInfo::RESERVED_ROUTE) != 0)
            reservations[ctx->getWireName(entry.first).str(ctx)] =
                ctx->getWireName(wire.wires_uphill.at(wire.flags & 0xFF)).str(ctx);
    }
    for (const auto &entry : ctx->nets) {
        J::array pips;
        for (const auto &binding : entry.second->wires)
            if (binding.second.pip != PipId())
                pips.push_back(ctx->getPipName(binding.second.pip).str(ctx));
        std::sort(pips.begin(), pips.end(), [](const J &a, const J &b) { return a.string_value() < b.string_value(); });
        if (!pips.empty()) background[entry.first.str(ctx)] = pips;
    }
    for (const auto &entry : ctx->pip_delay_observed)
        measured[ctx->getPipName(entry.first).str(ctx)] = double(entry.second);
    J::array calibration;
    for (const auto &entry : ctx->pip_type_calibration)
        calibration.push_back(J::array{entry.table_ps, entry.analogue_ps, double(entry.hops)});
    return J::object{{"version", 3}, {"routes", snapshot_routed_placement_routes(ctx, parent)},
        {"background", background}, {"reservations", reservations},
        {"calibrated", ctx->pip_delay_calibrated}, {"prior", double(ctx->pip_delay_prior)},
        {"observed", measured}, {"types", calibration}};
}

CriticalCohortRouteCosts::CriticalCohortRouteCosts(Context *ctx, const json11::Json &source, const json11::Json &parent)
{
    require(source["version"] == J(3) && source["background"].is_object() && source["reservations"].is_object() &&
            source["calibrated"].is_bool() && source["prior"].is_number() &&
            source["observed"].is_object() && source["types"].is_array() &&
            source["types"].array_items().size() == types.size(), "missing calibration or version");
    occupied = routed_placement_route_reservations(ctx, source["routes"], parent);
    for (const auto &entry : source["reservations"].object_items()) {
        require(entry.second.is_string(), "invalid reserved route source");
        auto target = ctx->getWireByName(IdStringList::parse(ctx, entry.first));
        auto driver = ctx->getWireByName(IdStringList::parse(ctx, entry.second.string_value()));
        require(ctx->wires.count(target) && ctx->wires.count(driver), "unknown reserved route wire");
        const auto &uphill = ctx->wires.at(target).wires_uphill;
        require(std::find(uphill.begin(), uphill.end(), driver) != uphill.end(), "reserved route is not a native edge");
        reservations.emplace(target, driver);
    }
    for (const auto &entry : source["background"].object_items()) {
        require(entry.second.is_array(), "missing background pips");
        auto &pips = background[ctx->id(entry.first)];
        for (const auto &name : entry.second.array_items()) {
            require(name.is_string(), "invalid background pip");
            auto pip = ctx->getPipByName(IdStringList::parse(ctx, name.string_value()));
            bool exists = false;
            require(ctx->wires.count(ctx->getPipSrcWire(pip)) && ctx->wires.count(ctx->getPipDstWire(pip)),
                    "unknown background wire");
            for (auto edge : ctx->getPipsDownhill(ctx->getPipSrcWire(pip))) exists |= edge == pip;
            require(exists, "unknown background pip");
            for (auto wire : {ctx->getPipSrcWire(pip), ctx->getPipDstWire(pip)}) {
                auto owner = occupied.emplace(wire, ctx->id(entry.first));
                require(owner.second || owner.first->second == ctx->id(entry.first), "background wire has two owners");
            }
            pips.push_back(pip);
        }
    }
    calibrated = source["calibrated"].bool_value();
    double factor = source["prior"].number_value();
    require(std::isfinite(factor) && factor > 0 && factor <= std::numeric_limits<float>::max(), "invalid prior");
    prior = float(factor);
    for (const auto &entry : source["observed"].object_items()) {
        double value = entry.second.number_value();
        require(entry.second.is_number() && std::isfinite(value) && value >= 0 && value <= 10000000 &&
                std::floor(value) == value, "invalid observed pip cost");
        PipId pip = ctx->getPipByName(IdStringList::parse(ctx, entry.first));
        require(pip != PipId() && ctx->wires.count(ctx->getPipSrcWire(pip)) &&
                ctx->wires.count(ctx->getPipDstWire(pip)), "unknown observed pip");
        bool exists = false;
        for (auto edge : ctx->getPipsDownhill(ctx->getPipSrcWire(pip))) exists |= edge == pip;
        require(exists, "observed pip is not an architecture edge");
        observed[pip] = delay_t(value);
    }
    for (size_t i = 0; i < types.size(); ++i) {
        const auto &row = source["types"][i].array_items();
        require(row.size() == 3, "invalid type calibration");
        for (const auto &value : row)
            require(value.is_number() && std::isfinite(value.number_value()) && value.number_value() >= 0 &&
                    value.number_value() <= 1e15, "invalid type calibration value");
        require(std::floor(row[2].number_value()) == row[2].number_value() &&
                (row[0].number_value() == 0 || row[2].number_value() > 0), "invalid calibration hop count");
        types[i] = {row[0].number_value(), row[1].number_value(), int64_t(row[2].number_value())};
    }
    CalibrationScope scope(ctx, calibrated, prior, observed, types);
    for (const auto &entry : source["routes"]["nets"].object_items()) {
        IdString name = ctx->id(entry.first);
        std::map<WireId, PipId> parents;
        for (const auto &binding : entry.second["wires"].array_items()) {
            WireId wire = ctx->getWireByName(IdStringList::parse(ctx, binding[0].string_value()));
            PipId pip = binding[1].string_value().empty() ? PipId() :
                    ctx->getPipByName(IdStringList::parse(ctx, binding[1].string_value()));
            parents.emplace(wire, pip);
            if (pip == PipId()) sources[name] = wire;
        }
        auto &values = references[name];
        trees[name] = parents;
        auto saved = background.find(name);
        std::set<PipId> native;
        if (saved != background.end()) native.insert(saved->second.begin(), saved->second.end());
        for (const auto &wire : parents)
            if (wire.second != PipId())
                require(native.count(wire.second), "background omits a parent tree edge");
        values[sources.at(name)] = 0;
        for (const auto &binding : parents) {
            std::vector<WireId> path;
            WireId cursor = binding.first;
            while (!values.count(cursor)) {
                path.push_back(cursor);
                cursor = ctx->getPipSrcWire(parents.at(cursor));
            }
            while (!path.empty()) {
                WireId next = path.back(); path.pop_back();
                int64_t value = int64_t(values.at(cursor)) + cost(ctx, parents.at(next));
                require(value < std::numeric_limits<delay_t>::max(), "reference path cost overflow");
                values[next] = delay_t(value);
                cursor = next;
            }
        }
    }
}

delay_t CriticalCohortRouteCosts::cost(Context *ctx, PipId pip) const
{
    auto saved = costs.find(pip);
    if (saved != costs.end()) return saved->second;
    if (calibrated && !observed.count(pip) && !ctx->getPipSrcWire(pip).is_nextpnr_created()) {
        const auto &cal = types.at(ctx->getPipSrcWire(pip).node.t());
        if (cal.table_ps > 0) {
            // Reject malformed exported calibration before native float-to-int
            // conversion. This envelope bounds both native scaling branches.
            double envelope = std::max({1.0, cal.analogue_ps / cal.table_ps,
                                        cal.analogue_ps / cal.hops}) * prior *
                              std::max(1, ctx->getPipDelayTable(pip).maxDelay());
            require(std::isfinite(envelope) && envelope < std::numeric_limits<delay_t>::max() - 100,
                    "native calibration would overflow");
        }
    }
    int64_t value = int64_t(ctx->getPipDelay(pip).maxDelay()) + ctx->getWireDelay(ctx->getPipDstWire(pip)).maxDelay() +
                    ctx->getDelayEpsilon();
    require(value >= 0 && value < std::numeric_limits<delay_t>::max(), "invalid native pip cost");
    auto result = delay_t(std::max(int64_t(1), value));
    costs[pip] = result;
    return result;
}

std::optional<delay_t> CriticalCohortRouteCosts::reference(IdString net, WireId source,
                                                        const std::vector<WireId> &sinks) const
{
    auto tree = references.find(net);
    if (tree == references.end() || source != sources.at(net) || sinks.empty()) return {};
    delay_t result = 0;
    for (auto sink : sinks) {
        auto value = tree->second.find(sink);
        if (value == tree->second.end()) return {};
        result = std::max(result, value->second);
    }
    return result;
}

std::map<WireId, delay_t> CriticalCohortRouteCosts::estimate(Context *ctx, NetInfo *net, WireId source,
    const std::set<WireId> &sinks, const std::set<IdString> &released, const std::map<WireId, IdString> &drivers,
    int &budget, json11::Json::object *trace, std::map<WireId, PipId> *tree_out) const
{
    return search(ctx, net, source, sinks, released, drivers, budget, trace, tree_out,
                  {{source, PipId()}}, PipId());
}

bool CriticalCohortRouteCosts::branch_stub(Context *ctx, IdString net, WireId target,
    const std::set<WireId> &sinks, std::map<WireId, PipId> &retained, std::vector<PipId> &removed) const
{
    auto found = trees.find(net);
    if (found == trees.end() || !sinks.count(target) || !found->second.count(target)) return false;
    std::map<WireId, int> children;
    for (const auto &wire : found->second)
        if (wire.second != PipId()) ++children[ctx->getPipSrcWire(wire.second)];
    for (auto sink : sinks)
        if (!found->second.count(sink)) return false;
    // An internal sink feeds another branch. Its upstream replacement needs a
    // different repair family; do not disconnect that downstream fanout here.
    if (children[target] != 0 || target == sources.at(net)) return false;
    retained = found->second;
    WireId cursor = target;
    while (cursor != sources.at(net)) {
        auto pip = retained.at(cursor);
        auto parent = ctx->getPipSrcWire(pip);
        removed.push_back(pip);
        retained.erase(cursor);
        --children[parent];
        if (parent == sources.at(net) || children[parent] != 0 || sinks.count(parent)) break;
        cursor = parent;
    }
    return !removed.empty();
}

std::vector<PipId> CriticalCohortRouteCosts::branch_alternatives(Context *ctx, IdString net, WireId target,
                                                               const std::set<WireId> &sinks) const
{
    std::map<WireId, PipId> retained;
    std::vector<PipId> removed, choices, result;
    if (!branch_stub(ctx, net, target, sinks, retained, removed)) return result;
    for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
        auto wire = ctx->getPipDstWire(*it);
        if (reservations.count(wire) || ctx->wires.at(wire).wires_uphill.size() < 2) continue;
        choices.push_back(*it);
    }
    // Cover the attachment, middle and sink ends without growing the trial budget.
    for (size_t i = 0; i < 3 && !choices.empty(); ++i) {
        auto pip = choices[i * (choices.size() - 1) / 2];
        if (std::find(result.begin(), result.end(), pip) == result.end()) result.push_back(pip);
    }
    return result;
}

std::map<WireId, delay_t> CriticalCohortRouteCosts::repair_branch(Context *ctx, NetInfo *net, WireId source,
    const std::set<WireId> &sinks, WireId target, PipId avoid, const std::set<IdString> &released,
    const std::map<WireId, IdString> &drivers, int &budget, json11::Json::object *trace,
    std::map<WireId, PipId> *tree_out) const
{
    std::map<WireId, PipId> retained, tree;
    std::vector<PipId> removed;
    std::map<WireId, delay_t> result;
    if (!sources.count(net->name) || source != sources.at(net->name) || avoid == PipId() ||
        !branch_stub(ctx, net->name, target, sinks, retained, removed) ||
        std::find(removed.begin(), removed.end(), avoid) == removed.end()) return result;
    auto repaired = search(ctx, net, source, {target}, released, drivers, budget, trace, &tree, retained, avoid);
    if (tree_out) *tree_out = tree;
    if (!repaired.count(target)) return result;
    require(tree != trees.at(net->name), "branch repair reproduced its excluded parent edge");
    CalibrationScope scope(ctx, calibrated, prior, observed, types);
    std::map<WireId, delay_t> values{{source, 0}};
    for (auto sink : sinks) {
        std::vector<WireId> path;
        std::set<WireId> seen;
        WireId cursor = sink;
        while (!values.count(cursor)) {
            auto binding = tree.find(cursor);
            if (binding == tree.end() || binding->second == PipId() || !seen.insert(cursor).second) return {};
            path.push_back(cursor);
            cursor = ctx->getPipSrcWire(binding->second);
        }
        while (!path.empty()) {
            auto next = path.back(); path.pop_back();
            int64_t value = int64_t(values.at(cursor)) + cost(ctx, tree.at(next));
            if (value >= std::numeric_limits<delay_t>::max()) return {};
            values[next] = delay_t(value);
            cursor = next;
        }
        result[sink] = values.at(sink);
    }
    return result;
}

std::map<WireId, PipId> CriticalCohortRouteCosts::reconnect_source(Context *ctx, NetInfo *net, WireId source,
    const std::set<WireId> &roots, const std::map<WireId, PipId> &forest,
    const std::set<IdString> &released, const std::map<WireId, IdString> &drivers,
    int &budget, json11::Json::object *trace) const
{
    auto original = trees.find(net->name);
    auto old_source = sources.find(net->name);
    if (original == trees.end() || old_source == sources.end() || source == WireId() ||
        source == old_source->second || original->second.count(source) || roots.empty() ||
        !released.count(net->name) || !net->wires.empty() || ctx->getNetinfoSourceWire(net) != source)
        return {};
    for (auto root : roots)
        if (!original->second.count(root) || root == old_source->second || forest.count(root)) return {};
    // Every retained edge must belong to a rooted component of the actual
    // original tree. The incoming edge of each root is deliberately absent.
    for (const auto &entry : forest) {
        auto found = original->second.find(entry.first);
        if (found == original->second.end() || found->second != entry.second || entry.second == PipId()) return {};
        WireId cursor = entry.first;
        std::set<WireId> seen;
        while (!roots.count(cursor)) {
            auto parent = forest.find(cursor);
            if (parent == forest.end() || parent->second == PipId() || !seen.insert(cursor).second) return {};
            cursor = ctx->getPipSrcWire(parent->second);
        }
    }
    std::set<WireId> sinks;
    for (const auto &user : net->users)
        for (auto sink : ctx->getNetinfoSinkWires(net, user)) {
            if (!roots.count(sink) && !forest.count(sink)) return {};
            sinks.insert(sink);
        }
    if (sinks.empty()) return {};
    auto retained = forest;
    retained.emplace(source, PipId());
    std::map<WireId, PipId> result;
    // The old LUT output is no longer a driver. Never price a route through
    // it as though moving a cell had left a source behind.
    auto costs = search(ctx, net, source, roots, released, drivers, budget, trace, &result,
                        retained, PipId(), {old_source->second});
    if (costs.size() != roots.size()) return {};
    for (const auto &entry : forest)
        if (!result.count(entry.first) || result.at(entry.first) != entry.second) return {};
    // No disconnected source, cycle, changed downstream parent or omitted
    // actual consumer may escape as a partial successful result.
    for (const auto &entry : result) {
        WireId cursor = entry.first;
        std::set<WireId> seen;
        while (cursor != source) {
            auto parent = result.find(cursor);
            if (parent == result.end() || parent->second == PipId() || !seen.insert(cursor).second) return {};
            cursor = ctx->getPipSrcWire(parent->second);
        }
    }
    for (auto sink : sinks) if (!result.count(sink)) return {};
    return result;
}

std::map<WireId, delay_t> CriticalCohortRouteCosts::search(Context *ctx, NetInfo *net, WireId source,
    const std::set<WireId> &sinks, const std::set<IdString> &released, const std::map<WireId, IdString> &drivers,
    int &budget, json11::Json::object *trace, std::map<WireId, PipId> *tree_out,
    const std::map<WireId, PipId> &retained, PipId avoid, const std::set<WireId> &barriers) const
{
    std::map<WireId, delay_t> result;
    if (source == WireId() || !ctx->wires.count(source) || sinks.empty()) return result;
    auto blocked = [&](WireId wire) {
        if (barriers.count(wire)) return true;
        auto owner = occupied.find(wire);
        if (owner != occupied.end() && owner->second != net->name &&
            (!trees.count(owner->second) || !released.count(owner->second))) return true;
        auto driver = drivers.find(wire);
        if (driver != drivers.end() && driver->second != net->name) return true;
        auto bound = ctx->getBoundWireNet(wire);
        return bound != nullptr && bound != net;
    };
    if (blocked(source)) return result;
    std::map<WireId, PipId> tree = retained;
    CalibrationScope scope(ctx, calibrated, prior, observed, types);
    using Entry = std::pair<delay_t, WireId>;
    const auto infinity = std::numeric_limits<delay_t>::max();
    for (WireId target : sinks) {
        if (blocked(target) || !ctx->wires.count(target)) continue;
        if (target == source) { result[target] = 0; continue; }
        std::array<std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>, 2> queue;
        std::array<dict<WireId, delay_t>, 2> distance;
        std::array<dict<WireId, PipId>, 2> parents;
        distance[0][source] = 0;
        distance[1][target] = 0;
        queue[0].emplace(0, source);
        queue[1].emplace(0, target);
        delay_t best = infinity;
        WireId meeting;
        while (true) {
            for (int side = 0; side < 2; ++side)
                while (!queue[side].empty() &&
                       distance[side].at(queue[side].top().second) != queue[side].top().first)
                    queue[side].pop();
            // A meeting path is an upper bound. Emit it only once the two
            // frontiers prove there is no shorter available native path.
            if (queue[0].empty() || queue[1].empty() ||
                int64_t(queue[0].top().first) + queue[1].top().first >= best) {
                if (best != infinity) {
                    result[target] = best;
                    {
                        std::vector<PipId> path;
                        WireId cursor = meeting;
                        while (cursor != source) {
                            auto pip = parents[0].at(cursor);
                            path.push_back(pip);
                            cursor = ctx->getPipSrcWire(pip);
                        }
                        std::reverse(path.begin(), path.end());
                        cursor = meeting;
                        while (cursor != target) {
                            auto pip = parents[1].at(cursor);
                            path.push_back(pip);
                            cursor = ctx->getPipDstWire(pip);
                        }
                        J::array pips;
                        int64_t sum = 0;
                        for (auto pip : path) {
                            auto delay = cost(ctx, pip);
                            sum += delay;
                            pips.push_back(J::array{ctx->getPipName(pip).str(ctx), delay});
                            auto inserted = tree.emplace(ctx->getPipDstWire(pip), pip);
                            require(inserted.second || inserted.first->second == pip, "incompatible branch parents");
                        }
                        require(sum == best, "search path and cost disagree");
                        if (trace) (*trace)[ctx->getWireName(target).str(ctx)] = J::object{{"cost_ps", best}, {"pips", pips}};
                    }
                }
                break;
            }
            if (budget <= 0) break;
            int side = queue[0].top().first <= queue[1].top().first ? 0 : 1;
            auto [value, wire] = queue[side].top(); queue[side].pop();
            --budget;
            auto relax = [&](PipId pip) {
                if (pip == avoid) return;
                auto reserved = reservations.find(ctx->getPipDstWire(pip));
                if (reserved != reservations.end() && reserved->second != ctx->getPipSrcWire(pip)) return;
                WireId next = side == 0 ? ctx->getPipDstWire(pip) : ctx->getPipSrcWire(pip);
                if (blocked(next) || !ctx->checkPipAvailForNet(pip, net)) return;
                auto parent = tree.find(ctx->getPipDstWire(pip));
                if (parent != tree.end() && parent->second != pip) return;
                int64_t cost_next = int64_t(value) + cost(ctx, pip);
                if (cost_next >= infinity) return;
                auto old = distance[side].find(next);
                if (old == distance[side].end() || cost_next < old->second) {
                    distance[side][next] = delay_t(cost_next);
                    parents[side][next] = pip;
                    queue[side].emplace(delay_t(cost_next), next);
                    auto opposite = distance[1 - side].find(next);
                    if (opposite != distance[1 - side].end()) {
                        int64_t joined = cost_next + opposite->second;
                        if (joined < best) {
                            best = delay_t(joined);
                            meeting = next;
                        }
                    }
                }
            };
            if (side == 0)
                for (auto pip : ctx->getPipsDownhill(wire)) relax(pip);
            else
                for (auto pip : ctx->getPipsUphill(wire)) relax(pip);
        }
        if (budget <= 0 && result.size() < sinks.size()) break;
    }
    if (tree_out) *tree_out = std::move(tree);
    return result;
}

void CriticalCohortRouteCosts::with_analogue(Context *ctx, const std::set<IdString> &released,
    const std::map<IdString, std::map<WireId, PipId>> &replacement, const std::function<void()> &inspect) const
{
    if (!analogue_model) analogue_model.reset(mistral::CycloneV::get_model(ctx->args.device));
    struct Scope {
        Context *ctx;
        std::unique_ptr<mistral::CycloneV> &model;
        Scope(Context *ctx, std::unique_ptr<mistral::CycloneV> &model) : ctx(ctx), model(model) {
            std::swap(ctx->cyclonev, model);
        }
        ~Scope() { std::swap(ctx->cyclonev, model); }
    } scope(ctx, analogue_model);
    pool<PipId> pips;
    for (const auto &entry : background)
        if (!released.count(entry.first) || !trees.count(entry.first))
            for (auto pip : entry.second) pips.insert(pip);
    for (const auto &entry : replacement)
        for (const auto &wire : entry.second)
            if (wire.second != PipId()) pips.insert(wire.second);
    configure_critical_cohort_analogue(ctx, pips);
    inspect();
}

std::optional<Arch::AnalogueArc> CriticalCohortRouteCosts::analogue(Context *ctx, NetInfo *net, PortRef sink,
    const std::map<WireId, PipId> &tree) const
{
    auto source = ctx->getNetinfoSourceWire(net);
    auto target = ctx->getNetinfoSinkWire(net, sink, 0);
    std::set<WireId> seen;
    auto cursor = target;
    std::vector<PipId> path;
    while (cursor != source) {
        auto edge = tree.find(cursor);
        if (edge == tree.end() || edge->second == PipId() || !seen.insert(cursor).second) return {};
        path.push_back(edge->second);
        cursor = ctx->getPipSrcWire(edge->second);
    }
    // Legal graph edges can select block muxes without analogue outputs in a
    // placement preview. Preflight the native circuits; signoff keeps its
    // assertions, and unsupported previews cannot authorize candidates.
    const auto type = sink.cell->type;
    const int width = type == id_MISTRAL_ALUT6 ? 6 : type == id_MISTRAL_ALUT5 ? 5 :
                      type == id_MISTRAL_ALUT4 ? 4 : type == id_MISTRAL_ALUT3 ? 3 :
                      type == id_MISTRAL_ALUT2 ? 2 : type.in(id_MISTRAL_BUF, id_MISTRAL_NOT) ? 1 : 0;
    const std::array<IdString, 6> inputs{id_A, id_B, id_C, id_D, id_E, id_F};
    bool terminal = std::find(inputs.begin(), inputs.begin() + width, sink.port) != inputs.begin() + width &&
                    !target.is_nextpnr_created() && target.node.t() == mistral::CycloneV::GOUT;
    auto supported = [&](WireId from, WireId to) {
        if (from.is_nextpnr_created()) return true;
        auto cv = ctx->cyclonev.get();
        auto node = cv->rc2ri(from.node);
        auto output = to == WireId() || to.is_nextpnr_created() ? 0xffffffff : cv->rc2ri(to.node);
        auto mode = cv->rnode_timing_get_mode(node);
        if (mode == mistral::CycloneV::RTM_UNSUPPORTED) return false;
        if (mode == mistral::CycloneV::RTM_P2P || mode == mistral::CycloneV::RTM_NO_DELAY) return true;
        for (auto edge : {mistral::CycloneV::RF_RISE, mistral::CycloneV::RF_FALL}) {
            mistral::AnalogSim sim;
            int input = -1;
            std::vector<std::pair<mistral::CycloneV::rnode_index, int>> outputs;
            cv->rnode_timing_build_circuit(node, mistral::CycloneV::T_100, mistral::CycloneV::DELAY_MAX,
                                          edge, sim, input, outputs);
            if (input < 0 || std::none_of(outputs.begin(), outputs.end(), [&](const auto &entry) {
                return entry.first == output;
            })) return false;
        }
        return true;
    };
    for (auto pip : path)
        if (!supported(ctx->getPipSrcWire(pip), ctx->getPipDstWire(pip))) return {};
    if (terminal && !supported(target, WireId())) return {};
    NetInfo trial(net->name);
    trial.driver = net->driver;
    for (const auto &edge : tree) trial.wires[edge.first] = PipMap{edge.second, STRENGTH_WEAK};
    DelayQuad delay, input;
    if (!ctx->analogue_arc_delay(&trial, sink, delay, nullptr, &input)) return {};
    return Arch::AnalogueArc{delay, true, input};
}
NEXTPNR_NAMESPACE_END
