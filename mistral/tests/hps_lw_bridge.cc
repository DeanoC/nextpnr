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

TEST(HpsLwBridge, UncharacterizedAxiTimingIsReportedAsUncovered)
{
    ArchArgs args;
    args.device = "5CSEMA5F31C6";
    auto ctx = std::make_unique<Context>(args);
    const IdString type = ctx->id("cyclonev_hps_interface_hps2fpga_light_weight");
    BelId bel;
    for (BelId candidate : ctx->getBels())
        if (ctx->getBelType(candidate) == type) {
            ASSERT_EQ(bel, BelId()) << "multiple lightweight bridge sites";
            bel = candidate;
        }
    ASSERT_NE(bel, BelId());

    CellInfo *bridge = ctx->createCell(ctx->id("lw"), type);
    for (IdString pin : ctx->getBelPins(bel)) {
        const PortType direction = ctx->getBelPinType(bel, pin);
        if (direction == PORT_IN)
            bridge->addInput(pin);
        else if (direction == PORT_OUT)
            bridge->addOutput(pin);
        else
            FAIL() << "unexpected bidirectional HPS bridge pin " << pin.c_str(ctx.get());

        int clocks = -1;
        const TimingPortClass expected = pin == ctx->id("clk") ? TMG_CLOCK_INPUT
                                         : direction == PORT_IN ? TMG_ENDPOINT
                                                                : TMG_STARTPOINT;
        EXPECT_EQ(ctx->getPortTimingClass(bridge, pin, clocks), expected) << pin.c_str(ctx.get());
        EXPECT_EQ(clocks, 0) << pin.c_str(ctx.get());
    }

    NetInfo *clock = ctx->createNet(ctx->id("clock"));
    PortInfo clock_port;
    clock_port.name = ctx->id("clock");
    clock_port.type = PORT_IN;
    clock_port.net = clock;
    ctx->ports[clock_port.name] = clock_port;
    ctx->addClock(clock_port.name, 50.0);
    bridge->connectPort(ctx->id("clk"), clock);

    auto make_ff = [&](const char *name, NetInfo *q_net) {
        CellInfo *ff = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        ff->addInput(id_CLK);
        ff->addInput(id_DATAIN);
        ff->addOutput(id_Q);
        ff->connectPort(id_CLK, clock);
        ff->connectPort(id_Q, q_net);
        return ff;
    };
    NetInfo *fabric_to_bridge = ctx->createNet(ctx->id("fabric_to_bridge"));
    NetInfo *bridge_to_fabric = ctx->createNet(ctx->id("bridge_to_fabric"));
    NetInfo *captured_q = ctx->createNet(ctx->id("captured_q"));
    make_ff("launch", fabric_to_bridge);
    CellInfo *capture = make_ff("capture", captured_q);
    bridge->connectPort(ctx->id("awready"), fabric_to_bridge);
    bridge->connectPort(ctx->id("awaddr[0]"), bridge_to_fabric);
    capture->connectPort(id_DATAIN, bridge_to_fabric);
    ctx->settings[ctx->id("target_freq")] = 1e8;

    struct WarningCapture
    {
        std::ostringstream stream;
        WarningCapture() { log_streams.emplace_back(&stream, LogLevel::WARNING_MSG); }
        ~WarningCapture() { log_streams.pop_back(); }
    } warnings;
    TimingAnalyser timing(ctx.get());
    timing.setup(true, false, false);
    EXPECT_NE(warnings.stream.str().find("Timing for HPS lightweight bridge"), std::string::npos);
    EXPECT_NE(warnings.stream.str().find("is not covered"), std::string::npos);

    const auto &reported = timing.get_timing_result().detailed_net_timings;
    auto has_uncovered_sink = [&](IdString net, CellInfo *sink, IdString pin) {
        auto found = reported.find(net);
        if (found == reported.end())
            return false;
        for (const auto &row : found->second)
            if (row.cell_port == std::make_pair(sink->name, pin) && !row.setup_checked && !row.hold_checked)
                return true;
        return false;
    };
    EXPECT_TRUE(has_uncovered_sink(fabric_to_bridge->name, bridge, ctx->id("awready")))
            << "fabric-to-HPS path must remain visible without setup/hold coverage";
    EXPECT_TRUE(has_uncovered_sink(bridge_to_fabric->name, capture, id_DATAIN))
            << "HPS-to-fabric path must remain visible without setup/hold coverage";
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
