/* Independent transaction and physical proof for symmetric routed LUT6 swaps. SPDX-License-Identifier: ISC */
#include "analogue_repair.h"
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


class RoutedLut6SwapBackend : public ::testing::TestWithParam<std::tuple<bool,int>> {
  protected:
    std::unique_ptr<Context> ctx;
    uint32_t lab=0, support_lab=0;
    CellInfo *target=nullptr, *sink=nullptr, *cohabiting=nullptr;
    std::array<NetInfo *,6> inputs{};
    std::string reason;
    int half() const { return std::get<1>(GetParam()); }
    static uint64_t symmetric_mask()
    {
        uint64_t result=0;
        for(unsigned r=0;r<64;++r) {
            bool a=r&1,b=(r>>1)&1,c=(r>>2)&1,d=(r>>3)&1,e=(r>>4)&1,f=(r>>5)&1;
            if((b!=f)&&(a||c)&&(!d||e))result|=uint64_t(1)<<r;
        }
        return result;
    }
    CellInfo *lut(const std::string &name,int width,uint64_t mask,BelId bel)
    {
        auto *cell=ctx->createCell(ctx->id(name),width==6?id_MISTRAL_ALUT6:id_MISTRAL_ALUT2);
        cell->params[id_LUT]=Property(int64_t(mask),1u<<width);
        for(int i=0;i<width;++i){cell->addInput(pins[i]);cell->pin_data[pins[i]].state=PIN_0;}
        cell->addOutput(id_Q);cell->connectPort(id_Q,ctx->createNet(ctx->id(name+"$q")));
        ctx->assign_comb_info(cell);ctx->assign_default_pinmap(cell);ctx->bindBel(bel,cell,STRENGTH_WEAK);return cell;
    }
    CellInfo *support(const std::string &name,int alm)
    { return lut(name,2,8,ctx->labs[support_lab].alms[alm].lut_bels[0]); }
    void build(int ff_mode=0)
    {
        ArchArgs args;args.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")]=1e8;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        std::vector<std::pair<int,uint32_t>> choices;
        for(uint32_t i=0;i<ctx->labs.size();++i)if(ctx->labs[i].is_mlab==std::get<0>(GetParam())){
            auto l=ctx->getBelLocation(ctx->labs[i].alms[0].lut_bels[0]);choices.emplace_back(std::abs(l.x-24)+std::abs(l.y-3),i);
        }
        std::sort(choices.begin(),choices.end());ASSERT_GE(choices.size(),2u);lab=choices[0].second;support_lab=choices[1].second;
        target=lut("target",6,symmetric_mask(),ctx->labs[lab].alms[0].lut_bels[half()]);
        for(unsigned i=0;i<6;++i){auto *d=lut("source"+std::to_string(i),2,8,ctx->labs[lab].alms[i+1].lut_bels[0]);inputs[i]=d->getPort(id_Q);target->connectPort(pins[i],inputs[i]);target->pin_data[pins[i]].state=PIN_SIG;}
        sink=lut("sink",2,8,ctx->labs[lab].alms[7].lut_bels[0]);sink->connectPort(id_A,target->getPort(id_Q));sink->pin_data[id_A].state=PIN_SIG;
        auto *outside=lut("outside_source",2,8,ctx->labs[lab].alms[8].lut_bels[0]);
        auto *outside_sink=lut("outside_sink",2,8,ctx->labs[lab].alms[9].lut_bels[0]);outside_sink->connectPort(id_A,outside->getPort(id_Q));outside_sink->pin_data[id_A].state=PIN_SIG;
        cohabiting=nullptr;
        if(ff_mode) {
            auto *clk=support("ff_clock",0)->getPort(id_Q),*enable=support("ff_enable",1)->getPort(id_Q),*clear=support("ff_clear",2)->getPort(id_Q),*data=support("ff_data",3)->getPort(id_Q);
            clk->clkconstr=std::make_unique<ClockConstraint>();clk->clkconstr->period=DelayPair(10000);clk->clkconstr->high=clk->clkconstr->low=DelayPair(5000);
            cohabiting=ctx->createCell(ctx->id("cohabiting_register"),id_MISTRAL_FF);
            for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})cohabiting->addInput(p);
            cohabiting->addOutput(id_Q);cohabiting->connectPort(id_Q,ctx->createNet(ctx->id("cohabiting_register$q")));
            cohabiting->connectPort(id_CLK,clk);cohabiting->connectPort(id_ENA,enable);cohabiting->connectPort(id_ACLR,clear);
            cohabiting->pin_data[id_ENA].state=PIN_INV;cohabiting->pin_data[id_ACLR].state=PIN_INV;cohabiting->pin_data[id_SCLR].state=PIN_0;cohabiting->pin_data[id_SLOAD].state=PIN_0;
            cohabiting->connectPort(id_DATAIN,ff_mode==1?data:target->getPort(id_Q));
            if(ff_mode==2){cohabiting->connectPort(id_SDATA,data);cohabiting->connectPort(id_SLOAD,enable);cohabiting->pin_data[id_SLOAD].state=PIN_SIG;}
            ctx->assign_ff_info(cohabiting);ctx->assign_default_pinmap(cohabiting);ctx->bindBel(ctx->labs[lab].alms[0].ff_bels[2*half()],cohabiting,STRENGTH_WEAK);
        }
        const auto alias=ctx->id("input_observer_alias");ctx->net_aliases[alias]=inputs[1]->name;inputs[1]->aliases.push_back(alias);
        for(auto *net:inputs){auto a=net->users.add(PortRef{}),b=net->users.add(PortRef{}),c=net->users.add(PortRef{});net->users.remove(b);net->users.remove(a);net->users.remove(c);}
        ctx->assignArchInfo();ctx->lab_pre_route();
        for(const auto &c:ctx->cells)ASSERT_TRUE(ctx->isBelLocationValid(c.second->bel))<<ctx->nameOf(c.second.get());
        int visits=3000000;
        // Route controls first, reserving the real FF selector path before the
        // two data nets can ever be torn up by the trial.
        if(cohabiting) {
            for(auto p:{id_CLK,id_ENA,id_ACLR}) {
                if(auto *n=cohabiting->getPort(p)) { ASSERT_TRUE(critical_local_route_net(ctx.get(),n,visits))<<ctx->nameOf(p); }
            }
        }
        for(auto *net:inputs) { ASSERT_TRUE(critical_local_route_net(ctx.get(),net,visits)); }
        if(cohabiting) {
            for(auto p:{id_SDATA,id_DATAIN}) {
                if(auto *n=cohabiting->getPort(p)) { ASSERT_TRUE(critical_local_route_net(ctx.get(),n,visits))<<ctx->nameOf(p); }
            }
        }
        for(const auto &entry:ctx->nets) {
            if(entry.second->driver.cell) { ASSERT_TRUE(critical_local_route_net(ctx.get(),entry.second.get(),visits))<<ctx->nameOf(entry.second.get()); }
        }
        ctx->attrs[id_step]=std::string("route");ctx->configure_bitstream();ctx->check();
        ASSERT_TRUE(ctx->analogue_cache_valid);ASSERT_TRUE(ctx->labs[lab].alms[0].l6_mode);
    }
    void SetUp() override { build(); }
    void cohabiting_proof(int mode);
    bool trial(const std::function<bool()> &accept=[]{return true;},const std::function<void(unsigned)> &stage={},int budget=500000)
    { return critical_routed_lut6_swap_trial(ctx.get(),target,id_B,id_F,budget,accept,&reason,stage); }
    void connected(NetInfo *net)
    {
        auto root=ctx->getNetinfoSourceWire(net);ASSERT_NE(root,WireId());ASSERT_TRUE(net->wires.count(root));EXPECT_EQ(net->wires.at(root).pip,PipId());
        for(const auto &user:net->users)for(auto wire:ctx->getNetinfoSinkWires(net,user)){
            std::set<WireId> seen;
            while(wire!=root){ASSERT_TRUE(seen.insert(wire).second);ASSERT_TRUE(net->wires.count(wire));auto pip=net->wires.at(wire).pip;ASSERT_NE(pip,PipId());EXPECT_EQ(ctx->getPipDstWire(pip),wire);wire=ctx->getPipSrcWire(pip);}
        }
    }
    Snapshot after_swap_expected(const Snapshot &before)
    {
        Snapshot expected=before;auto &c=expected.cells.at(target->name);std::swap(c.ports.at(id_B).net,c.ports.at(id_F).net);std::swap(c.ports.at(id_B).user_idx,c.ports.at(id_F).user_idx);
        std::swap(c.architecture.combInfo.lut_in[1],c.architecture.combInfo.lut_in[5]);
        auto &a=expected.nets.at(inputs[1]->name).users;auto &b=expected.nets.at(inputs[5]->name).users;
        a.at(before.cells.at(target->name).ports.at(id_B).user_idx)={target,id_F};b.at(before.cells.at(target->name).ports.at(id_F).user_idx)={target,id_B};return expected;
    }
};

TEST_P(RoutedLut6SwapBackend, AcceptedSymmetricSwapPreservesAllOutsideIdentitiesSlotsAndRouting)
{
    Snapshot before(ctx.get());std::vector<unsigned> stages;
    ASSERT_TRUE(trial([]{return true;},[&](unsigned n){stages.push_back(n);}))<<reason;
    EXPECT_EQ(stages,(std::vector<unsigned>{1,2,3,4,5}));
    auto expected=after_swap_expected(before);expected.expect_exact(ctx.get(),{inputs[1],inputs[5]});
    EXPECT_EQ(target->getPort(id_B),inputs[5]);EXPECT_EQ(target->getPort(id_F),inputs[1]);
    connected(inputs[1]);connected(inputs[5]);ctx->check();
    const uint64_t mask=uint64_t(target->params.at(id_LUT).as_int64());
    for(unsigned r=0;r<64;++r){unsigned swapped=(r&~34u)|((r&2u)<<4)|((r&32u)>>4);EXPECT_EQ((mask>>r)&1,(mask>>swapped)&1)<<r;}
}
TEST_P(RoutedLut6SwapBackend, AtomicTwoNetRollbackCoversRejectionThrowAndEveryFaultStage)
{
    Snapshot before(ctx.get());bool inspected=false;
    EXPECT_FALSE(trial([&]{inspected=true;return false;}));EXPECT_TRUE(inspected);before.expect_exact(ctx.get());
    EXPECT_THROW(trial([]()->bool{throw std::runtime_error("accept exception");}),std::runtime_error);before.expect_exact(ctx.get());
    for(unsigned stage=1;stage<=5;++stage){EXPECT_THROW(trial([]{return true;},[&](unsigned n){if(n==stage)throw std::runtime_error("injected stage");}),std::runtime_error);before.expect_exact(ctx.get());}
    inspected=false;EXPECT_FALSE(trial([&]{inspected=true;return true;},{},0));EXPECT_FALSE(inspected);before.expect_exact(ctx.get());
}
TEST_P(RoutedLut6SwapBackend, FailureOfSecondRouteRestoresFirstRouteLogicalPinsAndCache)
{
    Snapshot before(ctx.get());std::vector<unsigned> stages;bool accepted=false;
    EXPECT_FALSE(trial([&]{accepted=true;return true;},[&](unsigned n){
        stages.push_back(n);
        if(n==3){connected(inputs[1]);ASSERT_FALSE(inputs[1]->wires.empty());
            // Both original source roots are reserved before either reroute.
            // The first net is complete; the second still has only its root.
            ASSERT_EQ(inputs[5]->wires.size(),1u);
            auto root=ctx->getNetinfoSourceWire(inputs[5]);ASSERT_TRUE(inputs[5]->wires.count(root));
            ASSERT_EQ(inputs[5]->wires.at(root).pip,PipId());
            // The second route's sink is made physically unavailable using a
            // snapshotted map field. No foreign binding obstructs restoration.
            target->pin_data[id_B].bel_pins={id_COMBOUT};
        }
    }));
    EXPECT_FALSE(accepted);EXPECT_EQ(stages,(std::vector<unsigned>{1,2,3}));before.expect_exact(ctx.get());
}
TEST_P(RoutedLut6SwapBackend, NonSymmetricProtectedAliasedAndPartialNativeStatesRejectBeforeMutation)
{
    auto reject=[&](IdString a=id_B,IdString b=id_F){Snapshot s(ctx.get());bool touched=false;EXPECT_FALSE(critical_routed_lut6_swap_trial(ctx.get(),target,a,b,500000,[]{return true;},&reason,[&](unsigned){touched=true;}))<<reason;EXPECT_FALSE(touched);s.expect_exact(ctx.get());};
    reject(id_A,id_B);reject(id_B,id_B);reject(id_Q,id_F);
    for(const char *key:{"keep","dont_touch","BEL","FES_SLOT"}){target->attrs[ctx->id(key)]=1;reject();target->attrs.erase(ctx->id(key));}
    target->pin_data[id_B].state=PIN_INV;reject();target->pin_data[id_B].state=PIN_SIG;
    auto *old=target->ports.at(id_B).net;target->ports.at(id_B).net=inputs[5];reject();target->ports.at(id_B).net=old;
    target->constr_x=1;reject();target->constr_x=0;
    auto strength=target->belStrength;target->belStrength=STRENGTH_STRONG;reject();target->belStrength=strength;
    auto user=inputs[1]->users.add(PortRef{sink,id_B});reject();inputs[1]->users.remove(user);
    auto *driver=inputs[1]->driver.cell;driver->attrs[ctx->id("keep")]=1;reject();driver->attrs.erase(ctx->id("keep"));
    auto name=ctx->id("public_input");ctx->ports[name]=PortInfo{name,inputs[1],PORT_OUT,{}};reject();ctx->ports.erase(name);
    auto mask=target->params.at(id_LUT);target->params[id_LUT]=Property(int64_t(0x92ad6c1f58e3b407ULL),64);reject();target->params[id_LUT]=mask;
}

const std::array<unsigned,64> mlab_order{
    0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,
    2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18,
    32,33,36,37,40,41,44,45,61,60,57,56,53,52,49,48,
    34,35,38,39,42,43,46,47,63,62,59,58,55,54,51,50};
std::unique_ptr<CV> decode(Context *ctx)
{
    std::vector<uint8_t> bytes;ctx->cyclonev->rbf_save(bytes);
    auto model=std::unique_ptr<CV>(CV::get_model("5CSEBA6U23I7"));
    if(!model||bytes.empty())throw std::runtime_error("empty native RBF");
    model->rbf_load(bytes.data(),uint32_t(bytes.size()));return model;
}
CV::bmux_setting_t setting(Context *ctx,const CV &model,CellInfo *cell,CV::bmux_type_t mux,int index)
{
    CV::bmux_setting_t s;const auto &loc=ctx->bel_data(cell->bel).lab_data;
    if(!model.bmux_get(ctx->labs[loc.lab].is_mlab?CV::MLAB:CV::LAB,cell->bel.pos,mux,index,s))throw std::runtime_error("missing native mux");
    return s;
}
uint64_t mask(Context *ctx,const CV &model,CellInfo *cell)
{
    auto s=setting(ctx,model,cell,CV::LUT_MASK,ctx->bel_data(cell->bel).lab_data.alm);
    if(s.type!=CV::MT_RAM||s.r.size()!=8)throw std::runtime_error("missing 64-bit mask");
    uint64_t result=0;for(unsigned i=0;i<8;++i)result|=uint64_t(s.r[i])<<(8*i);return result;
}
void physical_truth(Context *ctx,const CV &model,CellInfo *cell,const std::array<NetInfo *,6> &original_inputs,uint64_t original_mask)
{
    const auto &loc=ctx->bel_data(cell->bel).lab_data;
    EXPECT_EQ(setting(ctx,model,cell,CV::TMODE,loc.alm).s,uint32_t(CV::D_E));
    EXPECT_EQ(setting(ctx,model,cell,CV::BMODE,loc.alm).s,uint32_t(CV::C_E));
    const auto bits=mask(ctx,model,cell);
    const std::array<IdString,6> physical{id_A,id_B,id_C,id_D,loc.idx?id_E1:id_E0,loc.idx?id_F1:id_F0};
    for(unsigned i=0;i<6;++i){EXPECT_EQ(cell->pin_data.at(pins[i]).state,PIN_SIG);EXPECT_EQ(cell->pin_data.at(pins[i]).bel_pins,(std::vector<IdString>{physical[i]}));}
    // Independent physical oracle. E selects the two banks and exchanges
    // C/D address significance; F is bit4. MLAB has a separate CRAM order.
    for(unsigned row=0;row<64;++row){std::array<unsigned,6> v{};
        for(unsigned i=0;i<6;++i){auto n=cell->getPort(pins[i]);auto it=std::find(original_inputs.begin(),original_inputs.end(),n);ASSERT_NE(it,original_inputs.end());v[i]=1^((row>>size_t(it-original_inputs.begin()))&1);}
        unsigned address=v[0]|(v[1]<<1)|(v[2]<<(v[4]?3:2))|(v[3]<<(v[4]?2:3))|(v[4]<<5)|(v[5]<<4);
        if(ctx->labs[loc.lab].is_mlab)address=mlab_order[address];
        EXPECT_EQ(1^((bits>>address)&1),(original_mask>>row)&1)<<row;
    }
}
TEST_P(RoutedLut6SwapBackend, EmittedRbfPreservesLut6BanksAndAll64NetworkRows)
{
    auto before=decode(ctx.get());physical_truth(ctx.get(),*before,target,inputs,symmetric_mask());const auto old_mask=mask(ctx.get(),*before,target);
    ASSERT_TRUE(trial())<<reason;auto after=decode(ctx.get());EXPECT_EQ(mask(ctx.get(),*after,target),old_mask);
    physical_truth(ctx.get(),*after,target,inputs,symmetric_mask());connected(inputs[1]);connected(inputs[5]);
}

void RoutedLut6SwapBackend::cohabiting_proof(int mode)
{
    build(mode);ASSERT_NE(cohabiting,nullptr);const auto &alm=ctx->labs[lab].alms[0];
    auto *ff=cohabiting;const int slot=2*half();
    const auto foreign_f=ctx->getBelPinWire(target->bel,half()?id_F0:id_F1);
    ASSERT_TRUE(ctx->wires_connected(foreign_f,alm.sel_ef[half()]));
    ASSERT_EQ(ctx->wires_connected(alm.sel_ef[half()],alm.ff_in[slot]),mode==1);
    ASSERT_EQ(ctx->wires_connected(alm.comb_out[half()],alm.ff_in[slot]),mode==2);
    for(auto p:{id_CLK,id_ENA,id_ACLR,id_DATAIN,id_SDATA,id_SLOAD})if(auto *net=ff->getPort(p))connected(net);
    const std::vector<CV::bmux_type_t> alm_muxes{
        CV::TMODE,CV::BMODE,CV::TEF_SEL,CV::BEF_SEL,CV::TPKREG0,CV::TPKREG1,CV::BPKREG0,CV::BPKREG1,
        CV::TCLK_SEL,CV::BCLK_SEL,CV::TCLR_SEL,CV::BCLR_SEL,CV::TSCLR_DIS,CV::BSCLR_DIS,CV::TSLOAD_EN,CV::BSLOAD_EN};
    const std::vector<CV::bmux_type_t> lab_muxes{
        CV::CLK0_INV,CV::CLK1_INV,CV::CLK2_INV,CV::EN0_EN,CV::EN1_EN,CV::EN2_EN,
        CV::EN0_NINV,CV::EN1_NINV,CV::EN2_NINV,CV::ACLR0_INV,CV::ACLR1_INV,CV::SLOAD_INV,CV::SCLR_INV};
    auto controls=[&](const CV &model){std::map<std::pair<int,int>,std::pair<int,uint32_t>> result;
        for(auto mux:alm_muxes){auto s=setting(ctx.get(),model,target,mux,0);result[{int(mux),0}]={s.type,s.s};}
        for(auto mux:lab_muxes){auto s=setting(ctx.get(),model,target,mux,0);result[{int(mux),0}]={s.type,s.s};}
        return result;
    };
    auto before_rbf=decode(ctx.get());const auto original_controls=controls(*before_rbf);physical_truth(ctx.get(),*before_rbf,target,inputs,symmetric_mask());
    EXPECT_EQ(setting(ctx.get(),*before_rbf,target,half()?CV::BEF_SEL:CV::TEF_SEL,0).s,uint32_t(CV::F));
    EXPECT_EQ(setting(ctx.get(),*before_rbf,target,half()?CV::BPKREG1:CV::TPKREG1,0).s,uint32_t(mode==1));
    EXPECT_EQ(setting(ctx.get(),*before_rbf,target,half()?CV::BSLOAD_EN:CV::TSLOAD_EN,0).s,uint32_t(mode==2));
    const auto group=ALMInfo::ctrl_group(slot),ce=alm.clk_ena_idx[group],aclr=alm.aclr_idx[group];
    const std::array<CV::bmux_type_t,3> en{CV::EN0_EN,CV::EN1_EN,CV::EN2_EN},ninv{CV::EN0_NINV,CV::EN1_NINV,CV::EN2_NINV};
    const std::array<CV::bmux_type_t,2> inv{CV::ACLR0_INV,CV::ACLR1_INV};
    EXPECT_EQ(setting(ctx.get(),*before_rbf,target,en.at(ce),0).s,1u);EXPECT_EQ(setting(ctx.get(),*before_rbf,target,ninv.at(ce),0).s,1u);EXPECT_EQ(setting(ctx.get(),*before_rbf,target,inv.at(aclr),0).s,1u);
    Snapshot before(ctx.get());
    EXPECT_FALSE(trial([]{return false;}));before.expect_exact(ctx.get());
    for(unsigned stage=1;stage<=5;++stage){EXPECT_THROW(trial([]{return true;},[&](unsigned n){if(n==stage)throw std::runtime_error("FF selector rollback");}),std::runtime_error);before.expect_exact(ctx.get());}
    ASSERT_TRUE(trial())<<reason;auto expected=after_swap_expected(before);expected.expect_exact(ctx.get(),{inputs[1],inputs[5]});
    auto after_rbf=decode(ctx.get());EXPECT_EQ(controls(*after_rbf),original_controls);EXPECT_EQ(mask(ctx.get(),*after_rbf,target),mask(ctx.get(),*before_rbf,target));
    physical_truth(ctx.get(),*after_rbf,target,inputs,symmetric_mask());
    EXPECT_TRUE(ctx->wires_connected(foreign_f,ctx->labs[lab].alms[0].sel_ef[half()]));
    EXPECT_EQ(ctx->wires_connected(ctx->labs[lab].alms[0].sel_ef[half()],ctx->labs[lab].alms[0].ff_in[slot]),mode==1);
    EXPECT_EQ(ctx->wires_connected(ctx->labs[lab].alms[0].comb_out[half()],ctx->labs[lab].alms[0].ff_in[slot]),mode==2);
    for(auto p:{id_CLK,id_ENA,id_ACLR,id_DATAIN,id_SDATA,id_SLOAD})if(auto *net=ff->getPort(p))connected(net);
}
TEST_P(RoutedLut6SwapBackend, CohabitingDirectDataEnableClearSelectorsAndRbfAreUnchanged)
{ cohabiting_proof(1); }
TEST_P(RoutedLut6SwapBackend, CohabitingSynchronousLoadEnableClearSelectorsAndRbfAreUnchanged)
{ cohabiting_proof(2); }

TEST(RoutedLut6SwapCoverage, CompleteCarryAndBufferConeKeepsAllCapturesAndRejectsHiddenBranches)
{
    ArchArgs args;args.device="5CSEBA6U23I7";Context ctx(args);ctx.settings[ctx.id("target_freq")]=1e8;
    auto *gnd=ctx.createNet(ctx.id("$PACKER_GND_NET")),*vcc=ctx.createNet(ctx.id("$PACKER_VCC_NET"));(void)gnd;
    auto clock=[&](const char *name,int shift){auto *n=ctx.createNet(ctx.id(name));n->clkconstr=std::make_unique<ClockConstraint>();n->clkconstr->period=DelayPair(10000);n->clkconstr->high=n->clkconstr->low=DelayPair(5000);n->clkconstr->phase_group=ctx.id("family");n->clkconstr->phase_shift=shift;return n;};
    auto *clk=clock("system_clock",0),*phase=clock("memory_clock",2500);
    auto ff=[&](const char *name,NetInfo *data){auto *c=ctx.createCell(ctx.id(name),id_MISTRAL_FF);for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})c->addInput(p);c->addOutput(id_Q);c->connectPort(id_Q,ctx.createNet(ctx.id(std::string(name)+"$q")));c->connectPort(id_DATAIN,data?data:c->getPort(id_Q));c->connectPort(id_CLK,clk);c->pin_data[id_ENA].state=PIN_1;c->pin_data[id_ACLR].state=PIN_1;c->pin_data[id_SCLR].state=PIN_0;c->pin_data[id_SLOAD].state=PIN_0;return c;};
    auto *source=ff("source",nullptr);
    auto *lut=ctx.createCell(ctx.id("six_input_root"),id_MISTRAL_ALUT6);lut->params[id_LUT]=Property(int64_t(0xfffffffffffffffeULL),64);
    for(auto p:pins){lut->addInput(p);lut->connectPort(p,source->getPort(id_Q));}lut->addOutput(id_Q);lut->connectPort(id_Q,ctx.createNet(ctx.id("six_input_root$q")));
    auto *buffer=ctx.createCell(ctx.id("route_buffer"),id_MISTRAL_BUF);buffer->addInput(id_A);buffer->connectPort(id_A,lut->getPort(id_Q));buffer->addOutput(id_Q);buffer->connectPort(id_Q,ctx.createNet(ctx.id("route_buffer$q")));
    auto *carry=ctx.createCell(ctx.id("native_carry"),id_MISTRAL_ALUT_ARITH);carry->params[id_LUT0]=Property(0x6996,16);carry->params[id_LUT1]=Property(0x9669,16);
    for(auto p:{id_A,id_B,id_C,id_D0,id_D1,id_CI}){carry->addInput(p);if(p==id_A)carry->connectPort(p,buffer->getPort(id_Q));else carry->pin_data[p].state=PIN_0;}
    for(auto p:{id_SO,id_CO}){carry->addOutput(p);carry->connectPort(p,ctx.createNet(ctx.id(p==id_SO?"sum":"carry")));}
    auto *sum_capture=ff("sum_capture",carry->getPort(id_SO));
    auto *ram=ctx.createCell(ctx.id("address_capture"),id_MISTRAL_M10K);ram->params[id_CFG_ABITS]=12;ram->params[id_CFG_DBITS]=1;ram->params[id_CFG_DUAL_CLOCK]=1;
    for(auto p:{id_CLK1,id_CLK2,id_B1EN,id_ACLR0,id_ACLR1,ctx.id("B1ADDR[0]")})ram->addInput(p);
    ram->connectPort(id_CLK1,clk);ram->connectPort(id_CLK2,phase);ram->connectPort(id_B1EN,vcc);ram->pin_data[id_B1EN].state=PIN_1;ram->pin_data[id_ACLR0].state=PIN_0;ram->pin_data[id_ACLR1].state=PIN_0;ram->connectPort(ctx.id("B1ADDR[0]"),carry->getPort(id_CO));
    std::string reason;
    auto check=[&](){ctx.assignArchInfo();TimingAnalyser t(&ctx);t.setup(false,false,true);return critical_lut6_swap_output_coverage(&ctx,t,lut->getPort(id_Q),&reason);};
    ASSERT_TRUE(check())<<reason;
    // The historical native class approximates ACLR as a register input.
    // This repair must still reject a parallel asynchronous reset observer.
    sum_capture->connectPort(id_ACLR,lut->getPort(id_Q));sum_capture->pin_data[id_ACLR].state=PIN_SIG;
    ctx.assignArchInfo();int clear_clocks=0;ASSERT_EQ(ctx.getPortTimingClass(sum_capture,id_ACLR,clear_clocks),TMG_REGISTER_INPUT);
    EXPECT_FALSE(check());
    sum_capture->disconnectPort(id_ACLR);sum_capture->pin_data[id_ACLR].state=PIN_1;ASSERT_TRUE(check())<<reason;
    ctx.assignArchInfo();TimingAnalyser t(&ctx);t.setup(false,false,true);
    TimingAnalyser setup_only(&ctx);setup_only.setup_only=true;setup_only.setup();EXPECT_FALSE(critical_lut6_swap_output_coverage(&ctx,setup_only,lut->getPort(id_Q),&reason));
    auto *hidden=ctx.createCell(ctx.id("hidden_unsupported"),id_MISTRAL_OB);hidden->addInput(id_I);hidden->connectPort(id_I,lut->getPort(id_Q));
    EXPECT_FALSE(critical_lut6_swap_output_coverage(&ctx,t,lut->getPort(id_Q),&reason));hidden->disconnectPort(id_I);auto hidden_name=hidden->name;ctx.cells.erase(hidden_name);
    auto public_name=ctx.id("public_observer");ctx.ports[public_name]=PortInfo{public_name,lut->getPort(id_Q),PORT_OUT,{}};EXPECT_FALSE(critical_lut6_swap_output_coverage(&ctx,t,lut->getPort(id_Q),&reason));ctx.ports.erase(public_name);
    Context::SdcClockException cut;cut.from={"system_clock"};cut.to={"memory_clock"};ctx.sdc_clock_exceptions.push_back(cut);EXPECT_FALSE(check());ctx.sdc_clock_exceptions.clear();ASSERT_TRUE(check())<<reason;
    auto *hybrid=ctx.createCell(ctx.id("hidden_hybrid"),id_MISTRAL_MUL9X9);auto a=ctx.id("A[0]"),y=ctx.id("Y[0]");hybrid->addInput(a);hybrid->connectPort(a,lut->getPort(id_Q));hybrid->addInput(id_CLK);hybrid->connectPort(id_CLK,clk);hybrid->params[ctx.id("AREG_CTRL")]=1;hybrid->addOutput(y);hybrid->connectPort(y,ctx.createNet(ctx.id("hybrid$q")));
    EXPECT_FALSE(critical_lut6_swap_output_coverage(&ctx,t,lut->getPort(id_Q),&reason));hybrid->disconnectPort(a);hybrid->disconnectPort(id_CLK);hybrid->disconnectPort(y);auto hybrid_name=hybrid->name;ctx.cells.erase(hybrid_name);
    auto *net=lut->getPort(id_Q);
    for(unsigned i=0;i<70;++i){auto *c=ctx.createCell(ctx.idf("deep%u",i),id_MISTRAL_BUF);c->addInput(id_A);c->connectPort(id_A,net);c->addOutput(id_Q);c->connectPort(id_Q,ctx.createNet(ctx.idf("deep%u$q",i)));net=c->getPort(id_Q);}
    EXPECT_FALSE(critical_lut6_swap_output_coverage(&ctx,t,lut->getPort(id_Q),&reason));
    auto *cycle=ctx.createCell(ctx.id("cycle"),id_MISTRAL_BUF);cycle->addInput(id_A);cycle->addOutput(id_Q);auto *q=ctx.createNet(ctx.id("cycle$q"));cycle->connectPort(id_A,q);cycle->connectPort(id_Q,q);EXPECT_FALSE(critical_lut6_swap_output_coverage(&ctx,t,q,&reason));
}
INSTANTIATE_TEST_SUITE_P(LabAndMlabBothHalves,RoutedLut6SwapBackend,::testing::Combine(::testing::Bool(),::testing::Values(0,1)));
} // namespace
