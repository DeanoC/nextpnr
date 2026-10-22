/* Bounded post-route LUT moves with incident-net rollback. SPDX-License-Identifier: ISC */
#include "analogue_repair.h"
#include "critical_cohort.h"
#include "timing.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>

NEXTPNR_NAMESPACE_BEGIN
namespace {
bool ordinary_lut(CellInfo *c)
{
    return c && c->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                          id_MISTRAL_ALUT5, id_MISTRAL_ALUT6);
}
bool protected_cell(Context *ctx, CellInfo *c)
{
    if (!c || c->isPseudo() || c->region) return true;
    for (const char *a : {"keep", "dont_touch", "BEL", "FES_SLOT"})
        if (c->attrs.count(ctx->id(a))) return true;
    return false;
}
bool footprint(Context *ctx, CellInfo *cell, std::vector<NetInfo *> &nets)
{
    if (!ordinary_lut(cell) || protected_cell(ctx, cell) || cell->cluster != ClusterId() ||
        cell->bel == BelId() || cell->belStrength > STRENGTH_WEAK) return false;
    std::set<IdString> names;
    for (const auto &port : cell->ports) {
        auto pin = cell->pin_data.find(port.first);
        if (pin != cell->pin_data.end() && pin->second.state != PIN_SIG && pin->second.state != PIN_INV) continue;
        auto *net = port.second.net;
        if (!net) continue;
        if (net->is_global || net->clkconstr || net->wires.empty() || net->users.entries() > 64 ||
            protected_cell(ctx, net->driver.cell) ||
            !(ordinary_lut(net->driver.cell) || net->driver.cell->type.in(id_MISTRAL_FF, id_MISTRAL_ALUT_ARITH)))
            return false;
        for (const auto &p : ctx->ports) if (p.second.net == net) return false;
        for (const auto &w : net->wires) if (w.second.strength > STRENGTH_STRONG) return false;
        for (const auto &u : net->users)
            if (protected_cell(ctx, u.cell) ||
                !(ordinary_lut(u.cell) ? u.port.in(id_A,id_B,id_C,id_D,id_E,id_F) :
                  u.cell->type == id_MISTRAL_FF && u.port == id_DATAIN)) return false;
        names.insert(net->name);
    }
    if (names.empty() || names.size() > 7) return false;
    for (auto name : names) nets.push_back(ctx->nets.at(name).get());
    return true;
}
struct SavedNet { NetInfo *net; std::vector<std::tuple<WireId,PipId,PlaceStrength>> wires; };
}

// Bounded reverse Dijkstra. Every foreign wire stays an obstruction; no rip-up
// of another net, negotiated congestion, GPU work or global router side effects.
bool critical_local_route_net(Context *ctx, NetInfo *net, int &visits)
{
    auto root = ctx->getNetinfoSourceWire(net);
    if (root == WireId() || !ctx->checkWireAvail(root)) return false;
    ctx->bindWire(root, net, STRENGTH_WEAK);
    std::vector<WireId> sinks;
    for (const auto &user : net->users)
        for (auto wire : ctx->getNetinfoSinkWires(net, user)) sinks.push_back(wire);
    std::sort(sinks.begin(), sinks.end());
    sinks.erase(std::unique(sinks.begin(), sinks.end()), sinks.end());
    for (WireId sink : sinks) {
        if (net->wires.count(sink)) continue;
        using Item = std::pair<double, WireId>;
        std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
        dict<WireId,double> distance;
        dict<WireId,PipId> next;
        distance[sink]=0; queue.push({0,sink});
        bool found=false;
        while (!queue.empty() && visits > 0) {
            auto current=queue.top();queue.pop();
            if (current.first != distance.at(current.second)) continue;
            --visits;
            if (current.second==root) { found=true;break; }
            if (!ctx->checkWireAvail(current.second) && ctx->getBoundWireNet(current.second)!=net) continue;
            for (auto pip : ctx->getPipsUphill(current.second)) {
                auto existing=net->wires.find(current.second);
                if (existing!=net->wires.end() && existing->second.pip!=pip) continue;
                if (!ctx->checkPipAvailForNet(pip, net)) continue;
                auto src=ctx->getPipSrcWire(pip);
                if (!ctx->checkWireAvail(src) && ctx->getBoundWireNet(src)!=net) continue;
                double cost=std::max(0.0,double(ctx->getPipDelay(pip).maxDelay()))+
                            std::max(0.0,double(ctx->getWireDelay(src).maxDelay()));
                if (!std::isfinite(cost)) continue;
                double proposed=current.first+cost;
                auto old=distance.find(src);
                if (old!=distance.end() && proposed>=old->second) continue;
                distance[src]=proposed;next[src]=pip;queue.push({proposed,src});
            }
        }
        if (!found) return false;
        for (auto cursor=root; cursor!=sink;) {
            auto pip=next.at(cursor);auto dst=ctx->getPipDstWire(pip);
            if (!net->wires.count(dst)) {
                if (!ctx->checkWireAvail(dst) || !ctx->checkPipAvailForNet(pip,net)) return false;
                ctx->bindPip(pip,net,STRENGTH_WEAK);
            } else if (net->wires.at(dst).pip!=pip) return false;
            cursor=dst;
        }
    }
    return true;
}

bool critical_routed_move_trial(Context *ctx, CellInfo *cell, BelId target, int visit_budget,
                                const std::function<bool()> &accept, std::string *reason)
{
    auto fail=[&](const char *s) {if(reason)*reason=s;return false;};
    std::vector<NetInfo *> nets;
    if (target==BelId() || ctx->getBoundBelCell(target) || !footprint(ctx,cell,nets))
        return fail("ineligible LUT, destination or incident data nets");
    bool legal=false;
    critical_cohort_swap_trial(ctx,cell,target,[&] {legal=true;return false;});
    if (!legal) return fail("placement legality rejected");
    std::vector<SavedNet> saved;
    for (auto *net:nets) {
        saved.push_back({net,{}});
        for (const auto &w:net->wires) saved.back().wires.emplace_back(w.first,w.second.pip,w.second.strength);
    }
    auto restore=[&]() {
        for (const auto &n:saved) ctx->ripupNet(n.net->name);
        for (const auto &n:saved) {
            for (const auto &w:n.wires) if (std::get<1>(w)==PipId()) ctx->bindWire(std::get<0>(w),n.net,std::get<2>(w));
            for (const auto &w:n.wires) if (std::get<1>(w)!=PipId()) ctx->bindPip(std::get<1>(w),n.net,std::get<2>(w));
        }
        ctx->configure_bitstream();
    };
    try {
        for (auto *net:nets) ctx->ripupNet(net->name);
        bool kept=critical_cohort_swap_trial(ctx,cell,target,[&]() {
            for (auto *net:nets) if (!critical_local_route_net(ctx,net,visit_budget))
                return fail("incident-net routing failed or exhausted node budget");
            ctx->configure_bitstream();
            return accept();
        });
        if (kept) return true;
    } catch (...) {restore();throw;}
    // Placement transaction has restored its bindings and complete LAB state
    // before old routes and physical mux/cache state are reconstructed.
    restore();return false;
}

void Arch::repair_critical_moves()
{
    if (!critical_move_route_budget) return;
    Context *ctx=getCtx();
    if (critical_move_route_budget<0 || critical_move_route_budget>16 || fes_any_slot_region_active ||
        !ctx->attrs.count(id_step) || ctx->attrs.at(id_step).as_string()!="route")
        log_error("Critical move repair requires an ordinary completed route and budget 1..16.\n");
    configure_bitstream();
    auto analyse=[&]() {auto t=std::make_unique<TimingAnalyser>(ctx);t->with_clock_skew=true;t->setup(false,false,true);return t;};
    auto snapshot=[&](TimingAnalyser &t) {TimingAnalyser u(ctx);u.setup(false,false,true);return critical_swap_endpoint_snapshot(ctx,t,u);};
    auto before=analyse();
    if (before->have_loops) log_error("Critical move repair refuses timing loops.\n");
    auto inventory=snapshot(*before);
    struct Choice {CellInfo *cell;BelId target;float slack;double gain;};
    std::vector<std::pair<float,CellInfo *>> cells;
    for (const auto &entry:ctx->cells) {
        auto *c=entry.second.get();std::vector<NetInfo *> nets;
        if (!footprint(ctx,c,nets)) continue;
        float slack=std::numeric_limits<float>::infinity();
        for (auto *n:nets) for (const auto &u:n->users) {
            auto s=before->get_setup_slack(CellPortKey(u));
            if (std::isfinite(s) && s!=std::numeric_limits<float>::lowest()) slack=std::min(slack,float(s));
        }
        if(slack<0) cells.emplace_back(slack,c);
    }
    std::sort(cells.begin(),cells.end(),[&](const auto &a,const auto &b) {
        return a.first!=b.first ? a.first<b.first : a.second->name.str(ctx)<b.second->name.str(ctx);
    });
    if(cells.size()>32) cells.resize(32);
    std::vector<std::vector<Choice>> choices;
    for(const auto &entry:cells) {
        auto *c=entry.second;std::vector<Loc> anchors{ctx->getBelLocation(c->bel)};
        struct Edge {Loc point;double weight;};std::vector<Edge> edges;
        auto add=[&](CellInfo *other,const PortRef &sink) {
            if(!other || other==c || other->bel==BelId()) return;
            auto loc=ctx->getBelLocation(other->bel);auto slack=before->get_setup_slack(CellPortKey(sink));
            double weight=std::isfinite(slack)&&slack<0&&slack!=std::numeric_limits<float>::lowest()?1.0-std::max(-10000.0,double(slack))/1000.0:1.0;
            edges.push_back({loc,weight});if(anchors.size()<5) anchors.push_back(loc);
        };
        for(const auto &port:c->ports) {
            auto *n=port.second.net;if(!n)continue;
            if(port.second.type==PORT_IN) add(n->driver.cell,PortRef{c,port.first});
            else for(const auto &u:n->users) add(u.cell,u);
        }
        auto cost=[&](BelId bel) {auto l=ctx->getBelLocation(bel);double result=0;
            for(const auto &e:edges) { result+=e.weight*(std::abs(l.x-e.point.x)+std::abs(l.y-e.point.y)); }
            return result;};
        double original=cost(c->bel);std::set<BelId> visited;std::vector<Choice> local;
        for(auto anchor:anchors) for(int dx=-1;dx<=1;++dx) for(int dy=-1;dy<=1;++dy) {
            int x=anchor.x+dx,y=anchor.y+dy;
            if(x<0||y<0||x>=ctx->getGridDimX()||y>=ctx->getGridDimY())continue;
            for(auto target:ctx->getBelsByTile(x,y)) {
                if(!visited.insert(target).second || ctx->getBoundBelCell(target) || !ctx->isValidBelForCellType(c->type,target))continue;
                double gain=original-cost(target);if(gain>0) local.push_back({c,target,entry.first,gain});
            }
        }
        std::sort(local.begin(),local.end(),[](const Choice &a,const Choice &b) {return a.gain!=b.gain?a.gain>b.gain:a.target<b.target;});
        std::vector<Choice> legal;
        for(const auto &choice:local) {
            bool ok=false;critical_cohort_swap_trial(ctx,c,choice.target,[&]{ok=true;return false;});
            if(ok) legal.push_back(choice);
            if(legal.size()==4) break;
        }
        choices.push_back(std::move(legal));
    }
    int tried=0,kept=0;
    for(size_t rank=0;rank<4&&tried<critical_move_route_budget;++rank) for(auto &list:choices) {
        if(tried>=critical_move_route_budget)break;
        if(rank>=list.size())continue;
        const auto choice=list[rank];++tried;
        std::unique_ptr<TimingAnalyser> after;json11::Json next_inventory;std::string reason="native timing rejected";
        bool accepted=critical_routed_move_trial(ctx,choice.cell,choice.target,500000,[&]() {
            after=analyse();if(after->have_loops){reason="timing loop";return false;}
            const auto &a=before->get_timing_result(),&b=after->get_timing_result();
            auto nonregressing=[&](const auto &old,const auto &now) {
                if(old.size()!=now.size())return false;
                for(const auto &row:old){auto f=now.find(row.first);if(f==now.end()||!std::isfinite(double(f->second))||f->second<row.second)return false;}return true;};
            if(!nonregressing(a.clock_setup_slack,b.clock_setup_slack)||!nonregressing(a.clock_hold_slack,b.clock_hold_slack)||a.clock_fmax.size()!=b.clock_fmax.size()) {
                reason="clock setup/hold coverage or margin regression";return false;
            }
            for(const auto &clock:a.clock_fmax) {auto f=b.clock_fmax.find(clock.first);
                if(f==b.clock_fmax.end()||!std::isfinite(f->second.achieved)||f->second.achieved<clock.second.achieved){reason="clock Fmax regression";return false;}}
            next_inventory=snapshot(*after);return critical_route_snapshot_safe(inventory,next_inventory,&reason);
        },&reason);
        log_info("Critical move trial %d: cell %s target %s accepted=%d; %s.\n",tried,ctx->nameOf(choice.cell),ctx->nameOfBel(choice.target),int(accepted),reason.c_str());
        if(accepted){before=std::move(after);inventory=std::move(next_inventory);++kept;list.clear();}
    }
    log_info("Critical move repair: cells=%zu tried=%d/%d kept=%d; unchanged unsupported coverage is not qualification.\n",cells.size(),tried,critical_move_route_budget,kept);
}
NEXTPNR_NAMESPACE_END
