/* Bounded post-route LUT moves with incident-net rollback. SPDX-License-Identifier: ISC */
#include "analogue_repair.h"
#include "critical_cohort.h"
#include "timing.h"
#include "private_pair_decomposition_native.h"
#include <array>
#include <tuple>
#include <algorithm>
#include <cmath>
#include <chrono>
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
// Pin assignments are frozen after route. All COMB BELs expose every ALM
// input name, so mere pin existence does not validate a change of LUT half.
bool fixed_pin_target(Context *ctx, CellInfo *cell, BelId target)
{
    if(!ordinary_lut(cell) || cell->bel==BelId() || target==BelId() ||
       !ctx->isValidBelForCellType(cell->type,target)) return false;
    int half=ctx->bel_data(cell->bel).lab_data.idx;
    if(ctx->bel_data(target).lab_data.idx!=half) return false;
    for(const auto &port:cell->ports) {
        if(port.second.type!=PORT_IN || !port.second.net) continue;
        auto pin=cell->pin_data.find(port.first);
        if(pin==cell->pin_data.end()) return false;
        if(pin->second.state!=PIN_SIG && pin->second.state!=PIN_INV) continue;
        if(pin->second.bel_pins.empty()) return false;
        for(auto physical:pin->second.bel_pins) {
            bool allowed=physical.in(id_A,id_B) || physical==(half?id_E1:id_E0) ||
                         physical==(half?id_F1:id_F0) ||
                         (cell->type==id_MISTRAL_ALUT6 ? physical.in(id_C,id_D) : physical==(half?id_D:id_C));
            if(!allowed || ctx->getBelPinWire(target,physical)==WireId()) return false;
        }
    }
    return true;
}
void refresh_modes(Context *ctx, BelId bel)
{
    const auto &location=ctx->bel_data(bel).lab_data;
    auto &alm=ctx->labs.at(location.lab).alms.at(location.alm);
    alm.l6_mode=false;alm.carry_mode=false;
    for(auto slot:alm.lut_bels) if(auto *cell=ctx->getBoundBelCell(slot)) {
        alm.l6_mode|=cell->type==id_MISTRAL_ALUT6;
        alm.carry_mode|=cell->combInfo.is_carry;
    }
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
struct SavedNet {
    NetInfo *net;
    std::vector<std::tuple<WireId,PipId,PlaceStrength>> wires;
    std::set<WireId> stationary;
    WireId root;
};
// Retain the complete original source-to-sink paths of every stationary user.
// The moved driver's output has a new source and must be routed afresh.
bool stationary_paths(Context *ctx, CellInfo *moved, SavedNet &saved)
{
    auto *net=saved.net;
    if(net->driver.cell==moved) return true;
    saved.root=ctx->getNetinfoSourceWire(net);
    auto root=net->wires.find(saved.root);
    if(root==net->wires.end() || root->second.pip!=PipId()) return false;
    saved.stationary.insert(saved.root);
    for(const auto &user:net->users) {
        if(user.cell==moved) continue;
        for(auto wire:ctx->getNetinfoSinkWires(net,user)) {
            std::set<WireId> seen;
            while(wire!=saved.root) {
                auto at=net->wires.find(wire);
                if(!seen.insert(wire).second || at==net->wires.end() || at->second.pip==PipId() ||
                   ctx->getPipDstWire(at->second.pip)!=wire) return false;
                saved.stationary.insert(wire);
                wire=ctx->getPipSrcWire(at->second.pip);
            }
        }
    }
    return true;
}
}

// Bounded reverse Dijkstra. Every foreign wire stays an obstruction; no rip-up
// of another net, negotiated congestion, GPU work or global router side effects.
bool critical_local_route_net(Context *ctx, NetInfo *net, int &visits)
{
    auto root = ctx->getNetinfoSourceWire(net);
    if (root == WireId()) return false;
    if (net->wires.count(root)) {
        if (ctx->getBoundWireNet(root)!=net || net->wires.at(root).pip!=PipId()) return false;
    } else {
        if (!net->wires.empty() || !ctx->checkWireAvail(root)) return false;
        ctx->bindWire(root, net, STRENGTH_WEAK);
    }
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
    if(!fixed_pin_target(ctx,cell,target)) return fail("destination incompatible with fixed physical LUT pins");
    auto home=cell->bel;
    bool legal=false;
    critical_cohort_swap_trial(ctx,cell,target,[&] {legal=true;return false;});
    if (!legal) return fail("placement legality rejected");
    std::vector<SavedNet> saved;
    for (auto *net:nets) {
        saved.push_back({net,{},{},WireId()});
        for (const auto &w:net->wires) saved.back().wires.emplace_back(w.first,w.second.pip,w.second.strength);
        if(!stationary_paths(ctx,cell,saved.back())) return fail("malformed stationary sink route");
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
            refresh_modes(ctx,home);refresh_modes(ctx,target);
            for(const auto &n:saved) {
                if(n.stationary.empty()) continue;
                if(ctx->getNetinfoSourceWire(n.net)!=n.root) return fail("stationary source wire changed");
                for(const auto &user:n.net->users) if(user.cell!=cell)
                    for(auto wire:ctx->getNetinfoSinkWires(n.net,user))
                        if(!n.stationary.count(wire)) return fail("stationary sink wire changed");
                for(const auto &w:n.wires) if(n.stationary.count(std::get<0>(w)) && std::get<1>(w)==PipId())
                    ctx->bindWire(std::get<0>(w),n.net,std::get<2>(w));
                for(const auto &w:n.wires) if(n.stationary.count(std::get<0>(w)) && std::get<1>(w)!=PipId())
                    ctx->bindPip(std::get<1>(w),n.net,std::get<2>(w));
            }
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
    auto snapshot=[&](TimingAnalyser &t) {TimingAnalyser u(ctx);u.setup(false,false,true);return critical_swap_live_endpoint_snapshot(ctx,t,u);};
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
                if(!visited.insert(target).second || ctx->getBoundBelCell(target) || !fixed_pin_target(ctx,c,target))continue;
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
            // Diagnostic only: report every clock before the unchanged acceptance
            // guard, including positive hold margins and missing/new coverage.
            auto report_margins=[&](const char *kind,const auto &old,const auto &now) {
                std::set<IdString> clocks;
                for(const auto &row:old) clocks.insert(row.first);
                for(const auto &row:now) clocks.insert(row.first);
                for(auto clock:clocks) {
                    auto x=old.find(clock),y=now.find(clock);
                    if(x==old.end()||y==now.end()) {
                        log_info("Critical move margin %d: %s clock %s coverage before=%d after=%d.\n",
                                 tried,kind,clock.c_str(ctx),int(x!=old.end()),int(y!=now.end()));
                        continue;
                    }
                    log_info("Critical move margin %d: %s clock %s before=%.6f ns after=%.6f ns delta=%.6f ns.\n",
                             tried,kind,clock.c_str(ctx),ctx->getDelayNS(x->second),ctx->getDelayNS(y->second),
                             ctx->getDelayNS(y->second-x->second));
                }
            };
            report_margins("setup",a.clock_setup_slack,b.clock_setup_slack);
            report_margins("hold",a.clock_hold_slack,b.clock_hold_slack);
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
namespace {
using FMConfigKey = std::tuple<int,int,int,int>;
using FMConfigValue = std::tuple<int,bool,uint32_t,std::vector<uint8_t>>;
using FMConfig = std::map<FMConfigKey,FMConfigValue>;
using FMAlm = std::pair<uint32_t,uint8_t>;
using FMCV = mistral::CycloneV;
const std::array<IdString,6> fm_pins{id_A,id_B,id_C,id_D,id_E,id_F};
const std::array<unsigned,64> fm_mlab_bits{
    0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,
    2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18,
    32,33,36,37,40,41,44,45,61,60,57,56,53,52,49,48,
    34,35,38,39,42,43,46,47,63,62,59,58,55,54,51,50};
bool fm_fail(std::string *reason,const char *message)
{if(reason)*reason=message;return false;}
FMAlm fm_alm(Context *ctx,BelId bel)
{const auto &b=ctx->bel_data(bel).lab_data;return {b.lab,b.alm};}
FMConfig fm_config(Context *ctx)
{
    FMConfig result;
    for(const auto &s:ctx->cyclonev->bmux_get())if(s.btype==FMCV::LAB || s.btype==FMCV::MLAB)
        result.emplace(FMConfigKey{int(s.btype),s.pos.v,int(s.mux),s.midx},FMConfigValue{s.type,s.def,s.s,s.r});
    return result;
}
FMConfigKey fm_mask_key(Context *ctx,BelId bel)
{const auto &b=ctx->bel_data(bel).lab_data;return {ctx->labs.at(b.lab).is_mlab?FMCV::MLAB:FMCV::LAB,bel.pos.v,FMCV::LUT_MASK,b.alm};}
bool fm_mask(const FMConfig &config,const FMConfigKey &key,uint64_t &mask)
{
    auto it=config.find(key);if(it==config.end() || std::get<0>(it->second)!=FMCV::MT_RAM || std::get<3>(it->second).size()!=8)return false;
    mask=0;for(unsigned i=0;i<8;++i)mask|=uint64_t(std::get<3>(it->second)[i])<<(8*i);return true;
}
unsigned fm_index(Context *ctx,BelId bel,unsigned address)
{return ctx->labs.at(ctx->bel_data(bel).lab_data.lab).is_mlab?fm_mlab_bits.at(address):address;}
bool fm_physical_value(bool l6,unsigned row,IdString pin,bool &value)
{
    if(pin==id_A)value=(row>>0)&1;
    else if(pin==id_B)value=(row>>1)&1;
    else if(pin==id_C)value=(row>>(l6&&row>=32?3:2))&1;
    else if(pin==id_D)value=(row>>(l6&&row<32?3:2))&1;
    else if(pin.in(id_E0,id_E1))value=(row>>(l6?5:3))&1;
    else if(pin.in(id_F0,id_F1))value=(row>>4)&1;
    else return false;
    return true;
}
unsigned fm_width(CellInfo *cell)
{return cell->type==id_MISTRAL_ALUT2?2:cell->type==id_MISTRAL_ALUT3?3:cell->type==id_MISTRAL_ALUT4?4:cell->type==id_MISTRAL_ALUT5?5:cell->type==id_MISTRAL_ALUT6?6:0;}
// Evaluate actual configured bits for every raw-net assignment, with correlated
// aliases and PIN_INV represented once. Physical input is !raw, independently
// of PIN_INV; the configured LUT mask already contains the logical inversion.
bool fm_truth(Context *ctx,CellInfo *cell,BelId bel,uint64_t mask,std::string *reason)
{
    const unsigned width=fm_width(cell),half=ctx->bel_data(bel).lab_data.idx;
    if(!width || !cell->params.count(id_LUT) || cell->params.at(id_LUT).is_string ||
       !cell->params.at(id_LUT).is_fully_def() || cell->params.at(id_LUT).size()!=(1u<<width))
        return fm_fail(reason,"ordinary LUT function must be fully defined at its exact width");
    const bool l6=width==6;std::vector<NetInfo *> signals;
    for(unsigned i=0;i<width;++i) {
        auto p=cell->pin_data.find(fm_pins[i]);if(p==cell->pin_data.end())return fm_fail(reason,"missing LUT pin state");
        if(p->second.state==PIN_0 || p->second.state==PIN_1)continue;
        auto *net=cell->getPort(fm_pins[i]);
        if(!net || (p->second.state!=PIN_SIG && p->second.state!=PIN_INV) || p->second.bel_pins.size()!=1)
            return fm_fail(reason,"invalid ordinary LUT physical input");
        if(std::find(signals.begin(),signals.end(),net)==signals.end())signals.push_back(net);
    }
    const uint64_t lut=uint64_t(cell->params.at(id_LUT).as_int64());
    for(unsigned raw=0;raw<(1u<<signals.size());++raw) {
        unsigned logical=0;std::map<IdString,bool> physical;
        for(unsigned i=0;i<width;++i) {
            const auto &p=cell->pin_data.at(fm_pins[i]);bool value=false;
            if(p.state==PIN_1)value=true;
            else if(p.state!=PIN_0) {
                auto at=std::find(signals.begin(),signals.end(),cell->getPort(fm_pins[i]));
                bool input=(raw>>(at-signals.begin()))&1;value=input^(p.state==PIN_INV);
                auto inserted=physical.emplace(p.bel_pins.front(),!input);
                if(!inserted.second && inserted.first->second!=!input)return fm_fail(reason,"unrepresentable shared physical input");
            }
            logical|=unsigned(value)<<i;
        }
        bool found=false;
        for(unsigned row=0;row<(l6?64:32);++row) {
            bool match=true;
            for(const auto &p:physical) {bool value=false;if(!fm_physical_value(l6,row,p.first,value))return fm_fail(reason,"unknown physical LUT pin");if(value!=p.second){match=false;break;}}
            if(!match)continue;found=true;
            unsigned index=fm_index(ctx,bel,row+(l6?0:32*half));
            if(((mask>>index)&1)!=(1^((lut>>logical)&1)))return fm_fail(reason,"configured moved LUT truth mismatch");
        }
        if(!found)return fm_fail(reason,"raw input assignment has no native LUT address");
    }
    return true;
}
bool fm_no_local_consumer(Context *ctx,CellInfo *cell,BelId source,std::string *reason)
{
    const auto &where=ctx->bel_data(source).lab_data;const auto &alm=ctx->labs.at(where.lab).alms.at(where.alm);
    auto *q=cell->getPort(id_Q);if(!q)return fm_fail(reason,"missing moved LUT Q");
    for(unsigned i=0;i<4;++i)if(auto *ff=ctx->getBoundBelCell(alm.ff_bels[i])) {
        if(i/2==where.idx && (ff->getPort(id_DATAIN)==q || ff->getPort(id_SDATA)==q ||
            ctx->wires_connected(alm.comb_out[where.idx],alm.ff_in[i])))
            return fm_fail(reason,"vacated half has a stationary local COMBOUT consumer");
    }
    return true;
}
bool fm_context(Context *ctx)
{
    return ctx && ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string()=="route" && ctx->bitstream_configured &&
        ctx->analogue_cache_valid && !ctx->fes_any_slot_region_active && !ctx->fes_fence_active && !ctx->fes_has_reserved_rect &&
        !ctx->fes_has_cram_region && ctx->fes_frozen_cells.empty() && ctx->fes_frozen_pips.empty();
}
bool fm_native_legal(Context *ctx)
{
    for(const auto &lab:ctx->labs)for(const auto &alm:lab.alms) {
        for(auto bel:alm.lut_bels)if(ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel))return false;
        for(auto bel:alm.ff_bels)if(ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel))return false;
    }
    return true;
}
#define FM_CHECK(condition) do { if (!(condition)) { if(reason)*reason="final move state mismatch: " #condition; return false; } } while (0)
struct FinalMoveSnapshot {
    struct Cell {
        CellInfo *identity;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        ClusterId cluster;
        Region *region; PseudoCell *pseudo; IdString hierpath;
        decltype(CellInfo::attrs) attrs;
        decltype(CellInfo::params) params;
        decltype(CellInfo::ports) ports;
        ArchCellInfo architecture;
    };
    struct Net {
        NetInfo *identity;
        Region *region; IdString constant, hierpath; bool global; ClockConstraint *clock;
        PortRef driver;
        indexed_store<PortRef> users;
        decltype(NetInfo::attrs) attrs;
        std::vector<IdString> aliases;
        std::map<WireId, std::pair<PipId, PlaceStrength>> wires;
    };
    std::map<IdString, Cell> cells;
    std::map<IdString, Net> nets;
    std::vector<IdString> cell_order, net_order;
    std::vector<LABInfo> labs;
    decltype(Context::net_aliases) aliases;
    decltype(Context::attrs) attrs;
    std::vector<uint8_t> rbf;
    decltype(Context::settings) settings; size_t id_count, cache_size;
    bool configured, cache_valid;
    decltype(Context::ports) public_ports;
    bool calibrated; float delay_prior; decltype(Context::pip_delay_observed) observed;
    decltype(Context::pip_type_calibration) calibration;
    using ClockValue=std::tuple<delay_t,delay_t,delay_t,delay_t,delay_t,delay_t,IdString,delay_t>;
    std::map<IdString,ClockValue> clocks;
    static std::map<IdString,ClockValue> clock_values(Context *ctx) {
        std::map<IdString,ClockValue> result;
        for(const auto &entry:ctx->nets)if(auto *c=entry.second->clkconstr.get())
            result.emplace(entry.first,ClockValue{c->high.minDelay(),c->high.maxDelay(),c->low.minDelay(),c->low.maxDelay(),c->period.minDelay(),c->period.maxDelay(),c->phase_group,c->phase_shift});
        return result;
    }
    using ArcKey = std::tuple<IdString, IdString, IdString>;
    using ArcValue = std::tuple<bool, std::array<delay_t,4>, std::array<delay_t,4>>;
    std::map<ArcKey, ArcValue> arcs;
    static std::array<delay_t,4> quad(const DelayQuad &q)
    { return {q.minRiseDelay(), q.maxRiseDelay(), q.minFallDelay(), q.maxFallDelay()}; }
    static std::map<ArcKey, ArcValue> cache(Context *ctx)
    {
        std::map<ArcKey, ArcValue> result;
        for (const auto &entry : ctx->nets) for (const auto &user : entry.second->users) {
            auto it=ctx->analogue_arc_cache.find(&user);
            if(it!=ctx->analogue_arc_cache.end()) result[{entry.first,user.cell->name,user.port}]={it->second.ok,quad(it->second.delay),quad(it->second.lut_input_delay)};
        }
        return result;
    }
    explicit FinalMoveSnapshot(Context *ctx) : labs(ctx->labs), aliases(ctx->net_aliases), attrs(ctx->attrs), settings(ctx->settings), id_count(ctx->idstring_idx_to_str->size()), cache_size(ctx->analogue_arc_cache.size()), configured(ctx->bitstream_configured), cache_valid(ctx->analogue_cache_valid), public_ports(ctx->ports), calibrated(ctx->pip_delay_calibrated), delay_prior(ctx->pip_delay_prior), observed(ctx->pip_delay_observed), calibration(ctx->pip_type_calibration), clocks(clock_values(ctx)), arcs(cache(ctx))
    {
        ctx->cyclonev->rbf_save(rbf);
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            cell_order.push_back(entry.first);
            cells.emplace(entry.first, Cell{cell, cell->type, cell->bel, cell->belStrength, cell->cluster, cell->region, cell->pseudo_cell.get(), cell->hierpath, cell->attrs, cell->params,
                                            cell->ports, *static_cast<ArchCellInfo *>(cell)});
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            net_order.push_back(entry.first);
            Net saved{net, net->region, net->constant_value, net->hierpath, net->is_global, net->clkconstr.get(), net->driver, net->users, net->attrs, net->aliases, {}};
            for (const auto &wire : net->wires) saved.wires[wire.first]={wire.second.pip,wire.second.strength};
            nets.emplace(entry.first, std::move(saved));
        }
    }
    bool matches(Context *ctx, std::string *reason, CellInfo *moved = nullptr, BelId target = BelId(),
                 const std::set<NetInfo *> &changed_routes = {}, const std::vector<LABInfo> *expected_labs = nullptr) const
    {
        FM_CHECK(ctx->pip_delay_calibrated==calibrated && ctx->pip_delay_prior==delay_prior && ctx->pip_delay_observed==observed);
        for(size_t i=0;i<calibration.size();++i) {
            const auto &a=calibration[i],&b=ctx->pip_type_calibration[i];
            FM_CHECK(a.table_ps==b.table_ps && a.analogue_ps==b.analogue_ps && a.hops==b.hops);
        }
        FM_CHECK(clock_values(ctx)==clocks); FM_CHECK(ctx->ports.size()==public_ports.size());
        for(const auto &p:public_ports) {
            auto q=ctx->ports.find(p.first);FM_CHECK(q!=ctx->ports.end());
            FM_CHECK(q->second.name==p.second.name && q->second.net==p.second.net && q->second.type==p.second.type && q->second.user_idx==p.second.user_idx);
        }
        FM_CHECK((ctx->settings) == ( settings)); FM_CHECK((ctx->idstring_idx_to_str->size()) == ( id_count));
        FM_CHECK((ctx->bitstream_configured) == ( configured)); FM_CHECK((ctx->analogue_cache_valid) == ( cache_valid));
        FM_CHECK((ctx->analogue_arc_cache.size()) == ( cache_size));
        FM_CHECK((ctx->net_aliases) == ( aliases));
        FM_CHECK((ctx->attrs) == ( attrs));

        if (changed_routes.empty()) {
            std::vector<uint8_t> now;ctx->cyclonev->rbf_save(now);FM_CHECK((now) == (rbf));FM_CHECK((cache(ctx)) == (arcs));
        } else {
            auto unchanged_arcs=cache(ctx),old_arcs=arcs;
            for(auto *net:changed_routes) {
                for(auto it=unchanged_arcs.begin();it!=unchanged_arcs.end();)if(std::get<0>(it->first)==net->name)it=unchanged_arcs.erase(it);else ++it;
                for(auto it=old_arcs.begin();it!=old_arcs.end();)if(std::get<0>(it->first)==net->name)it=old_arcs.erase(it);else ++it;
            }
            FM_CHECK((unchanged_arcs) == (old_arcs));
        }
        FM_CHECK((ctx->cells.size()) == ( cells.size()));
        FM_CHECK((ctx->nets.size()) == ( nets.size()));
        std::vector<IdString> current_cells, current_nets;
        for (const auto &entry : ctx->cells) current_cells.push_back(entry.first);
        for (const auto &entry : ctx->nets) current_nets.push_back(entry.first);
        FM_CHECK((current_cells) == ( cell_order));
        FM_CHECK((current_nets) == ( net_order));
        for (const auto &entry : cells) {
            auto *cell = ctx->cells.at(entry.first).get();
            const auto &old = entry.second;

            FM_CHECK((cell) == ( old.identity)); FM_CHECK(cell->name==entry.first);
            FM_CHECK((cell->type) == ( old.type));
            FM_CHECK((cell->bel) == ( cell == moved ? target : old.bel));
            FM_CHECK((cell->region) == ( old.region)); FM_CHECK((cell->pseudo_cell.get()) == ( old.pseudo)); FM_CHECK((cell->hierpath) == ( old.hierpath));
            FM_CHECK((cell->belStrength) == ( old.strength));
            if (cell->bel != BelId()) { FM_CHECK((ctx->getBoundBelCell(cell->bel)) == ( cell)); }
            if(cell==moved) { FM_CHECK((ctx->getBoundBelCell(old.bel)) == ( nullptr)); }
            FM_CHECK((cell->attrs) == ( old.attrs));
            FM_CHECK((cell->params) == ( old.params));
            FM_CHECK((cell->ports.size()) == ( old.ports.size()));
            for (const auto &p : old.ports) {
                FM_CHECK(cell->ports.count(p.first));
                const auto &now = cell->ports.at(p.first);
                FM_CHECK((now.name) == ( p.second.name));
                FM_CHECK((now.type) == ( p.second.type));
                FM_CHECK((now.net) == ( p.second.net));
                FM_CHECK((now.user_idx) == ( p.second.user_idx));
            }
            const auto &arch = old.architecture;
            FM_CHECK((cell->pin_data.size()) == ( arch.pin_data.size()));
            for (const auto &pin : arch.pin_data) {
                FM_CHECK(cell->pin_data.count(pin.first));
                FM_CHECK((cell->pin_data.at(pin.first).state) == ( pin.second.state));
                FM_CHECK((cell->pin_data.at(pin.first).bel_pins) == ( pin.second.bel_pins));
            }
            FM_CHECK((cell->cluster) == ( old.cluster));
            FM_CHECK((cell->constr_x) == ( arch.constr_x));
            FM_CHECK((cell->constr_y) == ( arch.constr_y));
            FM_CHECK((cell->constr_z) == ( arch.constr_z));
            FM_CHECK((cell->constr_abs_z) == ( arch.constr_abs_z));
            FM_CHECK((cell->constr_children) == ( arch.constr_children));
            if (ctx->is_comb_cell(cell->type)) {
                FM_CHECK(cell->combInfo.lut_input_count==arch.combInfo.lut_input_count);
                FM_CHECK(cell->combInfo.lut_input_count>=0 && cell->combInfo.lut_input_count<=int(cell->combInfo.lut_in.size()));
                for (int i = 0; i < cell->combInfo.lut_input_count; ++i)
                    FM_CHECK((cell->combInfo.lut_in[i]) == ( arch.combInfo.lut_in[i]));
                FM_CHECK((cell->combInfo.comb_out) == ( arch.combInfo.comb_out));
                FM_CHECK((cell->combInfo.lut_input_count) == ( arch.combInfo.lut_input_count));
                FM_CHECK((cell->combInfo.used_lut_input_count) == ( arch.combInfo.used_lut_input_count));
                FM_CHECK((cell->combInfo.lut_bits_count) == ( arch.combInfo.lut_bits_count));
                FM_CHECK((cell->combInfo.chain_shared_input_count) == ( arch.combInfo.chain_shared_input_count));
                FM_CHECK((cell->combInfo.is_carry) == ( arch.combInfo.is_carry));
                FM_CHECK((cell->combInfo.is_shared) == ( arch.combInfo.is_shared));
                FM_CHECK((cell->combInfo.is_extended) == ( arch.combInfo.is_extended));
                FM_CHECK((cell->combInfo.carry_start) == ( arch.combInfo.carry_start));
                FM_CHECK((cell->combInfo.carry_end) == ( arch.combInfo.carry_end));
                FM_CHECK((cell->combInfo.mlab_group) == ( arch.combInfo.mlab_group));
                if (cell->type == id_MISTRAL_MLAB) {
                    FM_CHECK((cell->combInfo.wclk) == ( arch.combInfo.wclk));
                    FM_CHECK((cell->combInfo.we) == ( arch.combInfo.we));
                }
            } else if (cell->type == id_MISTRAL_FF) {
                FM_CHECK((cell->ffInfo.ctrlset) == ( arch.ffInfo.ctrlset));
                FM_CHECK((cell->ffInfo.datain) == ( arch.ffInfo.datain));
                FM_CHECK((cell->ffInfo.sdata) == ( arch.ffInfo.sdata));
            }
        }
        for (const auto &entry : nets) {
            auto *net = ctx->nets.at(entry.first).get();
            const auto &old = entry.second;
            FM_CHECK((net) == ( old.identity)); FM_CHECK(net->name==entry.first);
            FM_CHECK((net->region) == ( old.region)); FM_CHECK((net->constant_value) == ( old.constant));
            FM_CHECK((net->hierpath) == ( old.hierpath)); FM_CHECK((net->is_global) == ( old.global)); FM_CHECK((net->clkconstr.get()) == ( old.clock));
            FM_CHECK((net->attrs) == ( old.attrs));FM_CHECK((net->aliases) == (old.aliases));
            std::map<WireId,std::pair<PipId,PlaceStrength>> current;
            for(const auto &wire:net->wires) {
                current[wire.first]={wire.second.pip,wire.second.strength};
                FM_CHECK((ctx->getBoundWireNet(wire.first)) == (net));
                if(wire.second.pip!=PipId()) { FM_CHECK((ctx->getBoundPipNet(wire.second.pip)) == (net)); }
            }
            if(!changed_routes.count(net)) { FM_CHECK((current) == (old.wires)); }
            FM_CHECK((net->driver.cell) == ( old.driver.cell));
            FM_CHECK((net->driver.port) == ( old.driver.port));
            FM_CHECK((net->users.entries()) == ( old.users.entries()));
            auto stored_users = old.users;
            for (auto user : stored_users.enumerate()) {
                FM_CHECK(net->users.count(user.index));
                FM_CHECK((net->users.at(user.index).cell) == ( user.value.cell));
                FM_CHECK((net->users.at(user.index).port) == ( user.value.port));
            }
            auto actual = net->users, expected = old.users;
            FM_CHECK((actual.capacity()) == ( expected.capacity()));
            // Include holes/free-list order, not only active user identities.
            for (int i = 0; i < old.users.capacity() + 8; ++i)
                FM_CHECK((actual.add(PortRef{})) == ( expected.add(PortRef{})));
        }
        FM_CHECK((ctx->labs.size()) == ( labs.size()));
        for (size_t lab = 0; lab < labs.size(); ++lab) {
            const auto &now = ctx->labs[lab], &old = expected_labs ? expected_labs->at(lab) : labs[lab];
            FM_CHECK((now.is_mlab) == ( old.is_mlab));
            FM_CHECK((now.clk_wires) == ( old.clk_wires));
            FM_CHECK((now.ena_wires) == ( old.ena_wires));
            FM_CHECK((now.aclr_wires) == ( old.aclr_wires));
            FM_CHECK((now.sclr_wire) == ( old.sclr_wire));
            FM_CHECK((now.sload_wire) == ( old.sload_wire));
            FM_CHECK((now.aclr_used) == ( old.aclr_used));
            for (size_t alm = 0; alm < old.alms.size(); ++alm) {
                const auto &a = now.alms[alm], &b = old.alms[alm];
                FM_CHECK((a.comb_out) == ( b.comb_out));
                FM_CHECK((a.sel_clk) == ( b.sel_clk));
                FM_CHECK((a.sel_ena) == ( b.sel_ena));
                FM_CHECK((a.sel_aclr) == ( b.sel_aclr));
                FM_CHECK((a.sel_ef) == ( b.sel_ef));
                FM_CHECK((a.ff_in) == ( b.ff_in));
                FM_CHECK((a.ff_out) == ( b.ff_out));
                FM_CHECK((a.lut_bels) == ( b.lut_bels));
                FM_CHECK((a.ff_bels) == ( b.ff_bels));
                FM_CHECK((a.l6_mode) == ( b.l6_mode));
                FM_CHECK((a.carry_mode) == ( b.carry_mode));
                FM_CHECK((a.clk_ena_idx) == ( b.clk_ena_idx));
                FM_CHECK((a.aclr_idx) == ( b.aclr_idx));
                FM_CHECK((a.unique_input_count) == ( b.unique_input_count));
            }
        }
        return true;
    }
};

#undef FM_CHECK
void fm_require_restore(Context *ctx,const FinalMoveSnapshot &state,BelId target)
{
    std::string why;
    if(ctx->getBoundBelCell(target) || !state.matches(ctx,&why))
        log_error("Final move rollback failed: %s.\n",why.empty()?"destination remained occupied":why.c_str());
}
bool fm_tree(Context *ctx,NetInfo *net,std::string *reason)
{
    auto fail=[&](const char *s){return fm_fail(reason,s);};
    if(!net || !ctx->nets.count(net->name) || ctx->nets.at(net->name).get()!=net || !net->driver.cell ||
       !ctx->cells.count(net->driver.cell->name) || ctx->cells.at(net->driver.cell->name).get()!=net->driver.cell ||
       net->wires.empty() || net->wires.size()>4096 || net->users.capacity()>4096)return fail("malformed final move routed net");
    auto d=net->driver.cell->ports.find(net->driver.port);
    if(d==net->driver.cell->ports.end() || d->second.name!=net->driver.port || d->second.type!=PORT_OUT || d->second.net!=net)
        return fail("final move driver backreference mismatch");
    auto root=ctx->getNetinfoSourceWire(net);
    if(!net->wires.count(root) || net->wires.at(root).pip!=PipId())return fail("final move route root mismatch");
    for(const auto &w:net->wires) {
        if(ctx->getBoundWireNet(w.first)!=net || (w.second.pip!=PipId() && ctx->getBoundPipNet(w.second.pip)!=net))return fail("foreign final move route");
        std::set<WireId> seen;
        for(auto wire=w.first;wire!=root;) {
            auto at=net->wires.find(wire);
            if(!seen.insert(wire).second || at==net->wires.end() || at->second.pip==PipId() || ctx->getPipDstWire(at->second.pip)!=wire)return fail("malformed final move tree");
            wire=ctx->getPipSrcWire(at->second.pip);
        }
    }
    for(const auto &u:net->users) {
        if(!u.cell || !ctx->cells.count(u.cell->name) || ctx->cells.at(u.cell->name).get()!=u.cell)return fail("missing final move user");
        auto p=u.cell->ports.find(u.port);
        if(p==u.cell->ports.end() || p->second.name!=u.port || p->second.type!=PORT_IN || p->second.net!=net ||
           !net->users.count(p->second.user_idx) || &net->users.at(p->second.user_idx)!=&u)return fail("final move user slot mismatch");
        auto sinks=ctx->getNetinfoSinkWires(net,u);if(sinks.size()==0)return fail("unmapped final move user");
        for(auto wire:sinks)if(!net->wires.count(wire))return fail("unrouted final move user");
    }
    return true;
}
bool fm_output_selectors(Context *ctx,const std::vector<NetInfo *> &incident,
                         const FMConfig &old,const FMConfig &now,const FinalMoveSnapshot &state,
                         std::set<FMConfigKey> &permitted,std::string *reason)
{
    if(old.size()!=now.size())return fm_fail(reason,"output selector inventory changed");
    const std::array<FMCV::bmux_type_t,6> muxes{FMCV::TDFF0,FMCV::TDFF1,FMCV::TDFF1L,
                                             FMCV::BDFF0,FMCV::BDFF1,FMCV::BDFF1L};
    const std::array<FMCV::port_type_t,6> ports{FMCV::FFT0,FMCV::FFT1,FMCV::FFT1L,
                                             FMCV::FFB0,FMCV::FFB1,FMCV::FFB1L};
    std::map<std::pair<int,int>,std::pair<const LABInfo *,bool>> lab_index;
    for(const auto &lab:ctx->labs) {
        auto key=std::make_pair(int(lab.is_mlab?FMCV::MLAB:FMCV::LAB),int(lab.alms[0].lut_bels[0].pos.v));
        auto inserted=lab_index.emplace(key,std::make_pair(&lab,false));
        if(!inserted.second)inserted.first->second.second=true;
    }
    struct OutputProof {FMConfigKey key;const ALMInfo *alm;unsigned output;WireId wire;};
    struct Owners {
        const FinalMoveSnapshot::Net *saved=nullptr;
        NetInfo *live=nullptr;
        bool saved_duplicate=false,live_duplicate=false;
    };
    std::vector<OutputProof> proofs;
    std::map<WireId,Owners> owners;
    for(const auto &entry:old) {
        auto current=now.find(entry.first);
        if(current==now.end())return fm_fail(reason,"missing configured field");
        const auto &[block,pos,mux,index]=entry.first;
        auto mi=std::find(muxes.begin(),muxes.end(),mux);
        if(mi==muxes.end())continue;
        unsigned output=mi-muxes.begin();
        auto li=lab_index.find({block,pos});
        if(li!=lab_index.end() && li->second.second)return fm_fail(reason,"duplicate output selector LAB identity");
        const LABInfo *lab=li==lab_index.end()?nullptr:li->second.first;
        if(!lab || index<0 || size_t(index)>=lab->alms.size())return fm_fail(reason,"missing output selector ALM identity");
        const auto &alm=lab->alms[index];auto loc=alm.lut_bels[0].pos;
        auto out=ctx->get_port(lab->is_mlab?FMCV::MLAB:FMCV::LAB,loc.x(),loc.y(),index,ports[output]);
        if(out==WireId())return fm_fail(reason,"missing output selector wire");
        bool touched=false;
        for(auto *net:incident) {
            auto n=state.nets.find(net->name);
            if(n!=state.nets.end() && n->second.identity==net && n->second.wires.count(out) &&
               n->second.driver.cell && n->second.driver.port==id_Q) {
                auto c=state.cells.find(n->second.driver.cell->name);
                if(c!=state.cells.end() && c->second.identity==n->second.driver.cell &&
                   c->second.bel==alm.lut_bels[output/3])touched=true;
            }
            if(net->wires.count(out) && net->driver.cell && net->driver.port==id_Q &&
               net->driver.cell->bel==alm.lut_bels[output/3])touched=true;
        }
        if(!touched && current->second==entry.second)continue;
        proofs.push_back({entry.first,&alm,output,out});
        owners.emplace(out,Owners{});
    }
    // Scan route stores once, retaining duplicate evidence for selected outputs.
    for(const auto &n:state.nets)for(const auto &w:n.second.wires) {
        auto at=owners.find(w.first);if(at==owners.end())continue;
        if(at->second.saved)at->second.saved_duplicate=true;
        else at->second.saved=&n.second;
    }
    for(const auto &n:ctx->nets)for(const auto &w:n.second->wires) {
        auto at=owners.find(w.first);if(at==owners.end())continue;
        if(at->second.live)at->second.live_duplicate=true;
        else at->second.live=n.second.get();
    }
    for(const auto &proof:proofs) {
        const auto &alm=*proof.alm;auto output=proof.output;auto out=proof.wire;
        const auto &entry=*old.find(proof.key);auto current=now.find(proof.key);
        const auto &indexed=owners.at(out);
        auto valid=[&](bool saved,const FMConfigValue &value) {
            NetInfo *owner=nullptr;PipId pip;WireId root;unsigned roots=0;
            const FinalMoveSnapshot::Net *prior=nullptr;
            if(saved) {
                if(indexed.saved_duplicate)return false;
                prior=indexed.saved;owner=prior?prior->identity:nullptr;
            } else {
                if(indexed.live_duplicate)return false;
                owner=indexed.live;
                if(ctx->getBoundWireNet(out)!=owner)return false;
            }
            if(!owner)return value==FMConfigValue{FMCV::MT_MUX,true,uint32_t(FMCV::REG),{}};
            if(std::find(incident.begin(),incident.end(),owner)==incident.end())return false;
            BelId driver_bel;IdString driver_port;CellInfo *driver=nullptr;
            if(saved) {
                driver=prior->driver.cell;driver_port=prior->driver.port;
                if(!driver)return false;
                auto c=state.cells.find(driver->name);
                if(c==state.cells.end() || c->second.identity!=driver)return false;
                driver_bel=c->second.bel;
                auto port=c->second.ports.find(driver_port);
                if(port==c->second.ports.end() || port->second.net!=owner || port->second.type!=PORT_OUT)return false;
                auto pin=c->second.architecture.pin_data.find(id_Q);
                if(pin==c->second.architecture.pin_data.end() || pin->second.state!=PIN_SIG ||
                   pin->second.bel_pins!=std::vector<IdString>{id_COMBOUT})return false;
                for(const auto &w:prior->wires)if(w.second.first==PipId()){root=w.first;++roots;}
                pip=prior->wires.at(out).first;
            } else {
                driver=owner->driver.cell;driver_port=owner->driver.port;if(!driver)return false;
                driver_bel=driver->bel;
                auto pin=driver->pin_data.find(id_Q);
                if(pin==driver->pin_data.end() || pin->second.state!=PIN_SIG ||
                   pin->second.bel_pins!=std::vector<IdString>{id_COMBOUT})return false;
                for(const auto &w:owner->wires)if(w.second.pip==PipId()){root=w.first;++roots;}
                pip=owner->wires.at(out).pip;
                if(pip==PipId() || ctx->getNetinfoSourceWire(owner)!=root || ctx->getBoundWireNet(root)!=owner || ctx->getBoundPipNet(pip)!=owner || ctx->getBoundBelCell(driver_bel)!=driver)return false;
            }
            // COMBOUT-only admission: FFOUT and non-Q source identities reject.
            if(driver_bel!=alm.lut_bels[output/3] || driver_port!=id_Q || roots!=1 ||
               root!=alm.comb_out[output/3] || ctx->getBelPinWire(driver_bel,id_COMBOUT)!=root || pip==PipId() ||
               ctx->getPipSrcWire(pip)!=root || ctx->getPipDstWire(pip)!=out)return false;
            return value==FMConfigValue{FMCV::MT_MUX,false,uint32_t(FMCV::NLUT),{}};
        };
        if(!valid(true,entry.second) || !valid(false,current->second))return fm_fail(reason,"changed output selector lacks incident direct COMBOUT ownership");
        permitted.insert(entry.first);
    }
    return true;
}
bool fm_masks(Context *ctx,CellInfo *cell,BelId home,BelId target,const FMConfig &before,const FinalMoveSnapshot &state,std::string *reason)
{
    auto now=fm_config(ctx),old=before;
    std::set<FMConfigKey> permitted{fm_mask_key(ctx,home),fm_mask_key(ctx,target)};
    if(now.size()!=old.size())return fm_fail(reason,"LAB configured field inventory changed");
    uint64_t source_mask=0,target_mask=0;
    if(!fm_mask(old,fm_mask_key(ctx,home),source_mask) || !fm_mask(now,fm_mask_key(ctx,target),target_mask) ||
       !fm_truth(ctx,cell,home,source_mask,reason) || !fm_truth(ctx,cell,target,target_mask,reason))return false;
    // Only a completely unobserved ALM can transition to/from native defaults.
    for(auto bel:{home,target}) {
        auto key=fm_mask_key(ctx,bel);uint64_t a=0,b=0;
        if(!fm_mask(old,key,a) || !fm_mask(now,key,b))return fm_fail(reason,"missing affected ALM mask");
        const auto where=ctx->bel_data(bel).lab_data;
        const auto &alm=ctx->labs.at(where.lab).alms.at(where.alm);
        auto occupant=[&](BelId slot,bool saved)->CellInfo* {
            if(!saved)return ctx->getBoundBelCell(slot);
            CellInfo *found=nullptr;
            for(const auto &c:state.cells)if(c.second.bel==slot) {
                if(found)return cell; // Duplicate saved occupancy cannot qualify as empty.
                found=c.second.identity;
            }
            return found;
        };
        auto empty=[&](bool saved) {
            for(auto slot:alm.lut_bels)if(occupant(slot,saved))return false;
            for(auto slot:alm.ff_bels)if(occupant(slot,saved))return false;
            return true;
        };
        bool old_empty=empty(true),new_empty=empty(false);
        if(old_empty!=new_empty) {
            if(fm_width(cell)<2 || fm_width(cell)>5)return fm_fail(reason,"empty ALM requires ordinary ALUT2..5");
            for(auto slot:alm.lut_bels)
                if(occupant(slot,!old_empty)!=(slot==bel?cell:nullptr))return fm_fail(reason,"empty ALM opposite frame has a mate");
            for(auto slot:alm.ff_bels)if(occupant(slot,!old_empty))return fm_fail(reason,"empty ALM opposite frame has an FF");
            std::set<WireId> observers;
            for(auto w:alm.comb_out)observers.insert(w);
            for(auto w:alm.ff_in)observers.insert(w);
            for(auto w:alm.ff_out)observers.insert(w);
            for(auto slot:alm.lut_bels)for(auto pin:{id_CI,id_CO,id_SHAREIN,id_SHAREOUT})
                observers.insert(ctx->getBelPinWire(slot,pin));
            for(auto w:observers) {
                if(w==WireId())return fm_fail(reason,"missing empty ALM observer identity");
                if(old_empty) {for(const auto &n:state.nets)if(n.second.wires.count(w))return fm_fail(reason,"saved empty ALM has routed observer");}
                else {
                    if(ctx->getBoundWireNet(w))return fm_fail(reason,"live empty ALM has routed observer");
                    for(const auto &n:ctx->nets)if(n.second->wires.count(w))return fm_fail(reason,"live empty ALM has routed observer");
                }
            }
            const auto &vacant=old_empty?old:now;const auto &occupied=old_empty?now:old;
            uint64_t vacant_mask=old_empty?a:b,occupied_mask=old_empty?b:a;
            uint64_t unused=0;for(unsigned row=0;row<32;++row)unused|=uint64_t(1)<<fm_index(ctx,bel,row+32*(1-where.idx));
            if(vacant_mask!=0 || (occupied_mask&unused)!=unused ||
               vacant.at(key)!=FMConfigValue{FMCV::MT_RAM,true,64,std::vector<uint8_t>(8,0)})
                return fm_fail(reason,"noncanonical empty or unused ALM mask");
            // The complete occupied RAM record must have native nondefault metadata.
            if(std::get<0>(occupied.at(key))!=FMCV::MT_RAM || std::get<1>(occupied.at(key)) || std::get<2>(occupied.at(key))!=64)
                return fm_fail(reason,"noncanonical occupied ALM mask metadata");
            for(auto mux:{FMCV::TMODE,FMCV::BMODE}) {
                auto mode=key;std::get<2>(mode)=mux;
                auto v=vacant.find(mode),o=occupied.find(mode);
                if(v==vacant.end() || o==occupied.end() ||
                   v->second!=FMConfigValue{FMCV::MT_MUX,true,uint32_t(mux==FMCV::TMODE?FMCV::D_E:FMCV::C_E),{}} ||
                   o->second!=FMConfigValue{FMCV::MT_MUX,false,uint32_t(mux==FMCV::TMODE?FMCV::E_0:FMCV::E_1),{}})
                    return fm_fail(reason,"noncanonical empty ALM mode transition");
                permitted.insert(mode);
            }
        } else if(cell->type==id_MISTRAL_ALUT6) {
            if(ctx->getBoundBelCell(alm.lut_bels[1-where.idx]))return fm_fail(reason,"ALUT6 move has a stationary mate");
        } else {
            uint64_t stationary=0;
            for(unsigned row=0;row<32;++row)stationary|=uint64_t(1)<<fm_index(ctx,bel,row+32*(1-where.idx));
            if((a&stationary)!=(b&stationary))return fm_fail(reason,"stationary mate LUT mask changed");
        }
    }
    std::vector<NetInfo *> incident;
    if(!footprint(ctx,cell,incident))return fm_fail(reason,"invalid selector incident footprint");
    if(!fm_output_selectors(ctx,incident,old,now,state,permitted,reason))return false;
    for(const auto &key:permitted) {old.erase(key);now.erase(key);}
    if(old!=now) {
        if(reason) {
            *reason="stationary LAB configured selector or mode changed";
            for(const auto &entry:old) {
                auto it=now.find(entry.first);
                if(it!=now.end() && it->second==entry.second)continue;
                const auto &[block,pos,mux,index]=entry.first;
                *reason += " ["+std::string(FMCV::bmux_type_names[mux])+" pos="+std::to_string(pos)+
                    " index="+std::to_string(index)+" old="+std::to_string(std::get<2>(entry.second))+
                    " new="+(it==now.end()?std::string("missing"):std::to_string(std::get<2>(it->second)))+"]";
            }
        }
        return false;
    }
    return true;
}
bool fm_coverage(Context *ctx,TimingAnalyser &skew,TimingAnalyser &reference,CellInfo *cell,
                 const std::vector<NetInfo *> &nets,std::string *reason)
{
    for(auto *timing:{&skew,&reference}) {
        if(timing->have_loops)return fm_fail(reason,"timing loop");
        bool source=cell->type==id_MISTRAL_ALUT6?critical_lut6_swap_source_coverage(ctx,*timing,cell,reason):
            critical_lut_permutation_source_coverage(ctx,*timing,cell,reason);
        if(!source)return false;
        for(auto *net:nets)if(!critical_lut6_swap_output_coverage(ctx,*timing,net,reason))return false;
    }
    return true;
}
} // namespace

bool critical_final_routed_move_trial(Context *ctx,CellInfo *cell,BelId target,int visit_budget,
                                      const std::function<bool()> &accept,std::string *reason)
{
    std::vector<NetInfo *> nets;
    if(!fm_context(ctx) || visit_budget<1 || visit_budget>500000 || target==BelId() || ctx->getBoundBelCell(target) ||
       !footprint(ctx,cell,nets) || !fixed_pin_target(ctx,cell,target) || !cell->constr_children.empty() ||
       cell->constr_x || cell->constr_y || cell->constr_z || cell->constr_abs_z)
        return fm_fail(reason,"ineligible final move context, LUT, destination or budget");
    if(!fm_native_legal(ctx))return fm_fail(reason,"original final move LAB legality rejected");
    if(!fm_no_local_consumer(ctx,cell,cell->bel,reason))return false;
    for(auto *net:nets)if(!fm_tree(ctx,net,reason))return false;
    // Prove complete reciprocal port inventory, including missing indexed slots.
    std::map<NetInfo *,size_t> references;size_t work=0;std::set<NetInfo *> changed(nets.begin(),nets.end());
    for(const auto &c:ctx->cells)for(const auto &p:c.second->ports) {
        if(++work>1000000)return fm_fail(reason,"final move reverse-port bound exceeded");
        if(changed.count(p.second.net))++references[p.second.net];
    }
    for(auto *net:nets)if(references[net]!=size_t(net->users.entries())+1)return fm_fail(reason,"final move net has hidden observers");
    auto home=cell->bel;const FinalMoveSnapshot before(ctx);const auto config=fm_config(ctx);
    uint64_t source_mask=0;
    if(!fm_mask(config,fm_mask_key(ctx,home),source_mask) || !fm_truth(ctx,cell,home,source_mask,reason))return false;
    // Reject a malformed original Q selector before any preview/restore can
    // regenerate and thereby canonicalize the caller's configured RBF.
    std::set<FMConfigKey> original_selectors;
    if(!fm_output_selectors(ctx,nets,config,config,before,original_selectors,reason))return false;
    // An empty target must already contain canonical native defaults. Preview
    // rollback rebuilds configuration, so reject malformed originals first.
    const auto target_loc=ctx->bel_data(target).lab_data;
    const auto &target_alm=ctx->labs.at(target_loc.lab).alms.at(target_loc.alm);
    bool target_empty=true;
    for(auto slot:target_alm.lut_bels)if(ctx->getBoundBelCell(slot))target_empty=false;
    for(auto slot:target_alm.ff_bels)if(ctx->getBoundBelCell(slot))target_empty=false;
    if(target_empty) {
        auto key=fm_mask_key(ctx,target);auto mask=config.find(key);
        if(mask==config.end() || mask->second!=FMConfigValue{FMCV::MT_RAM,true,64,std::vector<uint8_t>(8,0)})
            return fm_fail(reason,"original empty target has noncanonical mask");
        for(auto mux:{FMCV::TMODE,FMCV::BMODE}) {
            auto mode=key;std::get<2>(mode)=mux;auto at=config.find(mode);
            if(at==config.end() || at->second!=FMConfigValue{FMCV::MT_MUX,true,
                uint32_t(mux==FMCV::TMODE?FMCV::D_E:FMCV::C_E),{}})
                return fm_fail(reason,"original empty target has noncanonical mode");
        }
        std::set<WireId> observers;
        for(auto w:target_alm.comb_out)observers.insert(w);
        for(auto w:target_alm.ff_in)observers.insert(w);
        for(auto w:target_alm.ff_out)observers.insert(w);
        for(auto slot:target_alm.lut_bels)for(auto pin:{id_CI,id_CO,id_SHAREIN,id_SHAREOUT})
            observers.insert(ctx->getBelPinWire(slot,pin));
        for(auto w:observers) {
            if(w==WireId())return fm_fail(reason,"original empty target lacks observer identity");
            if(ctx->getBoundWireNet(w))return fm_fail(reason,"original empty target has routed observer");
            for(const auto &net:before.nets)if(net.second.wires.count(w))
                return fm_fail(reason,"original empty target has routed observer");
        }
    }
    std::vector<LABInfo> expected;bool legal=false;
    try {
        critical_cohort_swap_trial(ctx,cell,target,[&] {
            refresh_modes(ctx,home);refresh_modes(ctx,target);
            expected=before.labs;
            for(auto bel:{home,target}) {
                auto where=fm_alm(ctx,bel);const auto &now=ctx->labs.at(where.first).alms.at(where.second);
                auto &allowed=expected.at(where.first).alms.at(where.second);
                allowed.unique_input_count=now.unique_input_count;allowed.l6_mode=now.l6_mode;allowed.carry_mode=now.carry_mode;
            }
            legal=before.matches(ctx,reason,cell,target,changed,&expected);return false;
        });
    }catch(...){fm_require_restore(ctx,before,target);throw;}
    fm_require_restore(ctx,before,target);
    if(!legal)return fm_fail(reason,"final move placement or stationary native state rejected");
    std::vector<SavedNet> stationary;
    for(auto *net:nets) {
        stationary.push_back({net,{},{},WireId()});
        for(const auto &w:net->wires)stationary.back().wires.emplace_back(w.first,w.second.pip,w.second.strength);
        if(!stationary_paths(ctx,cell,stationary.back()))return fm_fail(reason,"malformed stationary final move path");
    }
    auto physical_safe=[&] {
        if(!before.matches(ctx,reason,cell,target,changed,&expected))return false;
        if(!fm_native_legal(ctx))return fm_fail(reason,"candidate final move LAB legality rejected");
        for(auto *net:nets)if(!fm_tree(ctx,net,reason))return false;
        for(const auto &n:stationary)for(const auto &wire:n.wires)if(n.stationary.count(std::get<0>(wire))) {
            auto it=n.net->wires.find(std::get<0>(wire));
            if(it==n.net->wires.end() || it->second.pip!=std::get<1>(wire) || it->second.strength!=std::get<2>(wire))
                return fm_fail(reason,"stationary input path or strength changed");
        }
        return fm_masks(ctx,cell,home,target,config,before,reason);
    };
    bool kept=false;
    try {
        kept=critical_routed_move_trial(ctx,cell,target,visit_budget,[&] {
            if(!physical_safe())return false;
            if(!accept())return false;
            return physical_safe();
        },reason);
    }catch(...){fm_require_restore(ctx,before,target);throw;}
    if(!kept)fm_require_restore(ctx,before,target);
    return kept;
}
void Arch::repair_final_critical_moves()
{
    if(!critical_final_move_budget)return;
    Context *ctx=getCtx();
    if(critical_final_move_budget<0 || critical_final_move_budget>16 || !fm_context(ctx) ||
       critical_move_route_budget || ctx->force)
        log_error("Final critical move requires a configured ordinary route and budget 1..16.\n");
    auto analyse=[&](bool skew) {auto t=std::make_unique<TimingAnalyser>(ctx);t->with_clock_skew=skew;t->setup(false,false,true);return t;};
    auto before=analyse(true),reference=analyse(false);
    if(before->have_loops || reference->have_loops)log_error("Final critical move refuses timing loops.\n");
    auto inventory=critical_swap_live_endpoint_snapshot(ctx,*before,*reference);
    struct Choice {CellInfo *cell;BelId target;float slack;double gain;};
    int tried=0,kept=0;size_t last_cell_count=0;
    for(int pass=0;pass<(critical_final_move_rediscover?2:1);++pass) {
        if(pass) {
            if(!kept || tried>=critical_final_move_budget)break;
            before=analyse(true);reference=analyse(false);
            if(before->have_loops || reference->have_loops)log_error("Final critical move rediscovery refuses timing loops.\n");
            inventory=critical_swap_live_endpoint_snapshot(ctx,*before,*reference);
        }
        std::chrono::steady_clock::time_point discovery_started;
        if(critical_final_move_rediscover)discovery_started=std::chrono::steady_clock::now();
    std::vector<std::pair<float,CellInfo *>> cells;
    for (const auto &entry:ctx->cells) {
        auto *c=entry.second.get();std::vector<NetInfo *> nets;
        if (!footprint(ctx,c,nets)) continue;
        float slack=std::numeric_limits<float>::infinity();
        for (auto *n:nets) for (const auto &u:n->users) {
            auto s=before->get_setup_slack(CellPortKey(u));
            if (std::isfinite(s) && s!=std::numeric_limits<float>::lowest()) slack=std::min(slack,float(s));
        }
        if(slack>=0) continue;
        std::string why;
        if(!fm_no_local_consumer(ctx,c,c->bel,&why) || !fm_coverage(ctx,*before,*reference,c,nets,&why)) {
            log_info("Final critical move filter: cell %s: %s.\n",ctx->nameOf(c),why.c_str());continue;
        }
        cells.emplace_back(slack,c);
    }
    std::sort(cells.begin(),cells.end(),[&](const auto &a,const auto &b) {
        return a.first!=b.first ? a.first<b.first : a.second->name.str(ctx)<b.second->name.str(ctx);
    });
    if(cells.size()>32) cells.resize(32);
    // Before any placement preview, including legality ranking. Configuration,
    // native cache and every exact indexed slot are checked after each preview.
    const FinalMoveSnapshot discovery(ctx);
    std::vector<std::vector<Choice>> choices;
    for(const auto &entry:cells) {
        auto *c=entry.second;std::vector<Loc> anchors{ctx->getBelLocation(c->bel)};
        if(critical_final_move_rediscover)log_info("Final critical move discovery origin pass=%d cell=%s bel=%s.\n",pass+1,ctx->nameOf(c),ctx->nameOfBel(c->bel));
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
                if(!visited.insert(target).second || ctx->getBoundBelCell(target) || !fixed_pin_target(ctx,c,target))continue;
                double gain=original-cost(target);if(gain>0) local.push_back({c,target,entry.first,gain});
            }
        }
        std::sort(local.begin(),local.end(),[](const Choice &a,const Choice &b) {return a.gain!=b.gain?a.gain>b.gain:a.target<b.target;});
        std::vector<Choice> legal;
        for(const auto &choice:local) {
            bool ok=false;
            try {critical_cohort_swap_trial(ctx,c,choice.target,[&]{ok=true;return false;});}
            catch(...){fm_require_restore(ctx,discovery,choice.target);throw;}
            fm_require_restore(ctx,discovery,choice.target);
            if(ok) legal.push_back(choice);
            if(legal.size()==4) break;
        }
        choices.push_back(std::move(legal));
    }
    last_cell_count=cells.size();
    if(critical_final_move_rediscover) {
        size_t candidates=0;for(const auto &list:choices)candidates+=list.size();
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-discovery_started).count();
        log_info("Final critical move discovery pass=%d cells=%zu candidates=%zu remaining=%d seconds=%.6f.\n",
                 pass+1,cells.size(),candidates,critical_final_move_budget-tried,seconds);
    }
    for(size_t rank=0;rank<4 && tried<critical_final_move_budget;++rank)for(auto &list:choices) {
        if(tried>=critical_final_move_budget)break;
        if(rank>=list.size())continue;
        const auto choice=list[rank];std::string reason="native timing rejected";std::vector<NetInfo *> nets;
        if(ctx->getBoundBelCell(choice.target) || !footprint(ctx,choice.cell,nets) ||
           !fixed_pin_target(ctx,choice.cell,choice.target) || !fm_no_local_consumer(ctx,choice.cell,choice.cell->bel,&reason) ||
           !fm_coverage(ctx,*before,*reference,choice.cell,nets,&reason)) {
            log_info("Final critical move stale: cell %s: %s.\n",ctx->nameOf(choice.cell),reason.c_str());continue;
        }
        const FinalMoveSnapshot pretrial(ctx);bool legal=false;
        try {critical_cohort_swap_trial(ctx,choice.cell,choice.target,[&]{legal=true;return false;});}
        catch(...){fm_require_restore(ctx,pretrial,choice.target);throw;}
        fm_require_restore(ctx,pretrial,choice.target);
        if(!legal) {log_info("Final critical move stale: cell %s: native target legality.\n",ctx->nameOf(choice.cell));continue;}
        ++tried;std::unique_ptr<TimingAnalyser> after,after_reference;json11::Json next_inventory;
        bool accepted=critical_final_routed_move_trial(ctx,choice.cell,choice.target,500000,[&] {
            after=analyse(true);after_reference=analyse(false);
            if(!fm_coverage(ctx,*after,*after_reference,choice.cell,nets,&reason))return false;
            for(auto frames:{std::make_pair(before.get(),after.get()),std::make_pair(reference.get(),after_reference.get())}) {
                const auto &a=frames.first->get_timing_result(),&b=frames.second->get_timing_result();
                auto report=[&](const char *kind,const auto &old,const auto &now) {
                    std::set<IdString> clocks;for(const auto &row:old)clocks.insert(row.first);for(const auto &row:now)clocks.insert(row.first);
                    for(auto clock:clocks) {auto x=old.find(clock),y=now.find(clock);
                        if(x==old.end() || y==now.end())log_info("Final critical move margin %d: skew=%d %s %s coverage=%d/%d.\n",tried,int(frames.first->with_clock_skew),kind,clock.c_str(ctx),int(x!=old.end()),int(y!=now.end()));
                        else log_info("Final critical move margin %d: skew=%d %s %s before=%d after=%d ps.\n",tried,int(frames.first->with_clock_skew),kind,clock.c_str(ctx),int(x->second),int(y->second));
                    }
                };
                report("setup",a.clock_setup_slack,b.clock_setup_slack);report("hold",a.clock_hold_slack,b.clock_hold_slack);
                if(!private_pair_clock_safe(a,b,&reason))return false;
            }
            next_inventory=critical_swap_live_endpoint_snapshot(ctx,*after,*after_reference);
            return critical_route_snapshot_safe(inventory,next_inventory,&reason);
        },&reason);
        log_info("Final critical move trial %d: cell %s target %s accepted=%d; %s.\n",tried,ctx->nameOf(choice.cell),ctx->nameOfBel(choice.target),int(accepted),reason.c_str());
        if(accepted){before=std::move(after);reference=std::move(after_reference);inventory=std::move(next_inventory);++kept;list.clear();}
    }
    }
    log_info("Final critical move repair: cells=%zu tried=%d/%d kept=%d; unchanged unsupported coverage is not qualification.\n",last_cell_count,tried,critical_final_move_budget,kept);
}

// Explicit zero and absent options preserve the original serialized bytes.
void Arch::refresh_post_route_repair_checkpoint()
{
    if (!critical_route_budget && !critical_final_route_budget && !critical_final_move_budget && !critical_move_route_budget && !critical_lut6_swap_budget && !critical_private_pair_budget && !critical_lut_permute_budget) return;
    save_fes_pin_maps();
    archInfoToAttributes();
}
NEXTPNR_NAMESPACE_END
