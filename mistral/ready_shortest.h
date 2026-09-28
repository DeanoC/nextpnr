/* Exact fixed-occupancy route diagnostic. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_READY_SHORTEST_H
#define MISTRAL_READY_SHORTEST_H
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <stdexcept>
#include <vector>
#include "nextpnr_namespaces.h"
NEXTPNR_NAMESPACE_BEGIN
namespace ready_shortest {
struct Edge
{
    uint32_t to;
    int64_t cost;
    bool allowed;
};
struct Node
{
    int64_t distance = std::numeric_limits<int64_t>::max();
    int64_t predecessor = -1, settled_index = -1;
    bool expanded = false;
    size_t unique_degree = 0;
};
struct Result
{
    std::map<uint32_t, Node> nodes;
    bool reachable = false;
    int64_t distance = -1;
    size_t settled = 0, expanded = 0;
};
// The enumerator supplies the COMPLETE unique outgoing adjacency, including
// blocked edges. It may stream evidence; it must not mutate the searched graph.
inline Result search(uint32_t source, uint32_t sink, int64_t source_delay,
                     const std::function<std::vector<Edge>(uint32_t)> &edges)
{
    if (source_delay < 0)
        throw std::runtime_error("negative source delay");
    Result r;
    using Item = std::pair<int64_t, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    r.nodes[source].distance = source_delay;
    queue.emplace(source_delay, source);
    while (!queue.empty()) {
        auto item = queue.top();
        queue.pop();
        auto &node = r.nodes.at(item.second);
        if (node.settled_index >= 0 || node.distance != item.first)
            continue;
        node.settled_index = r.settled++;
        if (item.second == sink) {
            r.reachable = true;
            r.distance = item.first;
            break;
        }
        auto outgoing = edges(item.second);
        std::sort(outgoing.begin(), outgoing.end(), [](const Edge &a, const Edge &b) { return a.to < b.to; });
        node.expanded = true;
        node.unique_degree = outgoing.size();
        ++r.expanded;
        for (size_t i = 0; i < outgoing.size(); ++i) {
            const auto &e = outgoing[i];
            if (i && outgoing[i - 1].to == e.to)
                throw std::runtime_error("duplicate edge");
            if (e.cost < 0)
                throw std::runtime_error("negative edge delay");
            if (!e.allowed)
                continue;
            if (e.cost > std::numeric_limits<int64_t>::max() - node.distance)
                throw std::runtime_error("route delay overflow");
            int64_t distance = node.distance + e.cost;
            auto &dst = r.nodes[e.to];
            if (distance < dst.distance) {
                if (dst.settled_index >= 0)
                    throw std::runtime_error("settled distance decreased");
                dst.distance = distance;
                dst.predecessor = item.second;
                queue.emplace(distance, e.to);
            }
        }
    }
    return r;
}
} // namespace ready_shortest
NEXTPNR_NAMESPACE_END
#endif
