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
// Isolated nine-argument overload, linked to the reviewed implementation against
// the internally consistent frozen 7faa Context ABI. In that isolated build,
// the old eight-argument overload delegates with false. In the shared build,
// the header default supplies false to the new ninth argument. Both paths
// exercise the legacy/default behavior against an explicit-false call.
NEXTPNR_NAMESPACE_BEGIN
bool critical_routed_lut_permutation_trial(Context *, CellInfo *, IdString, IdString, int,
    const std::function<bool()> &, std::string *, const std::function<void(unsigned)> &, bool);
NEXTPNR_NAMESPACE_END
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


class SourceExitPermutationBackend : public ::testing::TestWithParam<std::tuple<bool,int>> {
  protected:
    std::unique_ptr<Context> ctx;
    uint32_t target_lab=0,source_lab=0,sink_lab=0;
    CellInfo *target=nullptr,*mate=nullptr,*local_ff=nullptr,*mate_ff=nullptr;
    std::vector<NetInfo*> inputs;
    std::vector<CellInfo*> stationary;
    std::string reason;
    int width=3,swap_first=0;
    CellInfo *exit_driver=nullptr,*sibling_driver=nullptr;
    NetInfo *exit_net=nullptr;
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
        exit_driver=lut("inside_driver",2,6,{inputs[3],inputs[4]},ctx->labs[target_lab].alms[1].lut_bels[half()]);
        sibling_driver=lut("inside_sibling",2,8,{inputs[3],inputs[4]},ctx->labs[target_lab].alms[1].lut_bels[1-half()]);
        exit_net=exit_driver->getPort(id_Q);inputs[n-1]=exit_net;
        target=lut("target",n,mask_for_width(n),std::vector<NetInfo*>(inputs.begin(),inputs.begin()+n),ctx->labs[target_lab].alms[0].lut_bels[half()]);
        if(invert){target->pin_data[id_A].state=PIN_INV;target->pin_data[id_B].state=PIN_INV;}
        if(sharing){ASSERT_EQ(n,3);mate=lut("mate",5,0x96696996,{inputs[3],inputs[4],inputs[1],inputs[0],inputs[5]},ctx->labs[target_lab].alms[0].lut_bels[1-half()]);}
        for(int i=0;i<6;++i){auto *c=lut("stationary"+std::to_string(i),2,14,{inputs[swap_first],inputs[n-1]},ctx->labs[sink_lab].alms[i].lut_bels[0]);stationary.push_back(c);
            ff("stationary_register"+std::to_string(i),c,ctx->labs[sink_lab].alms[i].ff_bels[0],false);}
        lut("outside",2,6,{sibling_driver->getPort(id_Q),inputs[8]},ctx->labs[sink_lab].alms[6].lut_bels[0]);
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
        const auto native_cache=Snapshot::cache(ctx.get());ASSERT_FALSE(native_cache.empty());
        bool characterized=false;for(const auto &entry:native_cache)characterized|=std::get<0>(entry.second);
        ASSERT_TRUE(characterized); // Rollback comparisons exercise populated, characterized native arcs.
        ASSERT_EQ(inputs[swap_first]->users.entries(),sharing?8:7);
    }
    void SetUp() override {build(true,true,3,true);}
    bool trial(const std::function<bool()> &accept=[]{return true;},const std::function<void(unsigned)> &stage={},int visits=500000,bool keep=true) {
        return critical_routed_lut_permutation_trial(ctx.get(),target,pins[swap_first],pins[width-1],visits,accept,&reason,stage,keep);
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

    struct Exit { WireId wire; PipId pip; CV::bmux_type_t mux; PlaceStrength strength; };
    std::vector<Exit> unused_exits(CellInfo *driver) {
        const auto &where=ctx->bel_data(driver->bel).lab_data;
        const auto block=ctx->labs[where.lab].is_mlab?CV::MLAB:CV::LAB;
        const std::array<CV::port_type_t,6> port{CV::FFT0,CV::FFT1,CV::FFT1L,CV::FFB0,CV::FFB1,CV::FFB1L};
        const std::array<CV::bmux_type_t,6> mux{CV::TDFF0,CV::TDFF1,CV::TDFF1L,CV::BDFF0,CV::BDFF1,CV::BDFF1L};
        auto *net=driver->getPort(id_Q);const auto root=ctx->getNetinfoSourceWire(net);
        std::vector<Exit> result;
        for(int i=3*where.idx;i<3*where.idx+3;++i) {
            auto w=ctx->get_port(block,driver->bel.pos.x(),driver->bel.pos.y(),where.alm,port[i]);
            if(ctx->getBoundWireNet(w))continue;
            for(auto pip:ctx->getPipsDownhill(root))if(ctx->getPipDstWire(pip)==w&&!ctx->getBoundPipNet(pip)) {
                result.push_back({w,pip,mux[i],STRENGTH_WEAK});break;
            }
        }
        return result;
    }
    uint32_t selector(const CV &model,CellInfo *driver,const Exit &exit) {
        const auto &where=ctx->bel_data(driver->bel).lab_data;
        CV::bmux_setting_t value;
        if(!model.bmux_get(ctx->labs[where.lab].is_mlab?CV::MLAB:CV::LAB,driver->bel.pos,exit.mux,where.alm,value)||value.type!=CV::MT_MUX)
            throw std::runtime_error("native source-exit selector missing");
        return value.s;
    }
    void check_exit(CellInfo *driver,const Exit &exit) {
        auto *net=driver->getPort(id_Q);ASSERT_TRUE(net->wires.count(exit.wire));
        EXPECT_EQ(net->wires.at(exit.wire).pip,exit.pip);EXPECT_EQ(net->wires.at(exit.wire).strength,exit.strength);
        EXPECT_EQ(ctx->getBoundWireNet(exit.wire),net);EXPECT_EQ(ctx->getBoundPipNet(exit.pip),net);
        EXPECT_EQ(ctx->getPipSrcWire(exit.pip),ctx->getNetinfoSourceWire(net));EXPECT_EQ(ctx->getPipDstWire(exit.pip),exit.wire);
    }
    // This is an explicitly synthetic, rooted unused exit stub. The separately
    // retained board audit proves the live moved-target-only version of this
    // boundary. Enumerate at most the three native exits and choose one the
    // old route pruning retires; no architectural selector is invented.
    Exit make_retired_stub() {
        const auto candidates=unused_exits(exit_driver);
        if(candidates.empty())throw std::runtime_error("fixture has no free native source exit");
        for(const auto &exit:candidates) {
            for(const auto &u:exit_net->users)if(path(exit_net,u.cell,u.port).count(exit.wire))
                throw std::runtime_error("unused stub is unexpectedly an observed path");
            ctx->bindPip(exit.pip,exit_net,exit.strength);ctx->configure_bitstream();ctx->check();
            EXPECT_EQ(selector(*ctx->cyclonev,exit_driver,exit),uint32_t(CV::NLUT));
            Snapshot old(ctx.get());bool called=false;std::vector<unsigned> stages;
            const bool accepted=trial([&]{called=true;return false;},[&](unsigned s){stages.push_back(s);},500000,false);
            EXPECT_FALSE(accepted);old.expect_exact(ctx.get());
            if(!called && reason=="configured stationary LAB/FF selector fields changed" && !stages.empty() && stages.back()==20)
                return exit;
            ctx->unbindPip(exit.pip);ctx->configure_bitstream();
        }
        throw std::runtime_error("all free native exit stubs were reused or blocked before the selector boundary");
    }
    using Config=std::map<std::tuple<int,int,int,int>,std::tuple<int,bool,uint32_t,std::vector<uint8_t>>>;
    Config config(const CV &model) {
        const auto &lab=ctx->labs[target_lab];const int block=lab.is_mlab?CV::MLAB:CV::LAB;
        const auto pos=target->bel.pos;const auto target_alm=ctx->bel_data(target->bel).lab_data.alm;
        Config result;
        for(const auto &v:model.bmux_get())if(int(v.btype)==block&&v.pos.v==pos.v&&!(v.mux==CV::LUT_MASK&&v.midx==target_alm))
            result[{int(v.btype),v.pos.v,int(v.mux),v.midx}]={v.type,v.def,v.s,v.r};
        return result;
    }

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

TEST_P(SourceExitPermutationBackend, LegacyDefaultIsExactAndOptInRetainsOriginalOwnedExit)
{
    const auto exit=make_retired_stub();
    // A separate sibling output is outside the incident-net set. Its unused
    // native branch must remain exactly unchanged in both modes.
    auto siblings=unused_exits(sibling_driver);ASSERT_FALSE(siblings.empty());const auto sibling=siblings.front();
    ctx->bindPip(sibling.pip,sibling_driver->getPort(id_Q),sibling.strength);ctx->configure_bitstream();ctx->check();
    Snapshot before(ctx.get());const auto old_native=decoded(ctx.get());const auto old_config=config(*old_native);
    const auto old_controls=control_state(ctx.get(),*old_native,{target,mate});
    check_emitted_truth(ctx.get(),*old_native,target);check_emitted_truth(ctx.get(),*old_native,mate);
    std::vector<unsigned> legacy,off;std::string legacy_reason;
    bool called=false;
    EXPECT_FALSE(critical_routed_lut_permutation_trial(ctx.get(),target,pins[swap_first],pins[width-1],500000,
        [&]{called=true;return true;},&legacy_reason,[&](unsigned s){legacy.push_back(s);}));
    EXPECT_FALSE(called);before.expect_exact(ctx.get());
    EXPECT_FALSE(trial([&]{called=true;return true;},[&](unsigned s){off.push_back(s);},500000,false));
    EXPECT_FALSE(called);before.expect_exact(ctx.get());EXPECT_EQ(off,legacy);EXPECT_EQ(reason,legacy_reason);
    ASSERT_FALSE(off.empty());EXPECT_EQ(off.back(),20u);EXPECT_EQ(reason,"configured stationary LAB/FF selector fields changed");
    using Path=std::map<WireId,std::pair<PipId,PlaceStrength>>;
    std::map<std::pair<CellInfo*,IdString>,Path> stationary_paths;
    std::map<NetInfo*,std::set<WireId>> old_sinks;
    for(auto *net:incident()){old_sinks[net]=sinks(net);for(const auto &u:net->users)if(u.cell!=target)stationary_paths[{u.cell,u.port}]=path(net,u.cell,u.port);}
    std::vector<unsigned> on;
    ASSERT_TRUE(trial([&]{called=true;return true;},[&](unsigned s){
        on.push_back(s);if(s>=3)check_exit(exit_driver,exit);
        if(s==3)for(auto *net:incident())for(const auto &w:net->wires){
            const auto &original=before.nets.at(net->name).wires;
            ASSERT_TRUE(original.count(w.first));
            EXPECT_EQ(original.at(w.first),(std::make_pair(w.second.pip,w.second.strength)));
            EXPECT_EQ(ctx->getBoundWireNet(w.first),net);
            if(w.second.pip!=PipId()){EXPECT_EQ(ctx->getBoundPipNet(w.second.pip),net);}
        }
    }))<<reason;
    ASSERT_TRUE(called);ASSERT_FALSE(on.empty());EXPECT_EQ(on.back(),21u);
    check_exit(exit_driver,exit);check_exit(sibling_driver,sibling);
    auto now=decoded(ctx.get());EXPECT_EQ(control_state(ctx.get(),*now,{target,mate}),old_controls);EXPECT_EQ(config(*now),old_config);EXPECT_EQ(selector(*now,exit_driver,exit),uint32_t(CV::NLUT));
    check_emitted_truth(ctx.get(),*now,target);check_emitted_truth(ctx.get(),*now,mate);
    std::set<NetInfo*> changed;
    for(auto *net:incident()){
        if(sinks(net)!=old_sinks.at(net))changed.insert(net);
        for(const auto &u:net->users)if(u.cell!=target){EXPECT_EQ(path(net,u.cell,u.port),stationary_paths.at({u.cell,u.port}));}
        connected(net);
    }
    ASSERT_GE(changed.size(),2u);expected_after(before).expect_exact(ctx.get(),changed);
    // Independently relate every raw input assignment to the emitted LUT mask.
    const auto &original=before.cells.at(target->name);
    for(unsigned raw=0;raw<8;++raw){unsigned oldrow=0,address=0;
        for(unsigned i=0;i<3;++i)oldrow|=unsigned(bool((raw>>i)&1)^(original.architecture.pin_data.at(pins[i]).state==PIN_INV))<<i;
        for(unsigned i=0;i<3;++i){auto bit=std::find(inputs.begin(),inputs.begin()+3,target->getPort(pins[i]))-inputs.begin();ASSERT_LT(bit,3);
            address|=unsigned(!bool((raw>>bit)&1))<<physical_address_bit(ctx.get(),target,pins[i]);}
        EXPECT_EQ((uint64_t(original.params.at(id_LUT).as_int64())>>oldrow)&1,
            1^((emitted_mask(ctx.get(),*now,target)>>emitted_address(ctx.get(),target,address))&1));
    }
    for(auto *f:{local_ff,mate_ff})for(auto p:{id_DATAIN,id_CLK,id_ENA,id_ACLR})if(auto *net=f->getPort(p))connected(net);
    ctx->check();
}
TEST_P(SourceExitPermutationBackend, EveryFaultFalseThrowAndSecondNetFailureRestoreFullState)
{
    const auto exit=make_retired_stub();Snapshot before(ctx.get());std::vector<unsigned> stages;
    EXPECT_FALSE(trial([]{return false;},[&](unsigned s){stages.push_back(s);if(s>=3)check_exit(exit_driver,exit);}));
    before.expect_exact(ctx.get());ASSERT_FALSE(stages.empty());ASSERT_EQ(stages.back(),21u);
    EXPECT_THROW(trial([]()->bool{throw std::runtime_error("accept");}),std::runtime_error);before.expect_exact(ctx.get());
    for(auto point:stages){SCOPED_TRACE(point);EXPECT_THROW(trial([]{return true;},[&](unsigned s){if(s==point)throw std::runtime_error("stage");}),std::runtime_error);before.expect_exact(ctx.get());}
    bool called=false;EXPECT_FALSE(trial([&]{called=true;return true;},{},1));EXPECT_FALSE(called);before.expect_exact(ctx.get());
    std::map<NetInfo*,std::set<WireId>> original;for(auto *net:incident())original[net]=sinks(net);
    std::vector<NetInfo*> changed;std::vector<unsigned> failed;
    EXPECT_FALSE(trial([&]{called=true;return true;},[&](unsigned s){
        failed.push_back(s);
        if(s==2){for(auto *net:incident())if(sinks(net)!=original.at(net))changed.push_back(net);
            std::sort(changed.begin(),changed.end(),[&](auto*a,auto*b){return a->name.str(ctx.get())<b->name.str(ctx.get());});}
        if(s==10){ASSERT_GE(changed.size(),2u);connected(changed[0]);bool injected=false;
            for(const auto &u:changed[1]->users)if(u.cell==target){target->pin_data.at(u.port).bel_pins={id_COMBOUT};injected=true;}
            ASSERT_TRUE(injected);
        }
    }));
    EXPECT_FALSE(called);ASSERT_FALSE(failed.empty());EXPECT_EQ(failed.back(),10u);before.expect_exact(ctx.get());
}
TEST_P(SourceExitPermutationBackend, MalformedSelectorDriverAndProtectedStrengthRejectBeforeMutation)
{
    const auto exit=make_retired_stub();const auto &where=ctx->bel_data(exit_driver->bel).lab_data;
    const auto block=ctx->labs[where.lab].is_mlab?CV::MLAB:CV::LAB;
    auto reject=[&](){Snapshot before(ctx.get());bool touched=false,called=false;
        EXPECT_FALSE(trial([&]{called=true;return true;},[&](unsigned){touched=true;}));
        EXPECT_FALSE(touched);EXPECT_FALSE(called);before.expect_exact(ctx.get());};
    // Native-state corruption is intentional. Preflight must not normalize it
    // with configure_bitstream: the arbitrary original RBF bytes stay exact.
    ASSERT_TRUE(ctx->cyclonev->bmux_m_set(block,exit_driver->bel.pos,exit.mux,where.alm,CV::REG));
    reject();EXPECT_EQ(reason,"original source-exit native selector is not NLUT");
    EXPECT_EQ(selector(*ctx->cyclonev,exit_driver,exit),uint32_t(CV::REG));ctx->configure_bitstream();
    auto port=exit_net->driver.port;exit_net->driver.port=id_CO;reject();exit_net->driver.port=port;
    auto strength=exit_net->wires.at(exit.wire).strength;exit_net->wires.at(exit.wire).strength=STRENGTH_LOCKED;
    reject();exit_net->wires.at(exit.wire).strength=strength;
    ctx->check();
}
INSTANTIATE_TEST_SUITE_P(LabAndMlabBothHalves,SourceExitPermutationBackend,::testing::Combine(::testing::Bool(),::testing::Values(0,1)));
} // namespace
