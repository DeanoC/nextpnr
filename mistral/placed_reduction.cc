/* Experimental local placement of a verified reduction. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "timing.h"
#include "reduction_balance_plan.h"
#include "placed_reduction_policy.h"
#include "enable_replication_policy.h"
#include <algorithm>
#include <array>
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

bool placed_reduction(Context *ctx, const std::string &root_name, int radius, int selection,
                      int minimum_branch_gain_ps = 250)
{
    if (radius < 1 || radius > 6 || selection < -1) log_error("Placed reduction needs radius1..6 and selection>=-1.\n");
    if (minimum_branch_gain_ps < 1) log_error("Placed reduction needs a positive minimum branch gain in ps.\n");
    if (minimum_branch_gain_ps != 250)
        log_info("Placed reduction minimum branch gain: %dps.\n", minimum_branch_gain_ps);
    if (ctx->fes_any_slot_region_active) log_error("Placed reduction requires ordinary placement.\n");
    for (const auto &net : ctx->nets)
        if (!net.second->wires.empty()) log_error("Placed reduction requires an unrouted design.\n");
    ReductionBalancePlan plan;
    if (!plan_reduction(ctx,root_name,true,plan) || (plan.cells.size() != 3 && plan.cells.size() != 7)) {
        log_info("Placed reduction: root is not a movable three-LUT/7..12-literal or seven-LUT/24-literal conjunction.\n"); return false;
    }
    const bool wide = plan.cells.size() == 7;
    std::array<int,4> root_delays{};
    {
        // Score the ALUT2 or ALUT4 that the rewrite creates without changing
        // the live graph or interning any new IDs.
        CellInfo future_root(ctx,plan.root->name,wide ? id_MISTRAL_ALUT4 : id_MISTRAL_ALUT2);
        const size_t root_width=wide ? 4 : 2;
        for (size_t pin=0;pin<root_width;++pin) {
            DelayQuad delay;
            if (!ctx->getCellDelay(&future_root,pins[pin],id_Q,delay)) {
                log_info("Placed reduction: rewritten root input has no timing arc.\n");
                return false;
            }
            root_delays[pin]=delay.maxDelay();
        }
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
    std::set<NetInfo *> active;
    auto collect = [&](auto &&self, NetInfo *net) -> bool {
        // The shared 24-literal root can reach a register through multiple
        // downstream branches. Allow a completed DAG branch, but reject a
        // currently active branch so combinational cycles remain unsafe.
        if (!net || (wide && active.count(net))) return false;
        if (!visited.insert(net).second) return wide;
        if (visited.size() > 64 || net->is_global || net->clkconstr ||
            net->region || protected_attrs(net->attrs,ctx)) return false;
        if (wide) active.insert(net);
        for (auto user : net->users) {
            if (user.cell->type == id_MISTRAL_FF && user.port != id_CLK) {
                int clocks = 0;
                if (ctx->getPortTimingClass(user.cell,user.port,clocks) != TMG_REGISTER_INPUT || clocks != 1) return false;
                endpoints.insert(CellPortKey(user));
            } else if (width(user.cell->type)) {
                if (!self(self,user.cell->getPort(id_Q))) return false;
            } else return false;
        }
        if (wide) active.erase(net);
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
    std::map<NetInfo *,PortRef> drivers;
    std::set<Lab> original_labs;
    for (auto *cell : plan.cells) {
        saved.push_back({cell,cell->type,cell->ports,cell->params,cell->pin_data,cell->bel,cell->belStrength});
        original_labs.insert(lab(cell->bel));
    }
    for (const auto &entry : plan.slots) {
        users.emplace(entry.first,entry.first->users);
        drivers.emplace(entry.first,entry.first->driver);
    }
    // Keep the original dictionaries, including their iteration order and
    // allocation, while two retired objects are absent from live timing and
    // architecture scans. Moving surviving owners back restores the exact
    // dictionaries on rejection; pointers remain stable throughout the probe.
    decltype(ctx->cells) parked_cells;
    decltype(ctx->nets) parked_nets;
    decltype(ctx->net_aliases) parked_aliases;
    auto retire = [&]() {
        if (plan.retired.empty()) return;
        ctx->net_aliases.swap(parked_aliases);
        ctx->net_aliases = parked_aliases;
        remove_reduction_net_aliases(ctx,plan);
        ctx->cells.swap(parked_cells);
        for (auto &entry : parked_cells)
            if (std::find(plan.retired.begin(),plan.retired.end(),entry.second.get()) == plan.retired.end())
                ctx->cells[entry.first] = std::move(entry.second);
        ctx->nets.swap(parked_nets);
        for (auto &entry : parked_nets)
            if (std::find(plan.retired_nets.begin(),plan.retired_nets.end(),entry.second.get()) == plan.retired_nets.end())
                ctx->nets[entry.first] = std::move(entry.second);
    };
    auto restore_owners = [&]() {
        if (parked_cells.empty()) return;
        for (auto &entry : ctx->cells) parked_cells.at(entry.first) = std::move(entry.second);
        ctx->cells.swap(parked_cells);
        for (auto &entry : ctx->nets) parked_nets.at(entry.first) = std::move(entry.second);
        ctx->nets.swap(parked_nets);
        ctx->net_aliases.swap(parked_aliases);
    };
    auto unbind = [&]() { for (auto *cell : plan.cells) if (cell->bel != BelId()) ctx->unbindBel(cell->bel); };
    auto rollback = [&]() {
        unbind();
        restore_owners();
        for (auto &s : saved) { s.cell->type=s.type; s.cell->ports=s.ports; s.cell->params=s.params; s.cell->pin_data=s.pin_data; }
        for (auto &u : users) std::swap(u.first->users,u.second);
        for (auto &d : drivers) d.first->driver=d.second;
        ctx->assignArchInfo();
        for (auto &s : saved) ctx->bindBel(s.bel,s.cell,s.strength);
        ctx->check();
    };
    unbind(); rewrite_reduction(ctx,plan); retire(); ctx->assignArchInfo();
    const auto root_place = all_places.at(plan.root);
    ctx->bindBel(root_place.first,plan.root,root_place.second);
    const auto &children = plan.leaves;
    auto child_score = [&](CellInfo *cell) {
        int incoming = 0;
        for (int i=0;i<width(cell->type);++i) {
            DelayQuad delay; ctx->getCellDelay(cell,pins[i],id_Q,delay);
            incoming = std::max(incoming,int(ctx->predictArcDelay(cell->getPort(pins[i]),{cell,pins[i]}))+delay.maxDelay());
        }
        IdString root_pin;
        for (int i=0;i<width(plan.root->type);++i) if (plan.root->getPort(pins[i]) == cell->getPort(id_Q)) root_pin=pins[i];
        return incoming+int(ctx->predictArcDelay(cell->getPort(id_Q),{plan.root,root_pin}));
    };
    struct Site { int score; BelId bel; };
    std::vector<std::vector<Site>> sites(children.size());
    for (size_t child=0;child<children.size();++child) {
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
        // A narrow ALUT6 leaf uses all of an ALM's LUT storage. Its two halves must
        // not use both shortlist entries for a LAB and hide another ALM.
        // Smaller leaves can share an ALM; keep their existing selection.
        const bool distinct_alms=!wide && width(children[child]->type)==6;
        auto alm = [&](BelId bel) {
            auto loc=ctx->getBelLocation(bel);
            // Each ALM has two LUT and four FF BELs in the tile's z order.
            return std::make_tuple(loc.x,loc.y,loc.z/6);
        };
        std::set<std::tuple<int,int,int>> selected_alms;
        auto original=all_places.at(children[child]).first;
        for (const auto &site : sites[child]) if (site.bel==original) {
            shortlist.push_back(site); ++per_lab[lab(site.bel)];
            if (distinct_alms) selected_alms.insert(alm(site.bel));
            break;
        }
        for (const auto &site : sites[child]) {
            if (site.bel==original || per_lab[lab(site.bel)]>=2 ||
                (distinct_alms && selected_alms.count(alm(site.bel)))) continue;
            shortlist.push_back(site); ++per_lab[lab(site.bel)];
            if (distinct_alms) selected_alms.insert(alm(site.bel));
            if (shortlist.size()==24) break;
        }
        sites[child]=std::move(shortlist);
    }
    if (wide) {
        struct WideTrial {
            int score, total;
            std::array<BelId,4> bels;
        };
        std::vector<WideTrial> trials;
        std::array<BelId,4> tuple;
        auto enumerate = [&](auto &&self, size_t child, int score, int total) -> void {
            if (child == children.size()) {
                trials.push_back({score,total,tuple});
                return;
            }
            for (const auto &site : sites[child]) {
                bool distinct = true;
                for (size_t earlier=0;earlier<child;++earlier) distinct &= tuple[earlier] != site.bel;
                if (!distinct) continue;
                tuple[child] = site.bel;
                // The root's four pins have different logic delays. Include
                // that term before taking the maximum across leaf paths.
                int path_score=site.score+root_delays[child];
                self(self,child+1,std::max(score,path_score),total+path_score);
            }
        };
        // Four lists of at most 24 sites bound discovery to 24^4 tuples.
        // Expensive legality/timing probes retain the same limit of sixteen.
        enumerate(enumerate,0,0,0);
        std::sort(trials.begin(),trials.end(),[](const WideTrial &a,const WideTrial &b) {
            return std::tie(a.score,a.total,a.bels) < std::tie(b.score,b.total,b.bels);
        });
        int qualified=0;
        placed_reduction_policy::WideProbeBudget budget;
        for (const auto &trial : trials) {
            if (budget.exhausted()) break;
            std::array<Lab,4> labs;
            for (size_t child=0;child<children.size();++child) labs[child]=lab(trial.bels[child]);
            if (!budget.eligible(labs)) continue;
            for (auto *child : children) if (child->bel != BelId()) ctx->unbindBel(child->bel);
            for (size_t child=0;child<children.size();++child)
                ctx->bindBel(trial.bels[child],children[child],all_places.at(children[child]).second);
            std::set<Lab> affected=original_labs;
            affected.insert(labs.begin(),labs.end());
            bool legal=true;
            for (const auto &entry : ctx->cells)
                if (entry.second->bel != BelId() && affected.count(lab(entry.second->bel)))
                    legal &= ctx->isBelLocationValid(entry.second->bel);
            if (!legal) continue;
            NPNR_ASSERT(budget.admit_legal_ordered_tuple(labs));
            TimingAnalyser after(ctx); after.setup(false,false,true);
            float slack=std::numeric_limits<float>::max(); bool endpoints_safe=true;
            for (auto ep : endpoints) {
                auto now=after.get_setup_slack(ep); slack=std::min(slack,now);
                endpoints_safe &= timed(now) && now >= before.get_setup_slack(ep);
            }
            auto root_slack=after.get_setup_slack(root_output);
            bool improve=timed(root_slack) && root_slack>old_root_slack &&
                root_slack>=old_root_slack+minimum_branch_gain_ps;
            bool clocks=true;
            for (const auto &clock : before.get_timing_result().clock_fmax) {
                const auto &now=after.get_timing_result().clock_fmax;
                if (!now.count(clock.first) || now.at(clock.first).achieved+0.001f < clock.second.achieved) clocks=false;
            }
            bool hold=enable_replication_policy::hold_nonregressing(old_holds,holds(after));
            log_info("Placed reduction trial root=%s leaves=%s,%s,%s,%s branch_gain=%.0fps endpoint_gain=%.0fps improve=%d endpoints=%d clocks=%d hold=%d.\n",
                ctx->nameOf(plan.root),ctx->nameOfBel(trial.bels[0]),ctx->nameOfBel(trial.bels[1]),
                ctx->nameOfBel(trial.bels[2]),ctx->nameOfBel(trial.bels[3]),root_slack-old_root_slack,
                slack-old_slack,int(improve),int(endpoints_safe),int(clocks),int(hold));
            if (!improve || !endpoints_safe || !clocks || !hold) continue;
            log_info("Placed reduction candidate %d, %zu endpoints, branch gain %.0fps.\n",
                qualified,endpoints.size(),root_slack-old_root_slack);
            if (selection==qualified++) {
                for (const auto &place : all_places)
                    if (std::find(plan.cells.begin(),plan.cells.end(),place.first)==plan.cells.end()) {
                        NPNR_ASSERT(place.first->bel==place.second.first && place.first->belStrength==place.second.second);
                    }
                ctx->check();
                log_info("Placed reduction geometry probes: timed=%d distinct=%zu.\n",
                    budget.timed_count(),budget.geometry_count());
                return true;
            }
        }
        rollback();
        log_info("Placed reduction geometry probes: timed=%d distinct=%zu.\n",
            budget.timed_count(),budget.geometry_count());
        log_info("Placed reduction: %d qualified candidates; none applied.\n",qualified);
        return false;
    }
    struct Trial { int score; BelId a,b; };
    std::vector<Trial> trials;
    for (const auto &a : sites[0]) for (const auto &b : sites[1])
        if (a.bel != b.bel) trials.push_back({std::max(a.score+root_delays[0],b.score+root_delays[1]),a.bel,b.bel});
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
        bool improve=timed(root_slack) && root_slack>old_root_slack &&
            root_slack>=old_root_slack+minimum_branch_gain_ps;
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
    struct Step { std::string root; int radius,selection; int minimum_branch_gain_ps=250; };
    std::vector<Step> steps;
    std::istringstream lines(spec); std::string line;
    while (std::getline(lines,line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::istringstream options(line); Step step; std::string extra;
        if (!(options>>step.root>>step.radius>>step.selection))
            log_error("Invalid placed reduction diagnostic options.\n");
        options>>std::ws;
        if (!options.eof() && !(options>>step.minimum_branch_gain_ps))
            log_error("Invalid placed reduction diagnostic options.\n");
        if ((options>>extra) || step.radius<1 || step.radius>6 || step.selection<-1 ||
            step.minimum_branch_gain_ps<1 || steps.size()==8)
            log_error("Invalid placed reduction diagnostic options.\n");
        steps.push_back(std::move(step));
    }
    if (steps.empty()) log_error("Invalid placed reduction diagnostic options.\n");
    if (!ctx->decomposition_remap_report.empty())
        for (const auto &step : steps)
            if (step.selection < 0)
                log_error("A placed reduction listing cannot precede control decomposition.\n");
    for (size_t index=0;index+1<steps.size();++index)
        if (steps[index].selection==-1) log_error("Only the final placed reduction stage may list candidates.\n");
    // Prevalidate the entire request before applying an earlier selected stage.
    for (size_t index=0;index<steps.size();++index) {
        const auto &step=steps[index];
        if (steps.size()>1)
            log_info("Placed reduction stage %zu: root=%s radius=%d selection=%d.\n",
                index,step.root.c_str(),step.radius,step.selection);
        if (!placed_reduction(ctx,step.root,step.radius,step.selection,step.minimum_branch_gain_ps) && step.selection>=0)
            log_error("Requested placed reduction candidate was not qualified; routing was not started.\n");
    }
}
NEXTPNR_NAMESPACE_END
