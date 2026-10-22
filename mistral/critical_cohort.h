/* Bounded, transactional placement repair. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_CRITICAL_COHORT_H
#define MISTRAL_CRITICAL_COHORT_H

#include <functional>
#include <vector>
#include "nextpnr.h"
#include "timing.h"
#include "json11.hpp"

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
// Exchange two unconstrained ordinary LUTs (or move into an empty site).
// Reuse the cohort transaction and affected-LAB legality; callback is inspection only.
bool critical_cohort_swap_trial(Context *ctx, CellInfo *cell, BelId target, const std::function<bool()> &accept);
// Complete endpoint inventory, including explicit unsupported rows. Never intersect reports.
json11::Json critical_swap_endpoint_snapshot(Context *ctx, TimingAnalyser &timing, TimingAnalyser &unskewed);
// Two disjoint LUT exchanges are legalized and accepted as one transaction.
bool critical_cohort_pair_swap_trial(Context *ctx, CellInfo *a, BelId at, CellInfo *b, BelId bt,
                                      const std::function<bool()> &accept);
void repair_critical_pair_swaps(Context *ctx, int timing_budget);
void repair_critical_swaps(Context *ctx, int timing_budget);
void repair_critical_cohorts(Context *ctx, int timing_budget);

NEXTPNR_NAMESPACE_END
#endif
