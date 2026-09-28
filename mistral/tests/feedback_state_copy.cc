#include "gtest/gtest.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN
CellInfo *feedback_state_copy_into_lab(Context *, CellInfo *, const std::vector<PortRef> &, const char *);
void diagnostic_feedback_state_copy(Context *, const char *);
NEXTPNR_NAMESPACE_END
USING_NEXTPNR_NAMESPACE

namespace {
CellInfo *make_ff(Context &ctx, const char *name, NetInfo *clock, NetInfo *data)
{
    auto ff = ctx.createCell(ctx.id(name), id_MISTRAL_FF);
    for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
        ff->addInput(pin);
    ff->addOutput(id_Q);
    ff->connectPort(id_CLK, clock);
    ff->connectPort(id_DATAIN, data);
    ff->connectPort(id_Q, ctx.createNet(ctx.idf("%s$q", name)));
    ff->pin_data[id_ENA].state = PIN_1;
    ff->pin_data[id_ACLR].state = PIN_1;
    ff->pin_data[id_SCLR].state = PIN_0;
    ff->pin_data[id_SLOAD].state = PIN_0;
    ff->pin_data[id_SDATA].state = PIN_0;
    return ff;
}
CellInfo *make_sink(Context &ctx, const char *name, NetInfo *input)
{
    auto lut = ctx.createCell(ctx.id(name), id_MISTRAL_ALUT5);
    lut->params[id_LUT] = 0xaaaa;
    for (auto pin : {id_A, id_B, id_C, id_D, id_E}) {
        lut->addInput(pin);
        lut->pin_data[pin].state = PIN_0;
    }
    lut->connectPort(id_A, input);
    lut->pin_data[id_A].state = PIN_SIG;
    lut->addOutput(id_Q);
    lut->connectPort(id_Q, ctx.createNet(ctx.idf("%s$q", name)));
    return lut;
}
}

TEST(FeedbackStateCopyTest, CopiesStateAndSelectedConsumersWithoutMovingExistingCells)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    ctx.createNet(ctx.id("$PACKER_GND_NET"));
    ctx.createNet(ctx.id("$PACKER_VCC_NET"));
    auto clock = ctx.createNet(ctx.id("clock"));
    auto data = ctx.createNet(ctx.id("data"));
    auto original = make_ff(ctx, "guard", clock, data);
    auto moved_a = make_sink(ctx, "moved_a", original->getPort(id_Q));
    auto moved_b = make_sink(ctx, "moved_b", original->getPort(id_Q));
    auto retained = make_sink(ctx, "retained", original->getPort(id_Q));
    ctx.assignArchInfo();
    auto old_bel = ctx.getBelByLocation(Loc(39, 32, 58));
    ASSERT_NE(old_bel, BelId());
    ctx.bindBel(old_bel, original, STRENGTH_WEAK);
    ASSERT_TRUE(ctx.isBelLocationValid(old_bel));
    auto old_output = original->getPort(id_Q);
    auto copy = feedback_state_copy_into_lab(&ctx, original, {{moved_a, id_A}, {moved_b, id_A}}, "$local_copy");
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(original->bel, old_bel);
    EXPECT_EQ(ctx.getBelLocation(copy->bel).x, 39);
    EXPECT_EQ(ctx.getBelLocation(copy->bel).y, 32);
    EXPECT_EQ(copy->getPort(id_CLK), clock);
    EXPECT_EQ(copy->getPort(id_DATAIN), data);
    EXPECT_EQ(copy->get_pin_state(id_ACLR), original->get_pin_state(id_ACLR));
    EXPECT_EQ(copy->get_pin_state(id_ENA), original->get_pin_state(id_ENA));
    EXPECT_EQ(moved_a->getPort(id_A), copy->getPort(id_Q));
    EXPECT_EQ(moved_b->getPort(id_A), copy->getPort(id_Q));
    EXPECT_EQ(retained->getPort(id_A), old_output);
    EXPECT_EQ(old_output->users.entries(), 1);
    EXPECT_EQ(copy->getPort(id_Q)->users.entries(), 2);
    EXPECT_TRUE(ctx.isBelLocationValid(old_bel));
    EXPECT_TRUE(ctx.isBelLocationValid(copy->bel));
}

TEST(FeedbackStateCopyTest, MiswiredSelectionLeavesContextUnchanged)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    ctx.createNet(ctx.id("$PACKER_GND_NET"));
    ctx.createNet(ctx.id("$PACKER_VCC_NET"));
    auto original = make_ff(ctx, "guard", ctx.createNet(ctx.id("clock")), ctx.createNet(ctx.id("data")));
    auto other = ctx.createNet(ctx.id("other"));
    auto sink = make_sink(ctx, "sink", other);
    ctx.assignArchInfo();
    ctx.bindBel(ctx.getBelByLocation(Loc(39, 32, 58)), original, STRENGTH_WEAK);
    auto cell_count = ctx.cells.size(), net_count = ctx.nets.size();
    EXPECT_EQ(feedback_state_copy_into_lab(&ctx, original, {{sink, id_A}}, "$local_copy"), nullptr);
    EXPECT_EQ(ctx.cells.size(), cell_count);
    EXPECT_EQ(ctx.nets.size(), net_count);
    EXPECT_EQ(sink->getPort(id_A), other);
}

TEST(FeedbackStateCopyTest, DisabledModeDoesNotAllocateIdentifiers)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    auto before = ctx.id("before");
    diagnostic_feedback_state_copy(&ctx, nullptr);
    diagnostic_feedback_state_copy(&ctx, "");
    EXPECT_EQ(ctx.id("after").index, before.index + 1);
}
