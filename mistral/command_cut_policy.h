#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "timeout_refine_policy.h"
namespace command_cut_policy {
constexpr uint64_t root_mask = 0xddddddd0ULL;
inline bool improves(const std::vector<float> &before, const std::vector<float> &after,
                     const std::vector<unsigned> &command)
{
    if (before.empty() || before.size() != after.size() || command.empty())
        return false;
    for (size_t i = 0; i < before.size(); ++i)
        if (!std::isfinite(before[i]) || !std::isfinite(after[i]) || after[i] < before[i])
            return false;
    std::vector<float> a, b;
    for (auto i : command) {
        if (i >= before.size())
            return false;
        a.push_back(before[i]);
        b.push_back(after[i]);
    }
    return timeout_refine_policy::improves(a, b);
}
} // namespace command_cut_policy
