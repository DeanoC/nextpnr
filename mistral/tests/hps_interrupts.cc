#include <memory>
#include <set>
#include <vector>

#include "gtest/gtest.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

namespace {

std::unique_ptr<Context> make_context()
{
    ArchArgs args;
    args.device = "5CSEMA5F31C6";
    return std::make_unique<Context>(args);
}

BelId interrupts_bel(Context *ctx)
{
    const IdString type = ctx->id("cyclonev_hps_interface_interrupts");
    std::vector<BelId> sites;
    for (BelId bel : ctx->getBels())
        if (ctx->getBelType(bel) == type)
            sites.push_back(bel);
    return sites.size() == 1 ? sites.front() : BelId();
}

// rnode indices of the IRQ inputs whose inverter bitgen set
std::set<CycloneV::rnode_index> inverted(Context *ctx)
{
    std::set<CycloneV::rnode_index> result;
    for (const auto &inv : ctx->cyclonev->inv_get())
        if (inv.value)
            result.insert(inv.node);
    return result;
}

} // namespace

// The 64 FPGA-to-HPS interrupts are one site of input pins.
TEST(HpsInterrupts, SingleSiteWith64Inputs)
{
    auto ctx = make_context();
    const BelId bel = interrupts_bel(ctx.get());
    ASSERT_NE(bel, BelId());
    int pins = 0;
    for (IdString pin : ctx->getBelPins(bel)) {
        EXPECT_EQ(ctx->getBelPinType(bel, pin), PORT_IN) << pin.c_str(ctx.get());
        EXPECT_NE(ctx->getBelPinWire(bel, pin), WireId()) << pin.c_str(ctx.get());
        ++pins;
    }
    EXPECT_EQ(pins, 64);
}

// An undriven HPS input reads 1, so bitgen inverts every IRQ input that no
// net reaches; a routed line keeps its true polarity.
TEST(HpsInterrupts, UndrivenLinesAreTiedLow)
{
    auto ctx = make_context();
    const BelId bel = interrupts_bel(ctx.get());
    ASSERT_NE(bel, BelId());

    ctx->configure_bitstream();
    auto inv = inverted(ctx.get());
    for (int i = 0; i < 64; i++) {
        WireId wire = ctx->getBelPinWire(bel, ctx->idf("irq[%d]", i));
        EXPECT_TRUE(inv.count(ctx->cyclonev->rc2ri(wire.node))) << "irq[" << i << "] not tied low";
    }

    WireId irq0 = ctx->getBelPinWire(bel, ctx->id("irq[0]"));
    NetInfo *net = ctx->createNet(ctx->id("irq0"));
    ctx->bindWire(irq0, net, STRENGTH_WEAK);
    ctx->configure_bitstream();
    inv = inverted(ctx.get());
    EXPECT_FALSE(inv.count(ctx->cyclonev->rc2ri(irq0.node))) << "routed irq[0] must not be inverted";
    WireId irq1 = ctx->getBelPinWire(bel, ctx->id("irq[1]"));
    EXPECT_TRUE(inv.count(ctx->cyclonev->rc2ri(irq1.node)));
}
