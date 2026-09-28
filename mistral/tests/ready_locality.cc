#include "ready_locality.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
USING_NEXTPNR_NAMESPACE
namespace {
CellInfo *lut(Context &ctx, const char *name, int z)
{
    auto c = ctx.createCell(ctx.id(name), id_MISTRAL_ALUT5);
    c->params[id_LUT] = 0x12345678;
    for (auto pin : {id_A, id_B, id_C, id_D, id_E}) {
        c->addInput(pin);
        c->connectPort(pin, ctx.createNet(ctx.idf("%s_%s", name, pin.c_str(&ctx))));
    }
    c->addOutput(id_Q);
    c->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", name)));
    ctx.assign_comb_info(c);
    ctx.bindBel(ctx.getBelByLocation(Loc(30, 20, z)), c, STRENGTH_WEAK);
    return c;
}
} // namespace
TEST(ReadyLocalityTest, AtomicTranslationAndRestoration)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto x = lut(ctx, "x", 0), y = lut(ctx, "y", 6);
    auto xb = x->bel, yb = y->bel;
    bool observed = false;
    EXPECT_TRUE(ready_locality_trial(
            &ctx, {{x, ctx.getBelByLocation(Loc(30, 30, 0))}, {y, ctx.getBelByLocation(Loc(30, 30, 6))}},
            [&](bool legal) {
                observed = true;
                EXPECT_TRUE(legal);
                EXPECT_EQ(ctx.getBelLocation(x->bel).y, 30);
                EXPECT_EQ(ctx.getBelLocation(y->bel).y, 30);
            }));
    EXPECT_TRUE(observed);
    EXPECT_EQ(x->bel, xb);
    EXPECT_EQ(y->bel, yb);
    EXPECT_EQ(x->belStrength, STRENGTH_WEAK);
    EXPECT_TRUE(ctx.isBelLocationValid(xb));
}
TEST(ReadyLocalityTest, IllegalCompletePlacementAndExceptionsAlwaysRestore)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto x = lut(ctx, "x", 0), y = lut(ctx, "y", 6);
    auto xb = x->bel, yb = y->bel;
    bool observed = false;
    EXPECT_FALSE(ready_locality_trial(
            &ctx, {{x, ctx.getBelByLocation(Loc(30, 30, 0))}, {y, ctx.getBelByLocation(Loc(30, 30, 1))}},
            [&](bool legal) {
                observed = true;
                EXPECT_FALSE(legal);
                EXPECT_FALSE(ctx.isBelLocationValid(x->bel));
            }));
    EXPECT_TRUE(observed);
    EXPECT_EQ(x->bel, xb);
    EXPECT_EQ(y->bel, yb);
    EXPECT_TRUE(ctx.isBelLocationValid(xb));
    EXPECT_THROW(ready_locality_trial(&ctx, {{x, ctx.getBelByLocation(Loc(30, 30, 0))}},
                                      [](bool) { throw std::runtime_error("test"); }),
                 std::runtime_error);
    EXPECT_EQ(x->bel, xb);
    EXPECT_EQ(y->bel, yb);
    auto occupied = ctx.getBelByLocation(Loc(30, 20, 6));
    EXPECT_THROW(ready_locality_trial(&ctx, {{x, occupied}}, [](bool) {}), std::runtime_error);
    EXPECT_EQ(x->bel, xb);
    EXPECT_EQ(y->bel, yb);
}
TEST(ReadyLocalityTest, DisabledIsIdentifierNeutral)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto before = ctx.id("before");
    diagnostic_ready_locality(&ctx, nullptr);
    diagnostic_ready_locality(&ctx, "");
    EXPECT_EQ(ctx.id("after").index, before.index + 1);
}
