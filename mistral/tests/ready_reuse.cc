#include "ready_reuse.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
USING_NEXTPNR_NAMESPACE
TEST(ReadyReuseTest, ActualEquivalenceRejectsMaskInputAndPolarityChanges)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto d = ctx.createCell(ctx.id("driver"), id_MISTRAL_ALUT3),
         r = ctx.createCell(ctx.id("replica"), id_MISTRAL_ALUT3);
    for (auto c : {d, r}) {
        c->params[id_LUT] = 0x32;
        c->addOutput(id_Q);
        c->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", c->name.c_str(&ctx))));
    }
    for (auto p : {id_A, id_B, id_C}) {
        auto n = ctx.createNet(ctx.idf("input_%s", p.c_str(&ctx)));
        for (auto c : {d, r}) {
            c->addInput(p);
            c->connectPort(p, n);
            c->pin_data[p].state = PIN_SIG;
        }
    }
    EXPECT_TRUE(ready_reuse_equivalent(d, r));
    r->params[id_LUT] = 0x33;
    EXPECT_FALSE(ready_reuse_equivalent(d, r));
    r->params[id_LUT] = 0x32;
    r->pin_data[id_A].state = PIN_INV;
    EXPECT_FALSE(ready_reuse_equivalent(d, r));
    r->pin_data[id_A].state = PIN_SIG;
    r->disconnectPort(id_A);
    r->connectPort(id_A, d->getPort(id_B));
    EXPECT_FALSE(ready_reuse_equivalent(d, r));
}
TEST(ReadyReuseTest, ClockGuardAndLocalMetadataRefresh)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto clk = ctx.createNet(ctx.id("clock"));
    clk->is_global = true;
    auto old = ctx.createNet(ctx.id("old")), copy = ctx.createNet(ctx.id("copy")),
         other = ctx.createNet(ctx.id("other"));
    auto ff = ctx.createCell(ctx.id("ff"), id_MISTRAL_FF), neighbor = ctx.createCell(ctx.id("neighbor"), id_MISTRAL_FF);
    for (auto c : {ff, neighbor}) {
        for (auto p : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            c->addInput(p);
        c->addOutput(id_Q);
        c->connectPort(id_CLK, clk);
        c->pin_data[id_ENA].state = PIN_SIG;
        c->pin_data[id_ACLR].state = PIN_1;
        c->pin_data[id_SCLR].state = PIN_0;
        c->pin_data[id_SLOAD].state = PIN_0;
    }
    ff->connectPort(id_ENA, old);
    neighbor->connectPort(id_ENA, other);
    EXPECT_FALSE(ready_reuse_clock_valid(ff, clk));
    ctx.addClock(clk->name, 130);
    EXPECT_TRUE(ready_reuse_clock_valid(ff, clk));
    auto bad = ctx.createNet(ctx.id("bad_clock"));
    EXPECT_FALSE(ready_reuse_clock_valid(ff, bad));
    ff->pin_data[id_CLK].state = PIN_INV;
    EXPECT_FALSE(ready_reuse_clock_valid(ff, clk));
    ff->pin_data[id_CLK].state = PIN_SIG;
    ctx.assignArchInfo();
    ctx.bindBel(ctx.getBelByName(IdStringList::parse(&ctx, "MISTRAL_FF.34.22.14")), ff, STRENGTH_WEAK);
    ctx.bindBel(ctx.getBelByName(IdStringList::parse(&ctx, "MISTRAL_FF.34.22.20")), neighbor, STRENGTH_WEAK);
    ASSERT_TRUE(ctx.isBelLocationValid(ff->bel));
    auto bel = ff->bel;
    ready_reuse_move(&ctx, ff, copy);
    EXPECT_EQ(ff->getPort(id_ENA), copy);
    EXPECT_EQ(ff->ffInfo.ctrlset.ena.net, copy);
    EXPECT_EQ(ff->bel, bel);
    EXPECT_EQ(ff->get_pin_state(id_ENA), PIN_SIG);
    EXPECT_EQ(neighbor->ffInfo.ctrlset.ena.net, other);
    EXPECT_TRUE(ctx.isBelLocationValid(ff->bel));
    EXPECT_EQ(old->users.entries(), 0);
    EXPECT_EQ(copy->users.entries(), 1);
}
TEST(ReadyReuseTest, DisabledIsIdentifierNeutral)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto before = ctx.id("before");
    diagnostic_ready_reuse(&ctx, nullptr);
    diagnostic_ready_reuse(&ctx, "");
    diagnostic_ready_reuse_timing(&ctx, nullptr);
    diagnostic_ready_reuse_timing(&ctx, "");
    EXPECT_EQ(ctx.id("after").index, before.index + 1);
}
