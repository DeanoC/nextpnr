#ifndef MISTRAL_ENABLE_REPLICATION_POLICY_H
#define MISTRAL_ENABLE_REPLICATION_POLICY_H
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

// Geometry is measured in Mistral picoseconds. These conservative admission
// rules do not claim to model the extra electrical load of a replica.
namespace enable_replication_policy {
inline bool inputs_nonregressing(const std::vector<int> &before, const std::vector<int> &after)
{
    if (before.size() != after.size()) return false;
    for (size_t i = 0; i < before.size(); ++i)
        if (after[i] > before[i]) return false;
    return true;
}
inline int group_gain(const std::vector<int> &before, const std::vector<int> &after)
{
    if (before.empty() || before.size() != after.size()) return 0;
    int gain = before.front() - after.front();
    for (size_t i = 0; i < before.size(); ++i) gain = std::min(gain, before[i] - after[i]);
    return gain >= 250 ? gain : 0;
}
inline bool critical_failing(float criticality, float slack)
{
    return std::isfinite(criticality) && std::isfinite(slack) && criticality >= 0.9f && slack < 0;
}
inline bool hold_nonregressing(const std::map<std::string, int> &before, const std::map<std::string, int> &after)
{
    for (const auto &entry : after) {
        auto old = before.find(entry.first);
        if (old == before.end() || entry.second < old->second) return false;
    }
    return true;
}
inline bool valid_budget(int budget) { return budget >= 0 && budget <= 8; }
}
#endif
