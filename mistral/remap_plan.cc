/* Explicit staged local-remap experiments. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "remap_report.h"

NEXTPNR_NAMESPACE_BEGIN
bool Arch::execute_local_remap_plan()
{
    if (fes_any_slot_region_active) log_error("Local-remap plans require ordinary full-design placement.\n");
    if (local_remap_plan.empty() || local_remap_plan.size() > local_remap_max_steps)
        log_error("Local-remap plans require between one and %zu steps.\n", local_remap_max_steps);
    if (local_remap_plan_list_only != (local_remap_plan.back().candidate == -1))
        log_error("Local-remap plan list mode must select the final listing step.\n");
    // Validate the complete request before applying the first step.
    for (size_t i = 0; i < local_remap_plan.size(); ++i) {
        const auto &step = local_remap_plan[i];
        bool listing = i + 1 == local_remap_plan.size() && local_remap_plan_list_only && step.candidate == -1;
        if (step.report.empty() || (step.candidate < 0 && !listing) || step.groups < 1 || step.groups > 8)
            log_error("Invalid local-remap plan step %zu.\n", i);
        std::string error;
        auto report = json11::Json::parse(step.report, error);
        if (!error.empty() || !report["critical_paths"].is_array())
            log_error("Invalid local-remap plan report at step %zu.\n", i);
    }
    const bool saved_pins = local_remap_optimize_pins, saved_placement = local_remap_preserve_ff_placement;
    try {
        for (size_t i = 0; i < local_remap_plan.size(); ++i) {
            const auto &step = local_remap_plan[i];
            std::string error;
            auto report = json11::Json::parse(step.report, error);
            log_info("Local-remap plan step %zu: candidate=%d groups=%d pins=%d preserve_ffs=%d.\n",
                     i, step.candidate, step.groups, int(step.optimize_pins), int(step.preserve_ff_placement));
            mistral_remap_report::validate(getCtx(), report, false);
            local_remap_optimize_pins = step.optimize_pins;
            local_remap_preserve_ff_placement = step.preserve_ff_placement;
            bool applied = remap_critical(step.report, step.candidate, step.groups);
            if (step.candidate >= 0 && !applied)
                log_error("Local-remap plan step %zu did not qualify; routing was not started.\n", i);
            if (step.candidate < 0) {
                local_remap_optimize_pins = saved_pins;
                local_remap_preserve_ff_placement = saved_placement;
                return false;
            }
        }
    } catch (...) {
        local_remap_optimize_pins = saved_pins;
        local_remap_preserve_ff_placement = saved_placement;
        // Successful earlier steps remain in this discarded failing context.
        // remap_critical restores the graph for its own unqualified probes.
        throw;
    }
    local_remap_optimize_pins = saved_pins;
    local_remap_preserve_ff_placement = saved_placement;
    return true;
}
NEXTPNR_NAMESPACE_END
