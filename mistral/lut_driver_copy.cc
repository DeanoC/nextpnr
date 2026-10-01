/* Bounded report-guided one-edge LUT driver copies. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "json11.hpp"
#include "timing.h"
#include "enable_replication_policy.h"
#include "lut_driver_copy.h"
#include "remap_report.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
const std::array<IdString,6> copy_pins = {id_A,id_B,id_C,id_D,id_E,id_F};
using CopyLab = std::pair<int,int>;

bool copy_protected(const dict<IdString,Property> &attrs, Context *ctx)
{
    for (const auto &entry : attrs) {
        auto name = entry.first.str(ctx);
        if (name == "keep" || name == "dont_touch") return true;
    }
    return false;
}

bool copy_timed(float slack)
{
    return std::isfinite(slack) && slack < float(std::numeric_limits<delay_t>::max());
}

bool copy_domain_rows_match(const std::vector<EndpointClockPairTiming> &before,
                            const std::vector<EndpointClockPairTiming> &after)
{
    if (before.empty() || before.size() != after.size()) return false;
    for (size_t i = 0; i < before.size(); ++i) {
        const auto &old = before[i], &now = after[i];
        if (!(old.launch == now.launch) || !(old.capture == now.capture) ||
            old.setup_timed != now.setup_timed || old.hold_related != now.hold_related ||
            old.setup_window != now.setup_window ||
            old.setup_margin.has_value() != now.setup_margin.has_value() ||
            old.hold_margin.has_value() != now.hold_margin.has_value()) return false;
    }
    return true;
}

bool copy_domain_rows_nonregressing(const std::vector<EndpointClockPairTiming> &before,
                                    const std::vector<EndpointClockPairTiming> &after, bool reference_free)
{
    if (!copy_domain_rows_match(before, after)) return false;
    for (size_t i = 0; i < before.size(); ++i) {
        const auto &old = before[i], &now = after[i];
        if (!old.setup_timed) {
            // Both native-report and false-skew frames must retain every
            // unrelated maximum and minimum. MAX scalar slack is no proof.
            if (now.max_path_delay > old.max_path_delay || now.min_path_delay < old.min_path_delay) return false;
        } else if (!reference_free) {
            if (!old.setup_window || !old.setup_margin ||
                now.max_path_delay > old.max_path_delay || *now.setup_margin < *old.setup_margin) return false;
        }
        if (!reference_free && old.hold_related) {
            if (!old.hold_margin || *now.hold_margin < std::min(delay_t(0), *old.hold_margin)) return false;
        }
    }
    return true;
}

std::map<std::string,int> copy_holds(TimingAnalyser &timing)
{
    std::map<std::string,int> result;
    for (const auto &path : timing.get_timing_result().min_delay_violations) {
        if (path.segments.empty()) continue;
        int value = 0;
        for (const auto &segment : path.segments) value += segment.delay;
        auto end = path.segments.back().to;
        auto key = std::to_string(path.clock_pair.start.clock.index) + ":" +
            std::to_string(int(path.clock_pair.start.edge)) + ":" +
            std::to_string(path.clock_pair.end.clock.index) + ":" +
            std::to_string(int(path.clock_pair.end.edge)) + ":" +
            std::to_string(end.first.index) + ":" + std::to_string(end.second.index);
        auto inserted = result.emplace(key,value);
        if (!inserted.second) inserted.first->second = std::min(inserted.first->second,value);
    }
    return result;
}
}

namespace {
void prevalidate_remap_prefix(Context *ctx, const char *stage, bool include_post)
{
    if (ctx->local_remap_plan_list_only || ctx->comb_remap_plan_list_only ||
        (include_post && ctx->local_remap_post_plan_list_only) ||
        (!ctx->local_remap_report.empty() && ctx->local_remap_selection < 0) ||
        (!ctx->comb_remap_report.empty() && ctx->comb_remap_selection < 0) ||
        (!ctx->decomposition_remap_report.empty() && ctx->decomposition_remap_selection < 0))
        log_error("A remap listing must be final; it cannot precede %s.\n", stage);
    const char *spec = std::getenv("NEXTPNR_MISTRAL_PLACED_REDUCTION");
    if (!spec || !*spec) return;
    std::istringstream lines(spec); std::string line; size_t count = 0;
    while (std::getline(lines,line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::istringstream options(line); std::string root, extra;
        int radius = 0, selection = -1, minimum = 250;
        if (!(options >> root >> radius >> selection))
            log_error("Invalid placed reduction diagnostic options.\n");
        options >> std::ws;
        if (!options.eof() && !(options >> minimum))
            log_error("Invalid placed reduction diagnostic options.\n");
        if ((options >> extra) || radius < 1 || radius > 6 || selection < -1 || minimum < 1 || ++count > 8)
            log_error("Invalid placed reduction diagnostic options.\n");
        if (selection < 0)
            log_error("A placed reduction listing cannot precede %s.\n", stage);
    }
    if (!count) log_error("Invalid placed reduction diagnostic options.\n");
}
}

void prevalidate_lut_driver_copy_prefix(Context *ctx)
{
    prevalidate_remap_prefix(ctx, "LUT driver copy", true);
}

void prevalidate_local_remap_post_prefix(Context *ctx)
{
    prevalidate_remap_prefix(ctx, "post-remap plan", false);
}

bool Arch::remap_lut_driver_critical(const std::string &report, int selection)
{
    Context *ctx = getCtx();
    if (selection < -1) log_error("Invalid LUT driver copy candidate index.\n");
    if (fes_any_slot_region_active) log_error("LUT driver copy requires ordinary placement.\n");
    prevalidate_lut_driver_copy_prefix(ctx);
    for (const auto &entry : nets)
        if (!entry.second->wires.empty()) log_error("LUT driver copy requires an unrouted design.\n");
    std::string error;
    auto json = json11::Json::parse(report,error);
    if (!error.empty() || !json.is_object() || !json["critical_paths"].is_array())
        log_error("Invalid LUT driver copy timing report.\n");
    auto lab = [&](BelId bel) { auto at = getBelLocation(bel); return CopyLab(at.x,at.y); };
    auto movable = [&](const CellInfo *cell) {
        return cell && cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK &&
            cell->cluster == ClusterId() && !cell->region && !cell->isPseudo() && !copy_protected(cell->attrs,ctx);
    };
    std::set<CopyLab> protected_labs;
    std::map<CopyLab, const CellInfo *> protected_owners;
    std::vector<IdString> original_cell_order, original_net_order, original_alias_order;
    struct Original {
        CellInfo *cell;
        BelId bel;
        PlaceStrength strength;
        decltype(CellInfo::params) params, attrs;
        decltype(CellInfo::ports) ports;
        decltype(CellInfo::pin_data) pins;
    };
    std::vector<Original> originals;
    for (const auto &entry : cells) {
        auto *cell = entry.second.get(); original_cell_order.push_back(entry.first);
        originals.push_back({cell,cell->bel,cell->belStrength,cell->params,cell->attrs,cell->ports,cell->pin_data});
        if (cell->bel != BelId() && (!movable(cell) || cell->type == id_MISTRAL_MLAB)) {
            protected_labs.insert(lab(cell->bel));
            protected_owners.emplace(lab(cell->bel), cell);
        }
    }
    for (const auto &entry : nets) original_net_order.push_back(entry.first);
    for (const auto &entry : net_aliases) original_alias_order.push_back(entry.first);
    std::set<const NetInfo *> boundary;
    for (const auto &entry : ctx->ports) if (entry.second.net) boundary.insert(entry.second.net);
    auto ordinary = [&](const NetInfo *net) {
        return net && net->driver.cell && !net->is_global && !net->clkconstr && !net->region &&
            net->wires.empty() && net->constant_value == IdString() && !boundary.count(net) &&
            !copy_protected(net->attrs,ctx) && net->driver.cell->ports.count(net->driver.port) &&
            net->driver.cell->ports.at(net->driver.port).type == PORT_OUT &&
            net->driver.cell->getPort(net->driver.port) == net;
    };
    auto table_valid = [&](const CellInfo *cell) {
        int width = mistral_remap_report::lut_width(cell->type);
        if (!width || cell->params.size() != 1 || !cell->params.count(id_LUT) ||
            !cell->params.at(id_LUT).is_fully_def() || cell->params.at(id_LUT).size() != (1u << width) ||
            cell->get_pin_state(id_Q) != PIN_SIG || cell->ports.size() != size_t(width+1)) return false;
        for (int pin = 0; pin < width; ++pin) {
            if (!cell->ports.count(copy_pins[pin]) || cell->ports.at(copy_pins[pin]).type != PORT_IN) return false;
            auto state = cell->get_pin_state(copy_pins[pin]); auto net = cell->getPort(copy_pins[pin]);
            if (state == PIN_0 || state == PIN_1) { if (net) return false; }
            else if ((state != PIN_SIG && state != PIN_INV) || !ordinary(net)) return false;
        }
        auto output = cell->getPort(id_Q);
        return cell->ports.count(id_Q) && cell->ports.at(id_Q).type == PORT_OUT && ordinary(output) &&
            output->driver.cell == cell && output->driver.port == id_Q;
    };
    // Diagnostics retain only the first pretrial rejection for each bounded
    // report edge. They use existing names and never intern an IdString.
    std::string first_rejection;
    auto reject = [&](const char *reason, const CellInfo *cell = nullptr, IdString port = IdString(),
                      const NetInfo *net = nullptr, const std::string &detail = std::string()) {
        if (!first_rejection.empty()) return;
        std::ostringstream text;
        text << reason;
        if (cell) text << " cell=" << cell->name.str(ctx) << " type=" << cell->type.str(ctx);
        if (port != IdString()) text << " port=" << port.str(ctx);
        if (net) text << " net=" << net->name.str(ctx);
        if (!detail.empty()) text << ' ' << detail;
        first_rejection = text.str();
    };
    auto ordinary_flags = [&](const NetInfo *net) {
        if (!first_rejection.empty()) return std::string();
        const auto *driver = net ? net->driver.cell : nullptr;
        const bool driver_port_present = driver && driver->ports.count(net->driver.port);
        std::ostringstream text;
        text << "net_present=" << bool(net) << " driver_present=" << bool(driver)
             << " global=" << (net && net->is_global) << " clkconstr=" << (net && bool(net->clkconstr))
             << " region=" << (net && bool(net->region)) << " routed=" << (net && !net->wires.empty())
             << " constant=" << (net && net->constant_value != IdString())
             << " boundary=" << (net && boundary.count(net))
             << " protected=" << (net && copy_protected(net->attrs,ctx))
             << " driver_port_present=" << driver_port_present
             << " driver_port_output=" << (driver_port_present && driver->ports.at(net->driver.port).type == PORT_OUT)
             << " driver_port_matching_net=" << (driver_port_present && driver->getPort(net->driver.port) == net);
        return text.str();
    };
    auto table_flags = [&](const CellInfo *cell) {
        if (!first_rejection.empty()) return std::string();
        const int width = mistral_remap_report::lut_width(cell->type);
        const bool has_table = cell->params.count(id_LUT);
        std::ostringstream text;
        text << "width=" << width << " params_count=" << cell->params.size() << " lut_present=" << has_table
             << " lut_fully_defined=" << (has_table && cell->params.at(id_LUT).is_fully_def())
             << " lut_size=" << (has_table ? cell->params.at(id_LUT).size() : 0)
             << " ports_count=" << cell->ports.size() << " q_pin_state=" << int(cell->get_pin_state(id_Q));
        for (int pin = 0; pin < width && pin < int(copy_pins.size()); ++pin) {
            const auto logical = copy_pins[pin];
            const bool present = cell->ports.count(logical);
            const auto *net = present ? cell->getPort(logical) : nullptr;
            const auto prefix = " input_" + logical.str(ctx);
            text << prefix << "_present=" << present
                 << prefix << "_state=" << int(cell->get_pin_state(logical))
                 << prefix << "_hasnet=" << bool(net)
                 << prefix << "_ordinary=" << ordinary(net);
        }
        return text.str();
    };
    auto owner_flags = [&](const CellInfo *cell) {
        bool keep = false, dont_touch = false;
        for (const auto &attr : cell->attrs) {
            auto name = attr.first.str(ctx);
            keep |= name == "keep";
            dont_touch |= name == "dont_touch";
        }
        std::ostringstream text;
        text << "owner=" << cell->name.str(ctx) << " owner_type=" << cell->type.str(ctx)
             << " bound=" << (cell->bel != BelId()) << " weak=" << (cell->belStrength <= STRENGTH_WEAK)
             << " cluster=" << (cell->cluster != ClusterId()) << " region=" << bool(cell->region)
             << " pseudo=" << cell->isPseudo() << " keep=" << keep << " dont_touch=" << dont_touch
             << " mlab=" << (cell->type == id_MISTRAL_MLAB);
        return text.str();
    };
    auto clocked = [&](CellInfo *cell, IdString port, int count) {
        if (count <= 0 || cell->bel == BelId() || getBelPinsForCellPin(cell,port).empty()) {
            if (count <= 0) reject("clock-count-zero", cell, port, nullptr, "count=" + std::to_string(count));
            else if (cell->bel == BelId()) reject("clock-cell-unplaced", cell, port);
            else reject("clock-physical-pin-missing", cell, port);
            return false;
        }
        for (int index = 0; index < count; ++index) {
            auto info = getPortClockingInfo(cell,port,index);
            auto clock = cell->getPort(info.clock_port);
            if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay() <= 0) {
                const auto detail = "endpoint_port=" + port.str(ctx) + " clock_index=" + std::to_string(index);
                if (!clock) reject("clock-net-missing", cell, info.clock_port, clock, detail);
                else if (!clock->clkconstr) reject("clock-constraint-missing", cell, info.clock_port, clock, detail);
                else reject("clock-period-nonpositive", cell, info.clock_port, clock, detail);
                return false;
            }
        }
        return true;
    };
    struct Edge { PortRef source,sink; double excess; size_t distance; };
    std::vector<Edge> edges;
    std::map<std::tuple<IdString,IdString,IdString>,size_t> seen;
    // Validate the complete report before any new owner or graph edge exists.
    for (const auto &path : mistral_remap_report::validate(ctx,json,true))
        for (size_t index = 0; index < path.edges.size(); ++index) {
            auto source = path.edges[index].first, sink = path.edges[index].second;
            if (source.port != id_Q || !mistral_remap_report::lut_width(source.cell->type) ||
                sink.cell->type != id_MISTRAL_ALUT_ARITH || !sink.port.in(id_A,id_B,id_C,id_D0,id_D1)) continue;
            auto key = std::make_tuple(source.cell->name,sink.cell->name,sink.port);
            auto found = seen.find(key);
            if (found == seen.end()) {
                seen.emplace(key,edges.size()); edges.push_back({source,sink,path.excess,path.edges.size()-index});
            } else {
                auto &edge = edges.at(found->second);
                edge.excess = std::max(edge.excess,path.excess);
                edge.distance = std::min(edge.distance,path.edges.size()-index);
            }
        }
    std::sort(edges.begin(),edges.end(),[&](const Edge &a,const Edge &b) {
        return std::make_tuple(-a.excess,a.distance,a.source.cell->name.str(ctx),a.sink.cell->name.str(ctx),a.sink.port.str(ctx)) <
               std::make_tuple(-b.excess,b.distance,b.source.cell->name.str(ctx),b.sink.cell->name.str(ctx),b.sink.port.str(ctx));
    });
    if (edges.size() > 16) edges.resize(16);
    log_info("LUT driver copy discovery: %zu bounded data edges.\n",edges.size());
    int qualified = 0;
    for (const auto &edge : edges) {
        first_rejection.clear();
        auto *source = edge.source.cell, *sink = edge.sink.cell;
        auto old_net = source->getPort(id_Q); int width = mistral_remap_report::lut_width(source->type);
        auto report_rejection = [&]() {
            if (first_rejection.empty()) reject("pretrial-ineligible");
            log_info("LUT driver copy rejection source=%s sink=%s.%s reason=%s\n",
                     nameOf(source), nameOf(sink), edge.sink.port.c_str(ctx), first_rejection.c_str());
        };
        if (!movable(source) || protected_labs.count(lab(source->bel)) || !table_valid(source) ||
            getBelPinsForCellPin(source,id_Q).empty() ||
            sink->bel == BelId() || sink->region || sink->isPseudo() || copy_protected(sink->attrs,ctx) ||
            sink->getPort(edge.sink.port) != old_net || sink->ports.at(edge.sink.port).type != PORT_IN ||
            getBelPinsForCellPin(sink,edge.sink.port).empty()) {
            if (!movable(source)) reject("source-not-movable", source, id_Q, old_net, owner_flags(source));
            else if (protected_labs.count(lab(source->bel)))
                reject("source-protected-lab", source, id_Q, old_net, owner_flags(protected_owners.at(lab(source->bel))));
            else if (!table_valid(source)) reject("source-lut-table-invalid", source, id_Q, old_net, table_flags(source));
            else if (getBelPinsForCellPin(source,id_Q).empty()) reject("source-physical-pin-missing", source, id_Q, old_net);
            else if (sink->bel == BelId()) reject("sink-unplaced", sink, edge.sink.port, old_net);
            else if (sink->region) reject("sink-region", sink, edge.sink.port, old_net);
            else if (sink->isPseudo()) reject("sink-pseudo", sink, edge.sink.port, old_net);
            else if (copy_protected(sink->attrs,ctx)) reject("sink-protected", sink, edge.sink.port, old_net, owner_flags(sink));
            else if (sink->getPort(edge.sink.port) != old_net) reject("sink-net-mismatch", sink, edge.sink.port, old_net);
            else if (sink->ports.at(edge.sink.port).type != PORT_IN) reject("sink-direction", sink, edge.sink.port, old_net);
            else reject("sink-physical-pin-missing", sink, edge.sink.port, old_net);
            report_rejection();
            continue;
        }
        bool eligible = true;
        std::vector<NetInfo *> inputs;
        for (int pin = 0; pin < width; ++pin) {
            auto net = source->getPort(copy_pins[pin]);
            if (net && std::find(inputs.begin(),inputs.end(),net) == inputs.end()) inputs.push_back(net);
        }
        // An A-edge change also changes the following carry cell's cross-ALM
        // shared-input cache. Its placement and carry links remain untouched.
        std::vector<CellInfo *> cache_cells{sink};
        auto carry = sink->getPort(id_CO);
        if (carry) for (auto user : carry->users) {
            if (carry->driver.cell != sink || carry->driver.port != id_CO || user.port != id_CI ||
                user.cell->type != id_MISTRAL_ALUT_ARITH || user.cell->bel == BelId()) {
                reject("carry-successor-invalid", user.cell, user.port, carry);
                eligible = false;
            }
            else if (std::find(cache_cells.begin(),cache_cells.end(),user.cell) == cache_cells.end()) cache_cells.push_back(user.cell);
        }
        if (carry && carry->users.entries() > 1) {
            reject("carry-successor-multiple", sink, id_CO, carry, "users=" + std::to_string(carry->users.entries()));
            eligible = false;
        }
        std::set<CellPortKey> endpoint_keys;
        std::map<NetInfo *,int> visit;
        std::function<void(NetInfo *)> follow = [&](NetInfo *net) {
            if (!ordinary(net) || visit[net] == 1) {
                if (!ordinary(net)) reject("cone-net-not-ordinary", nullptr, IdString(), net, ordinary_flags(net));
                else reject("cone-cycle", nullptr, IdString(), net);
                eligible = false; return;
            }
            if (visit[net] == 2) return;
            visit[net] = 1;
            for (auto user : net->users) {
                if (!user.cell || !user.cell->ports.count(user.port) ||
                    user.cell->ports.at(user.port).type != PORT_IN || user.cell->getPort(user.port) != net ||
                    !net->users.count(user.cell->ports.at(user.port).user_idx)) {
                    if (!user.cell) reject("cone-user-cell-missing", nullptr, user.port, net);
                    else if (!user.cell->ports.count(user.port)) reject("cone-user-port-missing", user.cell, user.port, net);
                    else if (user.cell->ports.at(user.port).type != PORT_IN) reject("cone-user-direction", user.cell, user.port, net);
                    else if (user.cell->getPort(user.port) != net) reject("cone-user-net-mismatch", user.cell, user.port, net);
                    else reject("cone-user-index-missing", user.cell, user.port, net);
                    eligible = false; continue;
                }
                auto indexed = net->users.at(user.cell->ports.at(user.port).user_idx);
                if (indexed.cell != user.cell || indexed.port != user.port) {
                    reject("cone-user-index-mismatch", user.cell, user.port, net);
                    eligible = false; continue;
                }
                int count = 0; auto kind = getPortTimingClass(user.cell,user.port,count);
                if (kind == TMG_REGISTER_INPUT) {
                    if (!clocked(user.cell,user.port,count)) eligible = false;
                    else endpoint_keys.insert(CellPortKey(user));
                    continue;
                }
                bool known = mistral_remap_report::lut_width(user.cell->type) ||
                    user.cell->type.in(id_MISTRAL_ALUT_ARITH,id_MISTRAL_NOT,id_MISTRAL_BUF);
                if (kind != TMG_COMB_INPUT || !known || user.cell->bel == BelId()) {
                    if (kind != TMG_COMB_INPUT)
                        reject("cone-unsupported-timing-kind", user.cell, user.port, net, "kind=" + std::to_string(int(kind)));
                    else if (!known) reject("cone-unsupported-cell", user.cell, user.port, net);
                    else reject("cone-cell-unplaced", user.cell, user.port, net);
                    eligible = false; continue;
                }
                if (mistral_remap_report::lut_width(user.cell->type) && !table_valid(user.cell)) {
                    reject("cone-lut-table-invalid", user.cell, user.port, net, table_flags(user.cell));
                    eligible = false; continue;
                }
                if (user.cell->type == id_MISTRAL_ALUT_ARITH) {
                    for (auto table : {id_LUT0,id_LUT1})
                        if (!user.cell->params.count(table) || !user.cell->params.at(table).is_fully_def() ||
                            user.cell->params.at(table).size() != 16) {
                            reject("cone-arithmetic-table-invalid", user.cell, user.port, net, "table=" + table.str(ctx));
                            eligible = false;
                        }
                    if (!eligible) continue;
                }
                bool has_arc = false;
                for (const auto &port : user.cell->ports) {
                    if (port.second.type != PORT_OUT || !port.second.net) continue;
                    int clocks = 0; DelayQuad delay;
                    if (getPortTimingClass(user.cell,port.first,clocks) != TMG_COMB_OUTPUT ||
                        !getCellDelay(user.cell,user.port,port.first,delay)) continue;
                    has_arc = true; follow(port.second.net);
                }
                if (!has_arc) {
                    reject("cone-combinational-arc-missing", user.cell, user.port, net);
                    eligible = false;
                }
            }
            visit[net] = 2;
        };
        follow(old_net);
        // New clone inputs acquire loads, so guard every pre-existing branch
        // of each distinct input net as well as all original decoder Q users.
        for (auto input : inputs) {
            int count = 0; auto kind = getPortTimingClass(input->driver.cell,input->driver.port,count);
            if (input->driver.cell->bel == BelId() ||
                getBelPinsForCellPin(input->driver.cell,input->driver.port).empty()) {
                reject(input->driver.cell->bel == BelId() ? "input-driver-unplaced" : "input-driver-physical-pin-missing",
                       input->driver.cell, input->driver.port, input);
                eligible = false;
            }
            if (kind == TMG_REGISTER_OUTPUT) eligible &= clocked(input->driver.cell,input->driver.port,count);
            else if (kind != TMG_COMB_OUTPUT) {
                reject("input-unsupported-timing-kind", input->driver.cell, input->driver.port, input,
                       "kind=" + std::to_string(int(kind)));
                eligible = false;
            }
            follow(input);
        }
        if (!eligible || endpoint_keys.empty()) {
            if (endpoint_keys.empty()) reject("cone-registered-endpoints-empty");
            report_rejection();
            continue;
        }
        TimingAnalyser before(ctx); before.with_clock_skew = true; before.setup(false,false,true);
        if (before.have_loops) {
            reject("timing-loops"); report_rejection(); continue;
        }
        float old_slack = before.get_setup_slack(CellPortKey(edge.sink));
        if (!copy_timed(old_slack) || before.get_timing_result().clock_fmax.empty()) {
            if (!copy_timed(old_slack)) reject("sink-slack-untimed", sink, edge.sink.port, old_net);
            else reject("clock-fmax-empty");
            report_rejection(); continue;
        }
        auto old_holds = copy_holds(before);
        using DomainRows = std::vector<EndpointClockPairTiming>;
        std::map<CellPortKey,DomainRows> endpoints, reference_endpoints;
        size_t timed_pairs = 0, unrelated_pairs = 0, hold_pairs = 0;
        for (auto key : endpoint_keys) {
            DomainRows rows;
            if (!before.get_endpoint_clock_pair_timings(key, rows)) {
                reject("endpoint-domain-coverage-unavailable", cells.at(key.cell).get(), key.port);
                eligible = false;
            }
            else {
                for (const auto &row : rows) {
                    timed_pairs += row.setup_timed;
                    unrelated_pairs += !row.setup_timed;
                    hold_pairs += row.hold_related;
                }
                endpoints.emplace(key, std::move(rows));
            }
        }
        TimingAnalyser before_reference(ctx);
        if (eligible && unrelated_pairs) {
            before_reference.with_clock_skew = false;
            before_reference.setup(false,false,true);
            for (const auto &entry : endpoints) {
                DomainRows rows;
                if (!before_reference.get_endpoint_clock_pair_timings(entry.first, rows) ||
                    !copy_domain_rows_match(entry.second, rows)) {
                    reject("endpoint-reference-coverage-unavailable", cells.at(entry.first.cell).get(), entry.first.port);
                    eligible = false;
                } else {
                    reference_endpoints.emplace(entry.first, std::move(rows));
                }
            }
        }
        std::map<NetInfo *,delay_t> arrivals;
        for (auto input : inputs) {
            delay_t value = 0;
            if (!before.get_max_arrival(CellPortKey(input->driver),value) ||
                value == std::numeric_limits<delay_t>::max()) {
                reject("input-arrival-unavailable", input->driver.cell, input->driver.port, input);
                eligible = false;
            }
            else arrivals.emplace(input,value);
        }
        if (!eligible) { report_rejection(); continue; }
        auto cname = id(source->name.str(ctx) + "$lut_driver_copy$" + sink->name.str(ctx) + "$" + edge.sink.port.str(ctx));
        auto nname = id(cname.str(ctx) + "$Q");
        if (cells.count(cname) || nets.count(nname) || net_aliases.count(nname)) {
            reject("private-name-collision", source, id_Q, old_net,
                   "cell_name_exists=" + std::to_string(cells.count(cname)) +
                   " net_name_exists=" + std::to_string(nets.count(nname)) +
                   " alias_name_exists=" + std::to_string(net_aliases.count(nname)));
            report_rejection(); continue;
        }
        log_info("LUT driver copy discovery source=%s sink=%s.%s users=%zu rows=%u\n",
                 nameOf(source),nameOf(sink),edge.sink.port.c_str(ctx),size_t(old_net->users.entries()),1u<<width);
        log_info("LUT driver copy domains source=%s sink=%s.%s endpoints=%zu timed_pairs=%zu unrelated_pairs=%zu hold_pairs=%zu reference_free=%d\n",
                 nameOf(source),nameOf(sink),edge.sink.port.c_str(ctx),endpoints.size(),timed_pairs,unrelated_pairs,
                 hold_pairs,int(unrelated_pairs != 0));
        std::map<NetInfo *,indexed_store<PortRef>> saved_users;
        saved_users.emplace(old_net,old_net->users);
        for (auto input : inputs) saved_users.emplace(input,input->users);
        auto saved_port = sink->ports.at(edge.sink.port);
        std::map<CellInfo *,decltype(sink->combInfo)> saved_info;
        std::map<uint32_t,LABInfo> saved_labs;
        for (auto *cell : cache_cells) {
            saved_info.emplace(cell,cell->combInfo);
            auto index = bel_data(cell->bel).lab_data.lab;
            saved_labs.emplace(index,labs.at(index));
        }
        decltype(ctx->cells) parked_cells;
        decltype(ctx->nets) parked_nets;
        decltype(ctx->net_aliases) parked_aliases;
        CellInfo *clone = nullptr; NetInfo *output = nullptr; bool live = false;
        auto rollback = [&]() {
            if (!live) return;
            if (clone) {
                if (clone->bel != BelId()) unbindBel(clone->bel);
                for (const auto &port : clone->ports) clone->disconnectPort(port.first);
            }
            sink->disconnectPort(edge.sink.port);
            cells.erase(cname); nets.erase(nname);
            for (auto &entry : cells) parked_cells.at(entry.first) = std::move(entry.second);
            for (auto &entry : nets) parked_nets.at(entry.first) = std::move(entry.second);
            cells.swap(parked_cells); nets.swap(parked_nets); net_aliases.swap(parked_aliases);
            sink->ports.at(edge.sink.port) = saved_port;
            for (auto &entry : saved_users) std::swap(entry.first->users,entry.second);
            for (const auto &entry : saved_info) entry.first->combInfo = entry.second;
            for (const auto &entry : saved_labs) labs.at(entry.first) = entry.second;
            live = false; ctx->check();
        };
        auto original_fixed = [&]() {
            for (const auto &saved : originals) {
                auto *cell = saved.cell;
                if (cell->bel != saved.bel || cell->belStrength != saved.strength || cell->params != saved.params ||
                    cell->attrs != saved.attrs || cell->ports.size() != saved.ports.size() || cell->pin_data.size() != saved.pins.size()) return false;
                for (const auto &pin : saved.pins)
                    if (!cell->pin_data.count(pin.first) || cell->pin_data.at(pin.first).state != pin.second.state ||
                        cell->pin_data.at(pin.first).bel_pins != pin.second.bel_pins) return false;
                for (const auto &port : saved.ports) {
                    if (!cell->ports.count(port.first)) return false;
                    const auto &now = cell->ports.at(port.first);
                    if (now.name != port.second.name || now.type != port.second.type) return false;
                    if (cell == sink && port.first == edge.sink.port) {
                        if (now.net != output) return false;
                    } else if (now.net != port.second.net || now.user_idx != port.second.user_idx) return false;
                }
            }
            return true;
        };
        cells.swap(parked_cells); nets.swap(parked_nets); net_aliases.swap(parked_aliases); live = true;
        try {
            for (auto key = original_cell_order.rbegin(); key != original_cell_order.rend(); ++key)
                cells[*key] = std::move(parked_cells.at(*key));
            for (auto key = original_net_order.rbegin(); key != original_net_order.rend(); ++key)
                nets[*key] = std::move(parked_nets.at(*key));
            net_aliases = parked_aliases;
            clone = ctx->createCell(cname,source->type); clone->params = source->params;
            for (int pin = 0; pin < width; ++pin) {
                clone->addInput(copy_pins[pin]); clone->connectPort(copy_pins[pin],source->getPort(copy_pins[pin]));
                clone->pin_data[copy_pins[pin]].state = source->get_pin_state(copy_pins[pin]);
            }
            clone->addOutput(id_Q); output = ctx->createNet(nname); clone->connectPort(id_Q,output);
            clone->pin_data[id_Q].state = PIN_SIG;
            // This proof includes constants/inversions, rather than comparing
            // only the copied table bytes. No input support is pruned.
            auto value = [&](CellInfo *cell,unsigned row) {
                unsigned index = 0;
                for (int pin = 0; pin < width; ++pin) {
                    auto state = cell->get_pin_state(copy_pins[pin]);
                    bool bit = state == PIN_1 || ((state == PIN_SIG || state == PIN_INV) && bool(row & (1u<<pin)));
                    if (state == PIN_INV) bit = !bit;
                    if (bit) index |= 1u<<pin;
                }
                return bool((uint64_t(cell->params.at(id_LUT).as_int64()) >> index) & 1);
            };
            for (unsigned row = 0; row < (1u<<width); ++row) NPNR_ASSERT(value(source,row) == value(clone,row));
            sink->disconnectPort(edge.sink.port); sink->connectPort(edge.sink.port,output);
            assign_comb_info(clone); assign_default_pinmap(clone);
            for (auto *cell : cache_cells) assign_comb_info(cell);
            for (auto *cell : cache_cells) update_bel(cell->bel);
            // Probes restore the redirected sink's cache, while transaction
            // rollback restores the original graph's cache. They differ when
            // a weak arithmetic sink and the clone occupy the same LAB.
            std::map<uint32_t,LABInfo> probe_labs;
            for (auto *cell : cache_cells) {
                auto index = bel_data(cell->bel).lab_data.lab;
                probe_labs.emplace(index,labs.at(index));
            }
            auto isolated = [&](BelId bel) {
                const auto &target = bel_data(bel).lab_data;
                auto at = getBelLocation(bel);
                for (auto neighbour : getBelsByTile(at.x,at.y)) {
                    auto type = getBelType(neighbour);
                    if (!type.in(id_MISTRAL_COMB,id_MISTRAL_MCOMB,id_MISTRAL_FF)) continue;
                    const auto &other = bel_data(neighbour).lab_data;
                    if (other.lab == target.lab && other.alm == target.alm && getBoundBelCell(neighbour)) return false;
                }
                return true;
            };
            auto legal = [&]() {
                std::set<CopyLab> affected;
                for (auto *cell : cache_cells) affected.insert(lab(cell->bel));
                if (clone->bel != BelId()) affected.insert(lab(clone->bel));
                for (auto at : affected) for (auto bel : getBelsByTile(at.first,at.second))
                    if (getBoundBelCell(bel) && !isBelLocationValid(bel)) return false;
                return original_fixed();
            };
            struct Site { BelId bel; int64_t score; };
            std::vector<Site> sites;
            auto center = lab(sink->bel);
            for (int x = center.first-3; x <= center.first+3; ++x)
                for (int y = center.second-3; y <= center.second+3; ++y) {
                    if (std::abs(x-center.first)+std::abs(y-center.second) > 3 || protected_labs.count({x,y})) continue;
                    for (auto bel : getBelsByTile(x,y)) {
                        if (!checkBelAvail(bel) || !isValidBelForCellType(clone->type,bel) || !isolated(bel)) continue;
                        auto index = bel_data(bel).lab_data.lab;
                        saved_labs.emplace(index,labs.at(index));
                        probe_labs.emplace(index,labs.at(index));
                        bindBel(bel,clone,STRENGTH_WEAK);
                        bool supported = legal(); int64_t incoming = std::numeric_limits<int64_t>::lowest();
                        for (int pin = 0; supported && pin < width; ++pin) {
                            auto net = clone->getPort(copy_pins[pin]); if (!net) continue;
                            DelayQuad logic;
                            if (getBelPinsForCellPin(clone,copy_pins[pin]).empty() ||
                                !getCellDelay(clone,copy_pins[pin],id_Q,logic) || logic.maxDelay() < 0) { supported = false; break; }
                            auto wire = ctx->predictArcDelay(net,{clone,copy_pins[pin]});
                            if (wire < 0) { supported = false; break; }
                            auto cost = int64_t(arrivals.at(net)) + wire + logic.maxDelay();
                            incoming = std::max(incoming,cost);
                        }
                        auto output_wire = ctx->predictArcDelay(output,edge.sink);
                        if (supported && getBelPinsForCellPin(clone,id_Q).size() && output_wire >= 0 &&
                            incoming != std::numeric_limits<int64_t>::lowest())
                            sites.push_back({bel,incoming+output_wire});
                        unbindBel(bel); labs.at(index) = probe_labs.at(index);
                    }
                }
            std::sort(sites.begin(),sites.end(),[&](const Site &a,const Site &b) {
                auto x = getBelLocation(a.bel), y = getBelLocation(b.bel);
                return std::make_tuple(a.score,x.x,x.y,x.z) < std::make_tuple(b.score,y.x,y.y,y.z);
            });
            if (sites.size() > 24) sites.resize(24);
            int timed = 0;
            for (const auto &site : sites) {
                if (timed == 16) break;
                auto index = bel_data(site.bel).lab_data.lab;
                bindBel(site.bel,clone,STRENGTH_WEAK);
                if (!legal()) { unbindBel(site.bel); labs.at(index) = probe_labs.at(index); continue; }
                ++timed;
                TimingAnalyser after(ctx); after.with_clock_skew = true; after.setup(false,false,true);
                float slack = after.get_setup_slack(CellPortKey(edge.sink));
                bool improve = !after.have_loops && copy_timed(slack) && slack > old_slack && slack >= old_slack+250;
                bool endpoint_ok = true, clocks = true;
                for (const auto &entry : endpoints) {
                    DomainRows rows;
                    endpoint_ok &= after.get_endpoint_clock_pair_timings(entry.first, rows) &&
                        copy_domain_rows_nonregressing(entry.second, rows, false);
                }
                if (!reference_endpoints.empty()) {
                    TimingAnalyser after_reference(ctx); after_reference.with_clock_skew = false;
                    after_reference.setup(false,false,true);
                    for (const auto &entry : reference_endpoints) {
                        DomainRows rows;
                        endpoint_ok &= after_reference.get_endpoint_clock_pair_timings(entry.first, rows) &&
                            copy_domain_rows_nonregressing(entry.second, rows, true);
                    }
                }
                const auto &old_clocks = before.get_timing_result().clock_fmax;
                const auto &new_clocks = after.get_timing_result().clock_fmax;
                clocks = old_clocks.size() == new_clocks.size();
                for (const auto &clock : old_clocks)
                    if (!new_clocks.count(clock.first) || !std::isfinite(new_clocks.at(clock.first).achieved) ||
                        new_clocks.at(clock.first).constraint != clock.second.constraint ||
                        new_clocks.at(clock.first).achieved+1e-4 < clock.second.achieved) clocks = false;
                bool hold = enable_replication_policy::hold_nonregressing(old_holds,copy_holds(after));
                log_info("LUT driver copy trial source=%s sink=%s.%s bel=%s gain=%.0fps improve=%d endpoints=%d clocks=%d hold=%d\n",
                         nameOf(source),nameOf(sink),edge.sink.port.c_str(ctx),nameOfBel(site.bel),slack-old_slack,
                         int(improve),int(endpoint_ok),int(clocks),int(hold));
                if (improve && endpoint_ok && clocks && hold) {
                    log_info("LUT driver copy candidate %d: source=%s sink=%s.%s bel=%s gain=%.0fps.\n",
                             qualified,nameOf(source),nameOf(sink),edge.sink.port.c_str(ctx),nameOfBel(site.bel),slack-old_slack);
                    if (selection == qualified++) {
                        // dict iterates insertion storage backwards. Put new
                        // owners first in storage so every original remains
                        // the exact iteration prefix used by GPU net IDs.
                        decltype(ctx->cells) accepted_cells;
                        decltype(ctx->nets) accepted_nets;
                        decltype(ctx->net_aliases) accepted_aliases;
                        accepted_cells[cname] = nullptr; accepted_nets[nname] = nullptr;
                        accepted_aliases[nname] = nname;
                        for (auto key = original_cell_order.rbegin(); key != original_cell_order.rend(); ++key) accepted_cells[*key] = nullptr;
                        for (auto key = original_net_order.rbegin(); key != original_net_order.rend(); ++key) accepted_nets[*key] = nullptr;
                        for (auto key = original_alias_order.rbegin(); key != original_alias_order.rend(); ++key) accepted_aliases[*key] = parked_aliases.at(*key);
                        auto owner_moves = [](auto &from,auto &to) {
                            using Slot = decltype(&from.begin()->second);
                            std::vector<std::pair<Slot,Slot>> moves; moves.reserve(to.size());
                            for (auto &entry : to) {
                                auto &owner = from.at(entry.first); NPNR_ASSERT(owner && !entry.second);
                                moves.emplace_back(&owner,&entry.second);
                            }
                            return moves;
                        };
                        auto cell_moves = owner_moves(cells,accepted_cells);
                        auto net_moves = owner_moves(nets,accepted_nets);
                        for (auto slots : cell_moves) *slots.second = std::move(*slots.first);
                        for (auto slots : net_moves) *slots.second = std::move(*slots.first);
                        cells.swap(accepted_cells); nets.swap(accepted_nets); net_aliases.swap(accepted_aliases);
                        ctx->check(); live = false;
                        log_info("LUT driver copy applied candidate %d; full routing and signoff still required.\n",selection);
                        return true;
                    }
                }
                unbindBel(site.bel); labs.at(index) = probe_labs.at(index);
            }
            log_info("LUT driver copy probes: source=%s sink=%s.%s timed=%d\n",nameOf(source),nameOf(sink),edge.sink.port.c_str(ctx),timed);
            rollback();
        } catch (...) { rollback(); throw; }
    }
    log_info("LUT driver copy: %d qualified candidates; no candidate applied.\n",qualified);
    return false;
}
NEXTPNR_NAMESPACE_END
