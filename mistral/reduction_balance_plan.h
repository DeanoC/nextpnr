#ifndef MISTRAL_REDUCTION_BALANCE_PLAN_H
#define MISTRAL_REDUCTION_BALANCE_PLAN_H
#include "nextpnr.h"
#include <map>
#include <vector>

NEXTPNR_NAMESPACE_BEGIN
struct ReductionBalancePlan {
    CellInfo *root = nullptr;
    std::vector<CellInfo *> cells;
    std::vector<std::pair<NetInfo *, bool>> literals;
    std::map<NetInfo *, store_index<PortRef>> slots;
};
// Validate without mutation. Placed mode additionally requires weak, movable BELs.
bool plan_reduction(Context *, const std::string &, bool placed, ReductionBalancePlan &);
// Caller supplies a verified plan with all its cells unbound.
void rewrite_reduction(Context *, const ReductionBalancePlan &);
NEXTPNR_NAMESPACE_END
#endif
