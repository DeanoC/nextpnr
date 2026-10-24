#include <memory>
#include <mutex>

#include "gtest/gtest.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

// fes_slot_pin_narrow on a synthetic graph: a cart pin at (25,1) whose way
// out is a wide component of more wires than the search explores.
class FesNarrowPinTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *net, *frozen;
    WireId pin, wide;
    int serial = 0;

    WireId wire(int x, int y) { return ctx->add_wire(x, y, ctx->idf("narrow_test_%d", serial++)); }

    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        pin = wire(25, 1);
        wide = wire(26, 1);
        for (int i = 0; i < 2100; i++)
            ctx->add_pip(wide, wire(26 + i % 4, 2 + i / 4 % 30));
        net = ctx->createNet(ctx->id("cart_net"));
        frozen = ctx->createNet(ctx->id("shell_net"));
        auto cell = [&](const char *name, PortType direction, WireId at) {
            auto id = ctx->id(name);
            auto port = ctx->id("pin");
            ctx->createRegionPlug(id, ctx->id("test_plug"), Loc());
            ctx->addPlugPin(id, port, direction, at);
            auto ci = ctx->cells.at(id).get();
            ci->attrs[id_FES_SLOT] = std::string("cart");
            ci->connectPort(port, net);
        };
        cell("driver", PORT_OUT, pin);
        cell("user", PORT_IN, wire(27, 9));
        ctx->note_fes_cram_region("1769,32,2806,7024");
        ctx->fes_fence_active = true;
    }

    bool narrow()
    {
        std::lock_guard<std::mutex> guard(ctx->fes_narrow_mutex);
        ctx->fes_narrow_cache.clear();
        return ctx->fes_slot_pin_narrow(pin, net, true);
    }

    // A routing wire at (24,1) that other tiles can drive too.
    WireId shared_wire()
    {
        auto w = wire(24, 1);
        ctx->add_pip(wire(20, 5), w);
        return w;
    }
};

TEST_F(FesNarrowPinTest, WalledInPinIsNarrow)
{
    auto dead_end = wire(24, 1);
    ctx->add_pip(pin, dead_end);
    ctx->add_pip(dead_end, wire(23, 1));
    EXPECT_TRUE(narrow());
}

TEST_F(FesNarrowPinTest, SingleSharedExitIsNarrow)
{
    auto cut = shared_wire();
    ctx->add_pip(pin, cut);
    ctx->add_pip(cut, wide);
    EXPECT_TRUE(narrow());
}

TEST_F(FesNarrowPinTest, TwoExitsAreNotNarrow)
{
    for (int i = 0; i < 2; i++) {
        auto exit = shared_wire();
        ctx->add_pip(pin, exit);
        ctx->add_pip(exit, wide);
    }
    EXPECT_FALSE(narrow());
}

TEST_F(FesNarrowPinTest, FrozenShellRouteLeavesOneExit)
{
    WireId exits[2];
    for (auto &exit : exits) {
        exit = shared_wire();
        ctx->add_pip(pin, exit);
        ctx->add_pip(exit, wide);
    }
    ctx->bindWire(exits[0], frozen, STRENGTH_LOCKED);
    EXPECT_TRUE(narrow());
}

TEST_F(FesNarrowPinTest, MovableRoutingIsIgnored)
{
    WireId exits[2];
    for (auto &exit : exits) {
        exit = shared_wire();
        ctx->add_pip(pin, exit);
        ctx->add_pip(exit, wide);
    }
    ctx->bindWire(exits[0], frozen, STRENGTH_STRONG);
    EXPECT_FALSE(narrow());
}

TEST_F(FesNarrowPinTest, PinOwnWireIsNotACut)
{
    // A block output wire only its own tile drives belongs to this pin.
    auto own = wire(25, 1);
    ctx->add_pip(pin, own);
    ctx->add_pip(own, wide);
    EXPECT_FALSE(narrow());
}

TEST_F(FesNarrowPinTest, FrozenShellOwnsStartingPin)
{
    // The wide component would count as an escape. The pin wire is already
    // a frozen shell route, so the cart net cannot bind it.
    ctx->add_pip(pin, wide);
    ctx->bindWire(pin, frozen, STRENGTH_LOCKED);
    EXPECT_TRUE(narrow());
}

TEST_F(FesNarrowPinTest, CandidateNetOwnsStartingPin)
{
    ctx->add_pip(pin, wide);
    ctx->bindWire(pin, net, STRENGTH_LOCKED);
    EXPECT_FALSE(narrow());
}

TEST_F(FesNarrowPinTest, PlacerStrengthOnStartingPinIsIgnored)
{
    ctx->add_pip(pin, wide);
    ctx->bindWire(pin, frozen, STRENGTH_PLACER);
    EXPECT_FALSE(narrow());
}

TEST_F(FesNarrowPinTest, CarryChainOutputIsNotACheckedPin)
{
    // A carry output wire reaches only the next ALM's carry input, which
    // the search sees as a dead end. The chain is one placement cluster.
    BelId bel;
    for (BelId candidate : ctx->getBels())
        if (ctx->getBelType(candidate) == id_MISTRAL_COMB) {
            bel = candidate;
            break;
        }
    ASSERT_NE(bel, BelId());
    auto carry = ctx->createNet(ctx->id("carry"));
    auto arith = [&](const char *name) {
        auto ci = ctx->createCell(ctx->id(name), id_MISTRAL_ALUT_ARITH);
        ci->attrs[id_FES_SLOT] = std::string("cart");
        return ci;
    };
    auto first = arith("first"), next = arith("next");
    first->addOutput(id_CO);
    first->connectPort(id_CO, carry);
    first->pin_data[id_CO].bel_pins = {id_CO};
    next->addInput(id_CI);
    next->connectPort(id_CI, carry);
    {
        std::lock_guard<std::mutex> guard(ctx->fes_narrow_mutex);
        EXPECT_TRUE(ctx->fes_slot_pin_narrow(ctx->getBelPinWire(bel, id_CO), carry, true));
    }
    EXPECT_TRUE(ctx->fes_slot_pins_escape(bel, first, false));
}
