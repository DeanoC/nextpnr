/* Bounded, transactional placement repair. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_CRITICAL_COHORT_H
#define MISTRAL_CRITICAL_COHORT_H

#include <functional>
#include <map>
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
// sites. A single unclustered FF or ordinary LUT may change its slot by dz;
// other cohorts preserve their ALM arrangement. keep_registers leaves all FFs
// bound and excludes any cluster that would move one. The callback must only
// inspect the complete legal placement. Rejection, exceptions
// and search exhaustion restore all bindings, strengths and LAB input counts.
bool critical_cohort_trial(Context *ctx, const std::vector<CellInfo *> &cohort, int dx, int dy, int node_budget,
                           const std::function<bool()> &accept, CriticalCohortStats &stats, int dz = 0,
                           bool keep_registers = false);
bool critical_cohort_timing_safe(Context *ctx, TimingAnalyser &before, TimingAnalyser &after,
                                 const std::vector<CellPortKey> &focus = {});
void repair_critical_cohorts(Context *ctx, int timing_budget);
using CriticalCohortEndpointRows = std::map<CellPortKey, std::vector<EndpointClockPairTiming>>;
// Strict endpoint and score checks used only by routed feedback proposals.
bool critical_cohort_proposal_rows_safe(const CriticalCohortEndpointRows &before,
                                       const CriticalCohortEndpointRows &after);
struct CriticalCohortPrediction
{
    // References remain valid only during inspection of the complete trial.
    const CriticalCohortEndpointRows &before, &after;
    delay_t before_slack, after_slack;
    bool legacy_safe, eligible;
    bool cost_complete;
    int cost_nodes;
    const json11::Json &routing_trace;
};
// Inspect each independent timed trial, including rejected predictions. Return
// true only to emit an eligible proposal, up to candidates. Every trial restores
// the parent, including after callback failure. Predictions use ideal clocks;
// final routed observations remain the retention authority.
void propose_critical_cohorts(Context *ctx, const std::string &model, int timing_budget, int candidates,
                             const std::function<bool(const CriticalCohortPrediction &)> &inspect);
struct CriticalCohortGroupTrial
{
    std::vector<IdString> seed;
    int moved_cells, moved_registers;
    int64_t boundary_before, boundary_after;
};
// Legal structural alternatives with no predicted timing claim. Every trial
// restores the complete parent. Only fresh full routing can qualify a result.
void propose_critical_cohort_groups(Context *ctx, const std::string &model, int trial_limit,
                                   const std::function<void(const CriticalCohortGroupTrial &)> &inspect);
struct CriticalCohortLutTrial
{
    IdString cell;
    BelId original, from, to;
    int64_t boundary_before, boundary_after;
};
struct CriticalCohortLutSearch
{
    int cells = 0, attempted = 0, legal = 0;
};
// Ordinary unclustered LUTs whose original ALM now contains another moved cell.
std::vector<CellInfo *> critical_cohort_displaced_luts(Context *ctx, const json11::Json &origin);
// Inspect one otherwise-empty native site. Always restore the complete parent.
bool inspect_critical_cohort_lut_site(Context *ctx, CellInfo *cell, BelId target,
                                     const std::function<void()> &inspect);
void propose_critical_cohort_displaced_luts(Context *ctx, const json11::Json &origin,
    const std::string &model, int trial_limit, const std::function<void(const CriticalCohortLutTrial &)> &inspect,
    CriticalCohortLutSearch &stats);
bool apply_critical_cohort_placement(Context *ctx, const std::vector<std::pair<CellInfo *, BelId>> &moves);

NEXTPNR_NAMESPACE_END
#endif
