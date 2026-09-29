/* Experimental report-guided capture placement. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "json11.hpp"
#include "timing.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Lab = std::pair<int, int>;
std::map<std::string, int> capture_holds(TimingAnalyser &timing)
{
    std::map<std::string, int> result;
    for (const auto &path : timing.get_timing_result().min_delay_violations) {
        if (path.segments.empty()) continue;
        int slack = 0;
        for (const auto &segment : path.segments) slack += segment.delay;
        auto end = path.segments.back().to;
        auto key = std::to_string(path.clock_pair.start.clock.index) + ":" +
            std::to_string(int(path.clock_pair.start.edge)) + ":" +
            std::to_string(path.clock_pair.end.clock.index) + ":" +
            std::to_string(int(path.clock_pair.end.edge)) + ":" +
            std::to_string(end.first.index) + ":" + std::to_string(end.second.index);
        if (!result.count(key)) result[key] = slack;
        else result[key] = std::min(result[key], slack);
    }
    return result;
}
}

int capture_locality(Context *ctx, const std::string &report, int budget, int radius)
{
    if (budget < 1 || budget > 64 || radius < 1 || radius > 24)
        log_error("Capture locality budget must be 1..64 and radius 1..24.\n");
    if (ctx->fes_any_slot_region_active) log_error("Capture locality requires ordinary placement.\n");
    for (const auto &net : ctx->nets)
        if (!net.second->wires.empty()) log_error("Capture locality requires an unrouted design.\n");
    std::string error;
    auto json = json11::Json::parse(report, error);
    if (!error.empty() || !json["critical_paths"].is_array()) log_error("Invalid capture timing report.\n");
    auto lab = [&](BelId bel) { auto loc = ctx->getBelLocation(bel); return Lab(loc.x, loc.y); };
    auto movable = [&](const CellInfo *cell) {
        return cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK &&
            cell->cluster == ClusterId() && !cell->region && !cell->isPseudo() &&
            !cell->attrs.count(ctx->id("keep")) && !cell->attrs.count(ctx->id("dont_touch"));
    };
    std::set<Lab> protected_labs;
    std::map<IdString, int> bel_counts;
    std::map<std::string, CellInfo *> named;
    for (auto bel : ctx->getBels()) ++bel_counts[ctx->getBelType(bel)];
    for (auto &item : ctx->cells) {
        auto *cell = item.second.get(); named.emplace(cell->name.str(ctx), cell);
        if (cell->bel != BelId() && (!movable(cell) || cell->type == id_MISTRAL_MLAB))
            protected_labs.insert(lab(cell->bel));
    }
    std::set<NetInfo *> boundary;
    for (const auto &port : ctx->ports) if (port.second.net) boundary.insert(port.second.net);
    auto ordinary = [&](const NetInfo *net) {
        return net && !net->region && !net->is_global && !net->clkconstr &&
            net->constant_value == IdString() && !boundary.count(const_cast<NetInfo *>(net)) &&
            !net->attrs.count(ctx->id("keep")) && !net->attrs.count(ctx->id("dont_touch"));
    };
    auto registered_source = [&](NetInfo *net, TimingClockingInfo &info) {
        if (!ordinary(net) || !net->driver.cell || net->driver.cell->bel == BelId() || net->driver.cell->isPseudo()) return false;
        auto *driver = net->driver.cell;
        if (bel_counts[ctx->getBelType(driver->bel)] != 1) return false;
        int clocks = 0;
        if (ctx->getPortTimingClass(driver, net->driver.port, clocks) != TMG_REGISTER_OUTPUT || clocks != 1) return false;
        info = ctx->getPortClockingInfo(driver, net->driver.port, 0);
        return ctx->getNetinfoSourceWire(net) != WireId();
    };
    CellInfo *critical = nullptr, *source = nullptr;
    IdString source_clock;
    double worst_excess = 0;
    for (const auto &path : json["critical_paths"].array_items()) {
        auto segments = path["path"].array_items();
        if (segments.empty() || segments.back()["type"].string_value() != "setup" ||
            segments.back()["to"]["port"].string_value() != "DATAIN") continue;
        auto end = segments.back()["to"];
        auto found = named.find(end["cell"].string_value());
        if (found == named.end()) log_error("Stale capture report endpoint.\n");
        auto *cell = found->second;
        if (cell->type != id_MISTRAL_FF) continue;
        auto *data = cell->getPort(id_DATAIN);
        TimingClockingInfo info;
        if (!registered_source(data, info)) continue;
        auto xy = end["loc"].array_items(); auto loc = lab(cell->bel);
        if (xy.size() != 2 || !xy[0].is_number() || !xy[1].is_number() ||
            xy[0].number_value() != loc.first || xy[1].number_value() != loc.second)
            log_error("Stale capture report placement.\n");
        bool edge_matches = false;
        double delay = 0;
        for (const auto &segment : segments) {
            if (!segment["delay"].is_number() || !std::isfinite(segment["delay"].number_value()))
                log_error("Malformed capture report delay.\n");
            delay += segment["delay"].number_value();
            if (segment["type"].string_value() == "routing" &&
                segment["from"]["cell"].string_value() == data->driver.cell->name.str(ctx) &&
                segment["from"]["port"].string_value() == data->driver.port.str(ctx) &&
                segment["net"].string_value() == data->name.str(ctx)) edge_matches = true;
        }
        if (!edge_matches || !path["max_delay"].is_number() || path["max_delay"].number_value() <= 0)
            log_error("Stale or malformed capture report edge.\n");
        double excess = delay - path["max_delay"].number_value();
        if (excess > worst_excess) { worst_excess = excess; critical = cell; source = data->driver.cell; source_clock = info.clock_port; }
    }
    if (!source) { log_info("Capture locality: no eligible critical source.\n"); return 0; }
    std::vector<CellInfo *> group;
    auto *clock = source->getPort(source_clock);
    if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay() <= 0)
        log_error("Capture locality requires a derived source clock constraint.\n");
    for (auto &item : ctx->cells) {
        auto *cell = item.second.get();
        if (cell->type != id_MISTRAL_FF || !movable(cell) || protected_labs.count(lab(cell->bel)) ||
            cell->getPort(id_CLK) != clock) continue;
        auto *data = cell->getPort(id_DATAIN), *out = cell->getPort(id_Q);
        TimingClockingInfo info;
        if (!registered_source(data, info) || data->driver.cell != source || info.clock_port != source_clock ||
            info.edge != ctx->getPortClockingInfo(cell, id_DATAIN, 0).edge || !ordinary(out) ||
            out->users.empty() || out->users.entries() > 4) continue;
        bool safe = true;
        for (IdString control : {id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA}) if (cell->getPort(control)) safe = false;
        for (auto user : out->users) {
            if (user.cell->type != id_MISTRAL_FF || user.port != id_DATAIN || user.cell->bel == BelId() ||
                user.cell->getPort(id_CLK) != clock ||
                ctx->getPortClockingInfo(user.cell, id_DATAIN, 0).edge != info.edge) safe = false;
        }
        if (safe) group.push_back(cell);
    }
    auto incoming = [&](CellInfo *cell, BelId bel) {
        auto *data = cell->getPort(id_DATAIN);
        auto info = ctx->getPortClockingInfo(data->driver.cell, data->driver.port, 0);
        return info.clockToQ.maxDelay() + ctx->estimateDelay(ctx->getNetinfoSourceWire(data), ctx->getBelPinWire(bel,id_DATAIN));
    };
    std::sort(group.begin(), group.end(), [&](CellInfo *a, CellInfo *b) {
        if (a == critical || b == critical) return a == critical && b != critical;
        auto da = incoming(a,a->bel), db = incoming(b,b->bel);
        if (da != db) return da > db;
        return a->name.str(ctx) < b->name.str(ctx);
    });
    if (int(group.size()) > budget) group.resize(budget);
    if (group.empty()) { log_info("Capture locality: no movable plain capture pipelines.\n"); return 0; }
    TimingAnalyser before(ctx); before.setup(false,false,true);
    auto old_holds = capture_holds(before);
    std::map<CellInfo *, BelId> old;
    std::set<CellPortKey> output_endpoints;
    for (auto *cell : group) for (auto user : cell->getPort(id_Q)->users) output_endpoints.insert(CellPortKey(user));
    auto score = [&](CellInfo *cell, BelId bel) {
        int in = incoming(cell,bel), out = 0;
        int launch = ctx->getPortClockingInfo(cell,id_Q,0).clockToQ.maxDelay();
        for (auto user : cell->getPort(id_Q)->users)
            out = std::max(out, launch + ctx->estimateDelay(ctx->getBelPinWire(bel,id_Q),ctx->getNetinfoSinkWire(cell->getPort(id_Q),user,0)));
        return std::make_tuple(std::max(in,out),in+out,in,out);
    };
    for (auto *cell : group) {
        BelId original = cell->bel; auto loc = ctx->getBelLocation(original); auto initial = score(cell,original);
        using Candidate = std::tuple<int,int,int,int,int,int,int,int,BelId>;
        std::vector<Candidate> candidates;
        for (auto bel : ctx->getBels()) {
            auto at = ctx->getBelLocation(bel);
            if (ctx->getBelType(bel) != id_MISTRAL_FF || ctx->getBoundBelCell(bel) ||
                protected_labs.count(lab(bel)) || std::abs(at.x-loc.x) > radius || std::abs(at.y-loc.y) > radius ||
                (at.z%6 != 2 && at.z%6 != 4)) continue;
            // Keep an ordinary feed-through half available; do not evict logic.
            int comb_z = (at.z/6)*6 + (at.z%6 == 4 ? 1 : 0);
            auto comb = ctx->getBelByLocation(Loc(at.x,at.y,comb_z));
            auto partner = ctx->getBelByLocation(Loc(at.x,at.y,at.z+1));
            if (comb == BelId() || ctx->getBoundBelCell(comb) || partner == BelId() || ctx->getBoundBelCell(partner)) continue;
            auto value = score(cell,bel);
            if (std::get<0>(value)+500 >= std::get<0>(initial) ||
                std::get<3>(value) >= clock->clkconstr->period.minDelay()) continue;
            int distance = std::abs(at.x-loc.x)+std::abs(at.y-loc.y);
            candidates.emplace_back(std::get<0>(value),std::get<1>(value),distance,at.x,at.y,at.z,std::get<2>(value),std::get<3>(value),bel);
        }
        std::sort(candidates.begin(),candidates.end());
        for (const auto &candidate : candidates) {
            BelId target = std::get<8>(candidate);
            ctx->unbindBel(original); ctx->bindBel(target,cell,STRENGTH_WEAK);
            if (ctx->isBelLocationValid(target) && ctx->isBelLocationValid(original)) {
                old.emplace(cell,original);
                log_info("Capture locality moved %s: %s -> %s; predicted in %d->%d ps, out %d->%d ps.\n",
                    ctx->nameOf(cell),ctx->getBelName(original).str(ctx).c_str(),ctx->getBelName(target).str(ctx).c_str(),
                    std::get<2>(initial),std::get<6>(candidate),std::get<3>(initial),std::get<7>(candidate));
                break;
            }
            ctx->unbindBel(target); ctx->bindBel(original,cell,STRENGTH_WEAK);
        }
    }
    TimingAnalyser after(ctx); after.setup(false,false,true);
    bool safe = true;
    for (const auto &entry : before.get_timing_result().clock_fmax) {
        auto &now = after.get_timing_result().clock_fmax;
        if (!now.count(entry.first) || now.at(entry.first).achieved + 0.001f < entry.second.achieved) safe = false;
    }
    for (auto endpoint : output_endpoints)
        if (before.get_setup_slack(endpoint) >= 0 && after.get_setup_slack(endpoint) < 0) safe = false;
    for (const auto &entry : capture_holds(after))
        if (!old_holds.count(entry.first) || entry.second < old_holds.at(entry.first)) safe = false;
    if (!safe) {
        for (const auto &entry : old) ctx->unbindBel(entry.first->bel);
        for (const auto &entry : old) ctx->bindBel(entry.second,entry.first,STRENGTH_WEAK);
        log_info("Capture locality rolled back %zu moves after abstract timing checks.\n",old.size());
        ctx->check(); return 0;
    }
    ctx->check();
    log_info("Capture locality: source %s.%s, eligible %zu, retained %zu moves (route signoff still required).\n",
        ctx->nameOf(source),source_clock.c_str(ctx),group.size(),old.size());
    return int(old.size());
}

void diagnostic_capture_locality(Context *ctx, const char *spec)
{
    if (!spec) return;
    std::istringstream options(spec); std::string path; int budget, radius;
    if (!(options >> path >> budget >> radius)) log_error("Invalid capture locality diagnostic options.\n");
    std::ifstream file(path);
    if (!file) log_error("Cannot read capture locality timing report.\n");
    std::string report((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    capture_locality(ctx,report,budget,radius);
}
NEXTPNR_NAMESPACE_END
