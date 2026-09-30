/* Explicit staged internal LUT-cut remaps. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "json11.hpp"

NEXTPNR_NAMESPACE_BEGIN
bool Arch::execute_comb_remap_plan()
{
    if (fes_any_slot_region_active) log_error("Comb-remap plans require ordinary full-design placement.\n");
    if (comb_remap_plan.empty() || comb_remap_plan.size() > 8)
        log_error("Comb-remap plans require between one and eight steps.\n");
    if (comb_remap_plan_list_only != (comb_remap_plan.back().candidate == -1))
        log_error("Comb-remap plan list mode must select the final listing step.\n");
    // Validate the complete request before applying the first step.
    for (size_t i = 0; i < comb_remap_plan.size(); ++i) {
        const auto &step = comb_remap_plan[i];
        bool listing = i + 1 == comb_remap_plan.size() && comb_remap_plan_list_only && step.candidate == -1;
        if (step.report.empty() || (step.candidate < 0 && !listing))
            log_error("Invalid comb-remap plan step %zu.\n", i);
        std::string error;
        auto report = json11::Json::parse(step.report, error);
        if (!error.empty() || !report["critical_paths"].is_array())
            log_error("Invalid comb-remap plan report at step %zu.\n", i);
    }
    for (size_t i = 0; i < comb_remap_plan.size(); ++i) {
        const auto &step = comb_remap_plan[i];
        log_info("Comb-remap plan step %zu: candidate=%d.\n", i, step.candidate);
        // The existing pass validates continuous current data paths and clock
        // attribution before it probes any mutation. Each rejected probe
        // restores its graph, indexed consumer slots and pin states exactly.
        bool applied = remap_comb_critical(step.report, step.candidate);
        if (step.candidate >= 0 && !applied)
            log_error("Comb-remap plan step %zu did not qualify; routing was not started.\n", i);
        if (step.candidate < 0) return false;
    }
    // Accepted earlier stages remain only in the discarded context if a later
    // stage throws. No partial route or bitstream is published on that failure.
    return true;
}
NEXTPNR_NAMESPACE_END
