/* Optional measured-wire calibration. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_CRITICAL_COHORT_MODEL_H
#define MISTRAL_CRITICAL_COHORT_MODEL_H
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>
#include "nextpnr.h"
#include "timing.h"
#include "critical_cohort_route_cost.h"
NEXTPNR_NAMESPACE_BEGIN

// Load once at the matching original placement, inside the physical pin preview.
// Applying it later predicts geometry or protected-resource costs; final routed
// STA remains authoritative.
class CriticalCohortRouteModel
{
  public:
    CriticalCohortRouteModel(Context *ctx, const std::string &report);
    bool apply(Context *ctx, TimingAnalyser &timing, const CriticalCohortBranchRepair *branch = nullptr) const;
    std::vector<CriticalCohortBranchRepair> branch_repairs(Context *ctx, PortRef sink) const;
    int cost_nodes() const { return last_cost_nodes; }
    const json11::Json &routing_trace() const { return last_routing_trace; }
    bool active() const { return !arcs.empty(); }
    delay_t route_delay(Context *ctx, PortRef sink) const;

  private:
    struct Arc
    {
        NetInfo *net;
        PortRef sink;
        DelayPair offset, local_offset;
        bool was_route_through, was_direct;
        DelayPair measured_wire;
        std::optional<delay_t> reference_cost;
        WireId source;
        std::vector<WireId> sinks;
        std::optional<DelayPair> native_full;
        delay_t native_error = 0;
    };
    std::map<CellPortKey, Arc> arcs;
    std::unique_ptr<CriticalCohortRouteCosts> route_costs;
    std::map<CellInfo *, BelId> placement;
    std::map<CellPortKey, std::vector<IdString>> pins;
    mutable int last_cost_nodes = 0;
    mutable json11::Json last_routing_trace;
};
void write_critical_cohort_route_model(Context *ctx, std::ostream &out);
NetInfo *critical_cohort_logical_net(Context *ctx, CellInfo *cell, IdString port);
CellInfo *critical_cohort_route_through_ff(Context *ctx, CellInfo *cell);
NEXTPNR_NAMESPACE_END
#endif
