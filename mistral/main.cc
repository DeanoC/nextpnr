/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  gatecat <gatecat@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <cctype>
#include <limits>
#include <set>
#include "command.h"
#include "design_utils.h"
#include "jsonwrite.h"
#include "log.h"
#include "timing.h"
#include "enable_replication_policy.h"
#include "json11.hpp"
#include "lut_driver_copy.h"
#include "lut_pair_placement.h"
#include "critical_cohort_model.h"
#include "routed_placement.h"
#include "analogue_lut_branch.h"
#include <filesystem>

USING_NEXTPNR_NAMESPACE

namespace {
// json11 keeps the last duplicate key. Plans instead reject ambiguous options,
// including escaped spellings of the same key, after ordinary syntax validation.
void check_plan_keys(const std::string &text, const char *kind = "local-remap")
{
    std::vector<std::set<std::string>> objects;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') objects.emplace_back();
        else if (text[i] == '}') objects.pop_back();
        else if (text[i] == '"') {
            size_t start = i++;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\') ++i;
                ++i;
            }
            size_t next = i + 1;
            while (next < text.size() && std::isspace(static_cast<unsigned char>(text[next]))) ++next;
            if (next < text.size() && text[next] == ':') {
                std::string error;
                auto key = json11::Json::parse(text.substr(start, i - start + 1), error).string_value();
                if (objects.empty() || !objects.back().insert(key).second)
                    log_error("Duplicate %s plan key.\n", kind);
            }
        }
    }
}
}

class MistralCommandHandler : public CommandHandler
{
  public:
    MistralCommandHandler(int argc, char **argv);
    virtual ~MistralCommandHandler() {};
    std::unique_ptr<Context> createContext(dict<std::string, Property> &values) override;
    void setupArchContext(Context *ctx) override {};
    void customBitstream(Context *ctx) override;
    void customAfterLoad(Context *ctx) override;
    void customAfterRun(Context *ctx) override;

  protected:
    po::options_description getArchOptions() override;
    std::set<std::string> getArchSettingKeys() const override;
};

MistralCommandHandler::MistralCommandHandler(int argc, char **argv) : CommandHandler(argc, argv) {}

po::options_description MistralCommandHandler::getArchOptions()
{
    po::options_description specific("Architecture specific options");
    specific.add_options()("device", po::value<std::string>(), "device name (e.g. 5CSEBA6U23I7)");
    specific.add_options()("qsf", po::value<std::string>(), "path to QSF constraints file");
    specific.add_options()("rbf", po::value<std::string>(), "RBF bitstream to write");
    specific.add_options()("routed-placement-record", po::value<std::string>(),
                           "record native placement and final routed timing in a new directory (opt-in)");
    specific.add_options()("post-route-lut-branch-plan", po::value<std::string>(),
                           "trial a declared LUT move plus bounded branch repairs on its exact fresh parent (opt-in)");
    specific.add_options()("post-route-lut-moves", po::value<int>(),
                           "trial ordinary LUT relocations with full analogue endpoint guards (0..16, default 0)");
    specific.add_options()("post-route-private-lut-pins", po::value<int>(),
                           "trial private L5 input swaps after existing refinements (0..16, default 0)");
    specific.add_options()("post-route-net-trials", po::value<int>(),
                           "trial one data net at a time with complete analogue guards (0..64, default 0)");
    specific.add_options()("post-route-final-lut-pins", po::value<int>(),
                           "trial LUT6 input swaps after all routed refinements (0..16, default 0)");
    specific.add_options()("post-route-sole-lut-pins", po::value<int>(),
                           "trial L5 input swaps in otherwise empty ALMs after prior refinements (0..16, default 0)");
    specific.add_options()("post-route-lut-pairs", po::value<int>(),
                           "trial FF-free connected L5 co-packing after prior refinements (0..16, default 0)");
    specific.add_options()("post-route-lut-input-nets", po::value<int>(),
                           "trial LUT input swaps with both input net trees rebuilt (0..16, default 0)");
    specific.add_options()("post-route-static-ff-lut-pins", po::value<int>(),
                           "trial private L5 inputs beside independent static bypass FFs (0..16, default 0)");
    specific.add_options()("routed-placement-plan", po::value<std::string>(),
                           "apply a validated cumulative placement plan after fresh placement (opt-in)");
    specific.add_options()("routed-placement-propose", po::value<std::string>(),
                           "emit independent placements from a matching measured route model (requires --no-route)");
    specific.add_options()("routed-placement-trials", po::value<int>(), "proposal STA trials, 1..64 (explicit opt-in)");
    specific.add_options()("routed-placement-candidates", po::value<int>(), "proposal count, 1..4 (explicit opt-in)");
    specific.add_options()("routed-placement-preserve-routes", "record and preserve matching complete data trees (opt-in)");
    specific.add_options()("routed-placement-record-calibration", "record complete final routing calibration for exact replay (default off)");
    specific.add_options()("routed-placement-preserve-branches", "protect unchanged sink branches of group-move data trees (opt-in)");
    specific.add_options()("routed-placement-group-moves", "legal logic/register group candidates requiring full routing (opt-in)");
    specific.add_options()("routed-placement-retarget-luts", "search empty native sites for displaced LUTs of an explicit group plan (proposal only)");
    specific.add_options()("routed-placement-check-plan", "check group-plan placement legality only (requires --no-route)");
    specific.add_options()("routed-placement-routes", po::value<std::string>(), "validated immediate-parent data trees (opt-in)");
    specific.add_options()("compress-rbf", "generate compressed bitstream");
    specific.add_options()("mistral-ff-reset-reference",
                           "opt in to the bounded FF reset recovery reference; retains historical hold floor");
    specific.add_options()("mistral-ff-core-reference",
                           "opt in to guarded native-cut FF timing references for the slow100 model");
    specific.add_options()("mistral-gpio-core-reference",
                           "opt in to guarded native-cut SDR GPIO timing references for the slow100 model");
    specific.add_options()("mistral-lut-core-reference",
                           "opt in to guarded native-cut Boolean LUT timing references for the slow100 model");
    specific.add_options()("mistral-lut-route-min-reference",
                           "apply the characterized slow100 data minimum scale to native Boolean LUT input routes; requires LUT core reference and routed delay ranges");

    specific.add_options()("mistral-routed-delay-ranges",
                           "retain early/late routed delays for internal data and clocks without external IO constraints");
    specific.add_options()("mistral-clock-edge-ranges",
                           "select routed clock bounds for each register's actual edge; requires routed-delay-ranges");
    specific.add_options()("isolated-lut-placement",
                           "score the earlier L5 LUT inputs on their isolated legal pins during placement (opt-in)");
    specific.add_options()("arrival-pin-assignment",
                           "permute private LUT inputs by setup-relative arrival after legalisation (opt-in)");
    specific.add_options()("fes-scaffold", "lock loaded shell BEL+routing to STRENGTH_USER");
    specific.add_options()("fes-cart", po::value<std::string>(), "merge unbound cart JSON into the reserved socket");
    specific.add_options()("fes-cart-region", po::value<std::string>(),
                            "name of the FES_RESERVED_RECT region this --fes-cart targets (default: cart)");
    specific.add_options()("fes-slot-clock", po::value<std::string>(), "exact shell clock net for FES cart cells");
    specific.add_options()("fes-cram-region", po::value<std::string>(), "half-open CRAM x0,y0,x1,y1 region for new scaffold routing");
    specific.add_options()("mistral-ff4",
                           "also place flip-flops on the two secondary registers of each ALM (opt-in, see "
                           "mistral/tests/lab_ff4)");
    specific.add_options()("mistral-clkb",
                           "allow two clocks, or both edges of one clock, in a LAB using its CLKB source and per-pair "
                           "clock inversion (opt-in, see mistral/tests/lab_ff4)");

    specific.add_options()("mistral-dsp-signed-lanes",
                           "pack signed 9x9 multipliers three per DSP block using the block sign bits only "
                           "(hardware-unverified; default mirrors Quartus and gives each signed 9x9 its own block)");
    specific.add_options()("replicate-enables", po::value<int>(),
                           "replicate timing-critical LUT enables after placement (budget 0..8, default 0)");
    specific.add_options()("critical-cohort-budget", po::value<int>(),
                           "repair failing setup paths by relocating data neighborhoods (STA trials 0..64, default 0)");
    specific.add_options()("critical-cohort-model-out", po::value<std::string>(),
                           "write final analogue data-arc calibration for the baseline placement (repair budget 0)");
    specific.add_options()("critical-cohort-report", po::value<std::string>(),
                           "guide critical-cohort repair with a prior final analogue timing report");
    specific.add_options()("remap-critical", po::value<std::string>(), "prior routed timing report for local LUT remapping");
    specific.add_options()("remap-optimize-pins", "optimize local-remap LUT input order for predicted delay");
    specific.add_options()("remap-preserve-ffs", "keep every original FF placement during local remapping");
    specific.add_options()("remap-plan", po::value<std::string>(), "JSON plan of staged report-guided local remaps (1..16 steps)");
    specific.add_options()("remap-post-plan", po::value<std::string>(),
                           "JSON local-remap plan after internal cuts and decomposition (combined early/post limit 16)");
    specific.add_options()("remap-comb-critical", po::value<std::string>(), "prior routed timing report for bounded internal LUT cut remapping");
    specific.add_options()("remap-comb-candidate", po::value<int>(), "qualified internal-cut candidate index (default: list only)");
    specific.add_options()("remap-comb-plan", po::value<std::string>(), "JSON plan of staged internal LUT-cut remaps (1..8 steps)");
    specific.add_options()("remap-decompose-critical", po::value<std::string>(), "prior timing report for bounded seven-input control decomposition");
    specific.add_options()("remap-decompose-candidate", po::value<int>(), "qualified decomposition candidate index (default: list only)");
    specific.add_options()("remap-lut-pair-critical", po::value<std::string>(), "prior timing report for joint placement of two consecutive LUTs");
    specific.add_options()("remap-lut-pair-candidate", po::value<int>(), "qualified LUT-pair-placement candidate index (default: list only)");
    specific.add_options()("remap-lut-pair-compose-copy", "compose a LUT pair into one isolated copy for a whole same-LAB enable cohort");
    specific.add_options()("remap-lut-driver-critical", po::value<std::string>(), "prior timing report for one LUT driver copy to an arithmetic data input");
    specific.add_options()("remap-lut-driver-candidate", po::value<int>(), "qualified LUT-driver-copy candidate index (default: list only)");
    specific.add_options()("remap-candidate", po::value<int>(), "qualified local-remap candidate index (default: list only)");
    specific.add_options()("remap-groups", po::value<int>(), "maximum whole LAB enable groups per remap (1..8, default 1)");
    specific.add_options()("balance-reduction-root", po::value<std::vector<std::string>>()->composing(),
                           "pre-placement 3-LUT/11-input, 4-LUT/16-input or 7-LUT/24-input conjunction root cell (repeatable, opt-in)");
    return specific;
}

std::set<std::string> MistralCommandHandler::getArchSettingKeys() const
{
    std::set<std::string> keys;
    if (vm.count("device"))
        keys.insert("arch.type");
    if (vm.count("compress-rbf"))
        keys.insert("compress_rbf");
    if (vm.count("isolated-lut-placement"))
        keys.insert("mistral/isolated_lut_placement");
    if (vm.count("arrival-pin-assignment"))
        keys.insert("mistral/arrival_pin_assignment");
    if (vm.count("mistral-ff-reset-reference"))
        keys.insert("mistral/ff_reset_reference");
    if (vm.count("mistral-ff-core-reference"))
        keys.insert("mistral/ff_core_reference");
    if (vm.count("mistral-gpio-core-reference"))
        keys.insert("mistral/gpio_core_reference");
    if (vm.count("mistral-lut-core-reference"))
        keys.insert("mistral/lut_core_reference");
    if (vm.count("mistral-lut-route-min-reference"))
        keys.insert("mistral/lut_route_min_reference");

    if (vm.count("mistral-routed-delay-ranges"))
        keys.insert("mistral/routed_delay_ranges");
    if (vm.count("mistral-clock-edge-ranges"))
        keys.insert("mistral/clock_edge_ranges");
    if (vm.count("mistral-ff4"))
        keys.insert("mistral/ff4");
    if (vm.count("mistral-clkb"))
        keys.insert("mistral/clkb");
    if (vm.count("fes-slot-clock"))
        keys.insert("fes/slot_clock");
    return keys;
}

void MistralCommandHandler::customBitstream(Context *ctx)
{
    if (vm.count("rbf")) {
        std::string filename = vm["rbf"].as<std::string>();
        ctx->fes_validate_cram_routing();
        ctx->build_bitstream();
        std::vector<uint8_t> data;
        ctx->cyclonev->rbf_save(data);

        std::ofstream out(filename, std::ios::binary);
        if (!out.is_open()) {
            log_error("Failed to open RBF file '%s' for writing: %s.\n", filename.c_str(),
                      std::error_code(errno, std::generic_category()).message().c_str());
        }
        out.write(reinterpret_cast<const char *>(data.data()), data.size());
        if (!ctx->routed_placement_directory.empty()) {
            out.flush();
            if (!out)
                log_error("Failed to complete routed placement RBF output.\n");
            std::string error;
            auto status = json11::Json::parse(ctx->routed_placement_status, error).object_items();
            NPNR_ASSERT(error.empty() && !status.empty());
            status["rbf_bytes"] = double(data.size());
            ctx->routed_placement_status = json11::Json(status).dump();
        }
        if (vm.count("critical-cohort-model-out")) {
            std::ofstream model(vm["critical-cohort-model-out"].as<std::string>());
            if (!model.is_open())
                log_error("Failed to open critical cohort route model output.\n");
            write_critical_cohort_route_model(ctx, model);
        }
    } else if (vm.count("report") && vm.count("no-route") && ctx->attrs.count(id_step) &&
               ctx->attrs.at(id_step).as_string() == "place") {
        // Placement and optional remap probes keep their STA results local.
        // An explicit report needs fresh results for the final placed graph.
        log_info("Running predicted placed timing analysis for --report (no routing or analogue signoff).\n");
        timing_analysis(ctx, false, true, false, false, true);
    } else if (vm.count("report") && ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() == "route") {
        // GPU analogue repair can change the route after the router's timing
        // check. Refresh the report for the graph we are about to write.
        log_info("Running final routed timing analysis for --report (no bitstream signoff).\n");
        timing_analysis(ctx, false, true, false, false, true);
    }
}

void MistralCommandHandler::customAfterRun(Context *ctx)
{
    if (ctx->routed_placement_directory.empty())
        return;
    if (!ctx->routed_placement_model.empty() || ctx->routed_placement_check_only)
        return; // Proposal and legality workers make no routing/completion claim.
    std::string error;
    auto completion = json11::Json::parse(ctx->routed_placement_status, error).object_items();
    if (!error.empty() || completion.empty())
        log_error("Routed placement run has no final native observation.\n");
    completion["process_complete"] = true;
    completion["errors_before_signoff"] = ctx->routed_placement_errors_before_signoff;
    completion["errors_after_signoff"] = ctx->routed_placement_errors_after_signoff;
    completion["errors_at_completion"] = message_count_by_level[LogLevel::ERROR_MSG];
    auto path = std::filesystem::path(ctx->routed_placement_directory) / "completion.json";
    if (std::filesystem::exists(path))
        log_error("Refusing to overwrite routed placement completion record.\n");
    std::ofstream out(path);
    out << json11::Json(completion).dump() << '\n';
    if (!out)
        log_error("Failed to write routed placement completion record.\n");
}

std::unique_ptr<Context> MistralCommandHandler::createContext(dict<std::string, Property> &values)
{
    ArchArgs chipArgs;
    if (!vm.count("device")) {
        log_error("device must be specified on the command line (e.g. --device 5CSEBA6U23I7)\n");
    }
    chipArgs.device = vm["device"].as<std::string>();
    // The second LAB clock adds pips to the routing graph, so it is fixed before the graph is built.
    chipArgs.lab_clkb = vm.count("mistral-clkb") != 0;
    chipArgs.dsp_signed_lanes = vm.count("mistral-dsp-signed-lanes") != 0;
    auto ctx = std::unique_ptr<Context>(new Context(chipArgs));
    if (vm.count("compress-rbf"))
        ctx->settings[id_compress_rbf] = Property::State::S1;
    ctx->isolated_lut_placement = vm.count("isolated-lut-placement") != 0;
    if (ctx->isolated_lut_placement)
        ctx->settings[ctx->id("mistral/isolated_lut_placement")] = Property::State::S1;
    ctx->arrival_pin_assignment = vm.count("arrival-pin-assignment") != 0;
    if (ctx->arrival_pin_assignment)
        ctx->settings[ctx->id("mistral/arrival_pin_assignment")] = Property::State::S1;
    ctx->signoff_after_route = vm.count("rbf") > 0;
    return ctx;
}

void MistralCommandHandler::customAfterLoad(Context *ctx)
{
    ctx->restore_fes_reservations();
    ctx->restore_place_regions(vm.count("qsf") != 0);
    // Saved provenance does not enable fresh placement heuristics. A routed
    // checkpoint already records the physical pins and needs no option to replay.
    auto pin_policy = [&](const char *name, bool enabled) {
        IdString recorded;
        for (const auto &setting : ctx->settings)
            if (setting.first.str(ctx) == name) {
                recorded = setting.first;
                break;
            }
        if (recorded != IdString())
            ctx->settings.erase(recorded);
        if (enabled)
            ctx->settings[ctx->id(name)] = Property::State::S1;
    };
    pin_policy("mistral/isolated_lut_placement", ctx->isolated_lut_placement);
    pin_policy("mistral/arrival_pin_assignment", ctx->arrival_pin_assignment);
    // Packing and timing reference models require an explicit command-line opt-in on every load. A JSON records it, and is refused
    // without the same option: its placement (and, for CLKB, its routing) relies on that model.
    auto lab_model = [&](const char *setting, const char *option) {
        const bool requested = vm.count(option) != 0;
        auto found = ctx->settings.find(ctx->id(setting));
        bool recorded = false;
        if (found != ctx->settings.end())
            recorded = found->second.is_string ? found->second.as_string() == "1" : found->second.as_bool();
        if (recorded && !requested)
            log_error("This design was packed or placed with --%s; pass --%s again.\n", option, option);
        if (requested)
            ctx->settings[ctx->id(setting)] = Property::State::S1;
        else if (found != ctx->settings.end())
            ctx->settings.erase(found);
        return requested;
    };
    lab_model("mistral/ff_reset_reference", "mistral-ff-reset-reference");
    lab_model("mistral/ff_core_reference", "mistral-ff-core-reference");
    lab_model("mistral/gpio_core_reference", "mistral-gpio-core-reference");
    // Avoid interning a new ID on the ordinary path: untouched modes retain
    // their placement identities and their default timing behavior.
    bool saved_lut_reference = false;
    for (const auto &entry : ctx->settings)
        saved_lut_reference |= entry.first.str(ctx) == "mistral/lut_core_reference";
    if (saved_lut_reference || vm.count("mistral-lut-core-reference"))
        ctx->use_lut_core_reference = lab_model("mistral/lut_core_reference", "mistral-lut-core-reference");
    bool saved_lut_route_min_reference = false;
    for (const auto &entry : ctx->settings)
        saved_lut_route_min_reference |= entry.first.str(ctx) == "mistral/lut_route_min_reference";
    if (saved_lut_route_min_reference || vm.count("mistral-lut-route-min-reference"))
        ctx->use_lut_route_min_reference = lab_model("mistral/lut_route_min_reference", "mistral-lut-route-min-reference");
    lab_model("mistral/routed_delay_ranges", "mistral-routed-delay-ranges");
    lab_model("mistral/clock_edge_ranges", "mistral-clock-edge-ranges");
    ctx->useClockEdgeRanges(); // Validate the explicit range-model dependency before packing.
    if (ctx->use_lut_route_min_reference)
        ctx->validate_lut_route_min_reference();

    ctx->lab_ff4 = lab_model("mistral/ff4", "mistral-ff4");
    lab_model("mistral/clkb", "mistral-clkb");
    // Preload request bytes before packing or placement; saved settings cannot enable it.
    const char *capture_pipeline = std::getenv("NEXTPNR_MISTRAL_CAPTURE_PIPELINE_LOCALITY");
    if (capture_pipeline) {
        if (vm.count("no-pack") || vm.count("no-place") || vm.count("pack-only") ||
            vm.count("fes-cart") || vm.count("fes-scaffold") ||
            (vm.count("placer") && vm["placer"].as<std::string>() != "heap") ||
            (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != ""))
            log_error("Capture pipeline locality requires fresh ordinary HeAP placement.\n");
        if (std::getenv("NEXTPNR_MISTRAL_CAPTURE_LOCALITY"))
            log_error("Capture pipeline locality cannot combine with single-capture locality.\n");
    }
    preload_capture_pipeline_locality(ctx, capture_pipeline);
    if (vm.count("critical-cohort-model-out") &&
        (!vm.count("rbf") || vm.count("no-route") || vm.count("no-pack") || vm.count("no-place") ||
         vm.count("pack-only") || vm.count("fes-cart") || vm.count("fes-scaffold") ||
         (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != "")))
        log_error("Critical cohort route model export requires a fresh ordinary full route with --rbf.\n");
    ctx->critical_cohort_budget = 0;
    ctx->critical_cohort_report.clear();
    if (vm.count("critical-cohort-budget")) {
        int budget = vm["critical-cohort-budget"].as<int>();
        if (budget < 0 || budget > 64)
            log_error("--critical-cohort-budget must be between 0 and 64.\n");
        if (budget && (vm.count("no-pack") || vm.count("no-place") || vm.count("pack-only") ||
                       vm.count("fes-cart") || vm.count("fes-scaffold") ||
                       (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != "")))
            log_error("Critical cohort repair requires fresh ordinary HeAP placement.\n");
        ctx->critical_cohort_budget = budget;
    }
    if (vm.count("critical-cohort-model-out") && ctx->critical_cohort_budget)
        log_error("Critical cohort route model export requires --critical-cohort-budget 0 (unrepaired baseline).\n");
    if (vm.count("critical-cohort-report")) {
        if (!ctx->critical_cohort_budget)
            log_error("--critical-cohort-report requires a positive --critical-cohort-budget.\n");
        auto in = open_ifstream_and_log_error(vm["critical-cohort-report"].as<std::string>(), "critical cohort report");
        ctx->critical_cohort_report.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        std::string error;
        auto report = json11::Json::parse(ctx->critical_cohort_report, error);
        if (!error.empty() || !report["critical_paths"].is_array() ||
            !report["timing_summary"]["final_analogue_model"].bool_value())
            log_error("Critical cohort guidance requires a final analogue timing report.\n");
    }
    const bool propose_placement = vm.count("routed-placement-propose") != 0;
    const bool check_placement = vm.count("routed-placement-check-plan") != 0;
    ctx->post_route_lut_branch_plan.clear();
    ctx->post_route_lut_branch_receipt.clear();
    if (vm.count("post-route-lut-branch-plan")) {
        if (!vm.count("routed-placement-record") || vm.count("no-route") || propose_placement || check_placement ||
            vm.count("routed-placement-plan") || vm.count("routed-placement-group-moves") ||
            vm.count("routed-placement-preserve-routes") || vm.count("routed-placement-preserve-branches"))
            log_error("Compound LUT branch plans require a fresh full recorded route without placement plans or preserved-route modes.\n");
        auto in = open_ifstream_and_log_error(vm["post-route-lut-branch-plan"].as<std::string>(), "compound LUT branch plan");
        ctx->post_route_lut_branch_plan.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        std::string error;
        const auto plan = json11::Json::parse(ctx->post_route_lut_branch_plan, error);
        if (!error.empty() || !plan.is_object())
            log_error("Invalid compound LUT branch JSON document.\n");
        check_plan_keys(ctx->post_route_lut_branch_plan, "compound LUT branch");
        lut_branch_detail::validate_plan(plan);
    }
    ctx->post_route_lut_moves = 0;
    ctx->post_route_lut_move_receipt.clear();
    if (vm.count("post-route-lut-moves")) {
        const int budget = vm["post-route-lut-moves"].as<int>();
        if (budget < 0 || budget > 16)
            log_error("--post-route-lut-moves must be between 0 and 16.\n");
        if (budget && (!vm.count("routed-placement-record") || vm.count("no-route") ||
            propose_placement || check_placement || vm.count("routed-placement-plan") ||
            vm.count("routed-placement-group-moves") || vm.count("routed-placement-preserve-routes") ||
            vm.count("routed-placement-preserve-branches")))
            log_error("Post-route LUT moves require a fresh full recorded route without placement plans or preserved-route modes.\n");
        ctx->post_route_lut_moves = budget;
    }
    ctx->post_route_private_lut_pins = 0;
    if (vm.count("post-route-private-lut-pins")) {
        const int budget = vm["post-route-private-lut-pins"].as<int>();
        if (budget < 0 || budget > 16)
            log_error("--post-route-private-lut-pins must be between 0 and 16.\n");
        if (budget && (!vm.count("routed-placement-record") || vm.count("no-route") ||
            propose_placement || check_placement || vm.count("routed-placement-plan") ||
            vm.count("routed-placement-group-moves") || vm.count("routed-placement-preserve-routes") ||
            vm.count("routed-placement-preserve-branches")))
            log_error("Private LUT input trials require a fresh full recorded route without placement plans or preserved-route modes.\n");
        ctx->post_route_private_lut_pins = budget;
    }
    ctx->post_route_net_trials = 0;
    if (vm.count("post-route-net-trials")) {
        const int budget = vm["post-route-net-trials"].as<int>();
        if (budget < 0 || budget > 64)
            log_error("--post-route-net-trials must be between 0 and 64.\n");
        if (budget && (!vm.count("routed-placement-record") || vm.count("no-route") ||
            propose_placement || check_placement || vm.count("routed-placement-plan") ||
            vm.count("routed-placement-group-moves") || vm.count("routed-placement-preserve-routes") ||
            vm.count("routed-placement-preserve-branches")))
            log_error("Individual net trials require a fresh full recorded route without placement plans or preserved-route modes.\n");
        ctx->post_route_net_trials = budget;
    }
    ctx->post_route_final_lut_pins = 0;
    if (vm.count("post-route-final-lut-pins")) {
        const int budget = vm["post-route-final-lut-pins"].as<int>();
        if (budget < 0 || budget > 16)
            log_error("--post-route-final-lut-pins must be between 0 and 16.\n");
        if (budget && (!vm.count("routed-placement-record") || vm.count("no-route") ||
            propose_placement || check_placement || vm.count("routed-placement-plan") ||
            vm.count("routed-placement-group-moves") || vm.count("routed-placement-preserve-routes") ||
            vm.count("routed-placement-preserve-branches")))
            log_error("Final LUT input trials require a fresh full recorded route without placement plans or preserved-route modes.\n");
        ctx->post_route_final_lut_pins = budget;
    }
    ctx->post_route_sole_lut_pins = 0;
    if (vm.count("post-route-sole-lut-pins")) {
        const int budget = vm["post-route-sole-lut-pins"].as<int>();
        if (budget < 0 || budget > 16)
            log_error("--post-route-sole-lut-pins must be between 0 and 16.\n");
        if (budget && (!vm.count("routed-placement-record") || vm.count("no-route") ||
            propose_placement || check_placement || vm.count("routed-placement-plan") ||
            vm.count("routed-placement-group-moves") || vm.count("routed-placement-preserve-routes") ||
            vm.count("routed-placement-preserve-branches")))
            log_error("Sole L5 input trials require a fresh full recorded route without placement plans or preserved-route modes.\n");
        ctx->post_route_sole_lut_pins = budget;
    }
    ctx->post_route_lut_pairs = 0;
    if (vm.count("post-route-lut-pairs")) {
        const int budget = vm["post-route-lut-pairs"].as<int>();
        if (budget < 0 || budget > 16)
            log_error("--post-route-lut-pairs must be between 0 and 16.\n");
        if (budget && (!vm.count("routed-placement-record") || vm.count("no-route") ||
            propose_placement || check_placement || vm.count("routed-placement-plan") ||
            vm.count("routed-placement-group-moves") || vm.count("routed-placement-preserve-routes") ||
            vm.count("routed-placement-preserve-branches")))
            log_error("LUT pair trials require a fresh full recorded route without placement plans or preserved-route modes.\n");
        ctx->post_route_lut_pairs = budget;
    }
    ctx->post_route_lut_input_nets = 0;
    if (vm.count("post-route-lut-input-nets")) {
        const int budget = vm["post-route-lut-input-nets"].as<int>();
        if (budget < 0 || budget > 16)
            log_error("--post-route-lut-input-nets must be between 0 and 16.\n");
        if (budget && (!vm.count("routed-placement-record") || vm.count("no-route") ||
            propose_placement || check_placement || vm.count("routed-placement-plan") ||
            vm.count("routed-placement-group-moves") || vm.count("routed-placement-preserve-routes") ||
            vm.count("routed-placement-preserve-branches")))
            log_error("Whole input net trials require a fresh full recorded route without placement plans or preserved-route modes.\n");
        ctx->post_route_lut_input_nets = budget;
    }
    ctx->post_route_static_ff_lut_pins = 0;
    if (vm.count("post-route-static-ff-lut-pins")) {
        const int budget = vm["post-route-static-ff-lut-pins"].as<int>();
        if (budget < 0 || budget > 16)
            log_error("--post-route-static-ff-lut-pins must be between 0 and 16.\n");
        if (budget && (!vm.count("routed-placement-record") || vm.count("no-route") ||
            propose_placement || check_placement || vm.count("routed-placement-plan") ||
            vm.count("routed-placement-group-moves") || vm.count("routed-placement-preserve-routes") ||
            vm.count("routed-placement-preserve-branches")))
            log_error("Static FF L5 input trials require a fresh full recorded route without placement plans or preserved-route modes.\n");
        ctx->post_route_static_ff_lut_pins = budget;
    }
    ctx->routed_placement_record_calibration = vm.count("routed-placement-record-calibration") != 0;
    if (ctx->routed_placement_record_calibration && (propose_placement || check_placement || vm.count("no-route")))
        log_error("Calibration recording requires a fresh full recorded route.\n");
    if ((propose_placement || vm.count("routed-placement-plan") || vm.count("routed-placement-trials") ||
         vm.count("routed-placement-candidates") || vm.count("routed-placement-preserve-routes") ||
         vm.count("routed-placement-routes") || vm.count("routed-placement-group-moves") ||
         vm.count("routed-placement-preserve-branches") || vm.count("routed-placement-retarget-luts") ||
         check_placement || ctx->routed_placement_record_calibration) && !vm.count("routed-placement-record"))
        log_error("Routed placement controls require --routed-placement-record.\n");
    if (vm.count("routed-placement-record")) {
        auto validate_document = [](const std::string &document) {
            std::string error;
            auto parsed = json11::Json::parse(document, error);
            if (!error.empty() || !parsed.is_object())
                log_error("Invalid routed placement JSON document.\n");
            check_plan_keys(document, "routed placement");
        };
        if (vm.count("no-pack") || vm.count("no-place") || vm.count("pack-only") ||
            vm.count("run") || vm.count("pre-pack") || vm.count("pre-place") || vm.count("pre-route") ||
            vm.count("post-route") || vm.count("fes-cart") || vm.count("fes-scaffold") || ctx->critical_cohort_budget ||
            (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != "") ||
            ctx->setting<std::string>("placer") != "heap" || ctx->setting<std::string>("router") != "gpu" ||
            !ctx->setting<bool>("timing_driven"))
            log_error("Routed placement recording requires fresh ordinary HeAP/GPU flow and critical-cohort budget 0.\n");
        if (check_placement && (propose_placement || !vm.count("routed-placement-plan") ||
            !vm.count("routed-placement-group-moves") || !vm.count("no-route") || vm.count("rbf") ||
            vm.count("write") || vm.count("report") || vm.count("routed-placement-routes") ||
            vm.count("routed-placement-trials") || vm.count("routed-placement-candidates")))
            log_error("Plan checking requires group mode, a plan and --no-route, without timing outputs or proposal controls.\n");
        if ((propose_placement || check_placement) ? (!vm.count("no-route") || vm.count("rbf")) :
                               (vm.count("no-route") || !vm.count("rbf") || !vm.count("report") || !vm.count("write")))
            log_error("Routed placement requires --no-route for proposals, or --rbf/--report/--write for full routes.\n");
        if (ctx->force || vm.count("force") || vm.count("timing-allow-fail"))
            log_error("Routed placement recording rejects force and timing-failure overrides.\n");
        for (const auto &setting : ctx->settings) {
            const auto &key = setting.first.str(ctx);
            if (key != "timing/allowFail" && key != "timing/ignoreLoops" && key != "timing/ignoreRelClk")
                continue;
            const auto &value = setting.second;
            if (value.is_string ? (value.as_string() != "0" && value.as_string() != "false") : value.as_bool())
                log_error("Routed placement recording rejects saved timing override '%s'.\n", key.c_str());
        }
        if (vm.count("routed-placement-plan")) {
            auto in = open_ifstream_and_log_error(vm["routed-placement-plan"].as<std::string>(), "routed placement plan");
            ctx->routed_placement_plan.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            validate_document(ctx->routed_placement_plan);
        }
        ctx->routed_placement_preserve_routes = vm.count("routed-placement-preserve-routes") != 0;
        ctx->routed_placement_group_moves = vm.count("routed-placement-group-moves") != 0;
        ctx->routed_placement_retarget_luts = vm.count("routed-placement-retarget-luts") != 0;
        if (ctx->routed_placement_retarget_luts &&
            (!propose_placement || !ctx->routed_placement_group_moves || !vm.count("routed-placement-plan")))
            log_error("LUT retargeting requires proposal mode, group mode and an explicit group plan.\n");
        ctx->routed_placement_preserve_branches = vm.count("routed-placement-preserve-branches") != 0;
        if (ctx->routed_placement_preserve_branches &&
            (!ctx->routed_placement_preserve_routes || !ctx->routed_placement_group_moves))
            log_error("Branch preservation requires explicit preserve-routes and group mode.\n");
        ctx->routed_placement_check_only = check_placement;
        if (vm.count("routed-placement-routes")) {
            if (!ctx->routed_placement_preserve_routes || propose_placement || !vm.count("routed-placement-plan"))
                log_error("Routed placement routes require preserve-routes, a plan, and a full route.\n");
            auto in = open_ifstream_and_log_error(vm["routed-placement-routes"].as<std::string>(), "routed placement routes");
            ctx->routed_placement_routes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            validate_document(ctx->routed_placement_routes);
        }
        if (propose_placement) {
            if (!vm.count("routed-placement-trials") || !vm.count("routed-placement-candidates"))
                log_error("Routed placement proposals require explicit trial and candidate limits.\n");
            ctx->routed_placement_trials = vm["routed-placement-trials"].as<int>();
            ctx->routed_placement_candidates = vm["routed-placement-candidates"].as<int>();
            if (ctx->routed_placement_trials < 1 || ctx->routed_placement_trials > 64 ||
                ctx->routed_placement_candidates < 1 || ctx->routed_placement_candidates > 4)
                log_error("Routed placement limits must be trials 1..64 and candidates 1..4.\n");
            auto in = open_ifstream_and_log_error(vm["routed-placement-propose"].as<std::string>(), "routed placement model");
            ctx->routed_placement_model.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            validate_document(ctx->routed_placement_model);
        } else if (vm.count("routed-placement-trials") || vm.count("routed-placement-candidates")) {
            log_error("Routed placement trial/candidate limits require proposal mode.\n");
        }
        ctx->routed_placement_directory = vm["routed-placement-record"].as<std::string>();
        if (ctx->routed_placement_directory.empty() || std::filesystem::exists(ctx->routed_placement_directory))
            log_error("Routed placement recording requires a new output directory.\n");
        std::filesystem::create_directories(ctx->routed_placement_directory);
    }
    if (vm.count("balance-reduction-root")) {
        if (vm.count("fes-cart") || vm.count("fes-scaffold") ||
            (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != ""))
            log_error("Reduction balancing requires an unpacked, ordinary full design.\n");
        for (const auto &root : vm["balance-reduction-root"].as<std::vector<std::string>>())
            if (!ctx->balance_reduction(root))
                log_error("Selected reduction root is not a safe four-LUT/sixteen-input, three-LUT/eleven-input or seven-LUT/twenty-four-input conjunction.\n");
    }
    // JSON provenance never enables a pass, including an explicit zero budget.
    // Avoid interning a new identifier before placement when no old key exists.
    ctx->enable_replication_budget = 0;
    IdString stale_replication_setting;
    for (const auto &setting : ctx->settings)
        if (setting.first.str(ctx) == "mistral/replicateEnables") { stale_replication_setting = setting.first; break; }
    if (stale_replication_setting != IdString()) ctx->settings.erase(stale_replication_setting);
    if (vm.count("replicate-enables")) {
        int budget = vm["replicate-enables"].as<int>();
        if (!enable_replication_policy::valid_budget(budget))
            log_error("--replicate-enables must be between 0 and 8.\n");
        ctx->enable_replication_budget = budget;
    }
    ctx->local_remap_report.clear();
    ctx->local_remap_selection = -1;
    ctx->local_remap_groups = 1;
    ctx->local_remap_optimize_pins = false;
    ctx->local_remap_preserve_ff_placement = false;
    ctx->local_remap_plan.clear();
    ctx->local_remap_plan_list_only = false;
    ctx->local_remap_post_plan.clear();
    ctx->local_remap_post_plan_list_only = false;
    if (vm.count("remap-plan") && (vm.count("remap-critical") || vm.count("remap-candidate") ||
        vm.count("remap-groups") || vm.count("remap-optimize-pins") || vm.count("remap-preserve-ffs")))
        log_error("--remap-plan cannot be combined with legacy local-remap options.\n");
    if ((vm.count("remap-candidate") || vm.count("remap-groups") || vm.count("remap-optimize-pins") ||
        vm.count("remap-preserve-ffs")) && !vm.count("remap-critical"))
        log_error("Local-remap options require --remap-critical.\n");
    if (vm.count("remap-critical")) {
        if (vm.count("no-place") || vm.count("pack-only") || vm.count("fes-cart") || vm.count("fes-scaffold") ||
            (vm.count("placer") && vm["placer"].as<std::string>() != "heap"))
            log_error("Local remap requires fresh ordinary HeAP placement.\n");
        auto in = open_ifstream_and_log_error(vm["remap-critical"].as<std::string>(), "local-remap timing report");
        ctx->local_remap_report.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (ctx->local_remap_report.empty()) log_error("Empty local-remap timing report.\n");
        if (vm.count("remap-candidate")) ctx->local_remap_selection = vm["remap-candidate"].as<int>();
        if (ctx->local_remap_selection < -1) log_error("Invalid local-remap candidate index.\n");
        if (vm.count("remap-groups")) ctx->local_remap_groups = vm["remap-groups"].as<int>();
        ctx->local_remap_optimize_pins = vm.count("remap-optimize-pins") != 0;
        ctx->local_remap_preserve_ff_placement = vm.count("remap-preserve-ffs") != 0;
        if (ctx->local_remap_groups < 1 || ctx->local_remap_groups > 8)
            log_error("--remap-groups must be between 1 and 8.\n");
    }
    auto load_local_plan = [&](const char *option, bool post) {
        const char *kind = post ? "post-remap" : "local-remap";
        auto &steps = post ? ctx->local_remap_post_plan : ctx->local_remap_plan;
        auto &list_only = post ? ctx->local_remap_post_plan_list_only : ctx->local_remap_plan_list_only;
        if (vm.count("no-pack") || vm.count("no-place") || vm.count("pack-only") || vm.count("fes-cart") ||
            vm.count("fes-scaffold") || (vm.count("placer") && vm["placer"].as<std::string>() != "heap") ||
            (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != ""))
            log_error(post ? "Post-remap plans require fresh ordinary HeAP placement.\n" :
                             "Local-remap plans require fresh ordinary HeAP placement.\n");
        auto filename = vm[option].as<std::string>();
        auto in = open_ifstream_and_log_error(filename, post ? "post-remap plan" : "local-remap plan");
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}, error;
        auto plan = json11::Json::parse(text, error);
        if (!error.empty() || !plan.is_object() || plan.object_items().size() != 1 || !plan["steps"].is_array() ||
            plan["steps"].array_items().empty() || plan["steps"].array_items().size() > Arch::local_remap_max_steps)
            log_error("Invalid %s plan; expected one to %zu steps.\n", kind, Arch::local_remap_max_steps);
        check_plan_keys(text, kind);
        if (post && ctx->local_remap_plan.size() + plan["steps"].array_items().size() > Arch::local_remap_max_steps)
            log_error("Combined early and post-remap plans exceed %zu steps.\n", Arch::local_remap_max_steps);
        auto integer = [&](const json11::Json &value, int low, int high) {
            double number = value.number_value();
            if (!value.is_number() || !std::isfinite(number) || number != std::floor(number) || number < low || number > high)
                log_error("Invalid integer in %s plan.\n", kind);
            return int(number);
        };
        const std::set<std::string> keys = {"report", "candidate", "groups", "optimize_pins", "preserve_ff_placement"};
        for (const auto &entry : plan["steps"].array_items()) {
            if (!entry.is_object() || entry.object_items().size() != keys.size())
                log_error("Invalid %s plan step fields.\n", kind);
            for (const auto &field : entry.object_items()) if (!keys.count(field.first))
                log_error("Unknown %s plan step field.\n", kind);
            if (!entry["report"].is_string() || entry["report"].string_value().empty() ||
                entry["report"].string_value().find('\0') != std::string::npos ||
                !entry["optimize_pins"].is_bool() || !entry["preserve_ff_placement"].is_bool())
                log_error("Invalid %s plan path or boolean.\n", kind);
            Arch::LocalRemapStep step;
            step.candidate = integer(entry["candidate"], -1, std::numeric_limits<int>::max());
            step.groups = integer(entry["groups"], 1, 8);
            step.optimize_pins = entry["optimize_pins"].bool_value();
            step.preserve_ff_placement = entry["preserve_ff_placement"].bool_value();
            if (step.candidate == -1) {
                if (&entry != &plan["steps"].array_items().back() || !vm.count("no-route") || vm.count("rbf"))
                    log_error(post ? "A post-plan listing step must be last, with --no-route and without --rbf.\n" :
                                     "A plan listing step must be last, with --no-route and without --rbf.\n");
                list_only = true;
            }
            std::filesystem::path path(entry["report"].string_value());
            if (path.is_relative()) path = std::filesystem::path(filename).parent_path() / path;
            auto report_in = open_ifstream_and_log_error(path.lexically_normal().string(), post ? "post-remap plan report" : "local-remap plan report");
            step.report.assign(std::istreambuf_iterator<char>(report_in), std::istreambuf_iterator<char>());
            auto report = json11::Json::parse(step.report, error);
            if (!error.empty() || !report["critical_paths"].is_array())
                log_error("Invalid %s plan report.\n", kind);
            steps.push_back(std::move(step));
        }
    };
    if (vm.count("remap-plan")) load_local_plan("remap-plan", false);
    ctx->comb_remap_report.clear();
    ctx->comb_remap_selection = -1;
    ctx->comb_remap_plan.clear();
    ctx->comb_remap_plan_list_only = false;
    if (vm.count("remap-comb-plan") && (vm.count("remap-comb-critical") || vm.count("remap-comb-candidate")))
        log_error("--remap-comb-plan cannot be combined with legacy comb-remap options.\n");
    if (vm.count("remap-comb-plan") && (ctx->local_remap_plan_list_only ||
        (!ctx->local_remap_report.empty() && ctx->local_remap_selection < 0)))
        log_error("Local-remap listing must be final; it cannot precede a comb-remap plan.\n");
    if (ctx->local_remap_plan_list_only && (vm.count("remap-comb-critical") || vm.count("remap-comb-candidate")))
        log_error("Local-remap listing must be final; it cannot precede legacy comb remapping.\n");
    if (vm.count("remap-comb-candidate") && !vm.count("remap-comb-critical"))
        log_error("--remap-comb-candidate requires --remap-comb-critical.\n");
    if (vm.count("remap-comb-critical")) {
        if (vm.count("no-place") || vm.count("pack-only") || vm.count("fes-cart") || vm.count("fes-scaffold") ||
            (vm.count("placer") && vm["placer"].as<std::string>() != "heap"))
            log_error("Comb remap requires fresh ordinary HeAP placement.\n");
        auto in = open_ifstream_and_log_error(vm["remap-comb-critical"].as<std::string>(), "internal-cut timing report");
        ctx->comb_remap_report.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (ctx->comb_remap_report.empty()) log_error("Empty internal-cut timing report.\n");
        if (vm.count("remap-comb-candidate")) ctx->comb_remap_selection = vm["remap-comb-candidate"].as<int>();
        if (ctx->comb_remap_selection < -1) log_error("Invalid comb-remap candidate index.\n");
    }
    if (vm.count("remap-comb-plan")) {
        if (vm.count("no-pack") || vm.count("no-place") || vm.count("pack-only") || vm.count("fes-cart") ||
            vm.count("fes-scaffold") || (vm.count("placer") && vm["placer"].as<std::string>() != "heap") ||
            (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != ""))
            log_error("Comb-remap plans require fresh ordinary HeAP placement.\n");
        auto filename = vm["remap-comb-plan"].as<std::string>();
        auto in = open_ifstream_and_log_error(filename, "comb-remap plan");
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}, error;
        auto plan = json11::Json::parse(text, error);
        if (!error.empty() || !plan.is_object() || plan.object_items().size() != 1 || !plan["steps"].is_array() ||
            plan["steps"].array_items().empty() || plan["steps"].array_items().size() > 8)
            log_error("Invalid comb-remap plan; expected one to eight steps.\n");
        check_plan_keys(text, "comb-remap");
        for (const auto &entry : plan["steps"].array_items()) {
            if (!entry.is_object() || entry.object_items().size() != 2 || !entry.object_items().count("report") ||
                !entry.object_items().count("candidate"))
                log_error("Invalid comb-remap plan step fields.\n");
            if (!entry["report"].is_string() || entry["report"].string_value().empty() ||
                entry["report"].string_value().find('\0') != std::string::npos)
                log_error("Invalid comb-remap plan path.\n");
            double candidate = entry["candidate"].number_value();
            if (!entry["candidate"].is_number() || !std::isfinite(candidate) || candidate != std::floor(candidate) ||
                candidate < -1 || candidate > std::numeric_limits<int>::max())
                log_error("Invalid integer in comb-remap plan.\n");
            Arch::CombRemapStep step;
            step.candidate = int(candidate);
            if (step.candidate == -1) {
                if (&entry != &plan["steps"].array_items().back() || !vm.count("no-route") || vm.count("rbf"))
                    log_error("A comb plan listing step must be last, with --no-route and without --rbf.\n");
                ctx->comb_remap_plan_list_only = true;
            }
            std::filesystem::path path(entry["report"].string_value());
            if (path.is_relative()) path = std::filesystem::path(filename).parent_path() / path;
            auto report_in = open_ifstream_and_log_error(path.lexically_normal().string(), "comb-remap plan report");
            step.report.assign(std::istreambuf_iterator<char>(report_in), std::istreambuf_iterator<char>());
            auto report = json11::Json::parse(step.report, error);
            if (!error.empty() || !report["critical_paths"].is_array())
                log_error("Invalid comb-remap plan report.\n");
            ctx->comb_remap_plan.push_back(std::move(step));
        }
    }
    ctx->decomposition_remap_report.clear();
    ctx->decomposition_remap_selection = -1;
    if (vm.count("remap-decompose-candidate") && !vm.count("remap-decompose-critical"))
        log_error("--remap-decompose-candidate requires --remap-decompose-critical.\n");
    if (vm.count("remap-decompose-critical")) {
        if (vm.count("no-pack") || vm.count("no-place") || vm.count("pack-only") || vm.count("fes-cart") ||
            vm.count("fes-scaffold") || (vm.count("placer") && vm["placer"].as<std::string>() != "heap") ||
            (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != ""))
            log_error("Control decomposition requires fresh ordinary HeAP placement.\n");
        if (ctx->local_remap_plan_list_only || ctx->comb_remap_plan_list_only ||
            (!ctx->local_remap_report.empty() && ctx->local_remap_selection < 0) ||
            (!ctx->comb_remap_report.empty() && ctx->comb_remap_selection < 0))
            log_error("A remap listing must be final; it cannot precede control decomposition.\n");
        auto in = open_ifstream_and_log_error(vm["remap-decompose-critical"].as<std::string>(), "control decomposition timing report");
        ctx->decomposition_remap_report.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        std::string error;
        auto report = json11::Json::parse(ctx->decomposition_remap_report,error);
        if (!error.empty() || !report["critical_paths"].is_array())
            log_error("Invalid control decomposition timing report.\n");
        if (vm.count("remap-decompose-candidate"))
            ctx->decomposition_remap_selection = vm["remap-decompose-candidate"].as<int>();
        if (ctx->decomposition_remap_selection < -1) log_error("Invalid control decomposition candidate index.\n");
        if (ctx->decomposition_remap_selection < 0 && (!vm.count("no-route") || vm.count("rbf")))
            log_error("Control decomposition listing requires --no-route and no --rbf.\n");
    }
    if (vm.count("remap-post-plan")) {
        load_local_plan("remap-post-plan", true);
        prevalidate_local_remap_post_prefix(ctx);
    }
    ctx->lut_pair_report.clear();
    ctx->lut_pair_selection = -1;
    ctx->lut_pair_compose_copy = false;
    if (vm.count("remap-lut-pair-compose-copy") && !vm.count("remap-lut-pair-critical"))
        log_error("--remap-lut-pair-compose-copy requires --remap-lut-pair-critical.\n");
    if (vm.count("remap-lut-pair-candidate") && !vm.count("remap-lut-pair-critical"))
        log_error("--remap-lut-pair-candidate requires --remap-lut-pair-critical.\n");
    if (vm.count("remap-lut-pair-critical")) {
        ctx->lut_pair_compose_copy = vm.count("remap-lut-pair-compose-copy") != 0;
        if (vm.count("no-pack") || vm.count("no-place") || vm.count("pack-only") || vm.count("fes-cart") ||
            vm.count("fes-scaffold") || (vm.count("placer") && vm["placer"].as<std::string>() != "heap") ||
            (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != ""))
            log_error("LUT pair placement requires fresh ordinary HeAP placement.\n");
        auto in = open_ifstream_and_log_error(vm["remap-lut-pair-critical"].as<std::string>(), "LUT pair placement timing report");
        ctx->lut_pair_report.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        std::string error;
        auto report = json11::Json::parse(ctx->lut_pair_report, error);
        if (!error.empty() || !report.is_object() || !report["critical_paths"].is_array())
            log_error("Invalid LUT pair placement timing report.\n");
        check_plan_keys(ctx->lut_pair_report, "LUT pair placement report");
        if (vm.count("remap-lut-pair-candidate"))
            ctx->lut_pair_selection = vm["remap-lut-pair-candidate"].as<int>();
        if (ctx->lut_pair_selection < -1) log_error("Invalid LUT pair placement candidate index.\n");
        if (ctx->lut_pair_selection < 0 && (!vm.count("no-route") || vm.count("rbf")))
            log_error("LUT pair placement listing requires --no-route and no --rbf.\n");
        prevalidate_lut_pair_prefix(ctx);
    }
    ctx->lut_driver_copy_report.clear();
    ctx->lut_driver_copy_selection = -1;
    if (vm.count("remap-lut-driver-candidate") && !vm.count("remap-lut-driver-critical"))
        log_error("--remap-lut-driver-candidate requires --remap-lut-driver-critical.\n");
    if (vm.count("remap-lut-driver-critical")) {
        if (vm.count("no-pack") || vm.count("no-place") || vm.count("pack-only") || vm.count("fes-cart") ||
            vm.count("fes-scaffold") || (vm.count("placer") && vm["placer"].as<std::string>() != "heap") ||
            (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != ""))
            log_error("LUT driver copy requires fresh ordinary HeAP placement.\n");
        auto in = open_ifstream_and_log_error(vm["remap-lut-driver-critical"].as<std::string>(), "LUT driver copy timing report");
        ctx->lut_driver_copy_report.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        std::string error;
        auto report = json11::Json::parse(ctx->lut_driver_copy_report, error);
        if (!error.empty() || !report.is_object() || !report["critical_paths"].is_array())
            log_error("Invalid LUT driver copy timing report.\n");
        check_plan_keys(ctx->lut_driver_copy_report, "LUT driver copy report");
        if (vm.count("remap-lut-driver-candidate"))
            ctx->lut_driver_copy_selection = vm["remap-lut-driver-candidate"].as<int>();
        if (ctx->lut_driver_copy_selection < -1) log_error("Invalid LUT driver copy candidate index.\n");
        if (ctx->lut_driver_copy_selection < 0 && (!vm.count("no-route") || vm.count("rbf")))
            log_error("LUT driver copy listing requires --no-route and no --rbf.\n");
        prevalidate_lut_driver_copy_prefix(ctx);
    }
    const bool routed = ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() == "route";
    if (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() == "place")
        ctx->restore_placed_pin_maps();
    if (vm.count("fes-cram-region")) {
        if (!routed)
            log_error("FES CRAM region requires an already routed scaffold.\n");
        ctx->note_fes_cram_region(vm["fes-cram-region"].as<std::string>());
    }
    if (vm.count("fes-slot-clock"))
        ctx->settings[ctx->id("fes/slot_clock")] = vm["fes-slot-clock"].as<std::string>();
    if (vm.count("router"))
        ctx->settings[ctx->id("router")] = vm["router"].as<std::string>();
    if (vm.count("qsf")) {
        std::string filename = vm["qsf"].as<std::string>();
        auto in = open_ifstream_and_log_error(filename, "input QSF file");
        ctx->begin_fes_qsf_reservation_check();
        ctx->read_qsf(in);
        ctx->finish_fes_qsf_reservation_check();
    }
    if (vm.count("fes-cart")) {
        std::string region = vm.count("fes-cart-region") ? vm["fes-cart-region"].as<std::string>() : "cart";
        log_info("FES merging cart JSON...\n");
        ctx->merge_fes_cart(vm["fes-cart"].as<std::string>(), region);
        log_info("FES packing unbound cart cells...\n");
        ctx->pack_unbound_cells();
        log_info("FES unbound pack complete.\n");
    } else if (vm.count("fes-cart-region")) {
        log_error("--fes-cart-region requires --fes-cart.\n");
    }
    if (vm.count("fes-scaffold") || routed)
        ctx->lock_fes_scaffold();
    // After the scaffold is locked, frozen LABs are known; constrain cart
    // cells to the reserved rectangle and report its capacity.
    ctx->fes_constrain_slot_region();
}

int main(int argc, char *argv[])
{
    MistralCommandHandler handler(argc, argv);
    return handler.exec();
}
