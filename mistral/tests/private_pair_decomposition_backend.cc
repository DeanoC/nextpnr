/* Independent native transaction, timing boundary and emitted RBF tests. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "private_pair_decomposition_native.h"
#include "cyclonev.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
USING_NEXTPNR_NAMESPACE
namespace {
namespace policy = private_pair_decomposition;
using CV = mistral::CycloneV;
const IdString pins[] = {id_A, id_B, id_C, id_D, id_E, id_F};
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
    };
    std::map<IdString, Cell> cells;
    std::map<IdString, Net> nets;
    std::vector<IdString> cell_order, net_order;
    std::vector<LABInfo> labs;
    decltype(Context::net_aliases) aliases;
    explicit Snapshot(Context *ctx) : labs(ctx->labs), aliases(ctx->net_aliases)
    {
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            cell_order.push_back(entry.first);
            cells.emplace(entry.first, Cell{cell, cell->type, cell->bel, cell->belStrength, cell->cluster, cell->attrs, cell->params,
                                            cell->ports, *static_cast<ArchCellInfo *>(cell)});
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            net_order.push_back(entry.first);
            nets.emplace(entry.first, Net{net, net->driver, net->users});
        }
    }
    void expect_exact(Context *ctx) const
    {
        EXPECT_EQ(ctx->net_aliases, aliases);
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

class PrivatePairBackend : public ::testing::TestWithParam<std::tuple<bool,int>> {
  protected:
    std::unique_ptr<Context> ctx;
    uint32_t lab = 0, source_lab = 0;
    NetInfo *clock = nullptr;
    std::vector<NetInfo *> inputs;
    CellInfo *producer = nullptr, *consumer = nullptr, *capture = nullptr;
    CellInfo *producer_mate = nullptr, *consumer_mate = nullptr, *unrelated = nullptr;
    std::string reason;
    int half() const { return std::get<1>(GetParam()); }
    BelId bel(int alm, int h) { return ctx->labs.at(lab).alms.at(alm).lut_bels.at(h); }
    NetInfo *clock_net(const char *name, int shift = 0)
    {
        auto *net = ctx->createNet(ctx->id(name)); net->is_global = true;
        net->clkconstr = std::make_unique<ClockConstraint>();
        net->clkconstr->period = DelayPair(10000);
        net->clkconstr->high = net->clkconstr->low = DelayPair(5000);
        net->clkconstr->phase_group = ctx->id("phase_family"); net->clkconstr->phase_shift = shift;
        return net;
    }
    CellInfo *ff(const std::string &name, NetInfo *data, BelId place, NetInfo *clk = nullptr)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN}) cell->addInput(pin);
        cell->pin_data[id_ENA].state = PIN_1; cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0; cell->pin_data[id_SLOAD].state = PIN_0;
        cell->connectPort(id_CLK,clk ? clk : clock); cell->addOutput(id_Q);
        cell->connectPort(id_Q,ctx->createNet(ctx->id(name+"$q")));
        cell->connectPort(id_DATAIN,data ? data : cell->getPort(id_Q));
        ctx->assign_ff_info(cell); ctx->assign_default_pinmap(cell); ctx->bindBel(place,cell,STRENGTH_WEAK);
        return cell;
    }
    CellInfo *lut(const std::string &name, int width, uint64_t mask, const std::vector<NetInfo *> &nets, BelId place)
    {
        const IdString types[] = {id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5,id_MISTRAL_ALUT6};
        auto *cell = ctx->createCell(ctx->id(name),types[width-2]);
        cell->params[id_LUT] = Property(int64_t(mask),1u<<width);
        for (int i=0;i<width;++i) { cell->addInput(pins[i]); if (i<int(nets.size()) && nets[i]) cell->connectPort(pins[i],nets[i]); else cell->pin_data[pins[i]].state = PIN_0; }
        cell->addOutput(id_Q); cell->connectPort(id_Q,ctx->createNet(ctx->id(name+"$q")));
        ctx->assign_comb_info(cell); ctx->assign_default_pinmap(cell); ctx->bindBel(place,cell,STRENGTH_WEAK); return cell;
    }
    void build(bool registered)
    {
        inputs.clear(); ArchArgs args; args.device="5CSEBA6U23I7"; ctx=std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")]=1e8; ctx->attrs[id_step]=std::string("place");
        ctx->createNet(ctx->id("$PACKER_GND_NET"));ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        std::vector<std::pair<int,uint32_t>> choices;
        for(uint32_t i=0;i<ctx->labs.size();++i) if(ctx->labs[i].is_mlab==std::get<0>(GetParam())) {
            auto l=ctx->getBelLocation(ctx->labs[i].alms[0].lut_bels[0]);choices.emplace_back(std::abs(l.x-24)+std::abs(l.y-24),i);
        }
        std::sort(choices.begin(),choices.end()); ASSERT_GE(choices.size(),2u);lab=choices[0].second;source_lab=choices[1].second;
        clock=clock_net("clock");
        for(int i=0;i<9;++i) {
            std::string name="source0"+std::to_string(i);
            CellInfo *source=registered ? ff(name,nullptr,ctx->labs[source_lab].alms[i].ff_bels[0]) :
                lut(name,2,8,{},ctx->labs[source_lab].alms[i].lut_bels[0]);
            inputs.push_back(source->getPort(id_Q));
        }
        producer=lut("producer",3,0x08,{inputs[0],inputs[1],inputs[2]},bel(0,half()));
        consumer=lut("consumer",5,0x1554bffe,{inputs[3],inputs[4],inputs[5],inputs[6],producer->getPort(id_Q)},bel(1,half()));
        producer->pin_data[id_B].state=PIN_INV; consumer->pin_data[id_E].state=PIN_INV;
        producer_mate=lut("producer_mate",2,6,{inputs[7],inputs[8]},bel(0,1-half()));
        consumer_mate=lut("consumer_mate",2,9,{inputs[7],inputs[8]},bel(1,1-half()));
        unrelated=lut("unrelated",2,14,{inputs[7],inputs[8]},bel(8,0));
        if(registered) capture=ff("capture",consumer->getPort(id_Q),ctx->labs[lab].alms[7].ff_bels[0]);
        else capture=lut("capture_logic",2,8,{consumer->getPort(id_Q),inputs[8]},bel(7,0));
        ctx->net_aliases[ctx->id("consumer_observer_alias")]=consumer->getPort(id_Q)->name;
        for(auto *net:inputs) {auto a=net->users.add(PortRef{}),b=net->users.add(PortRef{}),c=net->users.add(PortRef{});net->users.remove(b);net->users.remove(a);net->users.remove(c);}
        ctx->assignArchInfo();
        for(auto l:{lab,source_lab}) for(uint8_t a=0;a<10;++a) ctx->assign_alm_lut_inputs(l,a);
        ctx->check(); for(const auto &c:ctx->cells) ASSERT_TRUE(ctx->isBelLocationValid(c.second->bel))<<ctx->nameOf(c.second.get());
    }
    void SetUp() override { build(true); }
    policy::Pair original() const
    {
        policy::Pair p;
        for(auto *cell:{producer,consumer}) {
            auto &lut=cell==producer?p.producer:p.consumer;lut.mask=uint64_t(cell->params.at(id_LUT).as_int64());
            for(int i=0;i<(cell==producer?3:5);++i) {
                NetInfo *n=cell->getPort(pins[i]);int id=n==producer->getPort(id_Q)?policy::INTERNAL:int(std::find(inputs.begin(),inputs.begin()+7,n)-inputs.begin());
                lut.pins.push_back({id,cell->get_pin_state(pins[i])==PIN_INV});
            }
        }
        return p;
    }
    policy::Candidate candidate() const
    {
        auto all=policy::decompose(original());
        auto it=std::find_if(all.begin(),all.end(),[](const auto &v){return v.bound!=std::array<int,3>{0,1,2};});
        if(it==all.end()) throw std::runtime_error("fixture has no changed support partition");
        return *it;
    }
    bool trial(const std::function<bool()> &accept=[] {return true;},const std::function<void(unsigned)> &stage={})
    { return private_pair_trial(ctx.get(),producer,candidate(),accept,&reason,stage); }
    bool value(const NetInfo *net,unsigned row) const
    {
        auto it=std::find(inputs.begin(),inputs.end(),net);if(it!=inputs.end()) return (row>>size_t(it-inputs.begin()))&1;
        const auto *cell=net->driver.cell;unsigned index=0;
        for(int i=0;i<cell->combInfo.lut_input_count;++i) {bool v=value(cell->getPort(pins[i]),row);v^=cell->get_pin_state(pins[i])==PIN_INV;index|=unsigned(v)<<i;}
        return (uint64_t(cell->params.at(id_LUT).as_int64())>>index)&1;
    }
    std::array<bool,128> truth() const {std::array<bool,128> t{};for(unsigned i=0;i<t.size();++i)t[i]=value(consumer->getPort(id_Q),i);return t;}
    void rejected() {Snapshot s(ctx.get());EXPECT_FALSE(trial())<<reason;EXPECT_FALSE(reason.empty());s.expect_exact(ctx.get());}
};

TEST_P(PrivatePairBackend, SuccessfulChangedPartitionPreservesEveryUserSlotAndOutsideGraph)
{
    Snapshot before(ctx.get());auto expected=truth();auto aliases=ctx->net_aliases;std::vector<unsigned> stages;
    ASSERT_TRUE(trial([]{return true;},[&](unsigned s){stages.push_back(s);}))<<reason;
    EXPECT_EQ(stages,(std::vector<unsigned>{1,2,3,4,5}));EXPECT_EQ(truth(),expected);EXPECT_EQ(ctx->net_aliases,aliases);
    for(const auto &entry:before.cells) {
        auto *c=ctx->cells.at(entry.first).get();const auto &old=entry.second;
        EXPECT_EQ(c,old.identity);EXPECT_EQ(c->bel,old.bel);EXPECT_EQ(c->belStrength,old.strength);EXPECT_EQ(c->type,old.type);EXPECT_EQ(c->attrs,old.attrs);
        if(c==producer||c==consumer)continue;
        EXPECT_EQ(c->params,old.params);ASSERT_EQ(c->ports.size(),old.ports.size());
        for(const auto &p:old.ports){EXPECT_EQ(c->ports.at(p.first).net,p.second.net);EXPECT_EQ(c->ports.at(p.first).user_idx,p.second.user_idx);}
        for(const auto &p:old.architecture.pin_data){EXPECT_EQ(c->pin_data.at(p.first).state,p.second.state);EXPECT_EQ(c->pin_data.at(p.first).bel_pins,p.second.bel_pins);}
    }
    for(const auto &entry:before.nets) {
        auto *n=ctx->nets.at(entry.first).get();const auto &old=entry.second;EXPECT_EQ(n,old.identity);EXPECT_EQ(n->driver.cell,old.driver.cell);EXPECT_EQ(n->driver.port,old.driver.port);
        EXPECT_EQ(n->users.entries(),old.users.entries());auto saved=old.users;
        for(auto u:saved.enumerate()){ASSERT_TRUE(n->users.count(u.index));auto now=n->users.at(u.index);EXPECT_EQ(now.cell->ports.at(now.port).net,n);EXPECT_EQ(now.cell->ports.at(now.port).user_idx,u.index);if(u.value.cell!=producer&&u.value.cell!=consumer){EXPECT_EQ(now.cell,u.value.cell);EXPECT_EQ(now.port,u.value.port);}}
        auto actual=n->users,previous=old.users;ASSERT_EQ(actual.capacity(),previous.capacity());for(int i=0;i<old.users.capacity()+4;++i)EXPECT_EQ(actual.add(PortRef{}),previous.add(PortRef{}));
    }
    ctx->check();
}
TEST_P(PrivatePairBackend, AcceptanceRejectionAndAllFiveExceptionsRestoreExactly)
{
    {Snapshot s(ctx.get());EXPECT_FALSE(trial([]{return false;}));s.expect_exact(ctx.get());}
    {Snapshot s(ctx.get());EXPECT_THROW(trial([]()->bool{throw std::runtime_error("accept");}),std::runtime_error);s.expect_exact(ctx.get());}
    for(unsigned stage=1;stage<=5;++stage){Snapshot s(ctx.get());EXPECT_THROW(trial([]{return true;},[&](unsigned n){if(n==stage)throw std::runtime_error("injected");}),std::runtime_error);s.expect_exact(ctx.get());}
}
TEST_P(PrivatePairBackend, NativePostRewriteLegalityAndMaskRejectionsRestoreExactly)
{
    Snapshot s(ctx.get());const auto target=producer->bel;
    EXPECT_FALSE(trial([]{return true;},[&](unsigned stage){if(stage==3)ctx->fes_bel_region[target]=ctx->id("reserved_for_test");}));
    ctx->fes_bel_region.erase(target);s.expect_exact(ctx.get());
    EXPECT_FALSE(trial([]{return true;},[&](unsigned stage){if(stage==4)producer->pin_data[id_B].bel_pins=producer->pin_data[id_A].bel_pins;}));s.expect_exact(ctx.get());
}
TEST_P(PrivatePairBackend, ProtectedMalformedAliasedAndIllegalOriginalsFailClosed)
{
    for(auto *c:{producer,consumer,producer_mate,consumer_mate}) for(const char *attr:{"keep","dont_touch","BEL","FES_SLOT"}){c->attrs[ctx->id(attr)]=1;rejected();c->attrs.erase(ctx->id(attr));}
    auto bad=candidate();bad.network.consumer.mask^=1;Snapshot before(ctx.get());EXPECT_FALSE(private_pair_trial(ctx.get(),producer,bad,[]{return true;},&reason));before.expect_exact(ctx.get());
    producer->constr_x=1;rejected();producer->constr_x=0;
    for(auto *c:{producer,consumer,producer_mate,consumer_mate}){auto strength=c->belStrength;c->belStrength=STRENGTH_STRONG;rejected();c->belStrength=strength;}
    ctx->fes_fence_active=true;rejected();ctx->fes_fence_active=false;
    consumer->pin_data[id_Q].state=PIN_INV;rejected();consumer->pin_data[id_Q].state=PIN_SIG;
    auto original_type=producer_mate->type;producer_mate->type=id_MISTRAL_ALUT6;ctx->assign_comb_info(producer_mate);rejected();producer_mate->type=original_type;ctx->assign_comb_info(producer_mate);
    auto name=ctx->id("hidden_top_observer");ctx->ports[name]=PortInfo{name,producer->getPort(id_Q),PORT_OUT,{}};rejected();ctx->ports.erase(name);
}
TEST_P(PrivatePairBackend, CohabitingRegisterDataLoadAndControlsPreserveSelectorsAndRollback)
{
    // The producer half must keep E for independent register DATAIN. The
    // five-input consumer uses its E; its mate must release the opposite F
    // for synchronous load. Both cases invoke the native private-pin plan.
    auto *direct=ff("cohabiting_data",inputs[7],ctx->labs[lab].alms[0].ff_bels[2*half()]);
    auto *load=ff("cohabiting_load",consumer->getPort(id_Q),ctx->labs[lab].alms[1].ff_bels[2*half()]);
    load->connectPort(id_SDATA,inputs[8]);load->connectPort(id_SLOAD,inputs[7]);load->pin_data[id_SLOAD].state=PIN_SIG;
    for(auto *f:{direct,load}) {
        f->connectPort(id_ENA,inputs[7]);f->connectPort(id_ACLR,inputs[8]);
        f->pin_data[id_ENA].state=PIN_INV;f->pin_data[id_ACLR].state=PIN_SIG;
    }
    ctx->assignArchInfo();for(uint8_t a=0;a<10;++a)ctx->assign_alm_lut_inputs(lab,a);
    for(const auto &c:ctx->cells)ASSERT_TRUE(ctx->isBelLocationValid(c.second->bel))<<ctx->nameOf(c.second.get());
    auto uses=[](CellInfo *c,IdString physical){for(const auto &pin:c->pin_data)for(auto p:pin.second.bel_pins)if(p==physical)return true;return false;};
    auto selectors_valid=[&](){
        EXPECT_FALSE(uses(producer,half()?id_E1:id_E0));
        EXPECT_TRUE(uses(consumer,half()?id_E1:id_E0));
        EXPECT_FALSE(uses(consumer_mate,half()?id_F0:id_F1));
        EXPECT_EQ(direct->ffInfo.datain,inputs[7]);EXPECT_EQ(load->ffInfo.datain,consumer->getPort(id_Q));EXPECT_EQ(load->ffInfo.sdata,inputs[8]);
        for(auto *f:{direct,load}) {EXPECT_EQ(f->ffInfo.ctrlset.ena.net,inputs[7]);EXPECT_TRUE(f->ffInfo.ctrlset.ena.inverted);EXPECT_EQ(f->ffInfo.ctrlset.aclr.net,inputs[8]);EXPECT_FALSE(f->ffInfo.ctrlset.aclr.inverted);EXPECT_TRUE(ctx->isBelLocationValid(f->bel));}
    };
    selectors_valid();Snapshot before(ctx.get());
    EXPECT_FALSE(trial([]{return false;}));before.expect_exact(ctx.get());
    for(unsigned stage=1;stage<=5;++stage){EXPECT_THROW(trial([]{return true;},[&](unsigned s){if(s==stage)throw std::runtime_error("FF rollback");}),std::runtime_error);before.expect_exact(ctx.get());}
    const auto expected=truth();ASSERT_TRUE(trial())<<reason;EXPECT_EQ(truth(),expected);selectors_valid();
    for(auto *f:{direct,load}) {
        const auto &old=before.cells.at(f->name);EXPECT_EQ(f->bel,old.bel);EXPECT_EQ(f->belStrength,old.strength);
        EXPECT_EQ(f->ffInfo.ctrlset,old.architecture.ffInfo.ctrlset);EXPECT_EQ(f->ffInfo.datain,old.architecture.ffInfo.datain);EXPECT_EQ(f->ffInfo.sdata,old.architecture.ffInfo.sdata);
        ASSERT_EQ(f->pin_data.size(),old.architecture.pin_data.size());for(const auto &pin:old.architecture.pin_data){EXPECT_EQ(f->pin_data.at(pin.first).state,pin.second.state);EXPECT_EQ(f->pin_data.at(pin.first).bel_pins,pin.second.bel_pins);}
        for(const auto &port:old.ports){EXPECT_EQ(f->ports.at(port.first).net,port.second.net);EXPECT_EQ(f->ports.at(port.first).user_idx,port.second.user_idx);}
    }
    for(unsigned a=0;a<10;++a){const auto &old=before.labs[lab].alms[a],&now=ctx->labs[lab].alms[a];EXPECT_EQ(now.clk_ena_idx,old.clk_ena_idx);EXPECT_EQ(now.aclr_idx,old.aclr_idx);EXPECT_EQ(now.sel_ef,old.sel_ef);EXPECT_EQ(now.sel_clk,old.sel_clk);EXPECT_EQ(now.sel_ena,old.sel_ena);EXPECT_EQ(now.sel_aclr,old.sel_aclr);EXPECT_EQ(now.ff_in,old.ff_in);EXPECT_EQ(now.ff_out,old.ff_out);}
    ctx->check();
}
TEST_P(PrivatePairBackend, CompleteNativeClockMetricsRejectEveryCoverageAndPositiveMarginRegression)
{
    auto *other=clock_net("other_clock");uint32_t other_lab=0;while(other_lab==lab||other_lab==source_lab)++other_lab;
    ff("independent_clock_loop",nullptr,ctx->labs[other_lab].alms[0].ff_bels[0],other);
    ctx->assignArchInfo();TimingAnalyser timing(ctx.get());timing.setup(false,false,true);
    const TimingResult before=timing.get_timing_result();const auto a=clock->name,b=other->name;
    ASSERT_TRUE(before.clock_setup_slack.count(a));ASSERT_TRUE(before.clock_setup_slack.count(b));
    ASSERT_TRUE(before.clock_hold_slack.count(a));ASSERT_TRUE(before.clock_hold_slack.count(b));
    ASSERT_TRUE(before.clock_fmax.count(a));ASSERT_TRUE(before.clock_fmax.count(b));
    ASSERT_GT(before.clock_setup_slack.at(b),0);ASSERT_GT(before.clock_hold_slack.at(b),0);
    ASSERT_TRUE(private_pair_clock_safe(before,before,&reason))<<reason;
    auto rejected_metric=[&](auto mutate){auto after=before;mutate(after);EXPECT_FALSE(private_pair_clock_safe(before,after,&reason));EXPECT_FALSE(reason.empty());};
    rejected_metric([&](auto &t){t.clock_setup_slack.erase(b);});
    rejected_metric([&](auto &t){t.clock_hold_slack.erase(b);});
    rejected_metric([&](auto &t){t.clock_fmax.erase(b);});
    rejected_metric([&](auto &t){--t.clock_setup_slack[b];});
    rejected_metric([&](auto &t){--t.clock_hold_slack[b];});
    rejected_metric([&](auto &t){t.clock_fmax[b].achieved-=0.01f;});
    rejected_metric([&](auto &t){t.clock_fmax[b].achieved=std::numeric_limits<float>::quiet_NaN();});
    rejected_metric([&](auto &t){t.clock_fmax[b].constraint=std::numeric_limits<float>::infinity();});
    rejected_metric([&](auto &t){t.clock_fmax[b].constraint+=1;});
    auto invalid=before;invalid.clock_fmax[b].achieved=std::numeric_limits<float>::quiet_NaN();EXPECT_FALSE(private_pair_clock_safe(invalid,before,&reason));
    auto improved=before;++improved.clock_setup_slack[a];++improved.clock_hold_slack[b];improved.clock_fmax[a].achieved+=1;EXPECT_TRUE(private_pair_clock_safe(before,improved,&reason))<<reason;
}
TEST_P(PrivatePairBackend, NativeArrivalsRequireCompleteCommonLaunchFrame)
{
    std::vector<NetInfo *> signals(inputs.begin(),inputs.begin()+7);std::array<delay_t,7> values{};
    auto arrivals=[&](){ctx->assignArchInfo();TimingAnalyser t(ctx.get());t.setup();return private_pair_common_arrivals(ctx.get(),t,signals,values,&reason);};
    ASSERT_TRUE(arrivals())<<reason;
    auto *shifted=clock_net("shifted_launch",2500);auto *source=inputs[0]->driver.cell;
    source->disconnectPort(id_CLK);source->connectPort(id_CLK,shifted);EXPECT_FALSE(arrivals());EXPECT_NE(reason.find("mixed"),std::string::npos)<<reason;
    source->disconnectPort(id_CLK);source->connectPort(id_CLK,clock);
    auto constraint=std::move(clock->clkconstr);EXPECT_FALSE(arrivals());clock->clkconstr=std::move(constraint);
    ASSERT_TRUE(arrivals())<<reason;
    auto saved=signals[0];signals[0]=nullptr;EXPECT_FALSE(arrivals());signals[0]=signals[1];EXPECT_FALSE(arrivals());signals[0]=saved;
    {TimingAnalyser t(ctx.get());t.setup();auto driver=signals[0]->driver;signals[0]->driver.port=id_CLK;EXPECT_FALSE(private_pair_common_arrivals(ctx.get(),t,signals,values,&reason));signals[0]->driver=driver;}
    signals.pop_back();EXPECT_FALSE(arrivals());
}
TEST_P(PrivatePairBackend, DirectRegisteredMemoryArrivalsAreCharacterized)
{
    auto *old_source=inputs[0]->driver.cell;old_source->disconnectPort(id_Q);
    auto *ram=ctx->createCell(ctx->id("registered_memory_source"),id_MISTRAL_M10K);
    ram->params[id_CFG_ABITS]=12;ram->params[id_CFG_DBITS]=1;ram->params[id_CFG_DUAL_CLOCK]=1;
    for(auto p:{id_CLK1,id_CLK2,id_B1EN,id_ACLR0,id_ACLR1,ctx->id("B1ADDR[0]")})ram->addInput(p);
    ram->connectPort(id_CLK1,clock);ram->connectPort(id_CLK2,clock);ram->connectPort(ctx->id("B1ADDR[0]"),inputs[1]);
    ram->connectPort(id_B1EN,ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get());
    ram->pin_data[id_B1EN].state=PIN_1;ram->pin_data[id_ACLR0].state=PIN_0;ram->pin_data[id_ACLR1].state=PIN_0;
    ram->addOutput(id_B1DATA);ram->connectPort(id_B1DATA,inputs[0]);
    ctx->assignArchInfo();TimingAnalyser t(ctx.get());t.setup();std::array<delay_t,7> values{};
    int clocks=0;ASSERT_EQ(ctx->getPortTimingClass(ram,id_B1DATA,clocks),TMG_REGISTER_OUTPUT);ASSERT_GT(clocks,0);
    EXPECT_TRUE(private_pair_common_arrivals(ctx.get(),t,std::vector<NetInfo *>(inputs.begin(),inputs.begin()+7),values,&reason))<<reason;
    ram->params[id_CFG_ASYNC_READ]=1;TimingAnalyser asynchronous(ctx.get());asynchronous.setup();
    EXPECT_FALSE(private_pair_common_arrivals(ctx.get(),asynchronous,std::vector<NetInfo *>(inputs.begin(),inputs.begin()+7),values,&reason));
}
TEST_P(PrivatePairBackend, CompleteMultiCaptureCoverageRejectsHiddenUnsupportedAndPublicBranches)
{
    auto *phase=clock_net("phase_capture",2500);
    uint32_t other_lab=0;while(other_lab==lab||other_lab==source_lab)++other_lab;
    auto *second_capture=ff("second_capture",consumer->getPort(id_Q),ctx->labs[other_lab].alms[7].ff_bels[0],phase);
    ctx->assignArchInfo();TimingAnalyser t(ctx.get());t.setup(false,false,true);
    TimingAnalyser setup_only(ctx.get());setup_only.setup_only=true;setup_only.setup();
    EXPECT_FALSE(private_pair_output_coverage(ctx.get(),setup_only,consumer->getPort(id_Q),&reason));
    std::vector<TimingAnalyser::SetupWindowHint> hints;ASSERT_TRUE(t.get_setup_window_hints({consumer->name,id_Q},hints));
    std::set<delay_t> windows;for(const auto &h:hints){ASSERT_TRUE(h.window);windows.insert(*h.window);}EXPECT_EQ(windows,(std::set<delay_t>{2500,10000}));
    ASSERT_TRUE(private_pair_output_coverage(ctx.get(),t,consumer->getPort(id_Q),&reason))<<reason;
    // Preserve synchronous capture coverage while adding an asynchronous
    // observer that the historical native class misleadingly calls registered.
    second_capture->connectPort(id_ACLR,consumer->getPort(id_Q));second_capture->pin_data[id_ACLR].state=PIN_SIG;
    ctx->assignArchInfo();int clear_clocks=0;ASSERT_EQ(ctx->getPortTimingClass(second_capture,id_ACLR,clear_clocks),TMG_REGISTER_INPUT);
    TimingAnalyser reset_branch(ctx.get());reset_branch.setup(false,false,true);
    EXPECT_FALSE(private_pair_output_coverage(ctx.get(),reset_branch,consumer->getPort(id_Q),&reason));
    second_capture->disconnectPort(id_ACLR);second_capture->pin_data[id_ACLR].state=PIN_1;ctx->assignArchInfo();
    TimingAnalyser restored(ctx.get());restored.setup(false,false,true);ASSERT_TRUE(private_pair_output_coverage(ctx.get(),restored,consumer->getPort(id_Q),&reason))<<reason;
    auto *hidden=ctx->createCell(ctx->id("hidden_unsupported"),id_MISTRAL_OB);hidden->addInput(id_I);hidden->connectPort(id_I,consumer->getPort(id_Q));
    ctx->assignArchInfo();TimingAnalyser bad(ctx.get());bad.setup(false,false,true);
    ASSERT_TRUE(bad.get_setup_window_hints({consumer->name,id_Q},hints));
    EXPECT_FALSE(private_pair_output_coverage(ctx.get(),bad,consumer->getPort(id_Q),&reason));
    auto hidden_name=hidden->name;hidden->disconnectPort(id_I);ctx->cells.erase(hidden_name);
    auto name=ctx->id("public_output");ctx->ports[name]=PortInfo{name,consumer->getPort(id_Q),PORT_OUT,{}};
    EXPECT_FALSE(private_pair_output_coverage(ctx.get(),t,consumer->getPort(id_Q),&reason));ctx->ports.erase(name);
    phase->clkconstr.reset();TimingAnalyser missing(ctx.get());missing.setup(false,false,true);
    EXPECT_FALSE(private_pair_output_coverage(ctx.get(),missing,consumer->getPort(id_Q),&reason));
}
TEST_P(PrivatePairBackend, BoundedCoverageRejectsDeepConeAndCycles)
{
    ctx->assignArchInfo();TimingAnalyser t(ctx.get());t.setup(false,false,true);
    ASSERT_TRUE(private_pair_output_coverage(ctx.get(),t,consumer->getPort(id_Q),&reason))<<reason;
    auto *net=consumer->getPort(id_Q);
    for(int i=0;i<70;++i){auto *c=ctx->createCell(ctx->idf("deep%d",i),id_MISTRAL_ALUT2);c->params[id_LUT]=Property(int64_t(0xa),4);c->addInput(id_A);c->addInput(id_B);c->connectPort(id_A,net);c->pin_data[id_B].state=PIN_0;c->addOutput(id_Q);c->connectPort(id_Q,ctx->createNet(ctx->idf("deep%d$q",i)));net=c->getPort(id_Q);}
    EXPECT_FALSE(private_pair_output_coverage(ctx.get(),t,consumer->getPort(id_Q),&reason));
    auto *cycle=ctx->createCell(ctx->id("cycle"),id_MISTRAL_ALUT2);cycle->params[id_LUT]=Property(int64_t(6),4);cycle->addInput(id_A);cycle->addInput(id_B);cycle->addOutput(id_Q);auto *q=ctx->createNet(ctx->id("cycle$q"));cycle->connectPort(id_Q,q);cycle->connectPort(id_A,q);cycle->connectPort(id_B,inputs[0]);
    EXPECT_FALSE(private_pair_output_coverage(ctx.get(),t,q,&reason));
}

const std::array<unsigned,64> mlab_order{
    0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,
    2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18,
    32,33,36,37,40,41,44,45,61,60,57,56,53,52,49,48,
    34,35,38,39,42,43,46,47,63,62,59,58,55,54,51,50};
uint64_t emitted_mask(Context *ctx,const CV &decoded,CellInfo *cell)
{
    const auto &loc=ctx->bel_data(cell->bel).lab_data;CV::bmux_setting_t s;
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
TEST_P(PrivatePairBackend, EmittedRbfProvesBothFunctionsCompositionAndStationaryMates)
{
    build(false);const auto expected=truth();
    auto decode=[&](){ctx->configure_bitstream();std::vector<uint8_t> bytes;ctx->cyclonev->rbf_save(bytes);if(bytes.empty())throw std::runtime_error("empty RBF");auto d=std::unique_ptr<CV>(CV::get_model("5CSEBA6U23I7"));if(!d)throw std::runtime_error("model");d->rbf_load(bytes.data(),uint32_t(bytes.size()));return d;};
    auto before=decode();const auto old_pm=emitted_mask(ctx.get(),*before,producer_mate),old_cm=emitted_mask(ctx.get(),*before,consumer_mate);
    for(auto *c:{producer,consumer,producer_mate,consumer_mate})check_emitted_truth(ctx.get(),*before,c);
    ASSERT_TRUE(trial())<<reason;auto after=decode();
    for(auto *c:{producer,consumer,producer_mate,consumer_mate})check_emitted_truth(ctx.get(),*after,c);
    auto half_bits=[&](CellInfo *cell){uint64_t result=0;for(unsigned a=0;a<32;++a)result|=uint64_t(1)<<emitted_address(ctx.get(),cell,a);return result;};
    EXPECT_EQ(old_pm&half_bits(producer_mate),emitted_mask(ctx.get(),*after,producer_mate)&half_bits(producer_mate));
    EXPECT_EQ(old_cm&half_bits(consumer_mate),emitted_mask(ctx.get(),*after,consumer_mate)&half_bits(consumer_mate));
    for(unsigned row=0;row<128;++row){std::map<NetInfo *,bool> values;for(unsigned i=0;i<7;++i)values[inputs[i]]=(row>>i)&1;
        for(auto *cell:{producer,consumer}){unsigned address=0;for(int i=0;i<cell->combInfo.lut_input_count;++i)address|=unsigned(!values.at(cell->getPort(pins[i])))<<physical_address_bit(ctx.get(),cell,pins[i]);
            values[cell->getPort(id_Q)]=1^((emitted_mask(ctx.get(),*after,cell)>>emitted_address(ctx.get(),cell,address))&1);
        }
        EXPECT_EQ(values.at(consumer->getPort(id_Q)),expected[row])<<row;
    }
}
INSTANTIATE_TEST_SUITE_P(LabAndMlabBothHalves,PrivatePairBackend,::testing::Combine(::testing::Bool(),::testing::Values(0,1)));
} // namespace
