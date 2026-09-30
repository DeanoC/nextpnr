/* Current placed-path validation for optional remapping. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_REMAP_REPORT_H
#define MISTRAL_REMAP_REPORT_H
#include "nextpnr.h"
#include "json11.hpp"
#include "log.h"
#include <algorithm>
#include <cmath>

NEXTPNR_NAMESPACE_BEGIN
namespace mistral_remap_report {
struct Path {
    std::vector<std::pair<PortRef, PortRef>> edges;
    double excess;
};

inline int lut_width(IdString type)
{
    if (type == id_MISTRAL_ALUT2) return 2;
    if (type == id_MISTRAL_ALUT3) return 3;
    if (type == id_MISTRAL_ALUT4) return 4;
    if (type == id_MISTRAL_ALUT5) return 5;
    if (type == id_MISTRAL_ALUT6) return 6;
    return 0;
}

// Check every relevant violated path before a caller mutates any graph edge.
// Clock skew is separate from the continuous data path; setup must terminate
// the final routing edge at the actual registered input.
inline std::vector<Path> validate(Context *ctx, const json11::Json &json, bool include_data)
{
    if (!json["critical_paths"].is_array()) log_error("Remapping needs a valid timing report.\n");
    auto same = [](PortRef a, PortRef b) { return a.cell == b.cell && a.port == b.port; };
    auto endpoint = [&](const json11::Json &value) -> PortRef {
        if (!value["cell"].is_string() || !value["port"].is_string() || value["loc"].array_items().size() != 2)
            log_error("Malformed remap path endpoint.\n");
        auto found = ctx->cells.find(ctx->id(value["cell"].string_value()));
        if (found == ctx->cells.end()) log_error("Stale remap report cell.\n");
        auto cell = found->second.get();
        auto pin = ctx->id(value["port"].string_value());
        const auto &loc = value["loc"].array_items();
        if (!cell->ports.count(pin) || cell->bel == BelId() || !loc[0].is_number() || !loc[1].is_number())
            log_error("Stale remap report port or placement.\n");
        auto actual = ctx->getBelLocation(cell->bel);
        if (loc[0].number_value() != actual.x || loc[1].number_value() != actual.y)
            log_error("Stale remap report placement.\n");
        return {cell, pin};
    };
    const std::vector<IdString> pins = {id_A, id_B, id_C, id_D, id_E, id_F};
    std::vector<Path> result;
    for (const auto &path : json["critical_paths"].array_items()) {
        const auto &segments = path["path"].array_items();
        if (segments.empty()) continue;
        const auto &last = segments.back();
        auto port = last["to"]["port"].string_value();
        if (last["type"].string_value() != "setup" || (port != "ENA" && (!include_data || port != "DATAIN"))) continue;
        if (!path["max_delay"].is_number() || !std::isfinite(path["max_delay"].number_value()) ||
            path["max_delay"].number_value() <= 0) log_error("Malformed remap path constraint.\n");
        double delay = 0;
        for (const auto &segment : segments) {
            if (!segment["delay"].is_number() || !std::isfinite(segment["delay"].number_value()))
                log_error("Malformed remap path delay.\n");
            delay += segment["delay"].number_value();
        }
        if (!std::isfinite(delay)) log_error("Malformed remap path total delay.\n");
        if (delay <= path["max_delay"].number_value()) continue;
        Path checked;
        checked.excess = delay - path["max_delay"].number_value();
        PortRef previous, skew_launch, skew_capture;
        bool data_started = false, setup_seen = false, skew_seen = false, registered_launch = false;
        auto clock_matches = [&](PortRef data, PortRef clock, int count) {
            if (data.cell != clock.cell) return false;
            for (int index = 0; index < count; ++index)
                if (ctx->getPortClockingInfo(data.cell, data.port, index).clock_port == clock.port) return true;
            return false;
        };
        for (const auto &segment : segments) {
            auto from = endpoint(segment["from"]), to = endpoint(segment["to"]);
            const auto &type = segment["type"].string_value();
            if (type == "clk-skew") {
                int a = 0, b = 0;
                if (data_started || setup_seen || skew_seen ||
                    ctx->getPortTimingClass(from.cell, from.port, a) != TMG_CLOCK_INPUT ||
                    ctx->getPortTimingClass(to.cell, to.port, b) != TMG_CLOCK_INPUT)
                    log_error("Malformed remap clock-skew segment.\n");
                skew_seen = true;
                skew_launch = from;
                skew_capture = to;
                continue;
            }
            if (setup_seen || (data_started && !same(previous, from))) log_error("Disconnected remap report path.\n");
            if (type == "clk-to-q") {
                int clocks = 0;
                if (data_started || !same(from, to) ||
                    ctx->getPortTimingClass(from.cell, from.port, clocks) != TMG_REGISTER_OUTPUT || clocks <= 0)
                    log_error("Malformed remap clock-to-Q segment.\n");
                if (skew_seen && !clock_matches(from, skew_launch, clocks))
                    log_error("Remap clock skew does not match the launch register.\n");
                registered_launch = true;
            } else if (type == "logic") {
                int width = lut_width(from.cell->type), a = 0, b = 0;
                if (from.cell != to.cell ||
                    ctx->getPortTimingClass(from.cell, from.port, a) != TMG_COMB_INPUT ||
                    ctx->getPortTimingClass(to.cell, to.port, b) != TMG_COMB_OUTPUT ||
                    (width && (to.port != id_Q || std::find(pins.begin(), pins.begin() + width, from.port) == pins.begin() + width)))
                    log_error("Malformed remap logic segment.\n");
            } else if (type == "setup") {
                int clocks = 0;
                if (!data_started || checked.edges.empty() || !same(from, to) || !same(checked.edges.back().second, to) ||
                    to.cell->type != id_MISTRAL_FF || (to.port != id_ENA && (!include_data || to.port != id_DATAIN)) ||
                    ctx->getPortTimingClass(to.cell, to.port, clocks) != TMG_REGISTER_INPUT || clocks <= 0)
                    log_error("Malformed remap setup endpoint.\n");
                if (skew_seen && (!registered_launch || !clock_matches(to, skew_capture, clocks)))
                    log_error("Remap clock skew does not match the capture register.\n");
                setup_seen = true;
            } else if (type != "routing") {
                log_error("Unsupported remap path segment.\n");
            }
            previous = to;
            data_started = true;
            if (type != "routing") continue;
            auto net = from.cell->getPort(from.port);
            if (!net || net != to.cell->getPort(to.port) || net->driver.cell != from.cell || net->driver.port != from.port ||
                to.cell->ports.at(to.port).type != PORT_IN || segment["net"].string_value() != net->name.str(ctx))
                log_error("Stale remap report edge.\n");
            checked.edges.emplace_back(from, to);
        }
        result.push_back(std::move(checked));
    }
    return result;
}
} // namespace mistral_remap_report
NEXTPNR_NAMESPACE_END
#endif
