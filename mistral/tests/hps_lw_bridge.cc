#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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

// The bridge has a single fixed HPS site, so a cart that instantiates it
// imports without the cell: the site belongs to the shell.
TEST(HpsLwBridge, CartImportSkipsFixedBridgeSite)
{
    ArchArgs args;
    args.device = "5CSEMA5F31C6";
    auto ctx = std::make_unique<Context>(args);
    NetInfo *clock_net = ctx->createNet(ctx->id("FPGA_CLK1_50"));
    CellInfo *clock_source = ctx->createCell(ctx->id("clock_source"), ctx->id("MISTRAL_IB"));
    clock_source->addOutput(ctx->id("O"));
    clock_source->connectPort(ctx->id("O"), clock_net);

    struct TemporaryFile
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("nextpnr-hps-lw-bridge-cart-" +
                                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                      ".json");
        ~TemporaryFile()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cart;
    {
        std::ofstream file(cart.path);
        ASSERT_TRUE(file.good());
        file << R"({"modules":{"cart":{"cells":{"lw":{"type":"cyclonev_hps_interface_hps2fpga_light_weight"}}}}})";
        ASSERT_TRUE(file.good());
    }

    ctx->merge_fes_cart(cart.path.string(), "test");

    EXPECT_EQ(ctx->cells.count(ctx->id("fes_cart$lw")), 0);
    for (const auto &cell : ctx->cells)
        EXPECT_NE(cell.second->type, ctx->id("cyclonev_hps_interface_hps2fpga_light_weight"));
}
