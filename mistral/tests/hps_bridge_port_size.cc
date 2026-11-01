#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

// The full HPS-to-FPGA and FPGA-to-HPS bridges each occupy a single fixed
// HPS site with a full AXI3 port. Constant inputs such as PORT_SIZE_CONFIG
// are tied off through their inversion bits (hps_tie_offs.cc), so every pin
// must map to a wire with the direction of the device's pin map.
TEST(HpsBridgePortSize, SingleSiteWithFullAxi3Pins)
{
    ArchArgs args;
    args.device = "5CSEMA5F31C6";
    auto ctx = std::make_unique<Context>(args);

    struct Case
    {
        const char *type;
        int pins;
        std::vector<std::pair<const char *, PortType>> dirs;
    };
    const Case cases[] = {
            {
                    "cyclonev_hps_interface_hps2fpga",
                    447,
                    {
                            {"clk", PORT_IN},
                            {"awid[11]", PORT_OUT},
                            {"awaddr[29]", PORT_OUT},
                            {"awvalid", PORT_OUT},
                            {"awready", PORT_IN},
                            {"wdata[127]", PORT_OUT},
                            {"wstrb[15]", PORT_OUT},
                            {"bid[11]", PORT_IN},
                            {"bresp[1]", PORT_IN},
                            {"bready", PORT_OUT},
                            {"arid[11]", PORT_OUT},
                            {"araddr[29]", PORT_OUT},
                            {"arready", PORT_IN},
                            {"rid[11]", PORT_IN},
                            {"rdata[127]", PORT_IN},
                            {"rresp[1]", PORT_IN},
                            {"rlast", PORT_IN},
                            {"rvalid", PORT_IN},
                            {"rready", PORT_OUT},
                            {"port_size_config[1]", PORT_IN},
                    },
            },
            {
                    "cyclonev_hps_interface_fpga2hps",
                    441,
                    {
                            {"clk", PORT_IN},
                            {"awid[7]", PORT_IN},
                            {"awaddr[31]", PORT_IN},
                            {"awready", PORT_OUT},
                            {"wdata[127]", PORT_IN},
                            {"wready", PORT_OUT},
                            {"bid[7]", PORT_OUT},
                            {"bresp[1]", PORT_OUT},
                            {"bready", PORT_IN},
                            {"arid[7]", PORT_IN},
                            {"araddr[31]", PORT_IN},
                            {"arready", PORT_OUT},
                            {"rid[7]", PORT_OUT},
                            {"rdata[127]", PORT_OUT},
                            {"rresp[1]", PORT_OUT},
                            {"rready", PORT_IN},
                            {"port_size_config[1]", PORT_IN},
                    },
            },
    };
    for (const Case &c : cases) {
        const IdString type = ctx->id(c.type);

        std::vector<BelId> sites;
        for (BelId bel : ctx->getBels())
            if (ctx->getBelType(bel) == type)
                sites.push_back(bel);
        ASSERT_EQ(sites.size(), 1u) << c.type;
        const BelId bel = sites.front();

        int pins = 0;
        for (IdString pin : ctx->getBelPins(bel)) {
            EXPECT_NE(ctx->getBelPinWire(bel, pin), WireId()) << pin.c_str(ctx.get());
            ++pins;
        }
        EXPECT_EQ(pins, c.pins) << c.type;

        auto dir = [&](const char *pin) { return ctx->getBelPinType(bel, ctx->id(pin)); };
        for (const auto &d : c.dirs)
            EXPECT_EQ(dir(d.first), d.second) << c.type << " " << d.first;
    }
}

// The full bridges have a single fixed HPS site each, so a cart that
// instantiates them imports without the cells: the sites belong to the
// shell, not to the user design.
TEST(HpsBridgePortSize, CartImportSkipsFixedBridgeSites)
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
                                     ("nextpnr-hps-bridge-port-size-cart-" +
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
        file << R"({"modules":{"cart":{"cells":{"h2f":{"type":"cyclonev_hps_interface_hps2fpga"},"f2h":{"type":"cyclonev_hps_interface_fpga2hps"}}}}})";
        ASSERT_TRUE(file.good());
    }

    ctx->merge_fes_cart(cart.path.string(), "test");

    EXPECT_EQ(ctx->cells.count(ctx->id("fes_cart$h2f")), 0);
    EXPECT_EQ(ctx->cells.count(ctx->id("fes_cart$f2h")), 0);
    const IdString h2f_type = ctx->id("cyclonev_hps_interface_hps2fpga");
    const IdString f2h_type = ctx->id("cyclonev_hps_interface_fpga2hps");
    for (const auto &cell : ctx->cells)
        EXPECT_NE(cell.second->type, h2f_type) << cell.first.c_str(ctx.get());
    for (const auto &cell : ctx->cells)
        EXPECT_NE(cell.second->type, f2h_type) << cell.first.c_str(ctx.get());
}
