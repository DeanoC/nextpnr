/* Experimental local placement of a verified reduction. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "timing.h"
#include "reduction_balance_plan.h"
#include "enable_replication_policy.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Lab = std::pair<int, int>;
const IdString pins[] = {id_A,id_B,id_C,id_D,id_E,id_F};
int width(IdString t)
{
    if (t == id_MISTRAL_ALUT2) return 2;
    if (t == id_MISTRAL_ALUT3) return 3;
    if (t == id_MISTRAL_ALUT4) return 4;
    if (t == id_MISTRAL_ALUT5) return 5;
    if (t == id_MISTRAL_ALUT6) return 6;
    return 0;
}
bool protected_attrs(const dict<IdString,Property> &attrs, Context *ctx)
{
    for (const auto &attr : attrs) {
        auto name = attr.first.str(ctx);
        if (name == "keep" || name == "dont_touch") return true;
    }
    return false;
}
std::map<std::string,int> holds(TimingAnalyser &timing)
{
    std::map<std::string,int> result;
    for (const auto &path : timing.get_timing_result().min_delay_violations) {
        if (path.segments.empty()) continue;
        int slack = 0;
        for (const auto &s : path.segments) slack += s.delay;
        auto end = path.segments.back().to;
        auto key = std::to_string(path.clock_pair.start.clock.index)+":"+
            std::to_string(int(path.clock_pair.start.edge))+":"+std::to_string(path.clock_pair.end.clock.index)+":"+
            std::to_string(int(path.clock_pair.end.edge))+":"+std::to_string(end.first.index)+":"+std::to_string(end.second.index);
        if (!result.count(key)) result[key] = slack;
        else result[key] = std::min(result[key],slack);
    }
    return result;
}
}

bool placed_reduction(Context *ctx, const std::string &root_name, int radius, int selection)
{
    if (radius < 1 || radius > 6 || selection < -1) log_error("Placed reduction needs radius1..6 and selection>=-1.\n");
    if (ctx->fes_any_slot_region_active) log_error("Placed reduction requires ordinary placement.\n");
    for (const auto &net : ctx->nets)
        if (!net.second->wires.empty()) log_error("Placed reduction requires an unrouted design.\n");
    ReductionBalancePlan plan;
    if (!plan_reduction(ctx,root_name,true,plan) || plan.cells.size() != 3) {
        log_info("Placed reduction: root is not a movable three-LUT/eleven-literal conjunction.\n"); return false;
    }
    auto lab = [&](BelId bel) { auto loc = ctx->getBelLocation(bel); return Lab(loc.x,loc.y); };
    std::set<Lab> protected_labs;
    std::map<CellInfo *,std::pair<BelId,PlaceStrength>> all_places;
    for (const auto &item : ctx->cells) {
        auto *cell = item.second.get(); all_places[cell] = {cell->bel,cell->belStrength};
        if (cell->bel != BelId() && (cell->belStrength > STRENGTH_WEAK || cell->cluster != ClusterId() ||
            cell->region || cell->isPseudo() || cell->type == id_MISTRAL_MLAB || protected_attrs(cell->attrs,ctx)))
            protected_labs.insert(lab(cell->bel));
    }
    for (auto *cell : plan.cells) if (protected_labs.count(lab(cell->bel))) return false;
    std::set<CellPortKey> endpoints;
    std::set<NetInfo *> visited;
    auto collect = [&](auto &&self, NetInfo *net) -> bool {
        if (!net || !visited.insert(net).second || visited.size() > 64 || net->is_global || net->clkconstr ||
            net->region || protected_attrs(net->attrs,ctx)) return false;
        for (auto user : net->users) {
            if (user.cell->type == id_MISTRAL_FF && user.port != id_CLK) {
                int clocks = 0;
                if (ctx->getPortTimingClass(user.cell,user.port,clocks) != TMG_REGISTER_INPUT || clocks != 1) return false;
                endpoints.insert(CellPortKey(user));
            } else if (width(user.cell->type)) {
                if (!self(self,user.cell->getPort(id_Q))) return false;
            } else return false;
        }
        return true;
    };
    if (!collect(collect,plan.root->getPort(id_Q)) || endpoints.empty()) return false;
    TimingAnalyser before(ctx); before.setup(false,false,true);
    auto old_holds = holds(before);
    auto timed = [](float slack) { return std::isfinite(slack) && slack < float(std::numeric_limits<delay_t>::max()); };
    const CellPortKey root_output(plan.root->name,id_Q);
    const auto old_root_slack=before.get_setup_slack(root_output);
    if (!timed(old_root_slack)) return false;
    float old_slack = std::numeric_limits<float>::max();
    for (auto ep : endpoints) {
        auto value = before.get_setup_slack(ep);
        if (!timed(value)) return false;
        old_slack = std::min(old_slack,value);
    }
    struct Saved {
        CellInfo *cell; IdString type;
        decltype(CellInfo::ports) ports;
        decltype(CellInfo::params) params;
        decltype(CellInfo::pin_data) pin_data;
        BelId bel; PlaceStrength strength;
    };
    std::vector<Saved> saved;
    std::map<NetInfo *,indexed_store<PortRef>> users;
    std::set<Lab> original_labs;
    for (auto *cell : plan.cells) {
        saved.push_back({cell,cell->type,cell->ports,cell->params,cell->pin_data,cell->bel,cell->belStrength});
        original_labs.insert(lab(cell->bel));
    }
    for (const auto &entry : plan.slots) users.emplace(entry.first,entry.first->users);
    auto unbind = [&]() { for (auto *cell : plan.cells) if (cell->bel != BelId()) ctx->unbindBel(cell->bel); };
    auto rollback = [&]() {
        unbind();
        for (auto &s : saved) { s.cell->type=s.type; s.cell->ports=s.ports; s.cell->params=s.params; s.cell->pin_data=s.pin_data; }
        for (auto &u : users) std::swap(u.first->users,u.second);
        ctx->assignArchInfo();
        for (auto &s : saved) ctx->bindBel(s.bel,s.cell,s.strength);
        ctx->check();
    };
    unbind(); rewrite_reduction(ctx,plan); ctx->assignArchInfo();
    const auto root_place = all_places.at(plan.root);
    ctx->bindBel(root_place.first,plan.root,root_place.second);
    std::vector<CellInfo *> children;
    for (auto *cell : plan.cells) if (cell != plan.root) children.push_back(cell);
    auto child_score = [&](CellInfo *cell) {
        int incoming = 0;
        for (int i=0;i<width(cell->type);++i) {
            DelayQuad delay; ctx->getCellDelay(cell,pins[i],id_Q,delay);
            incoming = std::max(incoming,int(ctx->predictArcDelay(cell->getPort(pins[i]),{cell,pins[i]}))+delay.maxDelay());
        }
        IdString root_pin;
        for (int i=0;i<2;++i) if (plan.root->getPort(pins[i]) == cell->getPort(id_Q)) root_pin=pins[i];
        return incoming+int(ctx->predictArcDelay(cell->getPort(id_Q),{plan.root,root_pin}));
    };
    struct Site { int score; BelId bel; };
    std::vector<Site> sites[2];
    for (int child=0;child<2;++child) {
        for (auto bel : ctx->getBels()) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(children[child]->type,bel) ||
                protected_labs.count(lab(bel))) continue;
            auto at=lab(bel); bool nearby=false;
            for (auto old : original_labs)
                nearby |= std::abs(at.first-old.first)+std::abs(at.second-old.second) <= radius;
            if (!nearby) continue;
            ctx->bindBel(bel,children[child],all_places.at(children[child]).second);
            if (ctx->isBelLocationValid(bel)) sites[child].push_back({child_score(children[child]),bel});
            ctx->unbindBel(bel);
        }
        std::sort(sites[child].begin(),sites[child].end(),[&](const Site &a,const Site &b) {
            auto x=ctx->getBelLocation(a.bel),y=ctx->getBelLocation(b.bel);
            return std::make_tuple(a.score,x.x,x.y,x.z) < std::make_tuple(b.score,y.x,y.y,y.z);
        });
        // Keep spatial diversity: otherwise one LAB's many ALUT sites can
        // exhaust the shortlist before any different LAB pair is timed.
        std::vector<Site> shortlist;
        std::map<Lab,int> per_lab;
        auto original=all_places.at(children[child]).first;
        for (const auto &site : sites[child]) if (site.bel==original) {
            shortlist.push_back(site); ++per_lab[lab(site.bel)]; break;
        }
        for (const auto &site : sites[child]) {
            if (site.bel==original || per_lab[lab(site.bel)]>=2) continue;
            shortlist.push_back(site); ++per_lab[lab(site.bel)];
            if (shortlist.size()==24) break;
        }
        sites[child]=std::move(shortlist);
    }
    struct Trial { int score; BelId a,b; };
    std::vector<Trial> trials;
    for (const auto &a : sites[0]) for (const auto &b : sites[1])
        if (a.bel != b.bel) trials.push_back({std::max(a.score,b.score),a.bel,b.bel});
    std::sort(trials.begin(),trials.end(),[](const Trial &a,const Trial &b) {
        return std::make_tuple(a.score,a.a,a.b) < std::make_tuple(b.score,b.a,b.b);
    });
    int examined=0,qualified=0;
    std::set<std::pair<Lab,Lab>> tested;
    for (const auto &trial : trials) {
        if (tested.count({lab(trial.a),lab(trial.b)})) continue;
        for (auto *child : children) if (child->bel != BelId()) ctx->unbindBel(child->bel);
        ctx->bindBel(trial.a,children[0],all_places.at(children[0]).second);
        ctx->bindBel(trial.b,children[1],all_places.at(children[1]).second);
        std::set<Lab> affected=original_labs; affected.insert(lab(trial.a)); affected.insert(lab(trial.b));
        bool legal=true;
        for (const auto &entry : ctx->cells)
            if (entry.second->bel != BelId() && affected.count(lab(entry.second->bel)))
                legal &= ctx->isBelLocationValid(entry.second->bel);
        if (!legal) continue;
        if (examined++==16) break;
        tested.insert({lab(trial.a),lab(trial.b)});
        TimingAnalyser after(ctx); after.setup(false,false,true);
        float slack=std::numeric_limits<float>::max(); bool endpoints_safe=true;
        for (auto ep : endpoints) {
            auto now=after.get_setup_slack(ep); slack=std::min(slack,now);
            endpoints_safe &= timed(now) && now >= before.get_setup_slack(ep);
        }
        // Another fanin can dominate endpoint slack before routing. Measure
        // this branch at its fixed output, while guarding every endpoint.
        auto root_slack=after.get_setup_slack(root_output);
        bool improve=timed(root_slack) && root_slack>=old_root_slack+250;
        bool clocks=true;
        for (const auto &clock : before.get_timing_result().clock_fmax) {
            const auto &now=after.get_timing_result().clock_fmax;
            if (!now.count(clock.first) || now.at(clock.first).achieved+0.001f < clock.second.achieved) clocks=false;
        }
        bool hold=enable_replication_policy::hold_nonregressing(old_holds,holds(after));
        log_info("Placed reduction trial root=%s leaves=%s,%s branch_gain=%.0fps endpoint_gain=%.0fps improve=%d endpoints=%d clocks=%d hold=%d.\n",
            ctx->nameOf(plan.root),ctx->nameOfBel(trial.a),ctx->nameOfBel(trial.b),root_slack-old_root_slack,
            slack-old_slack,int(improve),int(endpoints_safe),int(clocks),int(hold));
        if (!improve || !endpoints_safe || !clocks || !hold) continue;
        log_info("Placed reduction candidate %d, %zu endpoints, branch gain %.0fps.\n",qualified,endpoints.size(),root_slack-old_root_slack);
        if (selection==qualified++) {
            for (const auto &place : all_places)
                if (std::find(plan.cells.begin(),plan.cells.end(),place.first)==plan.cells.end()) {
                    NPNR_ASSERT(place.first->bel==place.second.first && place.first->belStrength==place.second.second);
                }
            ctx->check(); return true;
        }
    }
    rollback();
    log_info("Placed reduction: %d qualified candidates; none applied.\n",qualified);
    return false;
}

void diagnostic_placed_reduction(Context *ctx,const char *spec)
{
    if (!spec || !*spec) return;
    std::istringstream options(spec); std::string root,extra; int radius,selection;
    if (!(options>>root>>radius>>selection) || (options>>extra)) log_error("Invalid placed reduction diagnostic options.\n");
    if (!placed_reduction(ctx,root,radius,selection) && selection>=0)
        log_error("Requested placed reduction candidate was not qualified; routing was not started.\n");
}
NEXTPNR_NAMESPACE_END
