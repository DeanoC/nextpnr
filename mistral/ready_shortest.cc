/* Exact search in the frozen occupied graph, not a globally timing-optimal router.
 * SPDX-License-Identifier: ISC */
#include "ready_shortest.h"
#include <fstream>
#include <set>
#include "ready_shortest_backend.h"
NEXTPNR_NAMESPACE_BEGIN
std::vector<GpuRouteTree> ready_shortest_candidate(Context *ctx, NetInfo *target, WireId source, WireId sink,
                                                   const char *prefix, bool relaxed)
{
    // Fence availability can depend on whether a target pip is currently bound.
    // This diagnostic requires a static graph through target ripup/rebinding.
    NPNR_ASSERT(!ctx->fes_fence_active && !ctx->fes_has_reserved_rect);
    NPNR_ASSERT(ctx->getBoundWireNet(source) == target);
    auto wire = [](uint32_t id) {
        CycloneV::rnode_coords n;
        n.v = id;
        return WireId(n);
    };
    std::ofstream ef(std::string(prefix) + ".shortest-edges.tsv");
    ef << "src\tdst\tdst_wire\tpip_delay_ps\twire_delay_ps\tallowed\treason\twire_owner\tpip_owner\n";
    std::map<uint32_t, size_t> raw_degrees;
    size_t edge_count = 0;
    auto result =
            ready_shortest::search(source.node.v, sink.node.v, ctx->getWireDelay(source).maxDelay(), [&](uint32_t id) {
                std::map<uint32_t, PipId> outgoing;
                size_t raw = 0;
                for (auto pip : ctx->getPipsDownhill(wire(id))) {
                    NPNR_ASSERT(ctx->getPipSrcWire(pip) == wire(id));
                    outgoing.emplace(ctx->getPipDstWire(pip).node.v, pip);
                    ++raw;
                }
                raw_degrees[id] = raw;
                std::vector<ready_shortest::Edge> edges;
                for (auto &entry : outgoing) {
                    auto pip = entry.second;
                    auto dst = ctx->getPipDstWire(pip);
                    auto owner = ctx->getBoundWireNet(dst), pip_owner = ctx->getBoundPipNet(pip);
                    bool foreign = owner && owner != target;
                    // Relax ownership only; static architecture restrictions still apply.
                    bool allowed = relaxed ? !ctx->is_pip_blocked(pip) && ctx->fes_pip_preserves_cram(pip)
                                           : !foreign && ctx->checkPipAvailForNet(pip, target);
                    auto pd = ctx->getPipDelay(pip).maxDelay(), wd = ctx->getWireDelay(dst).maxDelay();
                    NPNR_ASSERT(pd >= 0 && wd >= 0);
                    edges.push_back({entry.first, int64_t(pd) + wd, allowed});
                    ef << id << '\t' << entry.first << '\t' << ctx->nameOfWire(dst) << '\t' << pd << '\t' << wd << '\t'
                       << allowed << '\t'
                       << (allowed               ? ""
                           : !relaxed && foreign ? "other_wire"
                                                 : "pip_unavailable")
                       << '\t' << (owner ? owner->name.str(ctx) : "") << '\t'
                       << (pip_owner ? pip_owner->name.str(ctx) : "") << '\n';
                    ++edge_count;
                }
                return edges;
            });
    ef.close();
    NPNR_ASSERT(ef);
    std::ofstream nf(std::string(prefix) + ".shortest-nodes.tsv");
    nf << "id\twire\tdistance_ps\tpredecessor\tsettled_index\texpanded\traw_degree\tunique_degree\n";
    for (auto &entry : result.nodes) {
        auto &n = entry.second;
        nf << entry.first << '\t' << ctx->nameOfWire(wire(entry.first)) << '\t' << n.distance << '\t' << n.predecessor
           << '\t' << n.settled_index << '\t' << n.expanded << '\t' << (n.expanded ? raw_degrees.at(entry.first) : 0)
           << '\t' << n.unique_degree << '\n';
    }
    nf.close();
    NPNR_ASSERT(nf);
    std::ofstream jf(std::string(prefix) + ".shortest.json");
    jf << "{\"algorithm\":\"dijkstra\",\"queue_order\":\"distance,node_id\",\"stop\":\"sink_pop\",\"graph\":\""
       << (relaxed ? "occupancy_relaxed" : "fixed_occupancy") << "\",\"source_id\":" << source.node.v
       << ",\"sink_id\":" << sink.node.v << ",\"source_wire_delay_ps\":" << ctx->getWireDelay(source).maxDelay()
       << ",\"reachable\":" << (result.reachable ? "true" : "false") << ",\"distance_ps\":" << result.distance
       << ",\"settled\":" << result.settled << ",\"expanded\":" << result.expanded << ",\"edges\":" << edge_count
       << "}\n";
    jf.close();
    NPNR_ASSERT(jf);
    if (!result.reachable)
        return {};
    NPNR_ASSERT(result.distance <= std::numeric_limits<delay_t>::max());
    GpuRouteTree tree;
    tree.variant = relaxed ? 201 : 200;
    tree.route_delay = result.distance;
    uint32_t current = sink.node.v;
    while (current != source.node.v) {
        auto pred = result.nodes.at(current).predecessor;
        NPNR_ASSERT(pred >= 0);
        tree.wires.emplace_back(wire(current), PipId(wire(pred).node, wire(current).node));
        current = pred;
    }
    tree.wires.emplace_back(source, PipId());
    std::reverse(tree.wires.begin(), tree.wires.end());
    if (relaxed) {
        std::ofstream blockers(std::string(prefix) + ".shortest-blockers.tsv");
        blockers << "wire\tpip_src\twire_owner\tpip_owner\n";
        for (const auto &step : tree.wires) {
            auto owner = ctx->getBoundWireNet(step.first);
            auto pip_owner = step.second == PipId() ? nullptr : ctx->getBoundPipNet(step.second);
            if ((owner && owner != target) || (pip_owner && pip_owner != target))
                blockers << ctx->nameOfWire(step.first) << '\t'
                         << (step.second == PipId() ? "" : ctx->nameOfWire(ctx->getPipSrcWire(step.second))) << '\t'
                         << (owner ? owner->name.str(ctx) : "") << '\t' << (pip_owner ? pip_owner->name.str(ctx) : "")
                         << '\n';
        }
        blockers.close();
        NPNR_ASSERT(blockers);
    }
    return {tree};
}
NEXTPNR_NAMESPACE_END
