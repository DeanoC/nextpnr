/* Independent routed LUT permutation transaction and physical proof. SPDX-License-Identifier: ISC */
#include "analogue_repair.h"
#include "routed_lut_permutation_policy.h"
#include "cyclonev.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
#include "timing.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
USING_NEXTPNR_NAMESPACE
namespace {
using CV = mistral::CycloneV;
const std::array<IdString,6> pins{id_A,id_B,id_C,id_D,id_E,id_F};
struct Snapshot {
    struct Cell {
        CellInfo *identity;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        ClusterId cluster;
        decltype(CellInfo::attrs) attrs;
        decltype(CellInfo::params) params;
        decltype(CellInfo::ports) ports;
        ArchCellInfo architecture;
    };
    struct Net {
        NetInfo *identity;
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
    explicit Snapshot(Context *ctx) : labs(ctx->labs), aliases(ctx->net_aliases), attrs(ctx->attrs), arcs(cache(ctx))
    {
        ctx->cyclonev->rbf_save(rbf);
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            cell_order.push_back(entry.first);
            cells.emplace(entry.first, Cell{cell, cell->type, cell->bel, cell->belStrength, cell->cluster, cell->attrs, cell->params,
                                            cell->ports, *static_cast<ArchCellInfo *>(cell)});
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            net_order.push_back(entry.first);
            Net saved{net, net->driver, net->users, net->attrs, net->aliases, {}};
            for (const auto &wire : net->wires) saved.wires[wire.first]={wire.second.pip,wire.second.strength};
            nets.emplace(entry.first, std::move(saved));
        }
    }
    void expect_exact(Context *ctx, const std::set<NetInfo *> &changed_routes = {}) const
    {
        EXPECT_EQ(ctx->net_aliases, aliases);
        EXPECT_EQ(ctx->attrs, attrs);
        EXPECT_TRUE(ctx->analogue_cache_valid);
        if (changed_routes.empty()) {
            std::vector<uint8_t> now;ctx->cyclonev->rbf_save(now);EXPECT_EQ(now,rbf);EXPECT_EQ(cache(ctx),arcs);
        } else {
            auto unchanged_arcs=cache(ctx),old_arcs=arcs;
            for(auto *net:changed_routes) {
                for(auto it=unchanged_arcs.begin();it!=unchanged_arcs.end();)if(std::get<0>(it->first)==net->name)it=unchanged_arcs.erase(it);else ++it;
                for(auto it=old_arcs.begin();it!=old_arcs.end();)if(std::get<0>(it->first)==net->name)it=old_arcs.erase(it);else ++it;
            }
            EXPECT_EQ(unchanged_arcs,old_arcs);
        }
        ASSERT_EQ(ctx->cells.size(), cells.size());
        ASSERT_EQ(ctx->nets.size(), nets.size());
        std::vector<IdString> current_cells, current_nets;
        for (const auto &entry : ctx->cells) current_cells.push_back(entry.first);
        for (const auto &entry : ctx->nets) current_nets.push_back(entry.first);
        EXPECT_EQ(current_cells, cell_order);
        EXPECT_EQ(current_nets, net_order);
        for (const auto &entry : cells) {
            auto *cell = ctx->cells.at(entry.first).get();
            const auto &old = entry.second;
            SCOPED_TRACE(entry.first.str(ctx));
            EXPECT_EQ(cell, old.identity);
            EXPECT_EQ(cell->type, old.type);
            EXPECT_EQ(cell->bel, old.bel);
            EXPECT_EQ(cell->belStrength, old.strength);
            if (old.bel != BelId()) { EXPECT_EQ(ctx->getBoundBelCell(old.bel), cell); }
            EXPECT_EQ(cell->attrs, old.attrs);
            EXPECT_EQ(cell->params, old.params);
            ASSERT_EQ(cell->ports.size(), old.ports.size());
            for (const auto &p : old.ports) {
                ASSERT_TRUE(cell->ports.count(p.first));
                const auto &now = cell->ports.at(p.first);
                EXPECT_EQ(now.name, p.second.name);
                EXPECT_EQ(now.type, p.second.type);
                EXPECT_EQ(now.net, p.second.net);
                EXPECT_EQ(now.user_idx, p.second.user_idx);
            }
            const auto &arch = old.architecture;
            ASSERT_EQ(cell->pin_data.size(), arch.pin_data.size());
            for (const auto &pin : arch.pin_data) {
                ASSERT_TRUE(cell->pin_data.count(pin.first));
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
            EXPECT_EQ(cell->cluster, old.cluster);
            EXPECT_EQ(cell->constr_x, arch.constr_x);
            EXPECT_EQ(cell->constr_y, arch.constr_y);
            EXPECT_EQ(cell->constr_z, arch.constr_z);
            EXPECT_EQ(cell->constr_abs_z, arch.constr_abs_z);
            EXPECT_EQ(cell->constr_children, arch.constr_children);
            if (ctx->is_comb_cell(cell->type)) {
                for (int i = 0; i < cell->combInfo.lut_input_count; ++i)
                    EXPECT_EQ(cell->combInfo.lut_in[i], arch.combInfo.lut_in[i]);
                EXPECT_EQ(cell->combInfo.comb_out, arch.combInfo.comb_out);
                EXPECT_EQ(cell->combInfo.lut_input_count, arch.combInfo.lut_input_count);
                EXPECT_EQ(cell->combInfo.used_lut_input_count, arch.combInfo.used_lut_input_count);
                EXPECT_EQ(cell->combInfo.lut_bits_count, arch.combInfo.lut_bits_count);
                EXPECT_EQ(cell->combInfo.chain_shared_input_count, arch.combInfo.chain_shared_input_count);
                EXPECT_EQ(cell->combInfo.is_carry, arch.combInfo.is_carry);
                EXPECT_EQ(cell->combInfo.is_shared, arch.combInfo.is_shared);
                EXPECT_EQ(cell->combInfo.is_extended, arch.combInfo.is_extended);
                EXPECT_EQ(cell->combInfo.carry_start, arch.combInfo.carry_start);
                EXPECT_EQ(cell->combInfo.carry_end, arch.combInfo.carry_end);
                EXPECT_EQ(cell->combInfo.mlab_group, arch.combInfo.mlab_group);
                if (cell->type == id_MISTRAL_MLAB) {
                    EXPECT_EQ(cell->combInfo.wclk, arch.combInfo.wclk);
                    EXPECT_EQ(cell->combInfo.we, arch.combInfo.we);
                }
            } else if (cell->type == id_MISTRAL_FF) {
                EXPECT_EQ(cell->ffInfo.ctrlset, arch.ffInfo.ctrlset);
                EXPECT_EQ(cell->ffInfo.datain, arch.ffInfo.datain);
                EXPECT_EQ(cell->ffInfo.sdata, arch.ffInfo.sdata);
            }
        }
        for (const auto &entry : nets) {
            auto *net = ctx->nets.at(entry.first).get();
            const auto &old = entry.second;
            EXPECT_EQ(net, old.identity);
            EXPECT_EQ(net->attrs, old.attrs);EXPECT_EQ(net->aliases,old.aliases);
            std::map<WireId,std::pair<PipId,PlaceStrength>> current;
            for(const auto &wire:net->wires) {
                current[wire.first]={wire.second.pip,wire.second.strength};
                EXPECT_EQ(ctx->getBoundWireNet(wire.first),net);
                if(wire.second.pip!=PipId()) { EXPECT_EQ(ctx->getBoundPipNet(wire.second.pip),net); }
            }
            if(!changed_routes.count(net)) { EXPECT_EQ(current,old.wires); }
            EXPECT_EQ(net->driver.cell, old.driver.cell);
            EXPECT_EQ(net->driver.port, old.driver.port);
            ASSERT_EQ(net->users.entries(), old.users.entries());
            auto stored_users = old.users;
            for (auto user : stored_users.enumerate()) {
                ASSERT_TRUE(net->users.count(user.index));
                EXPECT_EQ(net->users.at(user.index).cell, user.value.cell);
                EXPECT_EQ(net->users.at(user.index).port, user.value.port);
            }
            auto actual = net->users, expected = old.users;
            ASSERT_EQ(actual.capacity(), expected.capacity());
            // Include holes/free-list order, not only active user identities.
            for (int i = 0; i < old.users.capacity() + 8; ++i)
                EXPECT_EQ(actual.add(PortRef{}), expected.add(PortRef{}));
        }
        ASSERT_EQ(ctx->labs.size(), labs.size());
        for (size_t lab = 0; lab < labs.size(); ++lab) {
            const auto &now = ctx->labs[lab], &old = labs[lab];
            EXPECT_EQ(now.is_mlab, old.is_mlab);
            EXPECT_EQ(now.clk_wires, old.clk_wires);
            EXPECT_EQ(now.ena_wires, old.ena_wires);
            EXPECT_EQ(now.aclr_wires, old.aclr_wires);
            EXPECT_EQ(now.sclr_wire, old.sclr_wire);
            EXPECT_EQ(now.sload_wire, old.sload_wire);
            EXPECT_EQ(now.aclr_used, old.aclr_used);
            for (size_t alm = 0; alm < old.alms.size(); ++alm) {
                const auto &a = now.alms[alm], &b = old.alms[alm];
                EXPECT_EQ(a.comb_out, b.comb_out);
                EXPECT_EQ(a.sel_clk, b.sel_clk);
                EXPECT_EQ(a.sel_ena, b.sel_ena);
                EXPECT_EQ(a.sel_aclr, b.sel_aclr);
                EXPECT_EQ(a.sel_ef, b.sel_ef);
                EXPECT_EQ(a.ff_in, b.ff_in);
                EXPECT_EQ(a.ff_out, b.ff_out);
                EXPECT_EQ(a.lut_bels, b.lut_bels);
                EXPECT_EQ(a.ff_bels, b.ff_bels);
                EXPECT_EQ(a.l6_mode, b.l6_mode);
                EXPECT_EQ(a.carry_mode, b.carry_mode);
                EXPECT_EQ(a.clk_ena_idx, b.clk_ena_idx);
                EXPECT_EQ(a.aclr_idx, b.aclr_idx);
                EXPECT_EQ(a.unique_input_count, b.unique_input_count);
            }
        }
    }
};


class RoutedLutPermutationBackend : public ::testing::TestWithParam<std::tuple<bool,int>> {
  protected:
    std::unique_ptr<Context> ctx;
    uint32_t target_lab=0,source_lab=0,sink_lab=0;
    CellInfo *target=nullptr,*mate=nullptr,*local_ff=nullptr,*mate_ff=nullptr;
    std::vector<NetInfo*> inputs;
    std::vector<CellInfo*> stationary;
    std::string reason;
    int width=3,swap_first=0;
    int half() const {return std::get<1>(GetParam());}
    CellInfo *lut(const std::string &name,int n,uint64_t mask,const std::vector<NetInfo*> &nets,BelId bel) {
        const IdString types[]{id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5};
        auto *c=ctx->createCell(ctx->id(name),types[n-2]);c->params[id_LUT]=Property(int64_t(mask),1u<<n);
        for(int i=0;i<n;++i){c->addInput(pins[i]);if(i<int(nets.size())&&nets[i])c->connectPort(pins[i],nets[i]);else c->pin_data[pins[i]].state=PIN_0;}
        c->addOutput(id_Q);c->connectPort(id_Q,ctx->createNet(ctx->id(name+"$q")));
        ctx->assign_comb_info(c);ctx->assign_default_pinmap(c);ctx->bindBel(bel,c,STRENGTH_WEAK);return c;
    }
    CellInfo *ff(const std::string &name,CellInfo *local,BelId bel,bool controls) {
        auto *c=ctx->createCell(ctx->id(name),id_MISTRAL_FF);
        for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})c->addInput(p);
        c->addOutput(id_Q);c->connectPort(id_Q,ctx->createNet(ctx->id(name+"$q")));
        c->connectPort(id_CLK,inputs[9]);c->connectPort(id_DATAIN,local->getPort(id_Q));
        if(controls){c->connectPort(id_ENA,inputs[7]);c->connectPort(id_ACLR,inputs[8]);c->pin_data[id_ENA].state=PIN_INV;c->pin_data[id_ACLR].state=PIN_INV;}
        else {c->pin_data[id_ENA].state=PIN_1;c->pin_data[id_ACLR].state=PIN_1;}
        c->pin_data[id_SCLR].state=PIN_0;c->pin_data[id_SLOAD].state=PIN_0;
        ctx->assign_ff_info(c);ctx->assign_default_pinmap(c);ctx->bindBel(bel,c,STRENGTH_WEAK);return c;
    }
    static uint64_t mask_for_width(unsigned n) {
        const std::array<uint64_t,4> masks{0x2,0x35,0x4e81,0x96a53cc3};
        return masks.at(n-2); // Every connected input is functionally essential.
    }
    void build(bool sharing=true,bool registers=false,int n=3,bool invert=false) {
        inputs.clear();stationary.clear();target=mate=local_ff=mate_ff=nullptr;width=n;swap_first=sharing&&half()==0?1:0;
        ArchArgs args;args.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(args);ctx->settings[ctx->id("target_freq")]=1e8;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        std::vector<std::pair<int,uint32_t>> places;
        for(uint32_t i=0;i<ctx->labs.size();++i)if(ctx->labs[i].is_mlab==std::get<0>(GetParam())) {
            auto p=ctx->getBelLocation(ctx->labs[i].alms[0].lut_bels[0]);places.emplace_back(std::abs(p.x-24)+std::abs(p.y-3),i);
        }
        std::sort(places.begin(),places.end());ASSERT_GE(places.size(),3u);
        target_lab=places[0].second;source_lab=places[1].second;sink_lab=places[2].second;
        std::vector<CellInfo*> drivers;
        for(int i=0;i<10;++i){auto *d=lut("stimulus0"+std::to_string(i),2,8,{},ctx->labs[source_lab].alms[i].lut_bels[0]);drivers.push_back(d);inputs.push_back(d->getPort(id_Q));}
        inputs[9]->clkconstr=std::make_unique<ClockConstraint>();inputs[9]->clkconstr->period=DelayPair(10000);inputs[9]->clkconstr->high=inputs[9]->clkconstr->low=DelayPair(5000);
        for(int i=0;i<7;++i)inputs[i]=ff("signal0"+std::to_string(i),drivers[i],ctx->labs[source_lab].alms[i].ff_bels[0],false)->getPort(id_Q);
        target=lut("target",n,mask_for_width(n),std::vector<NetInfo*>(inputs.begin(),inputs.begin()+n),ctx->labs[target_lab].alms[0].lut_bels[half()]);
        if(invert){target->pin_data[id_A].state=PIN_INV;target->pin_data[id_B].state=PIN_INV;}
        if(sharing){ASSERT_EQ(n,3);mate=lut("mate",5,0x96696996,{inputs[3],inputs[4],inputs[1],inputs[0],inputs[5]},ctx->labs[target_lab].alms[0].lut_bels[1-half()]);}
        for(int i=0;i<6;++i){auto *c=lut("stationary"+std::to_string(i),2,14,{inputs[swap_first],inputs[n-1]},ctx->labs[sink_lab].alms[i].lut_bels[0]);stationary.push_back(c);
            ff("stationary_register"+std::to_string(i),c,ctx->labs[sink_lab].alms[i].ff_bels[0],false);}
        lut("outside",2,6,{inputs[7],inputs[8]},ctx->labs[sink_lab].alms[6].lut_bels[0]);
        auto *capture=lut("capture",2,8,{target->getPort(id_Q),inputs[6]},ctx->labs[sink_lab].alms[7].lut_bels[0]);
        ff("capture_register",capture,ctx->labs[sink_lab].alms[7].ff_bels[0],false);
        if(mate){auto *c=lut("mate_capture",2,14,{mate->getPort(id_Q),inputs[6]},ctx->labs[sink_lab].alms[8].lut_bels[0]);
            ff("mate_capture_register",c,ctx->labs[sink_lab].alms[8].ff_bels[0],false);}
        if(registers){local_ff=ff("local_register",target,ctx->labs[target_lab].alms[0].ff_bels[2*half()],true);
            if(mate)mate_ff=ff("mate_register",mate,ctx->labs[target_lab].alms[0].ff_bels[2*(1-half())],true);}
        auto alias=ctx->id("shared_signal_alias");ctx->net_aliases[alias]=inputs[0]->name;inputs[0]->aliases.push_back(alias);
        for(auto *net:inputs){auto a=net->users.add(PortRef{}),b=net->users.add(PortRef{}),c=net->users.add(PortRef{});net->users.remove(b);net->users.remove(a);net->users.remove(c);}
        ctx->assignArchInfo();ctx->lab_pre_route();
        for(const auto &c:ctx->cells){ASSERT_TRUE(ctx->isBelLocationValid(c.second->bel))<<ctx->nameOf(c.second.get());}
        int visits=5000000;ASSERT_TRUE(critical_local_route_net(ctx.get(),inputs[9],visits));
        if(registers)for(auto *f:{local_ff,mate_ff})if(f)for(auto p:{id_CLK,id_ENA,id_ACLR})if(auto *net=f->getPort(p)){ASSERT_TRUE(critical_local_route_net(ctx.get(),net,visits));}
        for(const auto &entry:ctx->nets)if(entry.second->driver.cell){ASSERT_TRUE(critical_local_route_net(ctx.get(),entry.second.get(),visits))<<ctx->nameOf(entry.second.get());}
        ctx->attrs[id_step]=std::string("route");ctx->configure_bitstream();ctx->check();ASSERT_TRUE(ctx->analogue_cache_valid);
        ASSERT_EQ(inputs[swap_first]->users.entries(),sharing?8:7);
    }
    void SetUp() override {build();}
    bool trial(const std::function<bool()> &accept=[]{return true;},const std::function<void(unsigned)> &stage={},int visits=500000) {
        return critical_routed_lut_permutation_trial(ctx.get(),target,pins[swap_first],pins[width-1],visits,accept,&reason,stage);
    }
    std::set<NetInfo*> incident() const {return std::set<NetInfo*>(inputs.begin(),inputs.begin()+width);}
    std::set<WireId> sinks(NetInfo *net) {
        std::set<WireId> result;for(const auto &u:net->users)for(auto w:ctx->getNetinfoSinkWires(net,u))result.insert(w);return result;
    }
    std::map<WireId,std::pair<PipId,PlaceStrength>> path(NetInfo *net,CellInfo *sink,IdString pin) {
        std::map<WireId,std::pair<PipId,PlaceStrength>> result;PortRef user{sink,pin};auto root=ctx->getNetinfoSourceWire(net);
        for(auto wire:ctx->getNetinfoSinkWires(net,user)){std::set<WireId> seen;for(;;){
            if(!seen.insert(wire).second||!net->wires.count(wire))throw std::runtime_error("missing/cyclic native path");
            const auto &entry=net->wires.at(wire);result[wire]={entry.pip,entry.strength};
            if(wire==root){if(entry.pip!=PipId())throw std::runtime_error("invalid root");break;}
            if(entry.pip==PipId()||ctx->getPipDstWire(entry.pip)!=wire)throw std::runtime_error("broken native path");
            wire=ctx->getPipSrcWire(entry.pip);
        }}if(result.empty())throw std::runtime_error("empty native path");return result;
    }
    void connected(NetInfo *net){for(const auto &u:net->users){EXPECT_FALSE(path(net,u.cell,u.port).empty());}}
    Snapshot expected_after(const Snapshot &before) {
        Snapshot result=before;auto &old=result.cells.at(target->name);const auto a=pins[swap_first],b=pins[width-1];
        const auto first=old.ports.at(a),second=old.ports.at(b);
        std::swap(old.ports.at(a).net,old.ports.at(b).net);std::swap(old.ports.at(a).user_idx,old.ports.at(b).user_idx);
        std::swap(old.architecture.pin_data.at(a).state,old.architecture.pin_data.at(b).state);
        std::swap(old.architecture.combInfo.lut_in[swap_first],old.architecture.combInfo.lut_in[width-1]);
        uint64_t mask=uint64_t(old.params.at(id_LUT).as_int64()),changed=0;
        for(unsigned r=0;r<(1u<<width);++r){unsigned perm=r;if(((r>>swap_first)^(r>>(width-1)))&1u)perm^=(1u<<swap_first)|(1u<<(width-1));changed|=((mask>>r)&1u)<<perm;}
        old.params[id_LUT]=Property(int64_t(changed),1u<<width);
        result.nets.at(first.net->name).users.at(first.user_idx)={target,b};result.nets.at(second.net->name).users.at(second.user_idx)={target,a};
        for(int i=0;i<width;++i)old.architecture.pin_data.at(pins[i]).bel_pins=target->pin_data.at(pins[i]).bel_pins;
        return result;
    }
};

TEST_P(RoutedLutPermutationBackend, SharedToPrivatePinKeepsAllStationaryTreesSlotsAndMate)
{
    Snapshot before(ctx.get());ASSERT_NE(mate,nullptr);
    for(bool skew:{false,true}){TimingAnalyser t(ctx.get());t.with_clock_skew=skew;t.setup(false,false,true);
        ASSERT_TRUE(critical_lut_permutation_source_coverage(ctx.get(),t,target,&reason))<<reason;
        ASSERT_TRUE(critical_lut6_swap_output_coverage(ctx.get(),t,target->getPort(id_Q),&reason))<<reason;
        for(auto *net:incident()){ASSERT_TRUE(critical_lut6_swap_output_coverage(ctx.get(),t,net,&reason))<<reason;}
    }
    // The allocator scans the top LUT first. Moving its first shared net
    // behind its second shared net changes the mate and must roll back.
    if(half()==0){EXPECT_FALSE(critical_routed_lut_permutation_trial(ctx.get(),target,id_A,id_C,500000,[]{return true;},&reason));before.expect_exact(ctx.get());}
    const auto old_shared=target->pin_data.at(pins[swap_first]).bel_pins;
    ASSERT_EQ(old_shared.size(),1u);EXPECT_TRUE(old_shared[0]==id_A||old_shared[0]==id_B);
    std::map<std::pair<CellInfo*,IdString>,std::map<WireId,std::pair<PipId,PlaceStrength>>> paths;
    std::map<NetInfo*,std::set<WireId>> old_sinks;
    for(auto *net:incident()){old_sinks[net]=sinks(net);for(const auto &u:net->users)if(u.cell!=target)paths[{u.cell,u.port}]=path(net,u.cell,u.port);}
    std::vector<unsigned> stages;ASSERT_TRUE(trial([]{return true;},[&](unsigned s){stages.push_back(s);}))<<reason;
    ASSERT_GE(stages.size(),7u);EXPECT_EQ(stages[0],1u);EXPECT_EQ(stages[1],2u);EXPECT_EQ(stages[2],3u);
    EXPECT_EQ(stages[stages.size()-2],20u);EXPECT_EQ(stages.back(),21u);
    EXPECT_EQ(target->getPort(id_C),inputs[swap_first]);EXPECT_EQ(target->getPort(pins[swap_first]),inputs[2]);
    const auto new_private=target->pin_data.at(id_C).bel_pins;ASSERT_EQ(new_private.size(),1u);
    EXPECT_TRUE(new_private[0]==(half()?id_E1:id_E0)||new_private[0]==(half()?id_F1:id_F0));
    std::set<NetInfo*> changed;
    for(auto *net:incident()){
        if(sinks(net)!=old_sinks.at(net))changed.insert(net);
        for(const auto &u:net->users)if(u.cell!=target){EXPECT_EQ(path(net,u.cell,u.port),paths.at({u.cell,u.port}));}
        connected(net);
    }
    ASSERT_GE(changed.size(),2u);auto expected=expected_after(before);expected.expect_exact(ctx.get(),changed);ctx->check();
}
TEST_P(RoutedLutPermutationBackend, RejectAcceptThrowEveryStageAndBudgetRestoreEverything)
{
    Snapshot before(ctx.get());std::vector<unsigned> stages;
    EXPECT_FALSE(trial([]{return false;},[&](unsigned s){stages.push_back(s);}));before.expect_exact(ctx.get());
    ASSERT_FALSE(stages.empty());ASSERT_EQ(stages.back(),21u);
    EXPECT_THROW(trial([]()->bool{throw std::runtime_error("accept");}),std::runtime_error);before.expect_exact(ctx.get());
    for(auto stage:stages){EXPECT_THROW(trial([]{return true;},[&](unsigned s){if(s==stage)throw std::runtime_error("stage");}),std::runtime_error);before.expect_exact(ctx.get());}
    EXPECT_FALSE(trial([]{return true;},{},0));before.expect_exact(ctx.get());
}
TEST_P(RoutedLutPermutationBackend, SecondChangedTreeFailureRestoresFirstAndSharedMateBranches)
{
    Snapshot before(ctx.get());std::map<NetInfo*,std::set<WireId>> old_sinks;for(auto *net:incident())old_sinks[net]=sinks(net);
    std::vector<NetInfo*> changed;std::vector<unsigned> stages;bool accepted=false;
    EXPECT_FALSE(trial([&]{accepted=true;return true;},[&](unsigned stage){
        stages.push_back(stage);
        if(stage==2){for(auto *net:incident())if(sinks(net)!=old_sinks.at(net))changed.push_back(net);
            std::sort(changed.begin(),changed.end(),[&](auto*a,auto*b){return a->name.str(ctx.get())<b->name.str(ctx.get());});}
        if(stage==10){ASSERT_GE(changed.size(),2u);connected(changed[0]);bool fault=false;
            for(const auto &u:changed[1]->users)if(u.cell==target){target->pin_data.at(u.port).bel_pins={id_COMBOUT};fault=true;}
            ASSERT_TRUE(fault);
        }
    }));
    EXPECT_FALSE(accepted);ASSERT_FALSE(stages.empty());EXPECT_EQ(stages.back(),10u);before.expect_exact(ctx.get());
}
TEST_P(RoutedLutPermutationBackend, ProtectedMalformedUnroutedAndUnsupportedOccupantsRejectBeforeMutation)
{
    auto reject=[&](){Snapshot old(ctx.get());bool touched=false;EXPECT_FALSE(trial([]{return true;},[&](unsigned){touched=true;}))<<reason;EXPECT_FALSE(touched);old.expect_exact(ctx.get());};
    for(auto *c:{target,mate})for(const char *key:{"keep","dont_touch","BEL","FES_SLOT"}){c->attrs[ctx->id(key)]=1;reject();c->attrs.erase(ctx->id(key));}
    auto step=ctx->attrs.at(id_step);ctx->attrs[id_step]=std::string("place");reject();ctx->attrs[id_step]=step;
    target->constr_x=1;reject();target->constr_x=0;
    auto strength=mate->belStrength;mate->belStrength=STRENGTH_STRONG;reject();mate->belStrength=strength;
    auto type=mate->type;mate->type=id_MISTRAL_ALUT6;reject();mate->type=id_MISTRAL_ALUT_ARITH;reject();mate->type=id_MISTRAL_MLAB;reject();mate->type=type;
    auto *source=inputs[0]->driver.cell;source->attrs[ctx->id("keep")]=1;reject();source->attrs.erase(ctx->id("keep"));
    inputs[0]->attrs[ctx->id("dont_touch")]=1;reject();inputs[0]->attrs.erase(ctx->id("dont_touch"));
    auto public_name=ctx->id("public_shared_input");ctx->ports[public_name]=PortInfo{public_name,inputs[0],PORT_OUT,{}};reject();ctx->ports.erase(public_name);
    auto state=target->get_pin_state(id_A);target->pin_data[id_A].state=PIN_0;reject();target->pin_data[id_A].state=state;
    auto driver=inputs[0]->driver;inputs[0]->driver.port=id_CLK;reject();inputs[0]->driver=driver;
    ctx->fes_fence_active=true;reject();ctx->fes_fence_active=false;ctx->fes_has_cram_region=true;reject();ctx->fes_has_cram_region=false;
    Snapshot before(ctx.get());bool touched=false;
    EXPECT_FALSE(critical_routed_lut_permutation_trial(ctx.get(),target,id_A,id_A,500000,[]{return true;},&reason,[&](unsigned){touched=true;}));
    EXPECT_FALSE(critical_routed_lut_permutation_trial(ctx.get(),target,id_A,id_F,500000,[]{return true;},&reason,[&](unsigned){touched=true;}));
    EXPECT_FALSE(touched);before.expect_exact(ctx.get());
}

const std::array<unsigned,64> mlab_order{
    0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,
    2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18,
    32,33,36,37,40,41,44,45,61,60,57,56,53,52,49,48,
    34,35,38,39,42,43,46,47,63,62,59,58,55,54,51,50};
uint64_t emitted_mask(Context *ctx,const CV &decoded,CellInfo *cell)
{
    const auto &loc=ctx->bel_data(cell->bel).lab_data;CV::bmux_setting_t s{};
    if(!decoded.bmux_get(ctx->labs[loc.lab].is_mlab?CV::MLAB:CV::LAB,cell->bel.pos,CV::LUT_MASK,loc.alm,s)||s.type!=CV::MT_RAM||s.r.size()!=8)throw std::runtime_error("missing emitted LUT mask");
    uint64_t mask=0;for(unsigned i=0;i<8;++i)mask|=uint64_t(s.r[i])<<(8*i);return mask;
}
unsigned physical_address_bit(Context *ctx,CellInfo *cell,IdString logical)
{
    int half=ctx->bel_data(cell->bel).lab_data.idx;const auto &map=cell->pin_data.at(logical).bel_pins;
    if (map.size() != 1)
        throw std::runtime_error("invalid emitted pin map");
    auto p = map.front();
    if (p == id_A) return 0;
    if (p == id_B) return 1;
    if (p == (half ? id_D : id_C)) return 2;
    if (p == (half ? id_E1 : id_E0)) return 3;
    if (p == (half ? id_F1 : id_F0)) return 4;
    throw std::runtime_error("invalid physical half pin");
}
unsigned emitted_address(Context *ctx,CellInfo *cell,unsigned address)
{
    const auto &loc=ctx->bel_data(cell->bel).lab_data;unsigned row=address+32*loc.idx;return ctx->labs[loc.lab].is_mlab?mlab_order.at(row):row;
}
void check_emitted_truth(Context *ctx,const CV &decoded,CellInfo *cell)
{
    const auto &loc=ctx->bel_data(cell->bel).lab_data;const auto block=ctx->labs[loc.lab].is_mlab?CV::MLAB:CV::LAB;
    CV::bmux_setting_t top,bottom;ASSERT_TRUE(decoded.bmux_get(block,cell->bel.pos,CV::TMODE,loc.alm,top));ASSERT_TRUE(decoded.bmux_get(block,cell->bel.pos,CV::BMODE,loc.alm,bottom));EXPECT_EQ(top.s,uint32_t(CV::E_0));EXPECT_EQ(bottom.s,uint32_t(CV::E_1));
    const auto mask=emitted_mask(ctx,decoded,cell);std::set<unsigned> used;
    for(int i=0;i<cell->combInfo.lut_input_count;++i)ASSERT_TRUE(used.insert(physical_address_bit(ctx,cell,pins[i])).second);
    for(unsigned addr=0;addr<32;++addr){unsigned logical=0;for(int i=0;i<cell->combInfo.lut_input_count;++i){bool v=((addr>>physical_address_bit(ctx,cell,pins[i]))&1)!= (cell->get_pin_state(pins[i])!=PIN_INV);logical|=unsigned(v)<<i;}
        EXPECT_EQ((mask>>emitted_address(ctx,cell,addr))&1,1^((uint64_t(cell->params.at(id_LUT).as_int64())>>logical)&1))<<ctx->nameOf(cell)<<":"<<addr;
    }
}

std::unique_ptr<CV> decoded(Context *ctx)
{
    std::vector<uint8_t> bytes;ctx->cyclonev->rbf_save(bytes);if(bytes.empty())throw std::runtime_error("empty native RBF");
    auto result=std::unique_ptr<CV>(CV::get_model("5CSEBA6U23I7"));if(!result)throw std::runtime_error("native model missing");
    result->rbf_load(bytes.data(),uint32_t(bytes.size()));return result;
}

using MuxState=std::map<std::tuple<uint32_t,int,int>,std::pair<int,uint32_t>>;
MuxState control_state(Context *ctx,const CV &model,const std::vector<CellInfo *> &cells)
{
    const std::vector<CV::bmux_type_t> per_alm{CV::TMODE,CV::BMODE,CV::TEF_SEL,CV::BEF_SEL,CV::TPKREG0,CV::TPKREG1,CV::BPKREG0,CV::BPKREG1,CV::TCLK_SEL,CV::BCLK_SEL,CV::TCLR_SEL,CV::BCLR_SEL,CV::TSCLR_DIS,CV::BSCLR_DIS,CV::TSLOAD_EN,CV::BSLOAD_EN};
    const std::vector<CV::bmux_type_t> per_lab{CV::CLK0_INV,CV::CLK1_INV,CV::CLK2_INV,CV::EN0_EN,CV::EN1_EN,CV::EN2_EN,CV::EN0_NINV,CV::EN1_NINV,CV::EN2_NINV,CV::ACLR0_INV,CV::ACLR1_INV,CV::SLOAD_INV,CV::SCLR_INV};
    MuxState result;
    for(auto *c:cells){const auto &l=ctx->bel_data(c->bel).lab_data;auto block=ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB;
        for(auto mux:per_alm){CV::bmux_setting_t s{};if(!model.bmux_get(block,c->bel.pos,mux,l.alm,s))throw std::runtime_error("missing ALM selector");result[{l.lab,l.alm,int(mux)}]={s.type,s.s};}
        for(auto mux:per_lab){CV::bmux_setting_t s{};if(!model.bmux_get(block,c->bel.pos,mux,0,s))throw std::runtime_error("missing LAB selector");result[{l.lab,-1,int(mux)}]={s.type,s.s};}
    }
    return result;
}

TEST_P(RoutedLutPermutationBackend, EmittedTruthRowsAndStationaryHalfWithInvertedSourceAreExact)
{
    build(true,false,3,true);Snapshot before(ctx.get());auto old=decoded(ctx.get());
    const auto original=before.cells.at(target->name);const auto mate_mask=emitted_mask(ctx.get(),*old,mate);
    check_emitted_truth(ctx.get(),*old,target);check_emitted_truth(ctx.get(),*old,mate);
    ASSERT_TRUE(trial())<<reason;auto now=decoded(ctx.get());check_emitted_truth(ctx.get(),*now,target);check_emitted_truth(ctx.get(),*now,mate);
    uint64_t select=0;for(unsigned a=0;a<32;++a)select|=uint64_t(1)<<emitted_address(ctx.get(),mate,a);
    EXPECT_EQ(mate_mask&select,emitted_mask(ctx.get(),*now,mate)&select);
    for(unsigned r=0;r<8;++r){unsigned oldrow=0,newaddr=0;
        for(int i=0;i<3;++i){bool raw=(r>>i)&1;oldrow|=unsigned(raw^(original.architecture.pin_data.at(pins[i]).state==PIN_INV))<<i;}
        for(int i=0;i<3;++i){auto *net=target->getPort(pins[i]);auto bit=std::find(inputs.begin(),inputs.begin()+3,net)-inputs.begin();ASSERT_LT(bit,3);
            // Native input inversion is fixed; PIN_INV is already folded into the emitted mask.
            bool physical=!bool((r>>bit)&1);newaddr|=unsigned(physical)<<physical_address_bit(ctx.get(),target,pins[i]);}
        EXPECT_EQ((uint64_t(original.params.at(id_LUT).as_int64())>>oldrow)&1,1^((emitted_mask(ctx.get(),*now,target)>>emitted_address(ctx.get(),target,newaddr))&1));
    }
}
TEST_P(RoutedLutPermutationBackend, WidthsTwoThroughFiveAllRawTruthRowsArePreserved)
{
    for(int n=2;n<=5;++n){SCOPED_TRACE(n);build(false,false,n,true);Snapshot before(ctx.get());const auto original=before.cells.at(target->name);
        ASSERT_TRUE(trial())<<reason;auto now=decoded(ctx.get());check_emitted_truth(ctx.get(),*now,target);
        auto expected=expected_after(before);expected.expect_exact(ctx.get(),incident());
        for(unsigned r=0;r<(1u<<n);++r){unsigned oldrow=0,address=0;
            for(int i=0;i<n;++i)oldrow|=unsigned(bool((r>>i)&1)^(original.architecture.pin_data.at(pins[i]).state==PIN_INV))<<i;
            for(int i=0;i<n;++i){auto bit=std::find(inputs.begin(),inputs.begin()+n,target->getPort(pins[i]))-inputs.begin();ASSERT_LT(bit,n);
                // Native input inversion is fixed; PIN_INV is already folded into the emitted mask.
            bool physical=!bool((r>>bit)&1);address|=unsigned(physical)<<physical_address_bit(ctx.get(),target,pins[i]);}
            EXPECT_EQ((uint64_t(original.params.at(id_LUT).as_int64())>>oldrow)&1,1^((emitted_mask(ctx.get(),*now,target)>>emitted_address(ctx.get(),target,address))&1))<<r;
        }
    }
}
TEST_P(RoutedLutPermutationBackend, LocalCombRegistersKeepSelectorsControlsRoutesAndEveryRollbackStage)
{
    build(true,true);ASSERT_NE(local_ff,nullptr);ASSERT_NE(mate_ff,nullptr);Snapshot before(ctx.get());auto old=decoded(ctx.get());
    const auto controls=control_state(ctx.get(),*old,{target,mate});std::vector<unsigned> stages;
    EXPECT_FALSE(trial([]{return false;},[&](unsigned s){stages.push_back(s);}));before.expect_exact(ctx.get());ASSERT_FALSE(stages.empty());ASSERT_EQ(stages.back(),21u);
    for(auto stage:stages){EXPECT_THROW(trial([]{return true;},[&](unsigned s){if(s==stage)throw std::runtime_error("FF stage");}),std::runtime_error);before.expect_exact(ctx.get());}
    ASSERT_TRUE(trial())<<reason;auto now=decoded(ctx.get());EXPECT_EQ(control_state(ctx.get(),*now,{target,mate}),controls);
    auto expected=expected_after(before);expected.expect_exact(ctx.get(),incident());
    for(auto *c:{target,mate})check_emitted_truth(ctx.get(),*now,c);
    for(auto *f:{local_ff,mate_ff})for(auto p:{id_DATAIN,id_CLK,id_ENA,id_ACLR})if(auto *net=f->getPort(p)){connected(net);}
}
TEST_P(RoutedLutPermutationBackend, CorruptedFunctionMateMapAndFfCacheAreRejectedWithExactRollback)
{
    build(true,true);Snapshot before(ctx.get());
    bool reached=false,accepted=false;
    EXPECT_FALSE(trial([&]{accepted=true;return true;},[&](unsigned stage){if(stage==1){
        reached=true;auto mask=uint64_t(target->params.at(id_LUT).as_int64());target->params[id_LUT]=Property(int64_t(mask^1),8);
    }}));EXPECT_TRUE(reached);EXPECT_FALSE(accepted);before.expect_exact(ctx.get());
    reached=false;
    EXPECT_FALSE(trial([&]{accepted=true;return true;},[&](unsigned stage){if(stage==2){
        reached=true;mate->pin_data.at(id_C).bel_pins={id_COMBOUT};
    }}));EXPECT_TRUE(reached);EXPECT_FALSE(accepted);before.expect_exact(ctx.get());
    reached=false;
    EXPECT_FALSE(trial([&]{accepted=true;return true;},[&](unsigned stage){if(stage==20){
        reached=true;local_ff->ffInfo.datain=inputs[7];
    }}));EXPECT_TRUE(reached);EXPECT_FALSE(accepted);before.expect_exact(ctx.get());
}
TEST_P(RoutedLutPermutationBackend, IndependentDataAndSynchronousLoadFfStatesAreExcluded)
{
    build(true,true);auto *local=local_ff->getPort(id_DATAIN);
    local_ff->disconnectPort(id_DATAIN);local_ff->connectPort(id_DATAIN,inputs[7]);ctx->assign_ff_info(local_ff);
    auto reject=[&](){Snapshot old(ctx.get());bool touched=false;EXPECT_FALSE(trial([]{return true;},[&](unsigned){touched=true;}));EXPECT_FALSE(touched);old.expect_exact(ctx.get());};reject();
    local_ff->disconnectPort(id_DATAIN);local_ff->connectPort(id_DATAIN,local);ctx->assign_ff_info(local_ff);
    local_ff->connectPort(id_SDATA,inputs[8]);local_ff->connectPort(id_SLOAD,inputs[7]);local_ff->pin_data[id_SLOAD].state=PIN_SIG;ctx->assign_ff_info(local_ff);reject();
}
INSTANTIATE_TEST_SUITE_P(LabAndMlabBothHalves,RoutedLutPermutationBackend,::testing::Combine(::testing::Bool(),::testing::Values(0,1)));

TEST(RoutedLutPermutationPolicy, EveryTruthRowForAllWidthsAndPairsAndInvalidShapes)
{
    namespace p=routed_lut_permutation_policy;
    for(unsigned n=2;n<=5;++n)for(unsigned a=0;a<n;++a)for(unsigned b=a+1;b<n;++b)
        for(uint64_t seed:{0ULL,1ULL,0x35ULL,0x96696996ULL,0xffffffffULL}){
            const auto mask=seed&((uint64_t(1)<<(1u<<n))-1);auto transformed=p::transpose(mask,n,a,b);ASSERT_TRUE(transformed);
            for(unsigned row=0;row<(1u<<n);++row){unsigned swapped=row;if(((row>>a)^(row>>b))&1)swapped^=(1u<<a)|(1u<<b);
                EXPECT_EQ((mask>>row)&1,(*transformed>>swapped)&1);}
        }
    EXPECT_FALSE(p::transpose(0,1,0,1));EXPECT_FALSE(p::transpose(0,6,0,1));EXPECT_FALSE(p::transpose(0,3,0,3));
    EXPECT_FALSE(p::transpose(0,3,1,1));EXPECT_FALSE(p::transpose(0x100,3,0,1));
}

class RoutedLutPermutationCoverage : public ::testing::TestWithParam<int> {
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *root=nullptr,*capture=nullptr,*mix=nullptr;
    std::array<CellInfo*,6> source{};
    NetInfo *clock=nullptr,*phase=nullptr;
    std::string reason;
    NetInfo *net(const std::string &n){return ctx->createNet(ctx->id(n));}
    NetInfo *make_clock(const char *name,int shift)
    {
        auto *n=net(name);n->clkconstr=std::make_unique<ClockConstraint>();
        n->clkconstr->period=DelayPair(10000);n->clkconstr->high=n->clkconstr->low=DelayPair(5000);
        n->clkconstr->phase_group=ctx->id("family");n->clkconstr->phase_shift=shift;return n;
    }
    CellInfo *ff(const std::string &name,NetInfo *data=nullptr,NetInfo *clk=nullptr)
    {
        auto *f=ctx->createCell(ctx->id(name),id_MISTRAL_FF);
        for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})f->addInput(p);
        f->addOutput(id_Q);f->connectPort(id_Q,net(name+"$q"));
        f->connectPort(id_DATAIN,data?data:f->getPort(id_Q));f->connectPort(id_CLK,clk?clk:clock);
        f->pin_data[id_ENA].state=f->pin_data[id_ACLR].state=PIN_1;
        f->pin_data[id_SCLR].state=f->pin_data[id_SLOAD].state=PIN_0;return f;
    }
    CellInfo *lut(const std::string &name,int width)
    {
        const IdString types[]{id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5};
        auto *l=ctx->createCell(ctx->id(name),types[width-2]);
        l->params[id_LUT]=Property(int64_t((uint64_t(1)<<(1u<<width))-2),1u<<width);
        for(int n=0;n<width;++n){l->addInput(pins[n]);l->pin_data[pins[n]].state=PIN_0;}
        l->addOutput(id_Q);l->connectPort(id_Q,net(name+"$q"));return l;
    }
    void input(CellInfo *c,IdString p,NetInfo *n,CellPinState state=PIN_SIG)
    {if(c->getPort(p))c->disconnectPort(p);if(n)c->connectPort(p,n);c->pin_data[p].state=state;}
    CellInfo *buffer(const std::string &name,NetInfo *n)
    {
        auto *b=ctx->createCell(ctx->id(name),id_MISTRAL_BUF);b->addInput(id_A);b->connectPort(id_A,n);
        b->addOutput(id_Q);b->connectPort(id_Q,net(name+"$q"));return b;
    }
    CellInfo *memory(const std::string &name)
    {
        auto *m=ctx->createCell(ctx->id(name),id_MISTRAL_M10K);
        m->params[id_CFG_ABITS]=12;m->params[id_CFG_DBITS]=1;m->params[id_CFG_DUAL_CLOCK]=1;
        for(auto p:{id_CLK1,id_CLK2,id_B1EN,id_ACLR0,id_ACLR1,ctx->id("B1ADDR[0]")})m->addInput(p);
        m->connectPort(id_CLK1,clock);m->connectPort(id_CLK2,phase);m->connectPort(ctx->id("B1ADDR[0]"),source[1]->getPort(id_Q));
        m->connectPort(id_B1EN,ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get());m->pin_data[id_B1EN].state=PIN_1;
        m->pin_data[id_ACLR0].state=m->pin_data[id_ACLR1].state=PIN_0;m->addOutput(id_B1DATA);m->connectPort(id_B1DATA,net(name+"$q"));return m;
    }
    void SetUp() override
    {
        ArchArgs args;args.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(args);ctx->settings[ctx->id("target_freq")]=1e8;
        net("$PACKER_GND_NET");net("$PACKER_VCC_NET");clock=make_clock("clock",0);phase=make_clock("phase",2500);
        for(unsigned n=0;n<6;++n)source[n]=ff("source"+std::to_string(n));
        root=lut("root",GetParam());for(int n=0;n<GetParam();++n)input(root,pins[n],source[n]->getPort(id_Q));
        capture=ff("capture",root->getPort(id_Q));mix=lut("mixed_source",2);
        input(mix,id_A,source[0]->getPort(id_Q));input(mix,id_B,source[1]->getPort(id_Q));input(root,id_A,mix->getPort(id_Q));
    }
    void expect(bool expected)
    {
        ctx->assignArchInfo();
        for(bool skew:{false,true}) {
            TimingAnalyser timing(ctx.get());timing.with_clock_skew=skew;timing.setup(false,false,true);
            EXPECT_EQ(critical_lut_permutation_source_coverage(ctx.get(),timing,root,&reason),expected)<<"skew="<<skew<<" "<<reason;
        }
    }
};

TEST_P(RoutedLutPermutationCoverage, EverySupportedWidthKeepsAllPhasesAndRejectsSilentlyMissingOrigins)
{
    expect(true);auto *m=memory("read_source");input(root,id_B,m->getPort(id_B1DATA));expect(true);
    auto *b=buffer("upstream_buffer",mix->getPort(id_Q));
    auto *a=ctx->createCell(ctx->id("upstream_carry"),id_MISTRAL_ALUT_ARITH);
    a->params[id_LUT0]=Property(0x6996,16);a->params[id_LUT1]=Property(0x9669,16);
    for(auto p:{id_A,id_B,id_C,id_D0,id_D1,id_CI}){a->addInput(p);a->pin_data[p].state=PIN_0;}
    input(a,id_A,b->getPort(id_Q));input(a,id_B,source[2]->getPort(id_Q),PIN_INV);
    input(a,id_D0,source[3]->getPort(id_Q));input(a,id_CI,source[4]->getPort(id_Q));a->pin_data[id_D1].state=PIN_1;
    for(auto p:{id_SO,id_CO}){a->addOutput(p);a->connectPort(p,net(p==id_SO?"sum":"carry"));}
    input(root,id_A,a->getPort(id_SO));expect(true);
    input(mix,id_B,nullptr,PIN_1);expect(true);input(mix,id_B,nullptr,PIN_0);expect(true);
    input(mix,id_B,source[1]->getPort(id_Q),PIN_INV);expect(true);
    auto *missing=ff("missing_clock");missing->disconnectPort(id_CLK);input(mix,id_B,missing->getPort(id_Q));expect(false);
    input(mix,id_B,net("undriven"));expect(false);
    auto *ignored=ctx->createCell(ctx->id("ignored_source"),id_MISTRAL_IB);ignored->addOutput(id_O);ignored->connectPort(id_O,net("ignored$q"));
    input(mix,id_B,ignored->getPort(id_O));expect(false);
    auto *public_net=net("public_input");ctx->ports[ctx->id("public_input")]=PortInfo{ctx->id("public_input"),public_net,PORT_IN,{}};
    input(mix,id_B,public_net);expect(false);input(mix,id_B,source[1]->getPort(id_Q));expect(true);
    Context::SdcClockException cut;cut.from={"phase"};cut.to={"clock"};ctx->sdc_clock_exceptions.push_back(cut);expect(false);ctx->sdc_clock_exceptions.clear();expect(true);
    ctx->assignArchInfo();TimingAnalyser timing(ctx.get());timing.setup(false,false,true);
    EXPECT_FALSE(critical_lut6_swap_source_coverage(ctx.get(),timing,root,&reason));
    auto type=root->type;root->type=id_MISTRAL_ALUT6;EXPECT_FALSE(critical_lut_permutation_source_coverage(ctx.get(),timing,root,&reason));root->type=type;
}
TEST_P(RoutedLutPermutationCoverage, ParallelUnsupportedAsyncResetPublicAndCutCaptureBranchesReject)
{
    auto check=[&](bool expected){ctx->assignArchInfo();for(bool skew:{false,true}){TimingAnalyser t(ctx.get());t.with_clock_skew=skew;t.setup(false,false,true);
        EXPECT_EQ(critical_lut6_swap_output_coverage(ctx.get(),t,root->getPort(id_Q),&reason),expected)<<reason;}};
    check(true);
    capture->connectPort(id_ACLR,root->getPort(id_Q));capture->pin_data[id_ACLR].state=PIN_SIG;check(false);
    capture->disconnectPort(id_ACLR);capture->pin_data[id_ACLR].state=PIN_1;check(true);
    auto *hidden=ctx->createCell(ctx->id("hidden_unsupported"),id_MISTRAL_OB);hidden->addInput(id_I);hidden->connectPort(id_I,root->getPort(id_Q));check(false);
    hidden->disconnectPort(id_I);auto hidden_name=hidden->name;ctx->cells.erase(hidden_name);check(true);
    auto public_name=ctx->id("public_observer");ctx->ports[public_name]=PortInfo{public_name,root->getPort(id_Q),PORT_OUT,{}};check(false);ctx->ports.erase(public_name);check(true);
    auto *other=ff("phase_capture",root->getPort(id_Q),phase);(void)other;check(true);
    Context::SdcClockException cut;cut.from={"clock"};cut.to={"phase"};ctx->sdc_clock_exceptions.push_back(cut);check(false);ctx->sdc_clock_exceptions.clear();check(true);
}
INSTANTIATE_TEST_SUITE_P(AllSmallWidths,RoutedLutPermutationCoverage,::testing::Values(2,3,4,5));
} // namespace
