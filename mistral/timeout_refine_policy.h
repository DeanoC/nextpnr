#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
namespace timeout_refine_policy {
inline bool improves(const std::vector<float> &before, const std::vector<float> &after)
{
    if (before.empty() || before.size() != after.size())
        return false;
    for (size_t i = 0; i < before.size(); ++i)
        if (!std::isfinite(before[i]) || !std::isfinite(after[i]) || after[i] < before[i])
            return false;
    return *std::min_element(after.begin(), after.end()) >= *std::min_element(before.begin(), before.end()) + 1;
}
} // namespace timeout_refine_policy
