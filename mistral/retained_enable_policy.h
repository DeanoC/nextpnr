#ifndef MISTRAL_RETAINED_ENABLE_POLICY_H
#define MISTRAL_RETAINED_ENABLE_POLICY_H
#include <set>
#include "enable_replication_policy.h"
namespace retained_enable_policy {
inline int group_gain(bool protected_lab, const std::vector<int> &before, const std::vector<int> &after)
{
    return protected_lab ? 0 : enable_replication_policy::group_gain(before, after);
}
template <typename T> bool disjoint(const std::set<T> &a, const std::set<T> &b)
{
    for (const auto &value : a)
        if (b.count(value))
            return false;
    return true;
}
inline bool budget_available(int configured, int existing)
{
    return enable_replication_policy::valid_budget(configured) && existing >= 0 && existing < configured;
}
} // namespace retained_enable_policy
#endif
