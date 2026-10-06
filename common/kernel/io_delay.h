#ifndef NEXTPNR_IO_DELAY_H
#define NEXTPNR_IO_DELAY_H

#include <cmath>
#include <limits>
#include "log.h"
#include "json11.hpp"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// Store names and nanoseconds in settings so packed/routed JSON retains the
// constraints without depending on cell names chosen by an architecture packer.
inline json11::Json::array read_io_delays(const Context *ctx)
{
    auto it = ctx->settings.find(ctx->id("timing/io_delays"));
    if (it == ctx->settings.end())
        return {};
    std::string error;
    auto value = json11::Json::parse(it->second.as_string(), error);
    if (!error.empty() || !value.is_array())
        log_error("Invalid saved IO delay constraints.\n");
    return value.array_items();
}

inline NetInfo *io_delay_clock(Context *ctx, const std::string &name)
{
    IdString id = ctx->id(name);
    if (ctx->net_aliases.count(id))
        return ctx->getNetByAlias(id);
    auto net = ctx->nets.find(id);
    return net == ctx->nets.end() ? nullptr : net->second.get();
}

inline delay_t io_delay_value(Context *ctx, const json11::Json &value)
{
    // Leave ample headroom for adding routing/clock delays and negating output
    // delays. Do not let malformed checkpoint metadata overflow architecture units.
    double limit = double(ctx->getDelayNS(std::numeric_limits<delay_t>::max())) / 4;
    if (!value.is_number() || !std::isfinite(value.number_value()) || std::abs(value.number_value()) >= limit)
        log_error("IO delay must be a finite, representable number of nanoseconds.\n");
    return ctx->getDelayFromNS(value.number_value());
}

inline void restore_io_clocks(Context *ctx)
{
    auto it = ctx->settings.find(ctx->id("timing/io_clocks"));
    if (it == ctx->settings.end()) return;
    std::string error;
    auto value = json11::Json::parse(it->second.as_string(), error);
    if (!error.empty() || !value.is_array()) log_error("Invalid saved IO clock constraints.\n");
    for (const auto &row : value.array_items()) {
        if (!row["net"].is_string() || !row["group"].is_string())
            log_error("Invalid saved IO clock name or phase group.\n");
        auto *net = io_delay_clock(ctx, row["net"].string_value());
        if (!net) log_error("Saved IO clock net is missing.\n");
        if (net->clkconstr) continue;
        auto clock = std::make_unique<ClockConstraint>();
        auto pair = [&](const char *key) {
            const auto &data = row[key].array_items();
            if (data.size() != 2) log_error("Invalid saved IO clock interval.\n");
            auto lo = io_delay_value(ctx, data[0]), hi = io_delay_value(ctx, data[1]);
            if (lo <= 0 || lo > hi) log_error("Invalid saved IO clock interval.\n");
            return DelayPair(lo, hi);
        };
        clock->period = pair("period"); clock->high = pair("high"); clock->low = pair("low");
        clock->phase_group = ctx->id(row["group"].string_value());
        clock->phase_shift = io_delay_value(ctx, row["phase"]);
        net->clkconstr = std::move(clock);
    }
}

NEXTPNR_NAMESPACE_END
#endif
