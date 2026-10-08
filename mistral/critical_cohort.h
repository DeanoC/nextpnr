/* Bounded, transactional placement repair. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_CRITICAL_COHORT_H
#define MISTRAL_CRITICAL_COHORT_H

#include <functional>
#include <vector>
#include "nextpnr.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN

struct CriticalCohortStats
{
    int nodes = 0, cells = 0, displaced = 0;
    bool timed = false;
};

// Select the cell and its direct same-LAB data neighbors, closing over clusters.
std::vector<CellInfo *> critical_cohort_neighborhood(Context *ctx, CellInfo *cell);

// Temporarily assign routing's LUT pins, restoring pin data and LAB state even
// if inspection throws. No route-through cells or nets are created.
void critical_cohort_pin_preview(Context *ctx, const std::function<void()> &inspect);

// Setup native STA, folding predicted FF route-throughs into DATAIN delay.
// Call inside critical_cohort_pin_preview so ALM modes reflect physical usage.
void critical_cohort_setup_timing(Context *ctx, TimingAnalyser &timing);

// Translate a same-LAB cohort, closing over complete clusters. Re-place movable
// destination occupants at home first, then in source vacancies or nearby
// sites. A single unclustered FF may also change its slot by dz; other cohorts
// preserve their ALM arrangement. The callback must only inspect the complete legal placement. Rejection, exceptions
// and search exhaustion restore all bindings, strengths and LAB input counts.
bool critical_cohort_trial(Context *ctx, const std::vector<CellInfo *> &cohort, int dx, int dy, int node_budget,
                           const std::function<bool()> &accept, CriticalCohortStats &stats, int dz = 0);
bool critical_cohort_timing_safe(Context *ctx, TimingAnalyser &before, TimingAnalyser &after,
                                 const std::vector<CellPortKey> &focus = {});
void repair_critical_cohorts(Context *ctx, int timing_budget);

NEXTPNR_NAMESPACE_END
#endif
