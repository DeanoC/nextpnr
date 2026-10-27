/* Measured placement feedback. SPDX-License-Identifier: ISC */
#include "routed_placement.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include "critical_cohort.h"
#include "critical_cohort_model.h"
#include "log.h"
#include "timing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
using J = json11::Json;

J property(const Property &p) { return J::array{p.is_string, p.str}; }
J properties(Context *ctx, const dict<IdString, Property> &values, bool attributes = false)
{
    J::object result;
    for (const auto &entry : values) {
        const auto &name = entry.first.str(ctx);
        if (attributes && (name == "BEL_STRENGTH" || name.find("NEXTPNR_") == 0 || name.find("FES_PINMAP_") == 0))
            continue; // Derived physical snapshots are represented explicitly.
        result[name] = property(entry.second);
    }
    return result;
}
J pair(DelayPair value) { return J::array{double(value.minDelay()), double(value.maxDelay())}; }
J port(Context *ctx, const PortRef &p)
{
    return p.cell ? J(J::array{p.cell->name.str(ctx), p.port.str(ctx)}) : J();
}
J parse(const std::string &text)
{
    std::string error;
    J result = J::parse(text, error);
    if (!error.empty() || !result.is_object())
        log_error("Invalid routed placement JSON: %s.\n", error.c_str());
    return result;
}
void write(Context *ctx, const std::string &name, const J &value)
{
    std::filesystem::path path = std::filesystem::path(ctx->routed_placement_directory) / name;
    if (std::filesystem::exists(path))
        log_error("Refusing to overwrite routed placement record '%s'.\n", path.string().c_str());
    std::ofstream out(path);
    out << value.dump() << '\n';
    if (!out)
        log_error("Failed to write routed placement record '%s'.\n", path.string().c_str());
}

// Prove that no transition can launch at this port. Do not assign a zero
// setup/hold row to a static source, or treat an arbitrary hard-IP output as one.
bool static_source(Context *ctx, CellInfo *cell, IdString pin, J::array &proof)
{
    std::set<std::pair<CellInfo *, IdString>> seen;
    while (cell && seen.emplace(cell, pin).second && seen.size() <= 256) {
        auto state = cell->get_pin_state(pin);
        proof.push_back(J::array{cell->name.str(ctx), pin.str(ctx), int(state)});
        if (state == PIN_0 || state == PIN_1)
            return true;
        auto *net = critical_cohort_logical_net(ctx, cell, pin);
        if (!net || !net->driver.cell || net->driver.cell->getPort(net->driver.port) != net)
            return false;
        auto *driver = net->driver.cell;
        if (driver->type.in(id_MISTRAL_CONST, id_GND, id_VCC)) {
            proof.push_back(J::array{driver->name.str(ctx), net->driver.port.str(ctx), driver->type.str(ctx)});
            return true;
        }
        if (!driver->type.in(id_MISTRAL_BUF, id_MISTRAL_NOT) || net->driver.port != id_Q)
            return false;
        cell = driver;
        pin = id_A;
    }
    return false;
}

std::optional<bool> constant_value(Context *ctx, CellInfo *cell, IdString pin, J::array &proof)
{
    std::set<std::pair<CellInfo *, IdString>> seen;
    bool inverted = false;
    while (cell && seen.emplace(cell, pin).second && seen.size() <= 256) {
        auto state = cell->get_pin_state(pin);
        proof.push_back(J::array{cell->name.str(ctx), pin.str(ctx), int(state)});
        if (state == PIN_0 || state == PIN_1)
            return (state == PIN_1) != inverted;
        inverted ^= state == PIN_INV;
        auto *net = critical_cohort_logical_net(ctx, cell, pin);
        if (!net || !net->driver.cell || net->driver.cell->getPort(net->driver.port) != net)
            return {};
        auto *driver = net->driver.cell;
        if (driver->type.in(id_GND, id_VCC)) {
            proof.push_back(J::array{driver->name.str(ctx), net->driver.port.str(ctx), driver->type.str(ctx)});
            return (driver->type == id_VCC) != inverted;
        }
        if (driver->type == id_MISTRAL_CONST && net->driver.port == id_Q) {
            auto value = driver->params.find(id_LUT);
            if (value == driver->params.end() || !value->second.is_fully_def() ||
                value->second.str.size() > 64 || value->second.as_int64() < 0 || value->second.as_int64() > 1)
                return {};
            proof.push_back(J::array{driver->name.str(ctx), "LUT", property(value->second)});
            return bool(value->second.as_int64()) != inverted;
        }
        if (!driver->type.in(id_MISTRAL_BUF, id_MISTRAL_NOT) || net->driver.port != id_Q)
            return {};
        inverted ^= driver->type == id_MISTRAL_NOT;
        cell = driver;
        pin = id_A;
    }
    return {};
}

// Same-width TDP has independent stored bits. A disabled write port cannot
// observe its DATA input; a disconnected DATA input also cannot affect any
// consumer when both corresponding read results are unused. Neither condition
// excuses addresses, enables, write controls, live read data or mixed widths.
bool inactive_m10k_data(Context *ctx, CellInfo *cell, IdString pin, const std::set<NetInfo *> &top_nets,
                        J::array &proof)
{
    if (cell->type != id_MISTRAL_M10K || !bool_or_default(cell->params, id_CFG_TDP, false) ||
        bool_or_default(cell->params, id_CFG_MIXED_WIDTH, false) ||
        bool_or_default(cell->params, id_CFG_ASYNC_READ, false))
        return false;
    int width = int_or_default(cell->params, id_CFG_DBITS, 0);
    int abits = int_or_default(cell->params, id_CFG_ABITS, 0);
    if ((width != 1 && width != 2 && width != 5 && width != 10 && width != 20) ||
        abits < 7 || abits > 13 || (int64_t(1) << abits) * width > 10240)
        return false;
    int bit = -1;
    char side = 0;
    const auto name = pin.str(ctx);
    for (char candidate : {'A', 'B'})
        for (int i = 0; i < width; ++i)
            if (name == (width == 1 ? std::string(1, candidate) + "1DATA" :
                                     std::string(1, candidate) + "1DATA[" + std::to_string(i) + "]")) {
                side = candidate;
                bit = i;
            }
    if (bit < 0)
        return false;
    J::array control;
    IdString we = side == 'A' ? id_A1WE : id_B1WE;
    // TDP packing routes WE through WREN; it has no implicit constant or
    // inversion pin style. Do not interpret an unsupported saved mux state.
    if (cell->get_pin_state(we) != PIN_SIG || !cell->getPort(we))
        return false;
    auto write = constant_value(ctx, cell, we, control);
    if (write && !*write) {
        proof = {"m10k_write_disabled", cell->name.str(ctx), pin.str(ctx), control};
        return true;
    }
    if (cell->getPort(pin) || cell->get_pin_state(pin) != PIN_SIG)
        return false;
    J::array outputs;
    for (char read_side : {'A', 'B'}) {
        const auto output_name = width == 1 ? std::string(1, read_side) + "1Q" :
                std::string(1, read_side) + "1Q[" + std::to_string(bit) + "]";
        auto output = cell->ports.find(ctx->id(output_name));
        if (output == cell->ports.end() || output->second.type != PORT_OUT)
            return false;
        auto *net = output->second.net;
        if (net) {
            if (net->driver.cell != cell || net->driver.port != output->first || !net->users.empty() || net->clkconstr)
                return false;
            if (top_nets.count(net))
                return false;
        }
        outputs.push_back(J::array{output_name, net ? net->name.str(ctx) : ""});
    }
    proof = {"m10k_unobserved_data_bit", cell->name.str(ctx), pin.str(ctx), width, abits, outputs};
    return true;
}
bool registered_pad(const CellInfo *cell)
{
    return cell->type.in(id_MISTRAL_SDRIN, id_MISTRAL_SDROUT, id_MISTRAL_SDRIO, id_MISTRAL_DDRIN,
                         id_MISTRAL_DDROUT, id_MISTRAL_DDRBIDIR);
}
bool clock_only_net(Context *ctx, NetInfo *net, std::set<NetInfo *> &seen)
{
    if (!net || !net->clkconstr || net->clkconstr->period.minDelay() <= 0 || !seen.insert(net).second ||
        net->users.empty())
        return false;
    for (const auto &user : net->users) {
        int count = 0;
        if (ctx->getPortTimingClass(user.cell, user.port, count) == TMG_CLOCK_INPUT)
            continue;
        if (user.cell->type != id_MISTRAL_CLKBUF || user.port != id_A ||
            !clock_only_net(ctx, user.cell->getPort(id_Q), seen))
            return false;
    }
    return true;
}
bool clock_input(Context *ctx, CellInfo *cell, IdString pin, J::array &proof)
{
    IdString output;
    if (cell->type == id_MISTRAL_IB && pin == id_PAD) output = id_O;
    if (cell->type == id_MISTRAL_CLKBUF && pin == id_A) output = id_Q;
    if (output == IdString()) return false;
    auto *net = cell->getPort(output);
    std::set<NetInfo *> seen;
    if (!clock_only_net(ctx, net, seen)) return false;
    proof = {cell->type.str(ctx), output.str(ctx), net->name.str(ctx)};
    return true;
}
J clock_key(Context *ctx, const ClockDomainKey &key)
{
    return J::array{key.clock.str(ctx), int(key.edge)};
}
J optional(std::optional<delay_t> value) { return value ? J(double(*value)) : J(); }
J prediction_rows(Context *ctx, const CriticalCohortEndpointRows &rows)
{
    std::map<std::string, J::object> cells;
    for (const auto &entry : rows) {
        J::array values;
        for (const auto &row : entry.second)
            values.push_back(J::object{{"launch", clock_key(ctx, row.launch)}, {"capture", clock_key(ctx, row.capture)},
                                       {"setup_timed", row.setup_timed}, {"hold_related", row.hold_related},
                                       {"setup_window_ps", optional(row.setup_window)},
                                       {"setup_margin_ps", optional(row.setup_margin)},
                                       {"hold_margin_ps", optional(row.hold_margin)},
                                       {"max_path_ps", double(row.max_path_delay)},
                                       {"min_path_ps", double(row.min_path_delay)}});
        cells[entry.first.cell.str(ctx)][entry.first.port.str(ctx)] = values;
    }
    J::object result;
    for (const auto &entry : cells)
        result[entry.first] = entry.second;
    return result;
}
void require_keys(const J &object, std::initializer_list<const char *> names)
{
    if (!object.is_object() || object.object_items().size() != names.size())
        log_error("Invalid routed placement plan fields.\n");
    for (auto name : names)
        if (!object.object_items().count(name))
            log_error("Missing routed placement plan field '%s'.\n", name);
}
} // namespace

J routed_placement_snapshot(Context *ctx)
{
    J::object cells, nets, placement, clocks, top_ports;
    J::array cell_order, net_order, user_order;
    std::map<std::string, J::array> users;
    for (const auto &entry : ctx->cells) {
        CellInfo *cell = entry.second.get();
        if (critical_cohort_route_through_ff(ctx, cell))
            continue;
        const auto name = entry.first.str(ctx);
        cell_order.push_back(name);
        J::object ports;
        for (const auto &p : cell->ports) {
            NetInfo *net = critical_cohort_logical_net(ctx, cell, p.first);
            ports[p.first.str(ctx)] = J::array{int(p.second.type), net ? net->name.str(ctx) : "",
                                             int(cell->get_pin_state(p.first))};
            if (net && p.second.type == PORT_IN)
                users[net->name.str(ctx)].push_back(J::array{name, p.first.str(ctx)});
        }
        J::array children;
        for (const auto *child : cell->constr_children)
            children.push_back(child->name.str(ctx));
        auto attributes = properties(ctx, cell->attrs, true).object_items();
        if (cell->bel != BelId() && cell->belStrength == STRENGTH_USER) {
            // archInfoToAttributes consumes BEL and emits NEXTPNR_BEL plus
            // BEL_STRENGTH. Preserve the user constraint through that exact
            // architecture-owned conversion, backed by its actual binding.
            auto bound = property(Property(ctx->getBelName(cell->bel).str(ctx)));
            if (attributes.count("BEL") && attributes.at("BEL") != bound)
                log_error("Routed placement user BEL does not match its protected binding.\n");
            attributes["BEL"] = bound;
        }
        cells[name] = J::object{{"type", cell->type.str(ctx)}, {"params", properties(ctx, cell->params)},
                               {"attributes", attributes}, {"ports", ports},
                               {"cluster", cell->cluster.str(ctx)}, {"children", children},
                               {"relative", J::array{cell->constr_x, cell->constr_y, cell->constr_z,
                                                      cell->constr_abs_z}},
                               {"region", cell->region ? cell->region->name.str(ctx) : ""}};
        placement[name] = J::array{cell->bel == BelId() ? "" : ctx->getBelName(cell->bel).str(ctx),
                                   int(cell->belStrength)};
    }
    for (const auto &entry : ctx->nets) {
        auto *net = entry.second.get();
        if (critical_cohort_route_through_ff(ctx, net->driver.cell))
            continue;
        const auto name = entry.first.str(ctx);
        net_order.push_back(name);
        auto &u = users[name];
        std::sort(u.begin(), u.end(), [](const J &a, const J &b) { return a.dump() < b.dump(); });
        nets[name] = J::object{{"driver", port(ctx, net->driver)}, {"users", u}};
        for (auto user : net->users.enumerate())
            user_order.push_back(J::array{name, user.index.idx(), port(ctx, user.value)});
        if (net->clkconstr) {
            const auto &c = *net->clkconstr;
            clocks[name] = J::array{pair(c.period), pair(c.high), pair(c.low), c.phase_group.str(ctx), c.phase_shift};
        }
    }
    J::object timing_settings;
    for (const auto &entry : ctx->settings) {
        const auto key = entry.first.str(ctx);
        // These select execution hardware, not the route algorithm. CPU
        // reference, HIP, CUDA and Metal replay evidence is interchangeable.
        if (key == "gpurouter/cpu" || key == "gpurouter/device" || key == "gpurouter/required")
            continue;
        if (key == "target_freq" || key == "timing_driven" || key.find("timing/") == 0 || key.find("sdc/") == 0 ||
            key.find("mistral/") == 0 || key.find("placerHeap/") == 0 || key.find("gpurouter/") == 0)
            timing_settings[key] = property(entry.second);
    }
    J::object regions;
    for (const auto &entry : ctx->ports)
        top_ports[entry.first.str(ctx)] = J::array{int(entry.second.type),
                                                  entry.second.net ? entry.second.net->name.str(ctx) : ""};
    for (const auto &entry : ctx->region) {
        J::array bels, wires, piplocs;
        for (auto bel : entry.second->bels)
            bels.push_back(ctx->getBelName(bel).str(ctx));
        std::sort(bels.begin(), bels.end(), [](const J &a, const J &b) { return a.string_value() < b.string_value(); });
        for (auto wire : entry.second->wires)
            wires.push_back(ctx->getWireName(wire).str(ctx));
        for (auto loc : entry.second->piplocs)
            piplocs.push_back(J::array{loc.x, loc.y, loc.z});
        auto order = [](const J &a, const J &b) { return a.dump() < b.dump(); };
        std::sort(wires.begin(), wires.end(), order);
        std::sort(piplocs.begin(), piplocs.end(), order);
        regions[entry.first.str(ctx)] = J::object{{"constr_bels", entry.second->constr_bels}, {"bels", bels},
                                                 {"constr_wires", entry.second->constr_wires}, {"wires", wires},
                                                 {"constr_pips", entry.second->constr_pips}, {"piplocs", piplocs}};
    }
    J::object architecture{{"ff4", ctx->lab_ff4}, {"clkb", ctx->args.lab_clkb},
                            {"dsp_signed_lanes", ctx->args.dsp_signed_lanes}};
    if (ctx->routed_placement_preserve_routes) architecture["preserve_routes"] = true;
    if (ctx->routed_placement_group_moves) architecture["group_moves"] = true;
    return J::object{{"version", 1}, {"device", ctx->getChipName()},
                     {"design", J::object{{"cells", cells}, {"nets", nets}, {"regions", regions}, {"ports", top_ports}}},
                     {"placement", placement}, {"clocks", clocks}, {"timing_settings", timing_settings},
                     {"architecture", architecture},
                     {"rng", std::to_string(ctx->rngstate)},
                     {"ordering", J::object{{"cells", cell_order}, {"nets", net_order}, {"users", user_order}}}};
}

J routed_placement_endpoints(Context *ctx)
{
    TimingAnalyser timing(ctx), reference_free(ctx);
    timing.with_clock_skew = true;
    timing.collect_primitive_clock_timings = true;
    timing.setup(false, false, true);
    reference_free.with_clock_skew = false;
    reference_free.setup(false, false, true);
    J::array endpoints, unsupported, statics, inactive, clock_inputs, primitive_clocks, setup_paths;
    std::set<NetInfo *> top_nets;
    for (const auto &port : ctx->ports)
        if (port.second.net)
            top_nets.insert(port.second.net);
    std::vector<CellInfo *> cells;
    for (const auto &entry : ctx->cells)
        cells.push_back(entry.second.get());
    std::sort(cells.begin(), cells.end(), [](const CellInfo *a, const CellInfo *b) { return a->name < b->name; });
    for (auto *cell : cells) {
        if (critical_cohort_route_through_ff(ctx, cell))
            continue;
        // Fabric register rows do not qualify a registered pad boundary.
        if (registered_pad(cell))
            unsupported.push_back(J::object{{"cell", cell->name.str(ctx)}, {"port", ""},
                                            {"reason", "registered_pad_boundary_not_exported"}});
        for (const auto &p : cell->ports) {
            if (p.second.type == PORT_OUT && p.second.net &&
                (!p.second.net->users.empty() || top_nets.count(p.second.net))) {
                int count = 0;
                auto klass = ctx->getPortTimingClass(cell, p.first, count);
                if (klass == TMG_IGNORE || klass == TMG_STARTPOINT) {
                    J::array proof;
                    std::set<NetInfo *> clocks;
                    if (!static_source(ctx, cell, p.first, proof) && !clock_only_net(ctx, p.second.net, clocks))
                        unsupported.push_back(J::object{{"cell", cell->name.str(ctx)}, {"port", p.first.str(ctx)},
                                                        {"class", int(klass)}, {"direction", "output"},
                                                        {"reason", "unmodeled_launch_boundary"}});
                }
            }
            if (p.second.type != PORT_IN)
                continue;
            int count = 0;
            auto klass = ctx->getPortTimingClass(cell, p.first, count);
            const bool endpoint = klass == TMG_REGISTER_INPUT || klass == TMG_ENDPOINT;
            const bool ignored = klass == TMG_IGNORE && p.second.net;
            if (!endpoint && !ignored)
                continue;
            J::object key{{"cell", cell->name.str(ctx)}, {"port", p.first.str(ctx)}, {"class", int(klass)}};
            J::array proof;
            if (ignored && clock_input(ctx, cell, p.first, proof)) {
                key["proof"] = proof;
                clock_inputs.push_back(key);
                continue;
            }
            // The primitive's synchronous-load mux cannot capture SDATA
            // while its selection is folded to zero. This is an actual
            // architecture condition, not a missing-row exemption.
            if (cell->type == id_MISTRAL_FF && p.first == id_SDATA && cell->get_pin_state(id_SLOAD) == PIN_0) {
                key["proof"] = J::array{cell->name.str(ctx), "SLOAD", int(PIN_0)};
                inactive.push_back(key);
                continue;
            }
            if (static_source(ctx, cell, p.first, proof)) {
                key["proof"] = proof;
                statics.push_back(key);
                continue;
            }
            if (inactive_m10k_data(ctx, cell, p.first, top_nets, proof)) {
                key["proof"] = proof;
                inactive.push_back(key);
                continue;
            }
            if (cell->type == id_MISTRAL_FF && p.first == id_ACLR) {
                key["reason"] = "asynchronous_recovery_removal_not_exported";
                unsupported.push_back(key);
                continue;
            }
            std::vector<EndpointClockPairTiming> rows, reference;
            bool valid = endpoint && timing.get_endpoint_clock_pair_timings({cell->name, p.first}, rows) &&
                         reference_free.get_endpoint_clock_pair_timings({cell->name, p.first}, reference) &&
                         !rows.empty() && rows.size() == reference.size();
            J::array values;
            for (size_t i = 0; valid && i < rows.size(); ++i) {
                const auto &r = rows[i], &rf = reference[i];
                valid &= r.launch == rf.launch && r.capture == rf.capture && r.setup_timed == rf.setup_timed &&
                         r.hold_related == rf.hold_related && r.setup_window == rf.setup_window;
                values.push_back(J::object{{"launch", clock_key(ctx, r.launch)}, {"capture", clock_key(ctx, r.capture)},
                                           {"setup_timed", r.setup_timed}, {"hold_related", r.hold_related},
                                           {"setup_window_ps", optional(r.setup_window)},
                                           {"setup_margin_ps", optional(r.setup_margin)},
                                           {"hold_margin_ps", optional(r.hold_margin)},
                                           {"max_path_ps", double(rf.max_path_delay)},
                                           {"min_path_ps", double(rf.min_path_delay)}});
            }
            if (valid) {
                key["rows"] = values;
                endpoints.push_back(key);
                if (ctx->routed_placement_group_moves) {
                    std::vector<CriticalPath> paths;
                    if (!timing.get_endpoint_setup_paths({cell->name, p.first}, paths))
                        log_error("Cannot export complete native endpoint setup paths.\n");
                    size_t index = 0;
                    for (const auto &row : rows) {
                        if (!row.setup_timed) continue;
                        if (index >= paths.size()) log_error("Incomplete native endpoint setup path inventory.\n");
                        const auto &path = paths.at(index++);
                        J::array segments;
                        EndpointClockPairTiming::DelaySum sum = 0;
                        for (const auto &segment : path.segments) {
                            sum += segment.delay;
                            segments.push_back(J::object{{"type", CriticalPath::Segment::type_to_str(segment.type)},
                                {"delay_ps", double(segment.delay)}, {"net", segment.net.str(ctx)},
                                {"from", J::array{segment.from.first.str(ctx), segment.from.second.str(ctx)}},
                                {"to", J::array{segment.to.first.str(ctx), segment.to.second.str(ctx)}}});
                        }
                        if (sum != row.max_path_delay) log_error("Native endpoint path sum changed during export.\n");
                        setup_paths.push_back(J::object{{"cell", cell->name.str(ctx)}, {"port", p.first.str(ctx)},
                            {"launch", clock_key(ctx, row.launch)}, {"capture", clock_key(ctx, row.capture)},
                            {"setup_margin_ps", optional(row.setup_margin)}, {"path_sum_ps", double(sum)},
                            {"segments", segments}});
                    }
                    if (index != paths.size()) log_error("Unexpected native endpoint setup paths.\n");
                }
            } else {
                key["reason"] = ignored ? "unmodeled_connected_input" : "incomplete_native_clock_pair_coverage";
                unsupported.push_back(key);
            }
        }
    }
    for (const auto &row : timing.primitive_clock_timings)
        primitive_clocks.push_back(J::object{{"cell", row.endpoint.cell.str(ctx)},
                                             {"port", row.endpoint.port.str(ctx)}, {"clock", row.clock.str(ctx)},
                                             {"period_ps", pair(row.period)}, {"high_ps", pair(row.high)},
                                             {"low_ps", pair(row.low)}, {"route_ps", pair(row.route)},
                                             {"required_ps", J::array{double(row.requirement.min_period),
                                                                       double(row.requirement.min_high),
                                                                       double(row.requirement.min_low)}},
                                             {"actual_ps", J::array{row.actual_period, row.actual_high, row.actual_low}}});
    if (ctx->routed_placement_group_moves)
        write(ctx, "endpoint-paths.json", J::object{{"version", 1}, {"diagnostic_only", true},
            {"clock_skew", true}, {"final_analogue_model", ctx->timing_result_is_final_analogue && ctx->bitstream_configured},
            {"paths", setup_paths}});
    return J::object{{"version", 2}, {"analysis_complete", timing.observations_complete() && reference_free.observations_complete()},
                     {"final_analogue_model", ctx->timing_result_is_final_analogue && ctx->bitstream_configured},
                     {"complete", ctx->timing_result_is_final_analogue && ctx->bitstream_configured &&
                                      timing.observations_complete() && reference_free.observations_complete() &&
                                      !endpoints.empty() && unsupported.empty()},
                     {"expected_endpoint_count", int(endpoints.size() + statics.size() + inactive.size() +
                                                       clock_inputs.size() + unsupported.size())},
                     {"endpoints", endpoints}, {"static_endpoints", statics}, {"inactive_endpoints", inactive},
                     {"clock_inputs", clock_inputs}, {"unsupported", unsupported},
                     {"primitive_clocks", primitive_clocks}};
}

static J::object placement_plan_base(const J &origin, const J &parent, const J &target)
{
    J::array moves;
    for (const auto &entry : origin["placement"].object_items()) {
        auto next = target["placement"][entry.first];
        if (next == entry.second) continue;
        moves.push_back(J::object{{"cell", entry.first}, {"from_bel", entry.second[0]},
                                  {"to_bel", next[0]}, {"strength", entry.second[1]}});
    }
    return J::object{{"origin", origin}, {"parent", parent}, {"moves", moves}};
}

J make_routed_placement_group_plan(const J &origin, const J &parent, const J &target)
{
    auto plan = placement_plan_base(origin, parent, target);
    plan["version"] = 3;
    plan["evaluation"] = "full_route_required";
    return plan;
}

J make_routed_placement_plan(const J &origin, const J &parent, const J &target, delay_t before, delay_t after,
                            const J &routing)
{
    auto plan = placement_plan_base(origin, parent, target);
    plan["version"] = 1;
    plan["predicted"] = J::object{{"before_ps", double(before)}, {"after_ps", double(after)}};
    if (!routing.is_null()) {
        if (!target["architecture"]["preserve_routes"].bool_value() || !routing["joint_trees"].bool_value() ||
            !routing["native_analogue"].bool_value() || !routing["nets"].is_object())
            log_error("Route handoff requires complete native-scored data trees and preservation mode.\n");
        J::object nets;
        for (const auto &entry : routing["nets"].object_items()) {
            const auto &net = entry.second;
            if (!net["complete"].bool_value() || !net["source"].is_string() || !net["sinks"].is_array() ||
                !net["tree"].is_array())
                log_error("Cannot hand off an incomplete predicted data tree.\n");
            nets[entry.first] = J::object{{"source", net["source"]}, {"sinks", net["sinks"]}, {"wires", net["tree"]}};
        }
        plan["version"] = 2;
        plan["data_routes"] = J::object{{"version", 1}, {"target", target}, {"nets", nets}};
    }
    return plan;
}

bool apply_routed_placement_plan(Context *ctx, const J &plan, const J &origin, bool check_only)
{
    const bool handoff = plan["version"] == J(2);
    const bool group = plan["version"] == J(3);
    if (group) require_keys(plan, {"version", "origin", "parent", "moves", "evaluation"});
    else if (handoff) require_keys(plan, {"version", "origin", "parent", "moves", "predicted", "data_routes"});
    else require_keys(plan, {"version", "origin", "parent", "moves", "predicted"});
    if (group != ctx->routed_placement_group_moves ||
        (group && plan["evaluation"] != J("full_route_required")))
        log_error("Group placement requires explicit group mode and a full-route group plan.\n");
    require_keys(plan["parent"], {"version", "device", "design", "placement", "clocks", "timing_settings",
                                    "architecture", "rng", "ordering"});
    if (!group) require_keys(plan["predicted"], {"before_ps", "after_ps"});
    for (const auto &entry : plan["predicted"].object_items())
        if (!entry.second.is_number() || !std::isfinite(entry.second.number_value()) ||
            std::floor(entry.second.number_value()) != entry.second.number_value())
            log_error("Invalid routed placement predicted delay.\n");
    if ((!handoff && !group && plan["version"] != J(1)) || plan["origin"] != origin || !plan["moves"].is_array() ||
        plan["moves"].array_items().size() > 256 || (!group && !plan["predicted"].is_object()) ||
        plan["parent"]["version"] != origin["version"] || plan["parent"]["device"] != origin["device"] ||
        plan["parent"]["design"] != origin["design"] || plan["parent"]["clocks"] != origin["clocks"] ||
        plan["parent"]["timing_settings"] != origin["timing_settings"] ||
        plan["parent"]["architecture"] != origin["architecture"] ||
        plan["parent"]["rng"] != origin["rng"] || plan["parent"]["ordering"] != origin["ordering"])
        log_error("Routed placement plan does not match the fresh original placement and recipe.\n");
    auto parent_placement = plan["parent"]["placement"];
    if (!parent_placement.is_object() || parent_placement.object_items().size() != origin["placement"].object_items().size())
        log_error("Incomplete routed placement parent identity.\n");
    for (const auto &entry : origin["placement"].object_items()) {
        auto value = parent_placement[entry.first];
        if (!value.is_array() || value.array_items().size() != 2 || !value[0].is_string() ||
            value[1] != entry.second[1])
            log_error("Invalid routed placement parent binding.\n");
    }
    std::vector<std::pair<CellInfo *, BelId>> moves;
    auto expected = origin.object_items();
    auto expected_placement = origin["placement"].object_items();
    std::set<std::string> seen;
    for (const auto &row : plan["moves"].array_items()) {
        require_keys(row, {"cell", "from_bel", "to_bel", "strength"});
        if (!row["cell"].is_string() || !row["from_bel"].is_string() || !row["to_bel"].is_string() ||
            !seen.insert(row["cell"].string_value()).second)
            log_error("Routed placement plan has duplicate or invalid cells.\n");
        const auto &name = row["cell"].string_value();
        auto original = origin["placement"][name];
        if (!original.is_array() || row["from_bel"] != original[0] || row["strength"] != original[1])
            log_error("Routed placement plan has an unknown cell, stale BEL or altered strength.\n");
        auto cell = ctx->cells.find(ctx->id(name));
        BelId target = ctx->getBelByName(IdStringList::parse(ctx, row["to_bel"].string_value()));
        if (cell == ctx->cells.end() || target == BelId())
            log_error("Routed placement plan has an unknown cell or target BEL.\n");
        moves.emplace_back(cell->second.get(), target);
        expected_placement[name] = J::array{row["to_bel"], row["strength"]};
    }
    if (handoff) {
        const auto &routes = plan["data_routes"];
        require_keys(routes, {"version", "target", "nets"});
        expected["placement"] = expected_placement;
        if (!ctx->routed_placement_preserve_routes || routes["version"] != J(1) ||
            routes["target"] != J(expected) || !routes["nets"].is_object() ||
            (ctx->routed_placement_model.empty() && ctx->routed_placement_routes.empty()))
            log_error("Route handoff requires the exact target placement, preservation, and a parent route witness.\n");
    }
    if (!apply_critical_cohort_placement(ctx, moves)) {
        if (check_only) return false;
        log_error("Routed placement plan is protected, incomplete, or physically illegal; original restored.\n");
    }
    return true;
}

void prepare_routed_placement(Context *ctx)
{
    auto original = routed_placement_snapshot(ctx);
    ctx->routed_placement_origin = original.dump();
    bool legal = true;
    if (!ctx->routed_placement_plan.empty())
        legal = apply_routed_placement_plan(ctx, parse(ctx->routed_placement_plan), original, ctx->routed_placement_check_only);
    auto current = routed_placement_snapshot(ctx);
    ctx->routed_placement_current = current.dump();
    J::object placement{{"version", 1}, {"origin", original}, {"current", current}};
    if (!ctx->routed_placement_plan.empty()) {
        auto plan = parse(ctx->routed_placement_plan);
        if (plan["version"] == J(2)) placement["data_routes"] = plan["data_routes"];
    }
    if (!ctx->routed_placement_plan.empty() && parse(ctx->routed_placement_plan)["version"] == J(3))
        placement["group_plan"] = parse(ctx->routed_placement_plan);
    write(ctx, "placement.json", placement);
    if (ctx->routed_placement_check_only) {
        write(ctx, "plan-validation.json", J::object{{"version", 1}, {"legal", legal},
            {"placement_only", true}, {"route_complete", false}});
        return;
    }
    if (ctx->routed_placement_retarget_luts && !ctx->routed_placement_model.empty()) {
        struct Choice { J plan; int trial, displacement; int64_t gain; std::string cell, key; };
        std::vector<Choice> choices;
        J::array trials;
        CriticalCohortLutSearch stats;
        propose_critical_cohort_displaced_luts(ctx, original, ctx->routed_placement_model,
            ctx->routed_placement_trials, [&](const CriticalCohortLutTrial &trial) {
                auto target = routed_placement_snapshot(ctx);
                for (const char *field : {"design", "clocks", "timing_settings", "architecture", "ordering", "rng"})
                    if (target[field] != current[field]) log_error("LUT retargeting changed the logical or timing contract.\n");
                const auto name = trial.cell.str(ctx);
                for (const auto &entry : current["placement"].object_items()) {
                    auto expected = entry.second;
                    if (entry.first == name) expected = J::array{ctx->getBelName(trial.to).str(ctx), entry.second[1]};
                    if (target["placement"][entry.first] != expected)
                        log_error("LUT retargeting changed another cell or strength.\n");
                }
                const int index = int(trials.size());
                auto home = ctx->getBelLocation(trial.original), from = ctx->getBelLocation(trial.from);
                const int displacement = std::abs(home.x-from.x)+std::abs(home.y-from.y);
                trials.push_back(J::object{{"cell", name}, {"original_bel", original["placement"][name][0]},
                    {"from_bel", current["placement"][name][0]}, {"to_bel", target["placement"][name][0]},
                    {"boundary_cost_before", double(trial.boundary_before)},
                    {"boundary_cost_after", double(trial.boundary_after)}, {"displacement_tiles", displacement},
                    {"plan_index", J()},
                    {"evaluation", "full_route_required"}});
                // Source timing guides the search. Replay and route every
                // cumulative alternative from the original frozen baseline.
                auto plan = make_routed_placement_group_plan(original, original, target);
                choices.push_back({plan, index, displacement, trial.boundary_before-trial.boundary_after,
                                   name, target["placement"].dump()});
            }, stats);
        if (routed_placement_snapshot(ctx) != current)
            log_error("LUT retargeting failed to restore its complete source placement.\n");
        std::sort(choices.begin(), choices.end(), [](const Choice &a, const Choice &b) {
            if (a.displacement != b.displacement) return a.displacement > b.displacement;
            return a.gain != b.gain ? a.gain > b.gain : a.key < b.key;
        });
        J::array plans;
        std::set<std::string> cells, selected;
        auto select = [&](const Choice &choice) {
            if (plans.size() >= size_t(ctx->routed_placement_candidates) || !selected.insert(choice.key).second) return;
            auto row = trials.at(choice.trial).object_items();
            row["plan_index"] = int(plans.size()); trials.at(choice.trial) = row;
            plans.push_back(choice.plan);
        };
        for (const auto &choice : choices) if (cells.insert(choice.cell).second) select(choice);
        for (const auto &choice : choices) select(choice);
        write(ctx, "displaced-lut-trials.json", J::object{{"version", 1}, {"trials", trials},
            {"cells", stats.cells}, {"attempted", stats.attempted}, {"legal", stats.legal}, {"restored", true},
            {"attempt_limit", 128}, {"trial_limit", ctx->routed_placement_trials}});
        write(ctx, "proposals.json", J::object{{"version", 2}, {"kind", "displaced_luts"},
            {"parent", current}, {"replay_parent", original}, {"plans", plans}, {"restored", true}});
        return;
    }
    if (ctx->routed_placement_group_moves && !ctx->routed_placement_model.empty()) {
        struct Choice { J plan; int trial; int64_t gain; std::string family, key; };
        std::vector<Choice> choices;
        J::array trials;
        std::set<std::string> seen;
        propose_critical_cohort_groups(ctx, ctx->routed_placement_model, ctx->routed_placement_trials,
            [&](const CriticalCohortGroupTrial &trial) {
                auto target = routed_placement_snapshot(ctx);
                if (target["design"] != current["design"] || target["clocks"] != current["clocks"] ||
                    target["timing_settings"] != current["timing_settings"])
                    log_error("Group placement changed the logical or timing contract.\n");
                J::array seed;
                for (auto cell : trial.seed) seed.push_back(cell.str(ctx));
                int index = int(trials.size());
                trials.push_back(J::object{{"trial", index}, {"seed", seed}, {"moved_cells", trial.moved_cells},
                    {"moved_registers", trial.moved_registers}, {"boundary_cost_before", double(trial.boundary_before)},
                    {"boundary_cost_after", double(trial.boundary_after)}, {"plan_index", J()},
                    {"evaluation", "full_route_required"}});
                auto plan = make_routed_placement_group_plan(original, current, target);
                auto key = target["placement"].dump();
                if (trial.moved_registers > 0 && trial.boundary_after < trial.boundary_before &&
                    plan["moves"].array_items().size() <= 256 && seen.insert(key).second)
                    choices.push_back({plan, index, trial.boundary_before - trial.boundary_after, J(seed).dump(), key});
            });
        if (routed_placement_snapshot(ctx) != current)
            log_error("Group placement failed to restore the complete parent.\n");
        std::sort(choices.begin(), choices.end(), [](const Choice &a, const Choice &b) {
            return a.gain != b.gain ? a.gain > b.gain : a.key < b.key;
        });
        J::array plans;
        std::set<std::string> families, selected;
        auto select = [&](const Choice &choice) {
            if (plans.size() >= size_t(ctx->routed_placement_candidates) || !selected.insert(choice.key).second) return;
            auto row = trials.at(choice.trial).object_items();
            row["plan_index"] = int(plans.size()); trials.at(choice.trial) = row;
            plans.push_back(choice.plan);
        };
        for (const auto &choice : choices)
            if (families.insert(choice.family).second) select(choice);
        for (const auto &choice : choices) select(choice);
        write(ctx, "group-trials.json", J::object{{"version", 1}, {"trials", trials}, {"restored", true},
            {"trial_limit", ctx->routed_placement_trials}});
        write(ctx, "proposals.json", J::object{{"version", 2}, {"kind", "group_moves"}, {"parent", current},
            {"plans", plans}, {"restored", true}, {"trial_limit", ctx->routed_placement_trials}});
        return;
    }
    if (!ctx->routed_placement_model.empty()) {
        J::array plans, trials;
        J baseline_prediction;
        std::set<std::string> seen;
        propose_critical_cohorts(ctx, ctx->routed_placement_model, ctx->routed_placement_trials,
                                 ctx->routed_placement_candidates, [&](const CriticalCohortPrediction &prediction) {
            auto before = prediction_rows(ctx, prediction.before);
            auto after = prediction_rows(ctx, prediction.after);
            if (trials.empty())
                baseline_prediction = before;
            else if (baseline_prediction != before)
                log_error("Independent prediction baseline changed between trials.\n");
            auto proposed = routed_placement_snapshot(ctx);
            if (proposed["design"] != current["design"] || proposed["clocks"] != current["clocks"] ||
                proposed["timing_settings"] != current["timing_settings"])
                log_error("Cohort proposal changed the logical or timing contract.\n");
            std::map<std::string, J::object> changed;
            J::array removed;
            for (const auto &cell : after.object_items())
                for (const auto &pin : cell.second.object_items())
                    if (before[cell.first][pin.first] != pin.second)
                        changed[cell.first][pin.first] = pin.second;
            for (const auto &cell : before.object_items())
                for (const auto &pin : cell.second.object_items())
                    if (after[cell.first][pin.first].is_null())
                        removed.push_back(J::array{cell.first, pin.first});
            J::object changed_rows, placement;
            for (const auto &cell : changed)
                changed_rows[cell.first] = cell.second;
            for (const auto &binding : proposed["placement"].object_items())
                if (binding.second != current["placement"][binding.first])
                    placement[binding.first] = binding.second;
            J::object trial{{"trial", int(trials.size())}, {"legacy_safe", prediction.legacy_safe},
                             {"cost_complete", prediction.cost_complete}, {"cost_nodes", prediction.cost_nodes},
                             {"routing", prediction.routing_trace},
                             {"eligible", prediction.eligible}, {"placement", placement}, {"rows", changed_rows},
                             {"removed", removed}, {"before_slack_ps", double(prediction.before_slack)},
                             {"after_slack_ps", double(prediction.after_slack)}, {"plan_index", J()}};
            bool emitted = false;
            if (prediction.eligible) {
                auto plan = make_routed_placement_plan(original, current, proposed,
                                                       prediction.before_slack, prediction.after_slack,
                                                       ctx->routed_placement_preserve_routes ? prediction.routing_trace : J());
                auto identity = ctx->routed_placement_preserve_routes ?
                    J(J::array{proposed["placement"], plan["data_routes"]["nets"]}).dump() : proposed["placement"].dump();
                if (plan["moves"].array_items().size() <= 256 && seen.insert(identity).second) {
                    trial["plan_index"] = int(plans.size());
                    plans.push_back(plan);
                    emitted = true;
                }
            }
            trials.push_back(trial);
            return emitted;
        });
        if (routed_placement_snapshot(ctx) != current)
            log_error("Cohort proposal enumeration failed to restore the complete parent placement.\n");
        write(ctx, "proposals.json", J::object{{"version", 1}, {"parent", current}, {"plans", plans},
                                               {"restored", true}, {"trial_limit", ctx->routed_placement_trials}});
        write(ctx, "predictions.json", J::object{{"version", 3}, {"clock_skew", false},
                                                 {"routing_costs", ctx->routed_placement_preserve_routes},
                                                 {"baseline", baseline_prediction}, {"trials", trials}});
    }
}

void preserve_routed_placement_routes(Context *ctx)
{
    if (!ctx->routed_placement_preserve_routes || ctx->routed_placement_plan.empty())
        log_error("Route preservation requires an explicit fresh placement plan.\n");
    auto plan = parse(ctx->routed_placement_plan);
    auto parent = plan["parent"];
    auto result = apply_routed_placement_routes(ctx, parse(ctx->routed_placement_routes), parent, plan["data_routes"]);
    ctx->routed_placement_preserved_nets = result["preserved"].dump();
    if (ctx->routed_placement_preserve_branches) {
        ctx->routed_placement_branches = result["branches"].dump();
        auto summary = result.object_items();
        summary.erase("branches");
        result = summary;
    }
    if (!result["handoff"].is_null()) {
        ctx->routed_placement_handoff = result["handoff"].dump();
        write(ctx, "route-handoff.json", result["handoff"]);
        auto summary = result.object_items();
        summary.erase("handoff");
        result = summary;
    }
    write(ctx, "route-preservation.json", result);
}

void check_post_route_lut_moves(const J &receipt, const J &final)
{
    auto require = [](bool ok) {
        if (!ok) log_error("Invalid native post-route LUT move receipt.\n");
    };
    require(receipt["version"] == J(1) && receipt["parent"].is_object() &&
            receipt["target"].is_object() && receipt["moves"].is_array() &&
            receipt["moves"].array_items().size() <= 16);
    const auto &parent = receipt["parent"], &target = receipt["target"];
    for (const char *key : {"version", "device", "design", "clocks", "architecture", "ordering", "rng", "timing_settings"}) {
        require(!parent[key].is_null() && parent[key] == target[key]);
        require(target[key] == final[key]);
    }
    require(parent["placement"].is_object() && target["placement"].is_object());
    auto expected = parent["placement"].object_items();
    std::set<std::string> changed;
    for (const auto &move : receipt["moves"].array_items()) {
        require(move["cell"].is_string() && move["from_bel"].is_string() &&
                move["to_bel"].is_string() && move["strength"].is_number());
        auto name = move["cell"].string_value();
        auto cell = parent["design"]["cells"][name];
        const auto type = cell["type"].string_value();
        require(changed.insert(name).second && expected.count(name) &&
                (type == "MISTRAL_ALUT2" || type == "MISTRAL_ALUT3" || type == "MISTRAL_ALUT4" ||
                 type == "MISTRAL_ALUT5" || type == "MISTRAL_ALUT6") &&
                cell["cluster"] == J("") && cell["region"] == J("") &&
                cell["children"].is_array() && cell["children"].array_items().empty() &&
                expected.at(name) == J(J::array{move["from_bel"], move["strength"]}) &&
                !move["from_bel"].string_value().empty() && !move["to_bel"].string_value().empty() &&
                move["from_bel"] != move["to_bel"] &&
                move["strength"].number_value() <= int(STRENGTH_WEAK));
        for (const char *attr : {"keep", "dont_touch", "BEL", "FES_SLOT"})
            require(!cell["attributes"].object_items().count(attr));
        expected[name] = J::array{move["to_bel"], move["strength"]};
    }
    require(J(expected) == target["placement"] && target["placement"] == final["placement"]);
}

void finish_routed_placement(Context *ctx, bool timing_pass)
{
    if (!ctx->timing_result_is_final_analogue || ctx->routed_placement_origin.empty())
        log_error("Routed placement recording requires a completed fresh analogue route.\n");
    auto endpoints = routed_placement_endpoints(ctx);
    auto current = parse(ctx->routed_placement_current);
    auto after = routed_placement_snapshot(ctx);
    write(ctx, "routed-identity.json", after);
    const bool graph_match = current["design"] == after["design"] && current["clocks"] == after["clocks"];
    auto status = J::object{{"version", 2}, {"route_complete", ctx->routed_placement_route_complete},
                            {"analysis_complete", endpoints["analysis_complete"]}, {"timing_pass", timing_pass},
                            {"graph_matches", graph_match}, {"coverage_complete", endpoints["complete"]},
                            {"process_complete", false}};
    if (!ctx->post_route_lut_move_receipt.empty()) {
        const auto receipt = parse(ctx->post_route_lut_move_receipt);
        check_post_route_lut_moves(receipt, after);
        // The original placement precedes routing's native route-through
        // insertion. The receipt separately compares exact post-route order
        // and RNG state on both sides of the LUT-only refinement.
        for (const char *key : {"design", "placement", "clocks", "architecture"})
            if (receipt["parent"][key] != current[key])
                log_error("Post-route LUT move parent does not match the recorded placement.\n");
        write(ctx, "post-route-lut-moves.json", receipt);
        status["post_route_lut_moves_match"] = true;
        status["post_route_lut_moves"] = int(receipt["moves"].array_items().size());
    }
    if (ctx->routed_placement_group_moves) {
        if (current["placement"] != after["placement"])
            log_error("Group placement bindings changed during native routing.\n");
        status["group_placement_match"] = true;
    }
    if (ctx->routed_placement_preserve_routes) {
        if (!ctx->routed_placement_routes.empty()) {
            std::string error;
            auto preserved = J::parse(ctx->routed_placement_preserved_nets, error);
            if (!error.empty()) log_error("Missing preserved-route inventory.\n");
            check_routed_placement_routes(ctx, parse(ctx->routed_placement_routes), preserved);
        }
        status["preserved_routes_match"] = true;
        if (ctx->routed_placement_preserve_branches) {
            auto parent = ctx->routed_placement_plan.empty() ? current : parse(ctx->routed_placement_plan)["parent"];
            auto branches = ctx->routed_placement_branches.empty() ?
                J(J::object{{"version", 1}, {"parent", parent}, {"target", current}, {"nets", J::object{}}}) :
                parse(ctx->routed_placement_branches);
            check_routed_placement_branches(ctx, branches, parent, current);
            write(ctx, "route-branches.json", branches);
            status["preserved_branches_match"] = true;
        }
        if (!ctx->routed_placement_plan.empty() && parse(ctx->routed_placement_plan)["version"] == J(2)) {
            if (ctx->routed_placement_handoff.empty()) log_error("Missing proposed route handoff witness.\n");
            auto handed = parse(ctx->routed_placement_handoff);
            J::array names;
            for (const auto &entry : handed["nets"].object_items()) names.push_back(entry.first);
            check_routed_placement_routes(ctx, handed, names);
            status["proposed_routes_match"] = true;
        }
    }
    ctx->routed_placement_status = J(status).dump();
    write(ctx, "endpoints.json", endpoints);
    write(ctx, "status.json", status);
    std::filesystem::path model = std::filesystem::path(ctx->routed_placement_directory) / "model.json";
    std::ofstream stream(model);
    write_critical_cohort_route_model(ctx, stream);
    if (ctx->routed_placement_preserve_routes)
        write(ctx, "routes.json", snapshot_routed_placement_routes(ctx, current));
    log_info("Routed placement record: graph=%d coverage=%d supported=%zu unsupported=%zu.\n", int(graph_match),
             int(endpoints["complete"].bool_value()), endpoints["endpoints"].array_items().size(),
             endpoints["unsupported"].array_items().size());
}

NEXTPNR_NAMESPACE_END
