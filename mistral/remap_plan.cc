/* Explicit staged local-remap experiments. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "remap_report.h"
#include "lut_driver_copy.h"

NEXTPNR_NAMESPACE_BEGIN
void Arch::prevalidate_local_remap_plans()
{
    // Check both requests before an early stage can mutate the graph. Reports
    // are syntax-checked here; their live edges are checked at their own stage.
    auto validate = [&](const std::vector<LocalRemapStep> &request, bool listing_only, bool late) {
        const char *name = late ? "Local-remap post-plan" : "Local-remap plan";
        const char *lower = late ? "local-remap post-plan" : "local-remap plan";
        if (request.empty()) {
            if (listing_only) log_error("%s list mode must select the final listing step.\n", name);
            return;
        }
        if (request.size() > local_remap_max_steps)
            log_error("%ss require between one and %zu steps.\n", name, local_remap_max_steps);
        if (listing_only != (request.back().candidate == -1))
            log_error("%s list mode must select the final listing step.\n", name);
        for (size_t i = 0; i < request.size(); ++i) {
            const auto &step = request[i];
            bool listing = i + 1 == request.size() && listing_only && step.candidate == -1;
            if (step.report.empty() || (step.candidate < 0 && !listing) || step.groups < 1 || step.groups > 8)
                log_error("Invalid %s step %zu.\n", lower, i);
            std::string error;
            auto report = json11::Json::parse(step.report, error);
            if (!error.empty() || !report["critical_paths"].is_array())
                log_error("Invalid %s report at step %zu.\n", lower, i);
        }
    };
    validate(local_remap_plan, local_remap_plan_list_only, false);
    validate(local_remap_post_plan, local_remap_post_plan_list_only, true);
    if (!local_remap_post_plan.empty()) {
        if (local_remap_plan.size() > local_remap_max_steps - local_remap_post_plan.size())
            log_error("Local-remap early and post plans together require at most %zu steps.\n", local_remap_max_steps);
        prevalidate_local_remap_post_prefix(getCtx());
        if (local_remap_post_plan_list_only && !lut_driver_copy_report.empty())
            log_error("A post-plan listing must be final; it cannot precede LUT driver copy.\n");
    }
}

bool Arch::execute_local_remap_plan(bool post)
{
    const auto &steps = post ? local_remap_post_plan : local_remap_plan;
    const char *label = post ? "Local-remap post-plan" : "Local-remap plan";
    if (fes_any_slot_region_active) log_error("%ss require ordinary full-design placement.\n", label);
    if (steps.empty() || steps.size() > local_remap_max_steps)
        log_error("%ss require between one and %zu steps.\n", label, local_remap_max_steps);
    prevalidate_local_remap_plans();
    const bool saved_pins = local_remap_optimize_pins, saved_placement = local_remap_preserve_ff_placement;
    try {
        for (size_t i = 0; i < steps.size(); ++i) {
            const auto &step = steps[i];
            std::string error;
            auto report = json11::Json::parse(step.report, error);
            log_info("%s step %zu: candidate=%d groups=%d pins=%d preserve_ffs=%d.\n",
                     label, i, step.candidate, step.groups, int(step.optimize_pins), int(step.preserve_ff_placement));
            mistral_remap_report::validate(getCtx(), report, false);
            local_remap_optimize_pins = step.optimize_pins;
            local_remap_preserve_ff_placement = step.preserve_ff_placement;
            bool applied = remap_critical(step.report, step.candidate, step.groups);
            if (step.candidate >= 0 && !applied)
                log_error("%s step %zu did not qualify; routing was not started.\n", label, i);
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
