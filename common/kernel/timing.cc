/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  gatecat <gatecat@ds0.me>
 *  Copyright (C) 2018  Eddie Hung <eddieh@ece.ubc.ca>
 *  Copyright (C) 2023  rowanG077 <goemansrowan@gmail.com>
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

#include "timing.h"
#include <algorithm>
#include <boost/range/adaptor/reversed.hpp>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

// Opposite edges are separated by the selected high/low phase, not always
// half a period. Use the same interval for placement slack and reporting.
static delay_t clock_period(const Context *ctx, IdString clock)
{
    auto net = ctx->nets.find(clock);
    if (net != ctx->nets.end() && net->second->clkconstr)
        return net->second->clkconstr->period.minDelay();
    return ctx->getDelayFromNS(1.0e9 / ctx->setting<float>("target_freq"));
}

static delay_t clock_interval(const Context *ctx, IdString clock, ClockEdge launch, ClockEdge capture)
{
    if (launch == capture) return clock_period(ctx, clock);
    auto net = ctx->nets.find(clock);
    if (net != ctx->nets.end() && net->second->clkconstr)
        return launch == RISING_EDGE ? net->second->clkconstr->high.minDelay() :
                                       net->second->clkconstr->low.minDelay();
    return clock_period(ctx, clock) / 2;
}

bool phase_related_clocks(const Context *ctx, IdString launch, IdString capture)
{
    auto a = ctx->nets.find(launch), b = ctx->nets.find(capture);
    if (a == ctx->nets.end() || b == ctx->nets.end() || !a->second->clkconstr || !b->second->clkconstr)
        return false;
    const auto &ca = *a->second->clkconstr, &cb = *b->second->clkconstr;
    return ca.phase_group != IdString() && ca.phase_group == cb.phase_group && ca.period.minDelay() > 0 &&
           ca.period.minDelay() == cb.period.minDelay() && ca.period.maxDelay() == cb.period.maxDelay();
}

static bool timed_clocks(const Context *ctx, IdString launch, IdString capture)
{
    // SDC set_clock_groups and clock-level set_false_path cut a pair.
    if (ctx->sdc_clock_false(launch, capture))
        return false;
    return launch == capture || phase_related_clocks(ctx, launch, capture);
}

// Extra setup time granted by a clock-level set_multicycle_path -setup N:
// N - 1 periods of the capture clock (or of the launch clock with -start).
static delay_t multicycle_extra(const Context *ctx, IdString launch, IdString capture)
{
    const auto *exception = ctx->sdc_clock_multicycle(launch, capture);
    if (exception == nullptr || exception->setup_multiplier <= 1)
        return 0;
    IdString clock = exception->start ? launch : capture;
    // A clock with no create_clock (and no generated constraint) has a null
    // clkconstr. clock_period falls back to the target frequency.
    return (exception->setup_multiplier - 1) * clock_period(ctx, clock);
}

static delay_t clock_interval(const Context *ctx, IdString launch, IdString capture, ClockEdge launch_edge,
                              ClockEdge capture_edge)
{
    if (launch == capture)
        return clock_interval(ctx, launch, launch_edge, capture_edge);
    NPNR_ASSERT(phase_related_clocks(ctx, launch, capture));
    const auto &ca = *ctx->nets.at(launch)->clkconstr, &cb = *ctx->nets.at(capture)->clkconstr;
    delay_t interval = cb.phase_shift - ca.phase_shift;
    if (capture_edge == FALLING_EDGE)
        interval += cb.high.minDelay();
    if (launch_edge == FALLING_EDGE)
        interval -= ca.high.minDelay();
    const delay_t period = ca.period.minDelay();
    while (interval <= 0)
        interval += period;
    while (interval > period)
        interval -= period;
    return interval;
}

TimingAnalyser::TimingAnalyser(Context *ctx) : ctx(ctx)
{
    ClockDomainKey key{IdString(), ClockEdge::RISING_EDGE};
    domain_to_id.emplace(key, 0);
    domains.emplace_back(key);
    async_clock_id = 0;
};

void TimingAnalyser::setup(bool update_net_timings, bool update_histogram, bool update_crit_paths)
{
    analysis_complete = false;
    init_ports();
    get_cell_delays();
    topo_sort();
    setup_port_domains();
    identify_related_domains();
    run(true, update_net_timings, update_histogram, update_crit_paths);
}

void TimingAnalyser::run(bool update_route_delays, bool update_net_timings, bool update_histogram,
                         bool update_crit_paths)
{
    analysis_complete = false;
    reset_times();
    if (update_route_delays)
        get_route_delays();
    walk_forward();
    walk_backward();
    compute_slack();
    compute_criticality();

    // Ensure we clear all timing results if any of them has been marked as
    // as to be updated. This is done so we ensure it's not possible to have
    // timing_result which contains mixed reports
    if (update_net_timings || update_histogram || update_crit_paths) {
        result = TimingResult();
    }

    if (update_net_timings) {
        build_detailed_net_timing_report();
    }

    if (update_histogram) {
        build_slack_histogram_report();
    }

    if (update_crit_paths) {
        build_crit_path_reports();
    }
    analysed_clock_skew = with_clock_skew;
    analysed_setup_only = setup_only;
    analysis_complete = true;
}

void TimingAnalyser::init_ports()
{
    // Per cell port structures
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        for (auto &port : ci->ports) {
            auto &data = ports[CellPortKey(ci->name, port.first)];
            data.type = port.second.type;
            data.cell_port = CellPortKey(ci->name, port.first);
        }
    }
}

void TimingAnalyser::get_cell_delays()
{
    auto async_clk_key = domains.at(async_clock_id);

    for (auto &port : ports) {
        CellInfo *ci = cell_info(port.first);
        auto &pi = port_info(port.first);
        auto &pd = port.second;

        IdString name = port.first.port;
        // Ignore dangling ports altogether for timing purposes
        if (!pi.net)
            continue;
        pd.cell_arcs.clear();
        int clkInfoCount = 0;
        TimingPortClass cls = ctx->getPortTimingClass(ci, name, clkInfoCount);
        if (cls == TMG_CLOCK_INPUT || cls == TMG_GEN_CLOCK || cls == TMG_IGNORE)
            continue;
        if (pi.type == PORT_IN) {
            // Input ports might have setup/hold relationships
            if (cls == TMG_REGISTER_INPUT) {
                for (int i = 0; i < clkInfoCount; i++) {
                    auto info = ctx->getPortClockingInfo(ci, name, i);
                    if (!ci->ports.count(info.clock_port) || ci->ports.at(info.clock_port).net == nullptr)
                        continue;
                    pd.cell_arcs.emplace_back(CellArc::SETUP, info.clock_port, DelayQuad(info.setup, info.setup),
                                              info.edge);
                    pd.cell_arcs.emplace_back(CellArc::HOLD, info.clock_port, DelayQuad(info.hold, info.hold),
                                              info.edge);
                }
            }
            // asynchronous endpoint
            else if (cls == TMG_ENDPOINT) {
                pd.cell_arcs.emplace_back(CellArc::ENDPOINT, async_clk_key.key.clock, DelayQuad{});
            }
            // Combinational delays through cell
            for (auto &other_port : ci->ports) {
                auto &op = other_port.second;
                // ignore dangling ports and non-outputs
                if (op.net == nullptr || op.type != PORT_OUT)
                    continue;
                DelayQuad delay;
                bool is_path = ctx->getCellDelay(ci, name, other_port.first, delay);
                if (is_path)
                    pd.cell_arcs.emplace_back(CellArc::COMBINATIONAL, other_port.first, delay);
            }
        } else if (pi.type == PORT_OUT) {
            // Output ports might have clk-to-q relationships
            if (cls == TMG_REGISTER_OUTPUT) {
                for (int i = 0; i < clkInfoCount; i++) {
                    auto info = ctx->getPortClockingInfo(ci, name, i);
                    if (!ci->ports.count(info.clock_port) || ci->ports.at(info.clock_port).net == nullptr)
                        continue;
                    pd.cell_arcs.emplace_back(CellArc::CLK_TO_Q, info.clock_port, info.clockToQ, info.edge);
                }
            }
            // Asynchronous startpoint
            else if (cls == TMG_STARTPOINT) {
                pd.cell_arcs.emplace_back(CellArc::STARTPOINT, async_clk_key.key.clock, DelayQuad{});
            }
            // Combinational delays through cell
            for (auto &other_port : ci->ports) {
                auto &op = other_port.second;
                // ignore dangling ports and non-inputs
                if (op.net == nullptr || op.type != PORT_IN)
                    continue;
                DelayQuad delay;
                bool is_path = ctx->getCellDelay(ci, other_port.first, name, delay);
                if (is_path)
                    pd.cell_arcs.emplace_back(CellArc::COMBINATIONAL, other_port.first, delay);
            }
        }
    }
}

void TimingAnalyser::get_route_delays()
{
    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        if (ni->driver.cell == nullptr || ni->driver.cell->bel == BelId())
            continue;

        for (auto &usr : ni->users) {
            if (usr.cell->bel == BelId())
                continue;
            ports.at(CellPortKey(usr)).route_delay = DelayPair(ctx->getNetinfoRouteDelay(ni, usr));
        }
    }
}

void TimingAnalyser::set_route_delay(CellPortKey port, DelayPair value) { ports.at(port).route_delay = value; }

void TimingAnalyser::topo_sort()
{
    TopoSort<CellPortKey> topo;
    for (auto &port : ports) {
        auto &pd = port.second;
        // All ports are nodes
        topo.node(port.first);
        if (pd.type == PORT_IN) {
            // inputs: combinational arcs through the cell are edges
            for (auto &arc : pd.cell_arcs) {
                if (arc.type != CellArc::COMBINATIONAL)
                    continue;
                topo.edge(port.first, CellPortKey(port.first.cell, arc.other_port));
            }
        } else if (pd.type == PORT_OUT) {
            // output: routing arcs are edges
            const NetInfo *pn = port_info(port.first).net;
            if (pn != nullptr) {
                for (auto &usr : pn->users)
                    topo.edge(port.first, CellPortKey(usr));
            }
        }
    }

    bool ignore_loops = bool_or_default(ctx->settings, ctx->id("timing/ignoreLoops"), false);
    bool no_loops = topo.sort();
    if (!no_loops && !ignore_loops) {
        log_info("Found %d combinational loops:\n", int(topo.loops.size()));
        int i = 0;
        for (auto &loop : topo.loops) {
            log_info("    loop %d:\n", ++i);
            for (auto &port : loop) {
                log_info("        %s.%s (%s)\n", ctx->nameOf(port.cell), ctx->nameOf(port.port),
                         ctx->nameOf(port_info(port).net));
            }
        }

        if (ctx->force)
            log_warning("Timing analysis failed due to combinational loops.\n");
        else
            log_error("Timing analysis failed due to combinational loops.\n");
    }
    have_loops = !no_loops;
    std::swap(topological_order, topo.sorted);
}

void TimingAnalyser::setup_port_domains()
{
    for (auto &d : domains) {
        d.startpoints.clear();
        d.endpoints.clear();
    }
    bool first_iter = true;
    do {
        // Go forward through the topological order (domains from the PoV of arrival time)
        updated_domains = false;
        for (auto port : topological_order) {
            auto &pd = ports.at(port);
            auto &pi = port_info(port);
            if (pi.type == PORT_OUT) {
                if (first_iter) {
                    for (auto &fanin : pd.cell_arcs) {
                        domain_id_t dom;
                        // registered outputs are startpoints
                        if (fanin.type == CellArc::CLK_TO_Q)
                            dom = domain_id(port.cell, fanin.other_port, fanin.edge);
                        else if (fanin.type == CellArc::STARTPOINT)
                            dom = async_clock_id;
                        else
                            continue;
                        // create per-domain data
                        pd.arrival[dom];
                        domains.at(dom).startpoints.emplace_back(port, fanin.other_port);
                    }
                }
                // copy domains across routing
                if (pi.net != nullptr)
                    for (auto &usr : pi.net->users)
                        copy_domains(port, CellPortKey(usr), false);
            } else {
                // copy domains from input to output
                for (auto &fanout : pd.cell_arcs) {
                    if (fanout.type != CellArc::COMBINATIONAL)
                        continue;
                    copy_domains(port, CellPortKey(port.cell, fanout.other_port), false);
                }
            }
        }
        // Go backward through the topological order (domains from the PoV of required time)
        for (auto port : reversed_range(topological_order)) {
            auto &pd = ports.at(port);
            auto &pi = port_info(port);
            if (pi.type == PORT_OUT) {
                // copy domains from output to input
                for (auto &fanin : pd.cell_arcs) {
                    if (fanin.type != CellArc::COMBINATIONAL)
                        continue;
                    copy_domains(port, CellPortKey(port.cell, fanin.other_port), true);
                }
            } else {
                if (first_iter) {
                    for (auto &fanout : pd.cell_arcs) {
                        domain_id_t dom;
                        // registered inputs are endpoints
                        if (fanout.type == CellArc::SETUP)
                            dom = domain_id(port.cell, fanout.other_port, fanout.edge);
                        else if (fanout.type == CellArc::ENDPOINT)
                            dom = async_clock_id;
                        else
                            continue;
                        // create per-domain data
                        pd.required[dom];
                        domains.at(dom).endpoints.emplace_back(port, fanout.other_port);
                    }
                }
                // copy port to driver
                if (pi.net != nullptr && pi.net->driver.cell != nullptr)
                    copy_domains(port, CellPortKey(pi.net->driver), true);
            }
        }
        // Iterate over ports and find domain pairs
        for (auto port : topological_order) {
            auto &pd = ports.at(port);
            for (auto &arr : pd.arrival)
                for (auto &req : pd.required) {
                    pd.domain_pairs[domain_pair_id(arr.first, req.first)];
                }
        }
        first_iter = false;
        // If there are loops, repeat the process until a fixed point is reached, as there might be unusual ways to
        // visit points, which would result in a missing domain key and therefore crash later on
    } while (have_loops && updated_domains);
    for (auto &dp : domain_pairs) {
        auto &launch_data = domains.at(dp.key.launch);
        auto &capture_data = domains.at(dp.key.capture);
        dp.timed = timed_clocks(ctx, launch_data.key.clock, capture_data.key.clock);
        if (!dp.timed)
            continue;
        dp.multicycle_extra = multicycle_extra(ctx, launch_data.key.clock, capture_data.key.clock);
        dp.period = DelayPair(clock_interval(ctx, launch_data.key.clock, capture_data.key.clock, launch_data.key.edge,
                                             capture_data.key.edge) +
                              dp.multicycle_extra);
    }
}

void TimingAnalyser::identify_related_domains()
{

    // Identify clock nets
    pool<IdString> clock_nets;
    for (const auto &domain : domains) {
        clock_nets.insert(domain.key.clock);
    }

    // For each clock net identify all nets that can possibly drive it. Compute
    // cumulative delays to each of them.
    std::function<void(const NetInfo *, pool<IdString> &, dict<IdString, delay_t> &, delay_t)> find_net_drivers =
            [&](const NetInfo *ni, pool<IdString> &net_trace, dict<IdString, delay_t> &drivers, delay_t delay_acc) {
                // Get driving cell and port
                if (ni == nullptr)
                    return;
                const CellInfo *cell = ni->driver.cell;
                if (cell == nullptr)
                    return;

                const IdString port = ni->driver.port;

                bool didGoUpstream = false;

                // Ring oscillator driving the net
                if (net_trace.find(ni->name) != net_trace.end()) {
                    drivers[ni->name] = delay_acc;
                    return;
                }
                net_trace.insert(ni->name);

                // The cell has only one port
                if (cell->ports.size() == 1) {
                    drivers[ni->name] = delay_acc;
                    return;
                }

                // Get the driver timing class
                int info_count = 0;
                auto timing_class = ctx->getPortTimingClass(cell, port, info_count);

                // The driver must be a combinational output
                if (timing_class != TMG_COMB_OUTPUT) {
                    drivers[ni->name] = delay_acc;
                    return;
                }

                // Recurse upstream through all input ports that have combinational
                // paths to this driver
                for (const auto &it : cell->ports) {
                    const auto &pi = it.second;

                    // Only connected inputs
                    if (pi.type != PORT_IN) {
                        continue;
                    }
                    if (pi.net == nullptr) {
                        continue;
                    }

                    // The input must be a combinational input
                    timing_class = ctx->getPortTimingClass(cell, pi.name, info_count);
                    if (timing_class != TMG_COMB_INPUT) {
                        continue;
                    }
                    // There must be a combinational arc
                    DelayQuad delay;
                    if (!ctx->getCellDelay(cell, pi.name, port, delay)) {
                        continue;
                    }

                    // Recurse
                    find_net_drivers(pi.net, net_trace, drivers, delay_acc + delay.maxDelay());
                    didGoUpstream = true;
                }

                // Did not propagate upstream through the cell, mark the net as driver
                if (!didGoUpstream) {
                    drivers[ni->name] = delay_acc;
                }
            };

    // Identify possible drivers for each clock domain
    dict<IdString, dict<IdString, delay_t>> clock_drivers;
    for (const auto &domain : domains) {
        if (domain.key.is_async())
            continue;

        const NetInfo *ni = ctx->nets.at(domain.key.clock).get();
        if (ni == nullptr)
            continue;
        if (ni->driver.cell == nullptr)
            continue;

        dict<IdString, delay_t> drivers;
        pool<IdString> net_trace;
        find_net_drivers(ni, net_trace, drivers, 0);

        clock_drivers[domain.key.clock] = drivers;

        if (ctx->debug) {
            log("Clock '%s' can be driven by:\n", domain.key.clock.str(ctx).c_str());
            for (const auto &it : drivers) {
                const NetInfo *net = ctx->nets.at(it.first).get();
                log(" %s.%s delay %.3fns\n", net->driver.cell->name.str(ctx).c_str(), net->driver.port.str(ctx).c_str(),
                    ctx->getDelayNS(it.second));
            }
        }
    }

    // Identify related clocks. For simplicity do it both for A->B and B->A
    // cases.
    for (const auto &c1 : clock_drivers) {
        for (const auto &c2 : clock_drivers) {

            if (c1 == c2) {
                continue;
            }

            // Make an intersection of the two drivers sets
            pool<IdString> common_drivers;
            for (const auto &it : c1.second) {
                common_drivers.insert(it.first);
            }
            for (const auto &it : c2.second) {
                common_drivers.insert(it.first);
            }

            for (auto it = common_drivers.begin(); it != common_drivers.end();) {
                if (!c1.second.count(*it) || !c2.second.count(*it)) {
                    it = common_drivers.erase(it);
                } else {
                    ++it;
                }
            }

            if (ctx->debug) {

                log("Possible common driver(s) for clocks '%s' and '%s'\n", c1.first.str(ctx).c_str(),
                    c2.first.str(ctx).c_str());

                for (const auto &it : common_drivers) {

                    const NetInfo *ni = ctx->nets.at(it).get();
                    const CellInfo *cell = ni->driver.cell;
                    const IdString port = ni->driver.port;

                    log(" net '%s', cell %s (%s), port %s\n", it.str(ctx).c_str(), cell->name.str(ctx).c_str(),
                        cell->type.str(ctx).c_str(), port.str(ctx).c_str());
                }
            }

            // If there is no single driver then consider the two clocks
            // unrelated.
            if (common_drivers.size() != 1) {
                continue;
            }

            // Store how much later c1 arrives than c2. Paths are looked up as
            // (launch, capture), and the value is added to their delay: a late
            // launch clock makes the path longer, a late capture clock shorter.
            auto driver = *common_drivers.begin();
            auto delay = c1.second.at(driver) - c2.second.at(driver);
            clock_delays[std::make_pair(c1.first, c2.first)] = delay;
        }
    }
}

void TimingAnalyser::reset_times()
{
    static const auto init_delay =
            DelayPair(std::numeric_limits<delay_t>::max(), std::numeric_limits<delay_t>::lowest());
    for (auto &port : ports) {
        auto do_reset = [&](dict<domain_id_t, ArrivReqTime> &times) {
            for (auto &t : times) {
                t.second.value = init_delay;
                t.second.path_length = 0;
                t.second.bwd_min = CellPortKey();
                t.second.bwd_max = CellPortKey();
            }
        };
        do_reset(port.second.arrival);
        do_reset(port.second.required);
        for (auto &dp : port.second.domain_pairs) {
            dp.second.setup_slack = std::numeric_limits<delay_t>::max();
            dp.second.hold_slack = std::numeric_limits<delay_t>::max();
            dp.second.max_path_length = 0;
            dp.second.criticality = 0;
        }
        port.second.worst_crit = 0;
        port.second.worst_setup_slack = std::numeric_limits<delay_t>::max();
        port.second.worst_hold_slack = std::numeric_limits<delay_t>::max();
    }
}

void TimingAnalyser::set_arrival_time(CellPortKey target, domain_id_t domain, DelayPair arrival, int path_length,
                                      CellPortKey prev)
{
    auto &arr = ports.at(target).arrival.at(domain);
    if (arrival.max_delay > arr.value.max_delay) {
        arr.value.max_delay = arrival.max_delay;
        arr.bwd_max = prev;
    }
    if (!setup_only && (arrival.min_delay < arr.value.min_delay)) {
        arr.value.min_delay = arrival.min_delay;
        arr.bwd_min = prev;
    }
    arr.path_length = std::max(arr.path_length, path_length);
}

void TimingAnalyser::set_required_time(CellPortKey target, domain_id_t domain, DelayPair required, int path_length,
                                       CellPortKey prev)
{
    auto &req = ports.at(target).required.at(domain);
    if (required.min_delay < req.value.min_delay) {
        req.value.min_delay = required.min_delay;
        req.bwd_min = prev;
    }
    if (!setup_only && (required.max_delay > req.value.max_delay)) {
        req.value.max_delay = required.max_delay;
        req.bwd_max = prev;
    }
    req.path_length = std::max(req.path_length, path_length);
}

void TimingAnalyser::walk_forward()
{
    // Assign initial arrival time to domain startpoints
    for (domain_id_t dom_id = 0; dom_id < domain_id_t(domains.size()); ++dom_id) {
        auto &dom = domains.at(dom_id);
        for (auto &sp : dom.startpoints) {
            auto &pd = ports.at(sp.first);
            DelayPair init_arrival(0);
            CellPortKey clock_key;
            if (sp.second != IdString()) {
                // clocked startpoints have a clock-to-out time
                for (auto &fanin : pd.cell_arcs) {
                    if (fanin.type == CellArc::CLK_TO_Q && fanin.other_port == sp.second) {
                        init_arrival += fanin.value.delayPair();
                        // Include the clock delay if clock_skew analysis is enabled
                        if (with_clock_skew) {
                            init_arrival += ports.at(CellPortKey(sp.first.cell, fanin.other_port)).route_delay;
                        }
                        break;
                    }
                }
                clock_key = CellPortKey(sp.first.cell, sp.second);
            }
            set_arrival_time(sp.first, dom_id, init_arrival, 1, clock_key);
        }
    }
    // Walk forward in topological order
    for (auto p : topological_order) {
        auto &pd = ports.at(p);
        for (auto &arr : pd.arrival) {
            if (pd.type == PORT_OUT) {
                // Output port: propagate delay through net, adding route delay
                NetInfo *net = port_info(p).net;
                if (net != nullptr)
                    for (auto &usr : net->users) {
                        CellPortKey usr_key(usr);
                        auto &usr_pd = ports.at(usr_key);
                        auto next_arr = arr.second.value + usr_pd.route_delay;
                        set_arrival_time(usr_key, arr.first, next_arr, arr.second.path_length, p);
                    }
            } else if (pd.type == PORT_IN) {
                // Input port; propagate delay through cell, adding combinational delay
                for (auto &fanout : pd.cell_arcs) {
                    if (fanout.type != CellArc::COMBINATIONAL)
                        continue;

                    auto next_arr = arr.second.value + fanout.value.delayPair();
                    set_arrival_time(CellPortKey(p.cell, fanout.other_port), arr.first, next_arr,
                                     arr.second.path_length + 1, p);
                }
            }
        }
    }
}

void TimingAnalyser::walk_backward()
{
    // Assign initial required time to domain endpoints
    // Note that clock frequency will be considered later in the analysis for, for now all required times are normalised
    // to 0ns
    for (domain_id_t dom_id = 0; dom_id < domain_id_t(domains.size()); ++dom_id) {
        auto &dom = domains.at(dom_id);
        for (auto &ep : dom.endpoints) {
            auto &pd = ports.at(ep.first);
            DelayPair init_required(0);
            CellPortKey clock_key;
            // TODO: clock routing delay, if analysis of that is enabled
            if (ep.second != IdString()) {
                // Add setup/hold time, if this endpoint is clocked
                for (auto &fanin : pd.cell_arcs) {

                    if (fanin.type == CellArc::SETUP && fanin.other_port == ep.second) {
                        if (with_clock_skew) {
                            init_required += ports.at(CellPortKey(ep.first.cell, fanin.other_port)).route_delay;
                        }
                        init_required.min_delay -= fanin.value.maxDelay();
                    }
                    if (fanin.type == CellArc::HOLD && fanin.other_port == ep.second)
                        init_required.max_delay += fanin.value.maxDelay();
                }
                clock_key = CellPortKey(ep.first.cell, ep.second);
            }
            set_required_time(ep.first, dom_id, init_required, 1, clock_key);
        }
    }
    // Walk backwards in topological order
    for (auto p : reversed_range(topological_order)) {
        auto &pd = ports.at(p);
        for (auto &req : pd.required) {
            if (pd.type == PORT_IN) {
                // Input port: propagate delay back through net, subtracting route delay
                NetInfo *net = port_info(p).net;
                if (net != nullptr && net->driver.cell != nullptr)
                    set_required_time(CellPortKey(net->driver), req.first,
                                      req.second.value - DelayPair(pd.route_delay.maxDelay()), req.second.path_length,
                                      p);
            } else if (pd.type == PORT_OUT) {
                // Output port : propagate delay back through cell, subtracting combinational delay
                for (auto &fanin : pd.cell_arcs) {
                    if (fanin.type != CellArc::COMBINATIONAL)
                        continue;
                    set_required_time(CellPortKey(p.cell, fanin.other_port), req.first,
                                      req.second.value - DelayPair(fanin.value.maxDelay()), req.second.path_length + 1,
                                      p);
                }
            }
        }
    }
}

dict<domain_id_t, delay_t> TimingAnalyser::max_delay_by_domain_pairs()
{
    dict<domain_id_t, delay_t> domain_delay;

    for (domain_id_t capture_id = 0; capture_id < domain_id_t(domains.size()); ++capture_id) {
        const auto &capture = domains.at(capture_id);

        for (auto &ep : capture.endpoints) {
            auto &ep_port = ports.at(ep.first);

            auto &req = ep_port.required.at(capture_id);

            for (auto &[launch_id, arr] : ep_port.arrival) {
                const auto &launch = domains.at(launch_id);

                auto dp = domain_pair_id(launch_id, capture_id);

                auto clocks = std::make_pair(launch.key.clock, capture.key.clock);
                auto same_clock = timed_clocks(ctx, launch.key.clock, capture.key.clock);
                auto related_clocks = clock_delays.count(clocks) > 0;
                delay_t clock_to_clock = 0;
                if (related_clocks) {
                    clock_to_clock = clock_delays.at(clocks);
                }

                auto delay = arr.value.maxDelay() - req.value.minDelay() + clock_to_clock;

                // If domains are unrelated or not the same clock we need to make sure
                // to remove the clock delays from the arrival and required times
                // because the delays have no common reference.
                if (with_clock_skew && !same_clock && !related_clocks) {
                    for (auto &fanin : ep_port.cell_arcs) {
                        if (fanin.type == CellArc::SETUP) {
                            auto clock_delay = ports.at(CellPortKey(ep.first.cell, fanin.other_port)).route_delay;
                            delay += clock_delay.minDelay();
                        }
                    }

                    // walk back to startpoint
                    auto crit_path = walk_crit_path(domain_pair_id(launch_id, capture_id), ep.first, true);
                    auto first_inp = crit_path.back();
                    const auto &sp = first_inp.cell->ports.at(first_inp.port).net->driver;
                    auto &sp_port = ports.at(CellPortKey{sp.cell->name, sp.port});

                    for (auto &fanin : sp_port.cell_arcs) {
                        if (fanin.type == CellArc::CLK_TO_Q) {
                            auto clock_delay = ports.at(CellPortKey(sp.cell->name, fanin.other_port)).route_delay;
                            delay -= clock_delay.maxDelay();
                        }
                    }
                }

                if (!domain_delay.count(dp) || domain_delay.at(dp) < delay) {
                    domain_delay[dp] = delay;
                }
            }
        }
    }

    return domain_delay;
}

void TimingAnalyser::compute_slack()
{
    for (auto &dp : domain_pairs) {
        dp.worst_setup_slack = std::numeric_limits<delay_t>::max();
        dp.worst_hold_slack = std::numeric_limits<delay_t>::max();
    }
    for (auto p : topological_order) {
        auto &pd = ports.at(p);
        for (auto &pdp : pd.domain_pairs) {
            auto &dp = domain_pairs.at(pdp.first);

            // Get clock names
            const auto &launch_clock = domains.at(dp.key.launch).key.clock;
            const auto &capture_clock = domains.at(dp.key.capture).key.clock;

            // Get clock-to-clock delay if any
            delay_t clock_to_clock = 0;
            auto clocks = std::make_pair(launch_clock, capture_clock);
            if (clock_delays.count(clocks)) {
                clock_to_clock = clock_delays.at(clocks);
            }

            auto &arr = pd.arrival.at(dp.key.launch);
            auto &req = pd.required.at(dp.key.capture);
            pdp.second.setup_slack = 0 - (arr.value.maxDelay() - req.value.minDelay() + clock_to_clock);
            if (!setup_only)
                pdp.second.hold_slack = arr.value.minDelay() - req.value.maxDelay() + clock_to_clock;
            // Hold keeps the single-cycle relationship under a multicycle.
            if (!setup_only && phase_related_clocks(ctx, launch_clock, capture_clock))
                pdp.second.hold_slack += clock_period(ctx, launch_clock) - (dp.period.minDelay() - dp.multicycle_extra);
            pdp.second.max_path_length = arr.path_length + req.path_length;
            if (dp.timed)
                pd.worst_setup_slack = std::min(pd.worst_setup_slack, dp.period.minDelay() + pdp.second.setup_slack);
            dp.worst_setup_slack = std::min(dp.worst_setup_slack, pdp.second.setup_slack);
            if (!setup_only) {
                pd.worst_hold_slack = std::min(pd.worst_hold_slack, pdp.second.hold_slack);
                dp.worst_hold_slack = std::min(dp.worst_hold_slack, pdp.second.hold_slack);
            }
        }
    }
}

void TimingAnalyser::compute_criticality()
{
    for (auto p : topological_order) {
        auto &pd = ports.at(p);
        for (auto &pdp : pd.domain_pairs) {
            auto &dp = domain_pairs.at(pdp.first);
            // Do not set criticality for asynchronous paths
            if (domains.at(dp.key.launch).key.is_async() || domains.at(dp.key.capture).key.is_async())
                continue;

            float crit =
                    1.0f - (float(pdp.second.setup_slack) - float(dp.worst_setup_slack)) / float(-dp.worst_setup_slack);
            crit = std::min(crit, 1.0f);
            crit = std::max(crit, 0.0f);
            pdp.second.criticality = crit;
            pd.worst_crit = std::max(pd.worst_crit, crit);
        }
    }
}

void TimingAnalyser::build_detailed_net_timing_report()
{
    auto &net_timings = result.detailed_net_timings;

    for (domain_id_t dom_id = 0; dom_id < domain_id_t(domains.size()); ++dom_id) {
        auto &dom = domains.at(dom_id);
        for (auto &ep : dom.endpoints) {
            auto &pd = ports.at(ep.first);
            const NetInfo *net = port_info(ep.first).net;

            for (auto &arr : pd.arrival) {
                auto &launch = domains.at(arr.first).key;
                for (auto &req : pd.required) {
                    auto &capture = domains.at(req.first).key;

                    NetSinkTiming sink_timing;
                    sink_timing.clock_pair.start.clock = launch.clock;
                    sink_timing.clock_pair.start.edge = launch.edge;
                    sink_timing.clock_pair.end.clock = capture.clock;
                    sink_timing.clock_pair.end.edge = capture.edge;
                    sink_timing.cell_port = std::make_pair(pd.cell_port.cell, pd.cell_port.port);
                    sink_timing.delay = arr.second.value;

                    net_timings[net->name].push_back(sink_timing);
                }
            }
        }
    }
}

std::vector<CellPortKey> TimingAnalyser::get_worst_eps(domain_id_t domain_pair, int count)
{
    std::vector<CellPortKey> worst_eps;
    delay_t last_slack = std::numeric_limits<delay_t>::lowest();
    auto &dp = domain_pairs.at(domain_pair);
    auto &cap_d = domains.at(dp.key.capture);
    while (int(worst_eps.size()) < count) {
        CellPortKey next;
        delay_t next_slack = std::numeric_limits<delay_t>::max();
        for (auto ep : cap_d.endpoints) {
            auto &pd = ports.at(ep.first);
            if (!pd.domain_pairs.count(domain_pair))
                continue;
            delay_t ep_slack = pd.domain_pairs.at(domain_pair).setup_slack;
            if (ep_slack < next_slack && ep_slack > last_slack) {
                next = ep.first;
                next_slack = ep_slack;
            }
        }
        if (next == CellPortKey())
            break;
        worst_eps.push_back(next);
        last_slack = next_slack;
    }
    return worst_eps;
}

std::vector<CriticalPath> TimingAnalyser::get_report_setup_paths(int count)
{
    if (count < 1 || count > 16384)
        log_error("Timing report path count must be between 1 and 16384.\n");
    std::vector<CriticalPath> paths;
    if (count == 1)
        return paths;
    if (have_loops)
        log_error("Cannot export additional setup paths for a design with timing loops.\n");

    using PairKey = std::tuple<std::string, int, std::string, int>;
    auto pair_key = [&](const ClockDomainKey &launch, const ClockDomainKey &capture) {
        return PairKey(launch.clock.str(ctx), int(launch.edge), capture.clock.str(ctx), int(capture.edge));
    };
    std::map<PairKey, std::set<CellPortKey>> existing;
    auto preserve = [&](const CriticalPath &path) {
        if (path.segments.empty())
            return;
        const auto &pair = path.clock_pair;
        auto key = PairKey(pair.start.clock.str(ctx), int(pair.start.edge),
                           pair.end.clock.str(ctx), int(pair.end.edge));
        const auto &endpoint = path.segments.back().to;
        existing[key].insert(CellPortKey(endpoint.first, endpoint.second));
    };
    for (const auto &path : result.clock_paths)
        preserve(path.second);
    for (const auto &path : result.xclock_paths)
        preserve(path);

    std::vector<std::pair<PairKey, domain_id_t>> pairs;
    for (domain_id_t index = 0; index < domain_id_t(domain_pairs.size()); ++index) {
        const auto &pair = domain_pairs.at(index).key;
        pairs.emplace_back(pair_key(domains.at(pair.launch).key, domains.at(pair.capture).key), index);
    }
    std::sort(pairs.begin(), pairs.end());
    for (const auto &entry : pairs) {
        const auto &pair = domain_pairs.at(entry.second).key;
        auto &selected = existing[entry.first];
        struct Endpoint {
            CellPortKey key;
            delay_t slack;
        };
        std::vector<Endpoint> endpoints;
        std::set<CellPortKey> seen;
        auto finite_delay = [](delay_t value) {
            return std::isfinite(double(value)) && value != std::numeric_limits<delay_t>::lowest() &&
                   value != std::numeric_limits<delay_t>::max();
        };
        for (const auto &endpoint : domains.at(pair.capture).endpoints) {
            auto key = endpoint.first;
            if (selected.count(key) || !seen.insert(key).second)
                continue;
            const auto &data = ports.at(key);
            auto tag = data.domain_pairs.find(entry.second);
            auto arrival = data.arrival.find(pair.launch);
            auto required = data.required.find(pair.capture);
            if (tag == data.domain_pairs.end() || arrival == data.arrival.end() || required == data.required.end() ||
                !finite_delay(tag->second.setup_slack) || !finite_delay(arrival->second.value.maxDelay()) ||
                !finite_delay(required->second.value.minDelay()))
                continue;
            int clocks = 0;
            if (ctx->getPortTimingClass(cell_info(key), key.port, clocks) != TMG_REGISTER_INPUT || clocks <= 0)
                continue;
            endpoints.push_back({key, tag->second.setup_slack});
        }
        std::sort(endpoints.begin(), endpoints.end(), [&](const Endpoint &a, const Endpoint &b) {
            return std::make_tuple(a.slack, a.key.cell.str(ctx), a.key.port.str(ctx)) <
                   std::make_tuple(b.slack, b.key.cell.str(ctx), b.key.port.str(ctx));
        });
        for (const auto &endpoint : endpoints) {
            if (selected.size() >= size_t(count))
                break;
            paths.push_back(build_critical_path_report(entry.second, endpoint.key, true));
            selected.insert(endpoint.key);
        }
    }
    return paths;
}

bool TimingAnalyser::get_endpoint_clock_pair_timings(CellPortKey endpoint,
                                                    std::vector<EndpointClockPairTiming> &rows)
{
    rows.clear();
    auto found = ports.find(endpoint);
    auto owner = ctx->cells.find(endpoint.cell);
    if (!analysis_complete || setup_only || analysed_setup_only || with_clock_skew != analysed_clock_skew ||
        have_loops || found == ports.end() || owner == ctx->cells.end() ||
        !owner->second->ports.count(endpoint.port) || !owner->second->getPort(endpoint.port))
        return false;
    const auto &data = found->second;
    if (data.type != PORT_IN || data.arrival.empty() || data.required.empty() ||
        data.domain_pairs.size() != data.arrival.size() * data.required.size())
        return false;
    auto finite = [](delay_t value) {
        return std::isfinite(double(value)) && value != std::numeric_limits<delay_t>::lowest() &&
               value != std::numeric_limits<delay_t>::max();
    };
    auto finite_pair = [&](const DelayPair &value) {
        return finite(value.minDelay()) && finite(value.maxDelay());
    };
    auto valid_clock = [&](const ClockDomainKey &key) {
        auto net = ctx->nets.find(key.clock);
        if (key.is_async() || (key.edge != RISING_EDGE && key.edge != FALLING_EDGE) || net == ctx->nets.end() ||
            !net->second->clkconstr)
            return false;
        const auto &constraint = *net->second->clkconstr;
        return finite_pair(constraint.period) && constraint.period.minDelay() > 0 &&
               constraint.period.minDelay() <= constraint.period.maxDelay() &&
               finite_pair(constraint.high) && finite_pair(constraint.low) && finite(constraint.phase_shift);
    };
    auto registered = [&](CellPortKey key, TimingPortClass kind, const ClockDomainKey &domain) {
        auto cell = ctx->cells.find(key.cell);
        if (cell == ctx->cells.end() || !cell->second->ports.count(key.port))
            return false;
        int count = 0;
        if (ctx->getPortTimingClass(cell->second.get(), key.port, count) != kind || count <= 0)
            return false;
        bool matched = false;
        for (int index = 0; index < count; ++index) {
            auto info = ctx->getPortClockingInfo(cell->second.get(), key.port, index);
            auto clock = cell->second->getPort(info.clock_port);
            if (!clock || !valid_clock(ClockDomainKey(clock->name, info.edge)))
                return false;
            if (clock->name == domain.clock && info.edge == domain.edge)
                matched = true;
        }
        return matched;
    };
    // Validate the native backpointer walk before using the existing report
    // builder, which assumes a complete registered path and dereferences it.
    auto registered_path = [&](domain_id_t launch_id, bool longest) {
        auto cursor = endpoint;
        std::set<CellPortKey> seen;
        while (seen.insert(cursor).second) {
            auto port = ports.find(cursor);
            auto cell = ctx->cells.find(cursor.cell);
            if (port == ports.end() || cell == ctx->cells.end() || !cell->second->ports.count(cursor.port))
                return false;
            auto tag = port->second.arrival.find(launch_id);
            if (tag == port->second.arrival.end() || !finite_pair(tag->second.value))
                return false;
            const auto &connection = cell->second->ports.at(cursor.port);
            if (connection.type == PORT_OUT &&
                registered(cursor, TMG_REGISTER_OUTPUT, domains.at(launch_id).key))
                return connection.net && connection.net->driver.cell == cell->second.get() &&
                       connection.net->driver.port == cursor.port;
            auto previous = longest ? tag->second.bwd_max : tag->second.bwd_min;
            if (previous.cell == IdString() || previous.port == IdString())
                return false;
            if (connection.type == PORT_IN) {
                auto net = connection.net;
                if (!net || !net->driver.cell || previous != CellPortKey(net->driver))
                    return false;
            } else if (connection.type == PORT_OUT) {
                auto input = cell->second->ports.find(previous.port);
                DelayQuad delay;
                if (previous.cell != cursor.cell || input == cell->second->ports.end() ||
                    input->second.type != PORT_IN || !input->second.net ||
                    !ctx->getCellDelay(cell->second.get(), previous.port, cursor.port, delay) ||
                    !finite(delay.minDelay()) || !finite(delay.maxDelay()))
                    return false;
            } else {
                return false;
            }
            cursor = previous;
        }
        return false;
    };
    std::vector<EndpointClockPairTiming> complete;
    for (const auto &arrival : data.arrival) {
        if (arrival.first < 0 || size_t(arrival.first) >= domains.size() ||
            !valid_clock(domains.at(arrival.first).key) || !finite_pair(arrival.second.value) ||
            !registered_path(arrival.first, true) || !registered_path(arrival.first, false))
            return false;
        for (const auto &required : data.required) {
            if (required.first < 0 || size_t(required.first) >= domains.size() ||
                !valid_clock(domains.at(required.first).key) || !finite_pair(required.second.value) ||
                !registered(endpoint, TMG_REGISTER_INPUT, domains.at(required.first).key))
                return false;
            auto pair = pair_to_id.find(ClockDomainPairKey(arrival.first, required.first));
            if (pair == pair_to_id.end() || pair->second < 0 || size_t(pair->second) >= domain_pairs.size())
                return false;
            auto tag = data.domain_pairs.find(pair->second);
            if (tag == data.domain_pairs.end() || !finite(tag->second.setup_slack) || !finite(tag->second.hold_slack))
                return false;
            const auto &launch = domains.at(arrival.first).key;
            const auto &capture = domains.at(required.first).key;
            EndpointClockPairTiming row(launch, capture);
            row.setup_timed = timed_clocks(ctx, launch.clock, capture.clock);
            row.hold_related = arrival.first == required.first ||
                               clock_delays.count(std::make_pair(launch.clock, capture.clock)) ||
                               phase_related_clocks(ctx, launch.clock, capture.clock);
            if (row.setup_timed) {
                auto window = domain_pairs.at(pair->second).period.minDelay();
                if (!finite(window) || window <= 0)
                    return false;
                row.setup_window = window;
                const auto margin = EndpointClockPairTiming::DelaySum(window) +
                                    EndpointClockPairTiming::DelaySum(tag->second.setup_slack);
                if (!std::isfinite(double(margin)) || margin <= std::numeric_limits<delay_t>::lowest() ||
                    margin >= std::numeric_limits<delay_t>::max())
                    return false;
                row.setup_margin = delay_t(margin);
            }
            if (row.hold_related)
                row.hold_margin = tag->second.hold_slack;
            auto sum_path = [&](bool longest, EndpointClockPairTiming::DelaySum &sum) {
                auto path = build_critical_path_report(pair->second, endpoint, longest);
                if (path.segments.empty() || path.segments.back().to != std::make_pair(endpoint.cell, endpoint.port) ||
                    path.segments.back().type != (longest ? CriticalPath::Segment::Type::SETUP :
                                                          CriticalPath::Segment::Type::HOLD))
                    return false;
                sum = 0;
                for (const auto &segment : path.segments) {
                    if (!finite(segment.delay))
                        return false;
                    const auto value = EndpointClockPairTiming::DelaySum(segment.delay);
                    if constexpr (std::is_integral<delay_t>::value) {
                        if ((value > 0 && sum > std::numeric_limits<int64_t>::max() - value) ||
                            (value < 0 && sum < std::numeric_limits<int64_t>::lowest() - value))
                            return false;
                    }
                    sum += value;
                    if (!std::isfinite(double(sum)))
                        return false;
                }
                return true;
            };
            if (!sum_path(true, row.max_path_delay) || !sum_path(false, row.min_path_delay))
                return false;
            complete.push_back(row);
        }
    }
    std::sort(complete.begin(), complete.end(), [&](const EndpointClockPairTiming &a, const EndpointClockPairTiming &b) {
        return std::make_tuple(a.launch.clock.str(ctx), int(a.launch.edge), a.capture.clock.str(ctx), int(a.capture.edge)) <
               std::make_tuple(b.launch.clock.str(ctx), int(b.launch.edge), b.capture.clock.str(ctx), int(b.capture.edge));
    });
    if (complete.empty())
        return false;
    rows.swap(complete);
    return true;
}

std::vector<PortRef> TimingAnalyser::walk_crit_path(domain_id_t domain_pair, CellPortKey endpoint, bool longest_path)
{
    const auto &dp = domain_pairs.at(domain_pair);

    // Walk the min or max path backwards to find a single crit path
    pool<std::pair<IdString, IdString>> visited;
    std::vector<PortRef> crit_path_rev;
    auto cursor = endpoint;

    bool is_startpoint = false;
    do {
        auto cell = cell_info(cursor);
        auto &port = port_info(cursor);
        int port_clocks;
        auto portClass = ctx->getPortTimingClass(cell, port.name, port_clocks);

        // combinational loop
        if (!visited.insert(std::make_pair(cell->name, port.name)).second)
            break;

        // We store the reversed critical path as all input ports that lead to
        // the timing startpoint.
        auto is_input = portClass != TMG_CLOCK_INPUT && portClass != TMG_IGNORE && port.type == PortType::PORT_IN;

        if (is_input)
            crit_path_rev.emplace_back(PortRef{cell, port.name});

        if (!ports.at(cursor).arrival.count(dp.key.launch))
            break;

        if (longest_path) {
            cursor = ports.at(cursor).arrival.at(dp.key.launch).bwd_max;
        } else {
            cursor = ports.at(cursor).arrival.at(dp.key.launch).bwd_min;
        }
        is_startpoint = portClass == TMG_STARTPOINT;
    } while (!is_startpoint);

    return crit_path_rev;
}

CriticalPath TimingAnalyser::build_critical_path_report(domain_id_t domain_pair, CellPortKey endpoint,
                                                        bool longest_path)
{
    CriticalPath report;

    const auto &dp = domain_pairs.at(domain_pair);
    const auto &launch = domains.at(dp.key.launch).key;
    const auto &capture = domains.at(dp.key.capture).key;

    report.clock_pair.start.clock = launch.clock;
    report.clock_pair.start.edge = launch.edge;
    report.clock_pair.end.clock = capture.clock;
    report.clock_pair.end.edge = capture.edge;

    report.max_delay = ctx->getDelayFromNS(1.0e9 / ctx->setting<float>("target_freq"));
    if (launch.edge != capture.edge) {
        report.max_delay = report.max_delay / 2;
    }

    if (!launch.is_async() && ctx->nets.at(launch.clock)->clkconstr) {
        if (launch.edge == capture.edge) {
            report.max_delay = ctx->nets.at(launch.clock)->clkconstr->period.minDelay();
        } else if (capture.edge == RISING_EDGE) {
            report.max_delay = ctx->nets.at(launch.clock)->clkconstr->low.minDelay();
        } else if (capture.edge == FALLING_EDGE) {
            report.max_delay = ctx->nets.at(launch.clock)->clkconstr->high.minDelay();
        }
    }
    if (!launch.is_async() && timed_clocks(ctx, launch.clock, capture.clock))
        report.max_delay = clock_interval(ctx, launch.clock, capture.clock, launch.edge, capture.edge) +
                           multicycle_extra(ctx, launch.clock, capture.clock);

    auto crit_path_rev = walk_crit_path(domain_pair, endpoint, longest_path);
    auto crit_path = boost::adaptors::reverse(crit_path_rev);

    // Get timing and clocking info on the startpoint
    auto first_inp = crit_path.front();
    const auto &sp = first_inp.cell->ports.at(first_inp.port).net->driver;
    const auto &sp_cell = sp.cell;
    const auto &sp_port = sp_cell->ports.at(sp.port);
    int sp_clocks;
    const auto sp_portClass = ctx->getPortTimingClass(sp_cell, sp_port.name, sp_clocks);
    TimingClockingInfo sp_clk_info;
    const NetInfo *sp_clk_net = nullptr;
    bool register_start = sp_portClass == TMG_REGISTER_OUTPUT;

    if (register_start) {
        // If we don't find a clock we don't consider this startpoint to be registered.
        register_start = sp_clocks > 0;
        for (int i = 0; i < sp_clocks; i++) {
            sp_clk_info = ctx->getPortClockingInfo(sp_cell, sp_port.name, i);
            const auto clk_net = sp_cell->getPort(sp_clk_info.clock_port);
            register_start = clk_net != nullptr && clk_net->name == launch.clock && sp_clk_info.edge == launch.edge;
            if (register_start) {
                sp_clk_net = clk_net;
                break;
            }
        }
    }

    // Get timing and clocking info on the endpoint
    const auto &ep = crit_path.back();
    const auto &ep_cell = ep.cell;
    const auto &ep_port = ep_cell->ports.at(ep.port);
    int ep_clocks;
    const auto ep_portClass = ctx->getPortTimingClass(ep_cell, ep_port.name, ep_clocks);
    TimingClockingInfo ep_clk_info;
    const NetInfo *ep_clk_net = nullptr;

    bool register_end = ep_portClass == TMG_REGISTER_INPUT;

    if (register_end) {
        // If we don't find a clock we don't consider this startpoint to be registered.
        register_end = ep_clocks > 0;
        for (int i = 0; i < ep_clocks; i++) {
            ep_clk_info = ctx->getPortClockingInfo(ep_cell, ep_port.name, i);
            const auto clk_net = ep_cell->getPort(ep_clk_info.clock_port);

            register_end = clk_net != nullptr && clk_net->name == capture.clock && ep_clk_info.edge == capture.edge;
            if (register_end) {
                ep_clk_net = clk_net;
                break;
            }
        }
    }

    auto clock_pair = std::make_pair(launch.clock, capture.clock);
    auto related_clock = clock_delays.count(clock_pair) > 0;
    auto same_clock = launch.clock == capture.clock;
    auto phase_locked = phase_related_clocks(ctx, launch.clock, capture.clock);

    if (related_clock) {
        delay_t clock_delay = clock_delays.at(clock_pair);
        if (!is_zero_delay(clock_delay)) {
            CriticalPath::Segment seg_c2c;
            seg_c2c.type = CriticalPath::Segment::Type::CLK_TO_CLK;
            seg_c2c.delay = clock_delay;
            seg_c2c.from = std::make_pair(sp_cell->name, sp_clk_info.clock_port);
            seg_c2c.to = std::make_pair(ep_cell->name, ep_clk_info.clock_port);
            seg_c2c.net = IdString();
            report.segments.push_back(seg_c2c);
        }
    }

    if (!longest_path && phase_locked && register_start && register_end) {
        CriticalPath::Segment seg_phase;
        seg_phase.type = CriticalPath::Segment::Type::CLK_TO_CLK;
        seg_phase.delay = clock_period(ctx, launch.clock) -
                          clock_interval(ctx, launch.clock, capture.clock, launch.edge, capture.edge);
        seg_phase.from = std::make_pair(sp_cell->name, sp_clk_info.clock_port);
        seg_phase.to = std::make_pair(ep_cell->name, ep_clk_info.clock_port);
        seg_phase.net = IdString();
        report.segments.push_back(seg_phase);
    }

    if (with_clock_skew && register_start && register_end && (same_clock || related_clock || phase_locked)) {

        auto clock_delay_launch = ctx->getNetinfoRouteDelay(sp_clk_net, PortRef{sp_cell, sp_clk_info.clock_port});
        auto clock_delay_capture = ctx->getNetinfoRouteDelay(ep_clk_net, PortRef{ep_cell, ep_clk_info.clock_port});

        delay_t clock_skew = clock_delay_launch - clock_delay_capture;

        if (!is_zero_delay(clock_skew)) {
            CriticalPath::Segment seg_skew;
            seg_skew.type = CriticalPath::Segment::Type::CLK_SKEW;
            seg_skew.delay = clock_skew;
            seg_skew.from = std::make_pair(sp_cell->name, sp_clk_info.clock_port);
            seg_skew.to = std::make_pair(ep_cell->name, ep_clk_info.clock_port);
            if (same_clock) {
                seg_skew.net = launch.clock;
            } else {
                seg_skew.net = IdString();
            }
            report.segments.push_back(seg_skew);
        }
    }

    const CellInfo *prev_cell = sp_cell;
    IdString prev_port = sp_port.name;

    bool is_startpoint = true;
    for (auto sink : crit_path) {
        auto sink_cell = sink.cell;
        auto &port = sink_cell->ports.at(sink.port);
        auto net = port.net;
        auto &driver = net->driver;
        auto driver_cell = driver.cell;

        CriticalPath::Segment seg_logic;

        DelayQuad comb_delay;
        if (is_startpoint && register_start) {
            comb_delay = sp_clk_info.clockToQ;
            seg_logic.type = CriticalPath::Segment::Type::CLK_TO_Q;
        } else if (is_startpoint) {
            comb_delay = DelayQuad(0);
            seg_logic.type = CriticalPath::Segment::Type::SOURCE;
        } else {
            ctx->getCellDelay(driver_cell, prev_port, driver.port, comb_delay);
            seg_logic.type = CriticalPath::Segment::Type::LOGIC;
        }

        seg_logic.delay = longest_path ? comb_delay.maxDelay() : comb_delay.minDelay();
        seg_logic.from = std::make_pair(prev_cell->name, prev_port);
        seg_logic.to = std::make_pair(driver_cell->name, driver.port);
        seg_logic.net = IdString();
        report.segments.push_back(seg_logic);

        auto net_delay = DelayPair(ctx->getNetinfoRouteDelay(net, sink));

        CriticalPath::Segment seg_route;
        seg_route.type = CriticalPath::Segment::Type::ROUTING;
        seg_route.delay = longest_path ? net_delay.maxDelay() : net_delay.minDelay();
        seg_route.from = std::make_pair(driver_cell->name, driver.port);
        seg_route.to = std::make_pair(sink_cell->name, sink.port);
        seg_route.net = net->name;
        report.segments.push_back(seg_route);

        prev_cell = sink_cell;
        prev_port = sink.port;
        is_startpoint = false;
    }

    if (register_end) {
        CriticalPath::Segment seg_logic;
        seg_logic.delay = 0;
        if (longest_path) {
            seg_logic.type = CriticalPath::Segment::Type::SETUP;
            seg_logic.delay += ep_clk_info.setup.maxDelay();
        } else {
            seg_logic.type = CriticalPath::Segment::Type::HOLD;
            seg_logic.delay -= ep_clk_info.hold.maxDelay();
        }
        seg_logic.from = std::make_pair(prev_cell->name, prev_port);
        seg_logic.to = seg_logic.from;
        seg_logic.net = IdString();
        report.segments.push_back(seg_logic);
    }

    return report;
}

void TimingAnalyser::build_crit_path_reports()
{
    auto &clock_reports = result.clock_paths;
    auto &xclock_reports = result.xclock_paths;
    auto &clock_fmax = result.clock_fmax;
    auto &empty_clocks = result.empty_paths;

    if (!setup_only) {
        result.min_delay_violations = get_min_delay_violations();
    }

    auto delay_by_domain = max_delay_by_domain_pairs();

    for (int i = 0; i < int(domains.size()); i++) {
        empty_clocks.insert(domains.at(i).key.clock);
    }

    for (int i = 0; i < int(domain_pairs.size()); i++) {
        auto &dp = domain_pairs.at(i);
        auto &launch = domains.at(dp.key.launch).key;
        auto &capture = domains.at(dp.key.capture).key;

        const bool ordinary_timed = timed_clocks(ctx, launch.clock, capture.clock);
        const bool physically_related = clock_delays.count(std::make_pair(launch.clock, capture.clock));
        const bool phase_locked = phase_related_clocks(ctx, launch.clock, capture.clock);
        const bool ignored_related = bool_or_default(ctx->settings, ctx->id("timing/ignoreRelClk"), false) &&
                                     launch.clock != capture.clock && !phase_locked;
        if (!launch.is_async() && (ordinary_timed || (physically_related && !ignored_related))) {
            const delay_t setup_window = ordinary_timed
                                                   ? dp.period.minDelay()
                                                   : std::min(clock_period(ctx, launch.clock),
                                                              clock_period(ctx, capture.clock));
            const delay_t setup_slack = setup_window + dp.worst_setup_slack;
            auto inserted_slack = result.clock_setup_slack.emplace(launch.clock, setup_slack);
            if (!inserted_slack.second)
                inserted_slack.first->second = std::min(inserted_slack.first->second, setup_slack);
        }

        if (!ordinary_timed || launch.is_async())
            continue;

        auto path_delay = delay_by_domain.at(i);

        double Fmax;

        // dp.period is the launch-to-capture window, including any SDC
        // multicycle relaxation.
        if (launch.clock == capture.clock && launch.edge == capture.edge && dp.multicycle_extra == 0)
            Fmax = 1000 / ctx->getDelayNS(path_delay);
        else
            Fmax = 1000.0 * double(dp.period.minDelay()) / double(clock_period(ctx, launch.clock)) /
                   ctx->getDelayNS(path_delay);

        if (!clock_fmax.count(launch.clock) || Fmax < clock_fmax.at(launch.clock).achieved) {
            float target = ctx->setting<float>("target_freq") / 1e6;
            if (ctx->nets.at(launch.clock)->clkconstr)
                target = 1000 / ctx->getDelayNS(ctx->nets.at(launch.clock)->clkconstr->period.minDelay());

            auto worst_endpoint = get_worst_eps(i, 1);
            if (worst_endpoint.empty())
                continue;

            clock_fmax[launch.clock].achieved = Fmax;
            clock_fmax[launch.clock].constraint = target;

            clock_reports[launch.clock] = build_critical_path_report(i, worst_endpoint.at(0), true);

            empty_clocks.erase(launch.clock);
        }
    }

    for (int i = 0; i < int(domain_pairs.size()); i++) {
        auto &dp = domain_pairs.at(i);
        auto &launch = domains.at(dp.key.launch).key;
        auto &capture = domains.at(dp.key.capture).key;

        if (timed_clocks(ctx, launch.clock, capture.clock) && !launch.is_async())
            continue;

        auto worst_endpoint = get_worst_eps(i, 1);
        if (worst_endpoint.empty())
            continue;

        xclock_reports.emplace_back(build_critical_path_report(i, worst_endpoint.at(0), true));
    }

    auto cmp_crit_path = [&](const CriticalPath &ra, const CriticalPath &rb) {
        const auto &a = ra.clock_pair;
        const auto &b = rb.clock_pair;

        if (a.start.clock.str(ctx) < b.start.clock.str(ctx))
            return true;
        if (a.start.clock.str(ctx) > b.start.clock.str(ctx))
            return false;
        if (a.start.edge < b.start.edge)
            return true;
        if (a.start.edge > b.start.edge)
            return false;
        if (a.end.clock.str(ctx) < b.end.clock.str(ctx))
            return true;
        if (a.end.clock.str(ctx) > b.end.clock.str(ctx))
            return false;
        if (a.end.edge < b.end.edge)
            return true;
        return false;
    };

    std::sort(xclock_reports.begin(), xclock_reports.end(), cmp_crit_path);
}

void TimingAnalyser::build_slack_histogram_report()
{
    auto &slack_histogram = result.slack_histogram;

    for (domain_id_t dom_id = 0; dom_id < domain_id_t(domains.size()); ++dom_id) {
        for (auto &ep : domains.at(dom_id).endpoints) {
            auto &pd = ports.at(ep.first);

            for (auto &req : pd.required) {
                auto &capture = domains.at(req.first).key;
                for (auto &arr : pd.arrival) {
                    auto &launch = domains.at(arr.first).key;

                    if (!timed_clocks(ctx, launch.clock, capture.clock) || launch.is_async())
                        continue;

                    delay_t clk_period = clock_interval(ctx, launch.clock, capture.clock, launch.edge, capture.edge);

                    delay_t delay = arr.second.value.maxDelay() - req.second.value.minDelay();
                    delay_t slack = clk_period - delay;

                    int slack_ps = ctx->getDelayNS(slack) * 1000;
                    slack_histogram[slack_ps]++;
                }
            }
        }
    }
}

std::vector<CriticalPath> TimingAnalyser::get_min_delay_violations()
{
    std::vector<CriticalPath> violations;

    for (domain_id_t capture_id = 0; capture_id < domain_id_t(domains.size()); ++capture_id) {
        const auto &capture = domains.at(capture_id);
        const auto &capture_clock = capture.key.clock;

        for (const auto &ep : capture.endpoints) {
            const CellInfo *ci = cell_info(ep.first);
            int clkInfoCount = 0;
            const TimingPortClass cls = ctx->getPortTimingClass(ci, ep.first.port, clkInfoCount);
            if (cls != TMG_REGISTER_INPUT)
                continue;

            const auto &port = ports.at(ep.first);

            const auto &req = port.required.at(capture_id);

            for (auto &[launch_id, arr] : port.arrival) {
                const auto &launch = domains.at(launch_id);
                const auto &launch_clock = launch.key.clock;
                const auto dom_pair_id = domain_pair_id(launch_id, capture_id);

                auto clocks = std::make_pair(launch_clock, capture_clock);
                auto related_clocks = clock_delays.count(clocks) > 0;
                auto phase_locked = phase_related_clocks(ctx, launch_clock, capture_clock);

                if (launch_id == async_clock_id || (launch_id != capture_id && !related_clocks && !phase_locked)) {
                    continue;
                }
                // Setup already drops these pairs in timed_clocks(). Hold must
                // do the same, including phase-related and same-clock cuts.
                if (ctx->sdc_clock_false(launch_clock, capture_clock))
                    continue;

                delay_t clock_to_clock = 0;
                if (related_clocks) {
                    clock_to_clock = clock_delays.at(clocks);
                }

                auto hold_slack = arr.value.minDelay() - req.value.maxDelay() + clock_to_clock;
                if (phase_locked)
                    hold_slack += clock_period(ctx, launch_clock) -
                                  clock_interval(ctx, launch_clock, capture_clock, launch.key.edge, capture.key.edge);

                const bool ignored_related = bool_or_default(ctx->settings, ctx->id("timing/ignoreRelClk"), false) &&
                                             launch_clock != capture_clock && !phase_locked;
                if (!ignored_related) {
                    auto inserted = result.clock_hold_slack.emplace(launch_clock, hold_slack);
                    if (!inserted.second)
                        inserted.first->second = std::min(inserted.first->second, hold_slack);
                }

                if (hold_slack <= 0) {
                    auto report = build_critical_path_report(dom_pair_id, ep.first, false);
                    violations.emplace_back(report);
                }
            }
        }
    }

    std::vector<std::pair<size_t, delay_t>> sum_indices;
    sum_indices.reserve(violations.size());

    for (size_t i = 0; i < violations.size(); ++i) {
        delay_t delay = 0;
        for (const auto &seg : violations[i].segments) {
            delay += seg.delay;
        }

        sum_indices.emplace_back(i, delay);
    }

    std::sort(sum_indices.begin(), sum_indices.end(),
              [](auto &left, auto &right) { return left.second < right.second; });

    std::vector<CriticalPath> sorted_violations;
    sorted_violations.reserve(violations.size());
    for (const auto &pair : sum_indices) {
        sorted_violations.push_back(std::move(violations[pair.first]));
    }

    return sorted_violations;
}

domain_id_t TimingAnalyser::domain_id(IdString cell, IdString clock_port, ClockEdge edge)
{
    return domain_id(ctx->cells.at(cell)->ports.at(clock_port).net, edge);
}

domain_id_t TimingAnalyser::domain_id(const NetInfo *net, ClockEdge edge)
{
    NPNR_ASSERT(net != nullptr);
    ClockDomainKey key{net->name, edge};
    auto inserted = domain_to_id.emplace(key, domains.size());
    if (inserted.second) {
        domains.emplace_back(key);
    }
    return inserted.first->second;
}

domain_id_t TimingAnalyser::domain_pair_id(domain_id_t launch, domain_id_t capture)
{
    ClockDomainPairKey key{launch, capture};
    auto inserted = pair_to_id.emplace(key, domain_pairs.size());
    if (inserted.second) {
        domain_pairs.emplace_back(key);
    }
    return inserted.first->second;
}

void TimingAnalyser::copy_domains(const CellPortKey &from, const CellPortKey &to, bool backward)
{
    auto &f = ports.at(from), &t = ports.at(to);
    for (auto &dom : (backward ? f.required : f.arrival)) {
        updated_domains |= (backward ? t.required : t.arrival).emplace(dom.first, ArrivReqTime{}).second;
    }
}

const std::string TimingAnalyser::arcType_to_str(CellArc::ArcType typ)
{
    switch (typ) {
    case TimingAnalyser::CellArc::COMBINATIONAL:
        return "COMBINATIONAL";
    case TimingAnalyser::CellArc::SETUP:
        return "SETUP";
    case TimingAnalyser::CellArc::HOLD:
        return "HOLD";
    case TimingAnalyser::CellArc::CLK_TO_Q:
        return "CLK_TO_Q";
    case TimingAnalyser::CellArc::STARTPOINT:
        return "STARTPOINT";
    case TimingAnalyser::CellArc::ENDPOINT:
        return "ENDPOINT";
    default:
        NPNR_ASSERT_FALSE("Impossible CellArc::ArcType\n");
    }
}

CellInfo *TimingAnalyser::cell_info(const CellPortKey &key) { return ctx->cells.at(key.cell).get(); }

PortInfo &TimingAnalyser::port_info(const CellPortKey &key) { return ctx->cells.at(key.cell)->ports.at(key.port); }

bool timing_analysis(Context *ctx, bool print_slack_histogram, bool print_fmax, bool print_path, bool warn_on_failure,
                     bool update_results)
{
    if (update_results)
        ctx->timing_result_is_final_analogue = false;
    TimingAnalyser tmg(ctx);
    tmg.setup_only = false;
    tmg.with_clock_skew = true;
    const bool extra_report_paths = update_results && ctx->timing_report_paths > 1;
    tmg.setup(ctx->detailed_timing_report, print_slack_histogram, print_path || print_fmax || extra_report_paths);

    auto &result = tmg.get_timing_result();
    if (extra_report_paths) {
        result.report_setup_paths = tmg.get_report_setup_paths(ctx->timing_report_paths);
        log_info("Timing report includes %zu additional registered setup endpoint paths (limit %d per domain pair).\n",
                 result.report_setup_paths.size(), ctx->timing_report_paths);
    }
    bool timing_constraints_met =
            ctx->log_timing_results(result, print_slack_histogram, print_fmax, print_path, warn_on_failure);

    if (update_results)
        ctx->timing_result = result;

    return timing_constraints_met;
}

NEXTPNR_NAMESPACE_END
