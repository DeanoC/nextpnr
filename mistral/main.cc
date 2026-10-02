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

  protected:
    po::options_description getArchOptions() override;
};

MistralCommandHandler::MistralCommandHandler(int argc, char **argv) : CommandHandler(argc, argv) {}

po::options_description MistralCommandHandler::getArchOptions()
{
    po::options_description specific("Architecture specific options");
    specific.add_options()("device", po::value<std::string>(), "device name (e.g. 5CSEBA6U23I7)");
    specific.add_options()("qsf", po::value<std::string>(), "path to QSF constraints file");
    specific.add_options()("rbf", po::value<std::string>(), "RBF bitstream to write");
    specific.add_options()("compress-rbf", "generate compressed bitstream");
    specific.add_options()("fes-scaffold", "lock loaded shell BEL+routing to STRENGTH_USER");
    specific.add_options()("fes-cart", po::value<std::string>(), "merge unbound cart JSON into the reserved socket");
    specific.add_options()("fes-cart-region", po::value<std::string>(),
                            "name of the FES_RESERVED_RECT region this --fes-cart targets (default: cart)");
    specific.add_options()("fes-slot-clock", po::value<std::string>(), "exact shell clock net for FES cart cells");
    specific.add_options()("fes-cram-region", po::value<std::string>(), "half-open CRAM x0,y0,x1,y1 region for new scaffold routing");

    specific.add_options()("replicate-enables", po::value<int>(),
                           "replicate timing-critical LUT enables after placement (budget 0..8, default 0)");
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
    specific.add_options()("remap-lut-pair-copy-plan", po::value<std::string>(),
                           "JSON plan of one or two composed LUT-pair enable copies");
    specific.add_options()("remap-lut-driver-critical", po::value<std::string>(), "prior timing report for one LUT driver copy to an arithmetic data input");
    specific.add_options()("remap-lut-driver-candidate", po::value<int>(), "qualified LUT-driver-copy candidate index (default: list only)");
    specific.add_options()("remap-candidate", po::value<int>(), "qualified local-remap candidate index (default: list only)");
    specific.add_options()("remap-groups", po::value<int>(), "maximum whole LAB enable groups per remap (1..8, default 1)");
    specific.add_options()("balance-reduction-root", po::value<std::vector<std::string>>()->composing(),
                           "pre-placement 3-LUT/11-input, 4-LUT/16-input or 7-LUT/24-input conjunction root cell (repeatable, opt-in)");
    return specific;
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

std::unique_ptr<Context> MistralCommandHandler::createContext(dict<std::string, Property> &values)
{
    ArchArgs chipArgs;
    if (!vm.count("device")) {
        log_error("device must be specified on the command line (e.g. --device 5CSEBA6U23I7)\n");
    }
    chipArgs.device = vm["device"].as<std::string>();
    auto ctx = std::unique_ptr<Context>(new Context(chipArgs));
    if (vm.count("compress-rbf"))
        ctx->settings[id_compress_rbf] = Property::State::S1;
    ctx->signoff_after_route = vm.count("rbf") > 0;
    return ctx;
}

void MistralCommandHandler::customAfterLoad(Context *ctx)
{
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
    ctx->lut_pair_copy_plan.clear();
    ctx->lut_pair_copy_plan_list_only = false;
    ctx->lut_driver_copy_report.clear();
    ctx->lut_driver_copy_selection = -1;
    if (vm.count("remap-lut-pair-copy-plan") && (vm.count("remap-lut-pair-critical") ||
        vm.count("remap-lut-pair-candidate") || vm.count("remap-lut-pair-compose-copy")))
        log_error("--remap-lut-pair-copy-plan cannot be combined with legacy LUT pair options.\n");
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
    if (vm.count("remap-lut-pair-copy-plan")) {
        if (vm.count("no-pack") || vm.count("no-place") || vm.count("pack-only") || vm.count("fes-cart") ||
            vm.count("fes-scaffold") || (vm.count("placer") && vm["placer"].as<std::string>() != "heap") ||
            (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != ""))
            log_error("LUT pair copy plans require fresh ordinary HeAP placement.\n");
        auto filename = vm["remap-lut-pair-copy-plan"].as<std::string>();
        auto in = open_ifstream_and_log_error(filename, "LUT pair copy plan");
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}, error;
        auto plan = json11::Json::parse(text, error);
        if (!error.empty() || !plan.is_object() || plan.object_items().size() != 1 || !plan["steps"].is_array() ||
            plan["steps"].array_items().empty() || plan["steps"].array_items().size() > Arch::lut_pair_copy_max_steps)
            log_error("Invalid LUT pair copy plan; expected one or two steps.\n");
        check_plan_keys(text, "LUT pair copy");
        for (const auto &entry : plan["steps"].array_items()) {
            if (!entry.is_object() || entry.object_items().size() != 2 || !entry.object_items().count("report") ||
                !entry.object_items().count("candidate"))
                log_error("Invalid LUT pair copy plan step fields.\n");
            if (!entry["report"].is_string() || entry["report"].string_value().empty() ||
                entry["report"].string_value().find('\0') != std::string::npos)
                log_error("Invalid LUT pair copy plan path.\n");
            double candidate = entry["candidate"].number_value();
            if (!entry["candidate"].is_number() || !std::isfinite(candidate) || candidate != std::floor(candidate) ||
                candidate < -1 || candidate > std::numeric_limits<int>::max())
                log_error("Invalid integer in LUT pair copy plan.\n");
            Arch::LutPairCopyStep step;
            step.candidate = int(candidate);
            if (step.candidate == -1) {
                if (&entry != &plan["steps"].array_items().back() || !vm.count("no-route") || vm.count("rbf"))
                    log_error("A LUT pair copy plan listing step must be last, with --no-route and without --rbf.\n");
                if (vm.count("remap-lut-driver-critical") || vm.count("remap-lut-driver-candidate"))
                    log_error("A LUT pair copy plan listing must be final; it cannot precede LUT driver copy.\n");
                ctx->lut_pair_copy_plan_list_only = true;
            }
            std::filesystem::path path(entry["report"].string_value());
            if (path.is_relative()) path = std::filesystem::path(filename).parent_path() / path;
            auto report_in = open_ifstream_and_log_error(path.lexically_normal().string(), "LUT pair copy plan report");
            step.report.assign(std::istreambuf_iterator<char>(report_in), std::istreambuf_iterator<char>());
            auto report = json11::Json::parse(step.report, error);
            if (!error.empty() || !report.is_object() || !report["critical_paths"].is_array())
                log_error("Invalid LUT pair copy plan report.\n");
            check_plan_keys(step.report, "LUT pair copy plan report");
            ctx->lut_pair_copy_plan.push_back(std::move(step));
        }
        ctx->prevalidate_lut_pair_copy_plan();
    }
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
        ctx->read_qsf(in);
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
