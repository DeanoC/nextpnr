/* Shared protection for analogue candidate and full rerouting. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ANALOGUE_ROUTING_H
#define MISTRAL_ANALOGUE_ROUTING_H
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN
inline bool analogue_net_is_movable(const NetInfo *net)
{
    if (!net->driver.cell || net->is_global || net->wires.empty()) return false;
    for (const auto &binding : net->wires)
        if (binding.second.strength > STRENGTH_STRONG) return false;
    return true;
}

// Preserve cell-adjacent routing: virtual source/terminal wires and every
// connection entering or leaving them. This fixes FF input/output selectors
// and LUT egress choices while allowing real fabric wires to change.
inline std::map<WireId, PipId> analogue_cell_connections(
        Context *ctx, const std::vector<std::pair<WireId, PipId>> &tree)
{
    std::map<WireId, PipId> result;
    for (const auto &entry : tree)
        if (entry.first.is_nextpnr_created() ||
            (entry.second != PipId() && ctx->getPipSrcWire(entry.second).is_nextpnr_created()))
            result.emplace(entry.first, entry.second);
    return result;
}

// Capture the complete parent's cell connections. The router may rebuild
// real fabric paths, but may not discover an extra FF/LUT selector edge.
inline pool<PipId> analogue_parent_cell_pips(Context *ctx)
{
    pool<PipId> result;
    for (const auto &net : ctx->nets)
        for (const auto &wire : net.second->wires)
            if (wire.second.pip != PipId() &&
                (wire.first.is_nextpnr_created() ||
                 ctx->getPipSrcWire(wire.second.pip).is_nextpnr_created()))
                result.insert(wire.second.pip);
    return result;
}

class AnalogueCellPipScope
{
    Context *ctx;

  public:
    AnalogueCellPipScope(Context *ctx, const pool<PipId> &allowed) : ctx(ctx)
    {
        NPNR_ASSERT(ctx->analogue_cell_pip_allowlist == nullptr);
        ctx->analogue_cell_pip_allowlist = &allowed;
    }
    AnalogueCellPipScope(const AnalogueCellPipScope &) = delete;
    AnalogueCellPipScope &operator=(const AnalogueCellPipScope &) = delete;
    ~AnalogueCellPipScope() { ctx->analogue_cell_pip_allowlist = nullptr; }
};

// A connected seed for an explicitly requested whole-input-net trial.
// Keep the actual source and all its existing cell egresses. Fabric branches
// may be rebuilt; the caller must separately preserve every parent cell
// connection and all unselected nets before accepting a complete route.
inline std::optional<std::vector<std::pair<WireId, PipId>>> analogue_source_prefix(
        Context *ctx, WireId source, const std::vector<std::pair<WireId, PipId>> &tree)
{
    std::map<WireId, PipId> parent, kept;
    std::map<WireId, std::vector<WireId>> children;
    for (const auto &entry : tree) {
        if (!parent.emplace(entry.first, entry.second).second ||
            (entry.second != PipId() && ctx->getPipDstWire(entry.second) != entry.first)) return {};
    }
    auto root = parent.find(source);
    if (source == WireId() || root == parent.end() || root->second != PipId()) return {};
    for (const auto &entry : parent) if (entry.first != source) {
        if (entry.second == PipId()) return {};
        auto driver = ctx->getPipSrcWire(entry.second);
        if (!parent.count(driver)) return {};
        children[driver].push_back(entry.first);
    }
    std::vector<WireId> reachable{source};
    std::map<WireId, bool> seen{{source, true}};
    for (size_t index = 0; index < reachable.size(); ++index)
        for (auto child : children[reachable[index]]) {
            if (!seen.emplace(child, true).second) return {};
            reachable.push_back(child);
        }
    if (reachable.size() != parent.size()) return {};
    kept.emplace(source, PipId());
    std::vector<WireId> pending{source};
    for (size_t index = 0; index < pending.size(); ++index) {
        auto wire = pending[index];
        if (!wire.is_nextpnr_created()) continue;
        for (auto child : children[wire]) {
            if (!kept.emplace(child, parent.at(child)).second) return {};
            pending.push_back(child);
        }
    }
    return std::vector<std::pair<WireId, PipId>>(kept.begin(), kept.end());
}
// A whole-net route may prune an unused source-egress branch. Reapply only
// missing entries of the already-declared cell-adjacent seed; never replace
// an existing connection, take another net's wire, or restore fabric paths.
// New bindings are temporary PLACER reservations; the caller restores the
// parent's strengths and still checks every protected route and cell mux.
inline std::optional<size_t> analogue_retain_source_prefix(
        Context *ctx, NetInfo *net, WireId source, const std::vector<std::pair<WireId, PipId>> &prefix)
{
    auto checked = analogue_source_prefix(ctx, source, prefix);
    if (!checked || checked->size() != prefix.size()) return {};
    auto root = net->wires.find(source);
    if (root == net->wires.end() || root->second.pip != PipId() || ctx->getBoundWireNet(source) != net)
        return {};
    std::vector<std::pair<WireId, PipId>> missing;
    for (const auto &entry : *checked) {
        auto found = net->wires.find(entry.first);
        if (found != net->wires.end()) {
            if (found->second.pip != entry.second || ctx->getBoundWireNet(entry.first) != net) return {};
        } else {
            if (entry.second == PipId() || !ctx->checkWireAvail(entry.first)) return {};
            missing.push_back(entry);
        }
    }
    std::vector<WireId> added;
    auto rollback = [&]() {
        for (auto wire = added.rbegin(); wire != added.rend(); ++wire) ctx->unbindWire(*wire);
    };
    try {
        while (added.size() < missing.size()) {
            const size_t before = added.size();
            for (const auto &entry : missing) {
                if (net->wires.count(entry.first)) continue;
                if (ctx->getBoundWireNet(ctx->getPipSrcWire(entry.second)) != net) continue;
                if (!ctx->checkWireAvail(entry.first) || !ctx->checkPipAvailForNet(entry.second, net)) {
                    rollback(); return {};
                }
                ctx->bindPip(entry.second, net, STRENGTH_PLACER);
                added.push_back(entry.first);
            }
            if (added.size() == before) { rollback(); return {}; }
        }
    } catch (...) { rollback(); throw; }
    return added.size();
}
NEXTPNR_NAMESPACE_END
#endif
