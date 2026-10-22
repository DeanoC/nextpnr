/* Independent routed private-pair transaction and physical proof. SPDX-License-Identifier: ISC */
#include "analogue_repair.h"
#include "private_pair_decomposition_native.h"
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
namespace policy = private_pair_decomposition;
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


class RoutedPrivatePairBackend : public ::testing::TestWithParam<std::tuple<bool,int>> {
  protected:
    std::unique_ptr<Context> ctx;
    uint32_t producer_lab=0,source_lab=0,consumer_lab=0;
    CellInfo *producer=nullptr,*consumer=nullptr,*producer_mate=nullptr,*consumer_mate=nullptr;
    CellInfo *producer_ff=nullptr,*consumer_ff=nullptr;
    std::vector<NetInfo *> inputs;
    std::vector<CellInfo *> stationary;
    std::string reason;
    int half() const {return std::get<1>(GetParam());}
    static uint64_t parity(unsigned width) {
        uint64_t result=0;
        for(unsigned row=0;row<(1u<<width);++row) {unsigned v=row,n=0;while(v){n^=v&1u;v>>=1;}result|=uint64_t(n)<<row;}
        return result;
    }
    CellInfo *lut(const std::string &name,int width,uint64_t mask,const std::vector<NetInfo *> &nets,BelId bel) {
        const IdString types[]{id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5};
        auto *c=ctx->createCell(ctx->id(name),types[width-2]);c->params[id_LUT]=Property(int64_t(mask),1u<<width);
        for(int i=0;i<width;++i){c->addInput(pins[i]);if(i<int(nets.size())&&nets[i])c->connectPort(pins[i],nets[i]);else c->pin_data[pins[i]].state=PIN_0;}
        c->addOutput(id_Q);c->connectPort(id_Q,ctx->createNet(ctx->id(name+"$q")));
        ctx->assign_comb_info(c);ctx->assign_default_pinmap(c);ctx->bindBel(bel,c,STRENGTH_WEAK);return c;
    }
    CellInfo *ff(const char *name,CellInfo *local_lut,BelId bel) {
        auto *c=ctx->createCell(ctx->id(name),id_MISTRAL_FF);
        for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})c->addInput(p);
        c->addOutput(id_Q);c->connectPort(id_Q,ctx->createNet(ctx->id(std::string(name)+"$q")));
        c->connectPort(id_DATAIN,local_lut->getPort(id_Q));c->connectPort(id_CLK,inputs[9]);
        c->connectPort(id_ENA,inputs[7]);c->connectPort(id_ACLR,inputs[8]);
        c->pin_data[id_ENA].state=PIN_INV;c->pin_data[id_ACLR].state=PIN_INV;
        c->pin_data[id_SCLR].state=PIN_0;c->pin_data[id_SLOAD].state=PIN_0;
        ctx->assign_ff_info(c);ctx->assign_default_pinmap(c);ctx->bindBel(bel,c,STRENGTH_WEAK);return c;
    }
    void build(bool registers=false) {
        inputs.clear();stationary.clear();producer_ff=consumer_ff=nullptr;
        ArchArgs args;args.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")]=1e8;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        std::vector<std::pair<int,uint32_t>> places;
        for(uint32_t i=0;i<ctx->labs.size();++i)if(ctx->labs[i].is_mlab==std::get<0>(GetParam())) {
            auto p=ctx->getBelLocation(ctx->labs[i].alms[0].lut_bels[0]);places.emplace_back(std::abs(p.x-24)+std::abs(p.y-3),i);
        }
        std::sort(places.begin(),places.end());ASSERT_GE(places.size(),3u);
        producer_lab=places[0].second;source_lab=places[1].second;consumer_lab=places[2].second;
        for(int i=0;i<10;++i)inputs.push_back(lut("stimulus0"+std::to_string(i),2,8,{},ctx->labs[source_lab].alms[i].lut_bels[0])->getPort(id_Q));
        inputs[9]->clkconstr=std::make_unique<ClockConstraint>();inputs[9]->clkconstr->period=DelayPair(10000);inputs[9]->clkconstr->high=inputs[9]->clkconstr->low=DelayPair(5000);
        for(int i=0;i<7;++i) {
            auto *f=ctx->createCell(ctx->id("signal0"+std::to_string(i)),id_MISTRAL_FF);
            for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})f->addInput(p);
            f->pin_data[id_ENA].state=PIN_1;f->pin_data[id_ACLR].state=PIN_1;f->pin_data[id_SCLR].state=PIN_0;f->pin_data[id_SLOAD].state=PIN_0;
            f->connectPort(id_CLK,inputs[9]);f->connectPort(id_DATAIN,inputs[i]);f->addOutput(id_Q);
            auto *q=ctx->createNet(ctx->id("signal0"+std::to_string(i)+"$q"));f->connectPort(id_Q,q);inputs[i]=q;
            ctx->assign_ff_info(f);ctx->assign_default_pinmap(f);ctx->bindBel(ctx->labs[source_lab].alms[i].ff_bels[0],f,STRENGTH_WEAK);
        }
        producer=lut("producer",3,parity(3),{inputs[0],inputs[1],inputs[2]},ctx->labs[producer_lab].alms[0].lut_bels[half()]);
        consumer=lut("consumer",5,parity(5),{inputs[6],inputs[5],inputs[4],inputs[3],producer->getPort(id_Q)},ctx->labs[consumer_lab].alms[8].lut_bels[half()]);
        producer->pin_data[id_B].state=PIN_INV;consumer->pin_data[id_E].state=PIN_INV;
        producer_mate=lut("producer_mate",2,6,{inputs[7],inputs[8]},ctx->labs[producer_lab].alms[0].lut_bels[1-half()]);
        consumer_mate=lut("consumer_mate",2,9,{inputs[6],inputs[8]},ctx->labs[consumer_lab].alms[8].lut_bels[1-half()]);
        for(int i=0;i<6;++i)stationary.push_back(lut("stationary"+std::to_string(i),2,14,{inputs[0],inputs[8]},ctx->labs[consumer_lab].alms[i].lut_bels[0]));
        lut("outside",2,6,{inputs[7],inputs[8]},ctx->labs[consumer_lab].alms[6].lut_bels[0]);
        lut("capture",2,8,{consumer->getPort(id_Q),inputs[8]},ctx->labs[consumer_lab].alms[7].lut_bels[0]);
        if(registers) {
            // Producer Q must remain private to the consumer; use its stationary
            // mate's local COMBOUT for the cohabiting FF in that ALM.
            producer_ff=ff("producer_register",producer_mate,ctx->labs[producer_lab].alms[0].ff_bels[2*(1-half())]);
            consumer_ff=ff("consumer_register",consumer,ctx->labs[consumer_lab].alms[8].ff_bels[2*half()]);
        }
        const auto alias=ctx->id("shared_signal_alias");ctx->net_aliases[alias]=inputs[0]->name;inputs[0]->aliases.push_back(alias);
        for(auto *n:inputs){auto a=n->users.add(PortRef{}),b=n->users.add(PortRef{}),c=n->users.add(PortRef{});n->users.remove(b);n->users.remove(a);n->users.remove(c);}
        ctx->assignArchInfo();ctx->lab_pre_route();
        for(const auto &c:ctx->cells){ASSERT_TRUE(ctx->isBelLocationValid(c.second->bel))<<ctx->nameOf(c.second.get());}
        int visits=5000000;
        ASSERT_TRUE(critical_local_route_net(ctx.get(),inputs[9],visits));
        if(registers)for(auto *f:{producer_ff,consumer_ff})for(auto p:{id_CLK,id_ENA,id_ACLR}) {
            if(auto *n=f->getPort(p)){ASSERT_TRUE(critical_local_route_net(ctx.get(),n,visits))<<ctx->nameOf(p);}
        }
        for(const auto &entry:ctx->nets)if(entry.second->driver.cell) {
            ASSERT_TRUE(critical_local_route_net(ctx.get(),entry.second.get(),visits))<<ctx->nameOf(entry.second.get());
        }
        ctx->attrs[id_step]=std::string("route");ctx->configure_bitstream();ctx->check();ASSERT_TRUE(ctx->analogue_cache_valid);
        ASSERT_EQ(inputs[0]->users.entries(),7);
    }
    void SetUp() override {build();}
    policy::Pair original() const {
        policy::Pair p;
        for(auto *c:{producer,consumer}) {
            auto &l=c==producer?p.producer:p.consumer;l.mask=uint64_t(c->params.at(id_LUT).as_int64());
            for(int i=0;i<(c==producer?3:5);++i) {
                auto *n=c->getPort(pins[i]);int index=n==producer->getPort(id_Q)?policy::INTERNAL:int(std::find(inputs.begin(),inputs.begin()+7,n)-inputs.begin());
                l.pins.push_back({index,c->get_pin_state(pins[i])==PIN_INV});
            }
        }
        return p;
    }
    policy::Candidate candidate() const {
        auto all=policy::decompose(original());
        auto at=std::find_if(all.begin(),all.end(),[](const auto &v){return v.bound==std::array<int,3>{1,2,3};});
        if(at==all.end())throw std::runtime_error("fixture has no shared-net migration candidate");
        return *at;
    }
    bool trial(const std::function<bool()> &accept=[]{return true;},const std::function<void(unsigned)> &stage={},int visits=500000) {
        return critical_routed_private_pair_trial(ctx.get(),producer,candidate(),visits,accept,&reason,stage);
    }
    std::set<NetInfo *> incident() const {std::set<NetInfo *> r(inputs.begin(),inputs.begin()+7);r.insert(producer->getPort(id_Q));return r;}
    std::map<WireId,std::pair<PipId,PlaceStrength>> path(NetInfo *net,CellInfo *sink,IdString pin) {
        std::map<WireId,std::pair<PipId,PlaceStrength>> result;
        PortRef user{sink,pin};auto root=ctx->getNetinfoSourceWire(net);
        for(auto wire:ctx->getNetinfoSinkWires(net,user)) {
            std::set<WireId> seen;
            for(;;) {
                if(!seen.insert(wire).second||!net->wires.count(wire))throw std::runtime_error("missing/cyclic native path");
                const auto &entry=net->wires.at(wire);result[wire]={entry.pip,entry.strength};
                if(wire==root){if(entry.pip!=PipId())throw std::runtime_error("invalid root");break;}
                if(entry.pip==PipId()||ctx->getPipDstWire(entry.pip)!=wire)throw std::runtime_error("broken native path");
                wire=ctx->getPipSrcWire(entry.pip);
            }
        }
        if(result.empty())throw std::runtime_error("empty native path");
        return result;
    }
    void connected(NetInfo *net) {for(const auto &u:net->users){EXPECT_FALSE(path(net,u.cell,u.port).empty());}}
    Snapshot expected_after(const Snapshot &before,const policy::Candidate &chosen) {
        Snapshot expected=before;
        std::map<NetInfo *,decltype(PortInfo::user_idx)> slots;
        for(auto *cell:{producer,consumer})for(const auto &entry:before.cells.at(cell->name).ports)
            if(entry.second.type==PORT_IN&&entry.second.net)slots.emplace(entry.second.net,entry.second.user_idx);
        for(auto *cell:{producer,consumer}) {
            auto &old=expected.cells.at(cell->name);const auto &l=cell==producer?chosen.network.producer:chosen.network.consumer;
            old.params[id_LUT]=Property(int64_t(l.mask),1u<<l.pins.size());
            for(size_t i=0;i<l.pins.size();++i) {
                auto *n=l.pins[i].signal==policy::INTERNAL?producer->getPort(id_Q):inputs.at(l.pins[i].signal);
                old.ports.at(pins[i]).net=n;old.ports.at(pins[i]).user_idx=slots.at(n);
                old.architecture.combInfo.lut_in[i]=n;
                auto &state=old.architecture.pin_data[pins[i]];state.state=l.pins[i].inverted?PIN_INV:PIN_SIG;
                // The independent emitted-mask oracle below proves this native map.
                state.bel_pins=cell->pin_data.at(pins[i]).bel_pins;
                expected.nets.at(n->name).users.at(slots.at(n))={cell,pins[i]};
            }
        }
        return expected;
    }
};

TEST_P(RoutedPrivatePairBackend, SixStationaryBranchesKeepExactRoutesSlotsAndOutsideState)
{
    Snapshot before(ctx.get());auto chosen=candidate();std::vector<policy::Candidate> candidates;std::vector<NetInfo *> signals;
    ASSERT_TRUE(critical_routed_private_pair_candidates(ctx.get(),producer,candidates,signals,&reason))<<reason;
    EXPECT_EQ(signals,(std::vector<NetInfo *>(inputs.begin(),inputs.begin()+7)));ASSERT_FALSE(candidates.empty());
    std::vector<std::map<WireId,std::pair<PipId,PlaceStrength>>> paths;
    for(auto *sink:stationary)paths.push_back(path(inputs[0],sink,id_A));
    std::vector<unsigned> stages;ASSERT_TRUE(trial([]{return true;},[&](unsigned n){stages.push_back(n);}))<<reason;
    ASSERT_GE(stages.size(),6u);EXPECT_EQ(stages[0],1u);EXPECT_EQ(stages[1],2u);EXPECT_EQ(stages[2],3u);
    EXPECT_EQ(stages[stages.size()-2],20u);EXPECT_EQ(stages.back(),21u);
    EXPECT_EQ(consumer->getPort(id_A),inputs[0]);EXPECT_NE(producer->getPort(id_A),inputs[0]);
    for(size_t i=0;i<stationary.size();++i){EXPECT_EQ(path(inputs[0],stationary[i],id_A),paths[i]);}
    auto expected=expected_after(before,chosen);expected.expect_exact(ctx.get(),incident());
    for(auto *n:incident())connected(n);
    ctx->check();
}
TEST_P(RoutedPrivatePairBackend, RejectionAcceptThrowAndEveryObservedStageRestoreExactly)
{
    Snapshot before(ctx.get());std::vector<unsigned> stages;
    EXPECT_FALSE(trial([&]{return false;},[&](unsigned n){stages.push_back(n);}));before.expect_exact(ctx.get());
    ASSERT_FALSE(stages.empty());ASSERT_EQ(stages.back(),21u);
    EXPECT_THROW(trial([]()->bool{throw std::runtime_error("accept");}),std::runtime_error);before.expect_exact(ctx.get());
    for(unsigned stage:stages) {
        EXPECT_THROW(trial([]{return true;},[&](unsigned n){if(n==stage)throw std::runtime_error("stage");}),std::runtime_error);
        before.expect_exact(ctx.get());
    }
    EXPECT_FALSE(trial([]{return true;},{},0));before.expect_exact(ctx.get());
}
TEST_P(RoutedPrivatePairBackend, OriginalUnroutedGuardAndProtectedMalformedCandidatesFailClosed)
{
    Snapshot original_state(ctx.get());
    EXPECT_FALSE(private_pair_trial(ctx.get(),producer,candidate(),[]{return true;},&reason));original_state.expect_exact(ctx.get());
    auto reject=[&](){Snapshot s(ctx.get());bool touched=false;EXPECT_FALSE(trial([]{return true;},[&](unsigned){touched=true;}))<<reason;EXPECT_FALSE(touched);s.expect_exact(ctx.get());};
    for(auto *c:{producer,consumer,producer_mate,consumer_mate})for(const char *key:{"keep","dont_touch","BEL","FES_SLOT"}) {c->attrs[ctx->id(key)]=1;reject();c->attrs.erase(ctx->id(key));}
    auto bad=candidate();bad.network.consumer.mask^=1;
    EXPECT_FALSE(critical_routed_private_pair_trial(ctx.get(),producer,bad,500000,[]{return true;},&reason));original_state.expect_exact(ctx.get());
    auto step=ctx->attrs.at(id_step);ctx->attrs[id_step]=std::string("place");reject();ctx->attrs[id_step]=step;
    auto *middle=producer->getPort(id_Q);auto user=middle->users.add(PortRef{stationary[0],id_B});reject();middle->users.remove(user);
    auto public_name=ctx->id("public_shared_input");ctx->ports[public_name]=PortInfo{public_name,inputs[0],PORT_OUT,{}};reject();ctx->ports.erase(public_name);
    producer->constr_x=1;reject();producer->constr_x=0;
    auto *source=inputs[0]->driver.cell;source->attrs[ctx->id("keep")]=1;reject();source->attrs.erase(ctx->id("keep"));
    inputs[0]->attrs[ctx->id("dont_touch")]=1;reject();inputs[0]->attrs.erase(ctx->id("dont_touch"));
    auto strength=consumer_mate->belStrength;consumer_mate->belStrength=STRENGTH_STRONG;reject();consumer_mate->belStrength=strength;
    ctx->fes_fence_active=true;reject();ctx->fes_fence_active=false;
    ctx->fes_has_cram_region=true;reject();ctx->fes_has_cram_region=false;
}
TEST_P(RoutedPrivatePairBackend, SecondChangedNetFailureRestoresFirstAndAllSixStationaryBranches)
{
    Snapshot before(ctx.get());std::map<NetInfo *,std::set<WireId>> old_sinks;
    auto sinks=[&](NetInfo *n){std::set<WireId> r;for(const auto &u:n->users)for(auto w:ctx->getNetinfoSinkWires(n,u))r.insert(w);return r;};
    for(auto *n:incident())old_sinks[n]=sinks(n);
    std::vector<NetInfo *> changed;std::vector<unsigned> stages;bool accepted=false;
    EXPECT_FALSE(trial([&]{accepted=true;return true;},[&](unsigned stage){
        stages.push_back(stage);
        if(stage==2) {
            for(auto *n:incident())if(sinks(n)!=old_sinks.at(n))changed.push_back(n);
            std::sort(changed.begin(),changed.end(),[&](const auto *a,const auto *b){return a->name.str(ctx.get())<b->name.str(ctx.get());});
        }
        if(stage==10) {
            ASSERT_GE(changed.size(),2u);connected(changed[0]);
            bool fault=false;
            for(const auto &u:changed[1]->users)if(u.cell==producer||u.cell==consumer) {
                // An occupied output wire cannot become the second input sink.
                // This snapshotted map fault does not introduce a foreign tree.
                u.cell->pin_data.at(u.port).bel_pins={id_COMBOUT};fault=true;
            }
            ASSERT_TRUE(fault);
        }
    }));
    EXPECT_FALSE(accepted);ASSERT_FALSE(stages.empty());EXPECT_EQ(stages.back(),10u);before.expect_exact(ctx.get());
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
TEST_P(RoutedPrivatePairBackend, EmittedRbfProvesCompositionAndStationaryMasksForAll128Rows)
{
    auto before=decoded(ctx.get());const auto original_pair=original();
    auto half_mask=[&](CellInfo *c){uint64_t r=0;for(unsigned i=0;i<32;++i)r|=uint64_t(1)<<emitted_address(ctx.get(),c,i);return r;};
    const auto pm=emitted_mask(ctx.get(),*before,producer_mate),cm=emitted_mask(ctx.get(),*before,consumer_mate);
    for(auto *c:{producer,consumer,producer_mate,consumer_mate})check_emitted_truth(ctx.get(),*before,c);
    ASSERT_TRUE(trial())<<reason;auto after=decoded(ctx.get());
    for(auto *c:{producer,consumer,producer_mate,consumer_mate})check_emitted_truth(ctx.get(),*after,c);
    EXPECT_EQ(pm&half_mask(producer_mate),emitted_mask(ctx.get(),*after,producer_mate)&half_mask(producer_mate));
    EXPECT_EQ(cm&half_mask(consumer_mate),emitted_mask(ctx.get(),*after,consumer_mate)&half_mask(consumer_mate));
    for(unsigned row=0;row<128;++row) {
        std::map<NetInfo *,bool> values;for(unsigned i=0;i<7;++i)values[inputs[i]]=(row>>i)&1u;
        for(auto *c:{producer,consumer}) {
            unsigned address=0;
            for(int i=0;i<c->combInfo.lut_input_count;++i) {
                bool v=values.at(c->getPort(pins[i]));v^=c->get_pin_state(pins[i])!=PIN_INV;
                address|=unsigned(v)<<physical_address_bit(ctx.get(),c,pins[i]);
            }
            values[c->getPort(id_Q)]=1^((emitted_mask(ctx.get(),*after,c)>>emitted_address(ctx.get(),c,address))&1u);
        }
        bool expected=false;for(unsigned i=0;i<7;++i)expected^=(row>>i)&1u;
        EXPECT_EQ(values.at(consumer->getPort(id_Q)),expected)<<row;
    }
    (void)original_pair;
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
TEST_P(RoutedPrivatePairBackend, LocalCombRegistersKeepAllControlsSelectorsRoutesAndRollback)
{
    build(true);ASSERT_NE(producer_ff,nullptr);ASSERT_NE(consumer_ff,nullptr);
    Snapshot before(ctx.get());auto before_rbf=decoded(ctx.get());auto controls=control_state(ctx.get(),*before_rbf,{producer,consumer});
    std::vector<unsigned> stages;EXPECT_FALSE(trial([]{return false;},[&](unsigned s){stages.push_back(s);}));before.expect_exact(ctx.get());
    ASSERT_FALSE(stages.empty());ASSERT_EQ(stages.back(),21u);
    for(unsigned stage:stages){EXPECT_THROW(trial([]{return true;},[&](unsigned n){if(n==stage)throw std::runtime_error("shared FF stage");}),std::runtime_error);before.expect_exact(ctx.get());}
    auto chosen=candidate();ASSERT_TRUE(trial())<<reason;auto expected=expected_after(before,chosen);expected.expect_exact(ctx.get(),incident());
    auto after_rbf=decoded(ctx.get());EXPECT_EQ(control_state(ctx.get(),*after_rbf,{producer,consumer}),controls);
    for(auto *c:{producer,consumer,producer_mate,consumer_mate})check_emitted_truth(ctx.get(),*after_rbf,c);
    for(auto *f:{producer_ff,consumer_ff})for(auto p:{id_CLK,id_ENA,id_ACLR,id_DATAIN})if(auto *n=f->getPort(p)){connected(n);}
}
TEST_P(RoutedPrivatePairBackend, IndependentDataAndSynchronousLoadRegistersAreExplicitlyExcluded)
{
    build(true);auto *f=consumer_ff;auto *local=f->getPort(id_DATAIN);
    f->disconnectPort(id_DATAIN);f->connectPort(id_DATAIN,inputs[7]);ctx->assign_ff_info(f);
    {Snapshot before(ctx.get());bool touched=false;EXPECT_FALSE(trial([]{return true;},[&](unsigned){touched=true;}));EXPECT_FALSE(touched);before.expect_exact(ctx.get());}
    f->disconnectPort(id_DATAIN);f->connectPort(id_DATAIN,local);ctx->assign_ff_info(f);
    f->connectPort(id_SDATA,inputs[8]);f->connectPort(id_SLOAD,inputs[7]);f->pin_data[id_SLOAD].state=PIN_SIG;ctx->assign_ff_info(f);
    {Snapshot before(ctx.get());bool touched=false;EXPECT_FALSE(trial([]{return true;},[&](unsigned){touched=true;}));EXPECT_FALSE(touched);before.expect_exact(ctx.get());}
}
TEST(RoutedPrivatePairLaunch, CompleteSignalsRequireOneNativeLaunchClockAndEdge)
{
    ArchArgs args;args.device="5CSEBA6U23I7";Context ctx(args);ctx.settings[ctx.id("target_freq")]=1e8;
    ctx.attrs[id_step]=std::string("route");ctx.createNet(ctx.id("$PACKER_GND_NET"));ctx.createNet(ctx.id("$PACKER_VCC_NET"));
    auto make_clock=[&](const char *name,int shift){auto *n=ctx.createNet(ctx.id(name));n->clkconstr=std::make_unique<ClockConstraint>();n->clkconstr->period=DelayPair(10000);n->clkconstr->high=n->clkconstr->low=DelayPair(5000);n->clkconstr->phase_group=ctx.id("phase_family");n->clkconstr->phase_shift=shift;return n;};
    auto *clock=make_clock("clock",0),*phase=make_clock("other_phase",2500);std::vector<NetInfo *> signals;
    for(unsigned i=0;i<7;++i){auto *f=ctx.createCell(ctx.idf("launch%u",i),id_MISTRAL_FF);
        for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})f->addInput(p);
        f->pin_data[id_ENA].state=PIN_1;f->pin_data[id_ACLR].state=PIN_1;f->pin_data[id_SCLR].state=PIN_0;f->pin_data[id_SLOAD].state=PIN_0;
        f->addOutput(id_Q);auto *q=ctx.createNet(ctx.idf("launch%u$q",i));f->connectPort(id_Q,q);f->connectPort(id_DATAIN,q);f->connectPort(id_CLK,clock);signals.push_back(q);
    }
    std::string reason;
    auto check=[&](){ctx.assignArchInfo();TimingAnalyser t(&ctx);t.setup(false,false,true);return critical_routed_private_pair_common_launch(&ctx,t,signals,&reason);};
    ASSERT_TRUE(check())<<reason;
    auto *source=signals[0]->driver.cell;source->disconnectPort(id_CLK);source->connectPort(id_CLK,phase);EXPECT_FALSE(check());
    source->disconnectPort(id_CLK);source->connectPort(id_CLK,clock);ASSERT_TRUE(check())<<reason;
    source->pin_data[id_CLK].state=PIN_INV;EXPECT_FALSE(check());source->pin_data[id_CLK].state=PIN_SIG;
    auto constraint=std::move(clock->clkconstr);EXPECT_FALSE(check());clock->clkconstr=std::move(constraint);ASSERT_TRUE(check())<<reason;
    auto saved=signals[0];signals[0]=nullptr;EXPECT_FALSE(check());signals[0]=signals[1];EXPECT_FALSE(check());signals[0]=saved;
    ctx.assignArchInfo();TimingAnalyser t(&ctx);t.setup(false,false,true);auto driver=signals[0]->driver;signals[0]->driver.port=id_CLK;
    EXPECT_FALSE(critical_routed_private_pair_common_launch(&ctx,t,signals,&reason));signals[0]->driver=driver;
}

INSTANTIATE_TEST_SUITE_P(LabAndMlabBothHalves,RoutedPrivatePairBackend,::testing::Combine(::testing::Bool(),::testing::Values(0,1)));
} // namespace
