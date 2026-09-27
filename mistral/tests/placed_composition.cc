#include "gtest/gtest.h"
#include "nextpnr.h"
#include "json_frontend.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>

NEXTPNR_NAMESPACE_BEGIN
void diagnostic_placed_composition(Context *ctx, const char *prefix, const char *mode);
NEXTPNR_NAMESPACE_END
USING_NEXTPNR_NAMESPACE

class PlacedCompositionTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *consumer, *producer, *replica;
    NetInfo *clock, *read, *ready, *valid, *write;
    CellInfo *lut(const char *name, IdString type, int mask, int inputs, const char *bel)
    {
        auto c = ctx->createCell(ctx->id(name), type);
        c->params[id_LUT] = mask;
        for (int i = 0; i < inputs; ++i) { auto pin = ctx->id(std::string(1, 'A' + i)); c->addInput(pin); c->pin_data[pin].state = PIN_0; }
        c->addOutput(id_Q);
        c->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", name)));
        ctx->assignArchInfo();
        ctx->bindBel(ctx->getBelByName(IdStringList::parse(ctx.get(), bel)), c, STRENGTH_WEAK);
        return c;
    }
    void input(CellInfo *c, IdString p, NetInfo *n) { c->connectPort(p, n); c->pin_data[p].state = PIN_SIG; }
    void sinks(NetInfo *net, int count, const char *prefix, int x, int y)
    {
        for (int i = 0; i < count; ++i) {
            auto c = ctx->createCell(ctx->idf("%s%d", prefix, i), id_MISTRAL_FF);
            for (auto pin : {id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN}) c->addInput(pin);
            c->addOutput(id_Q); c->connectPort(id_CLK, clock); c->connectPort(id_ENA, net);
            c->pin_data[id_ACLR].state = PIN_1; c->pin_data[id_SCLR].state = PIN_0; c->pin_data[id_SLOAD].state = PIN_0;
            auto q = ctx->createNet(ctx->idf("%s$q", c->name.c_str(ctx.get())));
            c->connectPort(id_Q, q); c->connectPort(id_DATAIN, q);
            ctx->assignArchInfo();
            bool placed = false;
            for (auto bel : ctx->getBelsByTile(x == 24 ? std::array<int,6>{22,23,24,25,27,28}.at(i / 20) : x, y)) {
                if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(c->type, bel)) continue;
                ctx->bindBel(bel, c, STRENGTH_WEAK);
                if (ctx->isBelLocationValid(bel)) { placed = true; break; }
                ctx->unbindBel(bel);
            }
            ASSERT_TRUE(placed) << c->name.str(ctx.get());
        }
    }
    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7"; ctx = std::make_unique<Context>(args);
        ctx->createNet(ctx->id("$PACKER_GND_NET")); ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        clock = ctx->createNet(ctx->id("clock")); clock->is_global = true;
        auto hps = ctx->createCell(ctx->id("hps_ddr.f2sdram"), id_cyclonev_hps_interface_fpga2sdram);
        hps->addOutput(ctx->id("cmd_ready_1")); ready = ctx->createNet(ctx->id("hps_ddr.cmd_ready_1"));
        hps->connectPort(ctx->id("cmd_ready_1"), ready);
        ctx->bindBel(ctx->getBelByName(IdStringList::parse(ctx.get(), "cyclonev_hps_interface_fpga2sdram.52.53.0")), hps, STRENGTH_WEAK);
        read = lut("hps_ddr.port1.enter_read_MISTRAL_ALUT5_Q", id_MISTRAL_ALUT5, 0x20000, 5, "MISTRAL_MCOMB.39.27.0")->getPort(id_Q);
        write = lut("hps_ddr.port1.enter_write_MISTRAL_ALUT2_Q", id_MISTRAL_ALUT2, 14, 2, "MISTRAL_COMB.37.25.37")->getPort(id_Q);
        valid = lut("hps_ddr.f2sdram_cmd_valid_1_MISTRAL_ALUT2_Q", id_MISTRAL_ALUT2, 14, 2, "MISTRAL_COMB.30.26.42")->getPort(id_Q);
        producer = lut("hps_ddr.port1.slot_free_MISTRAL_ALUT2_Q", id_MISTRAL_ALUT2, 11, 2, "MISTRAL_COMB.30.26.48");
        input(producer,id_A,ready); input(producer,id_B,valid);
        consumer = lut("hps_ddr.port1.slot_free_MISTRAL_ALUT3_B", id_MISTRAL_ALUT3, 50, 3, "MISTRAL_COMB.24.20.36");
        replica = lut("hps_ddr.port1.slot_free_MISTRAL_ALUT3_B$enable_replica", id_MISTRAL_ALUT3, 50, 3, "MISTRAL_COMB.37.25.0");
        for (auto c : {consumer,replica}) { input(c,id_A,read); input(c,id_B,producer->getPort(id_Q)); input(c,id_C,write); }
        sinks(consumer->getPort(id_Q),107,"original",24,20);
        sinks(replica->getPort(id_Q),4,"replica",37,25);
        sinks(producer->getPort(id_Q),5,"other",30,26);
        ctx->assignArchInfo(); ctx->check();
    }
};

TEST_F(PlacedCompositionTest, PairedCompositionAndRelocationMatchSiteAndTruthOracle)
{
    std::map<IdString,BelId> bels; for (auto &c : ctx->cells) bels[c.first] = c.second->bel;
    auto q = consumer->getPort(id_Q);
    std::string prefix = ::testing::TempDir() + "mistral-placed-composition";
    diagnostic_placed_composition(ctx.get(), prefix.c_str(), "compose");
    ASSERT_EQ(consumer->type,id_MISTRAL_ALUT4);
    EXPECT_EQ(consumer->getPort(id_A),read); EXPECT_EQ(consumer->getPort(id_B),ready);
    EXPECT_EQ(consumer->getPort(id_C),valid); EXPECT_EQ(consumer->getPort(id_D),write);
    EXPECT_EQ(consumer->getPort(id_Q),q); EXPECT_EQ(q->users.entries(),107);
    EXPECT_EQ(producer->getPort(id_Q)->users.entries(),6); EXPECT_EQ(replica->getPort(id_Q)->users.entries(),4);
    for (int row = 0; row < 16; ++row) {
        bool a=row&1,b=row&2,c=row&4,d=row&8;
        bool slot_free = b || !c;
        bool expected = !slot_free && (d || a);
        EXPECT_EQ((consumer->params.at(id_LUT).as_int64() >> row) & 1, expected) << row;
    }
    for (auto &b : bels) {
        auto actual = ctx->cells.at(b.first).get();
        if (actual != consumer) EXPECT_EQ(actual->bel,b.second);
        EXPECT_TRUE(ctx->isBelLocationValid(actual->bel));
    }
    for (auto p : {id_A,id_B,id_C,id_D,id_Q}) EXPECT_EQ(consumer->get_pin_state(p),PIN_SIG);
    auto selected_name = std::string(ctx->nameOfBel(consumer->bel));
    EXPECT_NE(selected_name,"MISTRAL_COMB.24.20.36");
    auto mapped_d = ctx->getBelPinsForCellPin(consumer,id_D);
    EXPECT_NE(mapped_d.begin(),mapped_d.end());
    SetUp();
    diagnostic_placed_composition(ctx.get(), (prefix+"-control").c_str(), "relocate");
    EXPECT_EQ(std::string(ctx->nameOfBel(consumer->bel)),selected_name);
    EXPECT_EQ(consumer->type,id_MISTRAL_ALUT3); EXPECT_EQ(consumer->params.at(id_LUT).as_int64(),50);
    EXPECT_EQ(consumer->getPort(id_A),read); EXPECT_EQ(consumer->getPort(id_B),producer->getPort(id_Q));
    EXPECT_EQ(consumer->getPort(id_C),write); EXPECT_FALSE(consumer->ports.count(id_D));
    EXPECT_EQ(producer->getPort(id_Q)->users.entries(),7);
}

TEST_F(PlacedCompositionTest, DisabledDoesNotTransform)
{
    auto q=consumer->getPort(id_Q); auto b=consumer->getPort(id_B);
    diagnostic_placed_composition(ctx.get(), nullptr, nullptr);
    EXPECT_EQ(consumer->type,id_MISTRAL_ALUT3); EXPECT_EQ(consumer->getPort(id_Q),q); EXPECT_EQ(consumer->getPort(id_B),b);
    EXPECT_EQ(producer->getPort(id_Q)->users.entries(),7);
}

// Optional real-design preflight; never packs, places or routes the full design.
TEST(PlacedCompositionSnapshotTest, RealLabLimitAndPairedSearch)
{
    const char *path=std::getenv("MISTRAL_COMPOSITION_TEST_SNAPSHOT");
    if (!path || !*path) GTEST_SKIP() << "No external placement snapshot selected";
    std::string chosen;
    for (const char *mode : {"compose","relocate"}) {
        ArchArgs args; args.device="5CSEBA6U23I7"; Context ctx(args);
        std::ifstream json(path); ASSERT_TRUE(parse_json(json,path,&ctx));
        std::ifstream sidecar(std::string(path)+".pins.tsv"); ASSERT_TRUE(sidecar.good());
        std::string line; std::getline(sidecar,line);
        while (std::getline(sidecar,line)) {
            std::istringstream row(line); std::string cell,port,state;
            ASSERT_TRUE(bool(std::getline(row,cell,'\t'))); ASSERT_TRUE(bool(std::getline(row,port,'\t')));
            ASSERT_TRUE(bool(std::getline(row,state)));
            auto c=ctx.cells.at(ctx.id(cell)).get();
            // JSON groups scalar zero and indexed bus ports; restore the exact
            // logical spelling recorded by the in-process sidecar (PLL outclk).
            if (!c->ports.count(ctx.id(port)) && c->ports.count(ctx.id(port+"[0]")))
                c->renamePort(ctx.id(port+"[0]"),ctx.id(port));
            c->pin_data[ctx.id(port)].state=CellPinState(std::stoi(state));
        }
        // This diagnostic snapshot writer drops outclk[1] when scalar outclk
        // coexists. Restore the known packed second PLL output for preflight.
        auto pll=ctx.cells.at(ctx.id("ram_clock.pll")).get();
        auto second=ctx.nets.at(ctx.id("ram_clock.pll_outclk_1")).get();
        ASSERT_FALSE(pll->ports.count(ctx.id("outclk[1]"))); ASSERT_EQ(second->driver.cell,nullptr);
        pll->addOutput(ctx.id("outclk[1]")); pll->connectPort(ctx.id("outclk[1]"),second);
        ctx.assignArchInfo();
        for (const auto &e : ctx.cells) if (e.second->bel != BelId())
            ASSERT_TRUE(ctx.isBelLocationValid(e.second->bel)) << "Snapshot baseline illegal: " << e.first.str(&ctx);
        auto c=ctx.cells.at(ctx.id("hps_ddr.port1.slot_free_MISTRAL_ALUT3_B")).get();
        auto lab=ctx.bel_data(c->bel).lab_data.lab;
        int count=0; for (auto &alm : ctx.labs.at(lab).alms) count+=alm.unique_input_count;
        ASSERT_EQ(count,42); ASSERT_TRUE(ctx.isBelLocationValid(c->bel));
        c->type=id_MISTRAL_ALUT4; c->addInput(id_D);
        c->connectPort(id_D,ctx.cells.at(ctx.id("hps_ddr.port1.slot_free_MISTRAL_ALUT2_Q"))->getPort(id_A));
        ctx.assignArchInfo();
        count=0; for (auto &alm : ctx.labs.at(lab).alms) count+=alm.unique_input_count;
        ASSERT_EQ(count,43); ASSERT_FALSE(ctx.isBelLocationValid(c->bel));
        c->disconnectPort(id_D); c->ports.erase(id_D); c->pin_data.erase(id_D); c->type=id_MISTRAL_ALUT3;
        ctx.assignArchInfo();
        std::string prefix=::testing::TempDir()+"mistral-real-composition-"+mode;
        diagnostic_placed_composition(&ctx,prefix.c_str(),mode);
        auto selected=std::string(ctx.nameOfBel(c->bel));
        if (chosen.empty()) chosen=selected; else EXPECT_EQ(chosen,selected);
        std::cout << "Actual snapshot " << mode << " selected " << selected << "\n";
    }
}
