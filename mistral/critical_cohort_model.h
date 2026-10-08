/* Optional measured-wire calibration. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_CRITICAL_COHORT_MODEL_H
#define MISTRAL_CRITICAL_COHORT_MODEL_H
#include <map>
#include <ostream>
#include <string>
#include <vector>
#include "nextpnr.h"
#include "timing.h"
NEXTPNR_NAMESPACE_BEGIN

// Load once at the matching original placement, inside the physical pin preview.
// Applying it later predicts geometry changes; it does not perform routed STA.
class CriticalCohortRouteModel
{
  public:
    CriticalCohortRouteModel(Context *ctx, const std::string &report);
    void apply(Context *ctx, TimingAnalyser &timing) const;
    bool active() const { return !arcs.empty(); }
    delay_t route_delay(Context *ctx, PortRef sink) const;

  private:
    struct Arc
    {
        NetInfo *net;
        PortRef sink;
        DelayPair offset, local_offset;
        bool was_route_through, was_direct;
    };
    std::map<CellPortKey, Arc> arcs;
};
void write_critical_cohort_route_model(Context *ctx, std::ostream &out);
NEXTPNR_NAMESPACE_END
#endif
