#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

// The lightweight HPS-to-FPGA bridge is a single AXI3 master site: the
// address/write channels and the ready-to-accept signals leave the HPS, the
// slave's ready, response and read data and the bridge clock enter it.
TEST(HpsLwBridge, SingleSiteWithRoutedAxi3MasterPins)
{
    ArchArgs args;
    args.device = "5CSEMA5F31C6";
    auto ctx = std::make_unique<Context>(args);
    const IdString type = ctx->id("cyclonev_hps_interface_hps2fpga_light_weight");

    std::vector<BelId> sites;
    for (BelId bel : ctx->getBels())
        if (ctx->getBelType(bel) == type)
            sites.push_back(bel);
    ASSERT_EQ(sites.size(), 1u);
    const BelId bel = sites.front();

    int pins = 0;
    for (IdString pin : ctx->getBelPins(bel)) {
        EXPECT_NE(ctx->getBelPinWire(bel, pin), WireId()) << pin.c_str(ctx.get());
        ++pins;
    }
    // clk + AW 53 + W 51 + B 16 + AR 53 + R 49
    EXPECT_EQ(pins, 223);

    auto dir = [&](const char *pin) { return ctx->getBelPinType(bel, ctx->id(pin)); };
    EXPECT_EQ(dir("clk"), PORT_IN);
    EXPECT_EQ(dir("awaddr[20]"), PORT_OUT);
    EXPECT_EQ(dir("awvalid"), PORT_OUT);
    EXPECT_EQ(dir("awready"), PORT_IN);
    EXPECT_EQ(dir("wdata[31]"), PORT_OUT);
    EXPECT_EQ(dir("wstrb[3]"), PORT_OUT);
    EXPECT_EQ(dir("bid[11]"), PORT_IN);
    EXPECT_EQ(dir("bresp[1]"), PORT_IN);
    EXPECT_EQ(dir("bready"), PORT_OUT);
    EXPECT_EQ(dir("araddr[20]"), PORT_OUT);
    EXPECT_EQ(dir("arready"), PORT_IN);
    EXPECT_EQ(dir("rdata[31]"), PORT_IN);
    EXPECT_EQ(dir("rresp[1]"), PORT_IN);
    EXPECT_EQ(dir("rlast"), PORT_IN);
    EXPECT_EQ(dir("rready"), PORT_OUT);
}
