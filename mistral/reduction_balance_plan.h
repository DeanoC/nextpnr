#ifndef MISTRAL_REDUCTION_BALANCE_PLAN_H
#define MISTRAL_REDUCTION_BALANCE_PLAN_H
#include "nextpnr.h"
#include <map>
#include <vector>

NEXTPNR_NAMESPACE_BEGIN
struct ReductionBalancePlan {
    CellInfo *root = nullptr;
    // All original cells, including the public root, sorted by cell name.
    std::vector<CellInfo *> cells;
    // First two/three/four non-root cells in that order become the new leaves.
    std::vector<CellInfo *> leaves;
    // Only seven-node/twenty-four-literal plans retire two private cells/nets.
    std::vector<CellInfo *> retired;
    std::vector<NetInfo *> retired_nets;
    std::vector<std::pair<NetInfo *, bool>> literals;
    std::map<NetInfo *, store_index<PortRef>> slots;
};
// Validate without mutation. Placed mode additionally requires weak, movable BELs.
bool plan_reduction(Context *, const std::string &, bool placed, ReductionBalancePlan &);
// Caller supplies a verified plan with all its cells unbound. Retired cells and
// nets remain allocated in ctx but fully detached. The caller must remove their
// owners before architecture assignment/timing/checks, restoring the original
// objects, cell fields, net drivers and complete user stores on rejection.
void rewrite_reduction(Context *, const ReductionBalancePlan &);
// Remove every alias targeting a retired net while its owner is still alive.
void remove_reduction_net_aliases(Context *, const ReductionBalancePlan &);
NEXTPNR_NAMESPACE_END
#endif
