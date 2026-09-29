#ifndef MISTRAL_REDUCTION_BALANCE_POLICY_H
#define MISTRAL_REDUCTION_BALANCE_POLICY_H

#include "local_remap_policy.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace reduction_balance_policy {
using Pin = local_remap_policy::Pin;

struct Node {
    int output;
    uint64_t mask;
    std::vector<Pin> inputs;
};
struct Literal { int signal; bool required; };
struct Reduction {
    bool valid = false;
    std::vector<int> outputs;
    std::vector<Literal> literals;
};

// Only accept irredundant conjunctions of eleven literals in three LUTs or
// sixteen literals in four LUTs. Both can use the same cells in two levels.
// Each LUT must encode a unique input assignment for the output polarity needed
// by its parent. This deliberately excludes general Boolean factoring.
inline Reduction recognize(const std::vector<Node> &network, int root)
{
    Reduction result;
    if (network.size() != 3 && network.size() != 4) return result;
    std::map<int, const Node *> nodes;
    for (const auto &node : network)
        if (!nodes.emplace(node.output, &node).second || node.output < 0 || node.inputs.empty() ||
            node.inputs.size() > 6) return result;
    if (!nodes.count(root)) return result;

    std::map<int, bool> literals;
    std::set<int> visited;
    auto descend = [&](auto &&self, int output, bool required) -> bool {
        auto it = nodes.find(output);
        if (it == nodes.end()) {
            auto inserted = literals.emplace(output, required);
            return output >= 0 && inserted.second;
        }
        if (!visited.insert(output).second) return false;
        const Node &node = *it->second;
        const unsigned rows = 1u << node.inputs.size();
        int only = -1;
        for (unsigned row = 0; row < rows; ++row) {
            if (bool((node.mask >> row) & 1) != required) continue;
            if (only >= 0) return false;
            only = row;
        }
        if (only < 0) return false;
        for (unsigned i = 0; i < node.inputs.size(); ++i) {
            const Pin pin = node.inputs[i];
            if (pin.signal < 0 || !self(self, pin.signal, bool((only >> i) & 1) ^ pin.inverted))
                return false;
        }
        return true;
    };
    const size_t expected_literals = network.size() == 3 ? 11 : 16;
    if (!descend(descend, root, true) || visited.size() != network.size() || literals.size() != expected_literals)
        return result;
    for (const auto &entry : nodes) result.outputs.push_back(entry.first);
    for (const auto &entry : literals) result.literals.push_back({entry.first, entry.second});
    result.valid = true;
    return result;
}
}
#endif
