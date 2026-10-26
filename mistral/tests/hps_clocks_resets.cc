#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

// The HPS reset and user clock outputs have no routing node; each reaches
// global clock buffers through one fixed HPS_CLOCKS slot per clock mux.
TEST(HpsClocksResets, EveryOutputReachesAGlobalThroughItsOwnSlot)
{
    ArchArgs args;
    args.device = "5CSEMA5F31C6";
    auto ctx = std::make_unique<Context>(args);
    const BelId bel = ctx->hps_clocks_resets_bel;
    ASSERT_NE(bel, BelId());

    std::map<int, IdString> slot_owner;
    std::map<IdString, std::set<std::pair<int, int>>> selects; // pin -> {slot, INPUT_SEL value}
    int pins = 0;
    for (IdString pin : ctx->getBelPins(bel)) {
        ++pins;
        EXPECT_EQ(ctx->getBelPinType(bel, pin), PORT_OUT) << pin.c_str(ctx.get());
        EXPECT_FALSE(ctx->hps_clock_bels[pin].empty()) << pin.c_str(ctx.get()) << " reaches no clock buffer";
        WireId wire = ctx->getBelPinWire(bel, pin);
        for (PipId pip : ctx->getPipsDownhill(wire)) {
            ASSERT_TRUE(ctx->hps_clock_select.count(pip));
            auto select = ctx->hps_clock_select.at(pip);
            selects[pin].insert(select);
            auto owner = slot_owner.emplace(select.first, pin);
            EXPECT_EQ(owner.first->second, pin) << "slot " << select.first << " shared";
        }
    }
    EXPECT_EQ(pins, 5);

    // INPUT_SEL value v selects CLKIN(3 - v): the warm reset is CLKIN1 of
    // slot 9 on the horizontal global mux, as in vendor-built bitstreams.
    EXPECT_TRUE(selects[ctx->id("h2f_rst_n")].count({9, 2}));
    EXPECT_TRUE(selects[ctx->id("h2f_cold_rst_n")].count({5, 1}));
}

TEST(HpsClocksResets, PackerPrefersOtherLanesForHpsOutputs)
{
    ArchArgs args;
    args.device = "5CSEMA5F31C6";
    auto ctx = std::make_unique<Context>(args);

    const IdString pin = ctx->id("h2f_rst_n");
    CellInfo *hps = ctx->createCell(ctx->id("hps"), ctx->id("cyclonev_hps_interface_clocks_resets"));
    hps->addOutput(pin);
    NetInfo *clock = ctx->createNet(ctx->id("hps_clock"));
    hps->connectPort(pin, clock);

    CellInfo *buffer = ctx->createCell(ctx->id("hps_clock_buffer"), id_MISTRAL_CLKBUF);
    buffer->addInput(id_A);
    buffer->addOutput(id_Q);
    buffer->connectPort(id_A, clock);
    buffer->connectPort(id_Q, ctx->createNet(ctx->id("hps_global_clock")));

    bool has_fabric_lane = false;
    bool has_alternative = false;
    for (BelId lane : ctx->hps_clock_bels[pin]) {
        bool fabric_lane = ctx->getBelType(lane) == id_MISTRAL_CLKENA && ctx->bel_data(lane).block_index == 2;
        has_fabric_lane |= fabric_lane && ctx->checkBelAvail(lane);
        has_alternative |= !fabric_lane && ctx->checkBelAvail(lane);
    }
    ASSERT_TRUE(has_fabric_lane);
    ASSERT_TRUE(has_alternative);

    ASSERT_TRUE(ctx->pack());
    ASSERT_NE(buffer->bel, BelId());
    EXPECT_FALSE(ctx->getBelType(buffer->bel) == id_MISTRAL_CLKENA && ctx->bel_data(buffer->bel).block_index == 2);
}

TEST(HpsClocksResets, PackerReservesFabricLaneWhenOtherHpsLanesAreOccupied)
{
    ArchArgs args;
    args.device = "5CSEMA5F31C6";
    auto ctx = std::make_unique<Context>(args);

    const IdString pin = ctx->id("h2f_rst_n");
    CellInfo *hps = ctx->createCell(ctx->id("hps"), ctx->id("cyclonev_hps_interface_clocks_resets"));
    hps->addOutput(pin);
    NetInfo *hps_clock = ctx->createNet(ctx->id("hps_clock"));
    hps->connectPort(pin, hps_clock);

    CellInfo *hps_buffer = ctx->createCell(ctx->id("hps_clock_buffer"), id_MISTRAL_CLKBUF);
    hps_buffer->addInput(id_A);
    hps_buffer->addOutput(id_Q);
    hps_buffer->connectPort(id_A, hps_clock);
    hps_buffer->connectPort(id_Q, ctx->createNet(ctx->id("hps_global_clock")));

    CellInfo *fabric_source = ctx->createCell(ctx->id("fabric_clock_source"), id_MISTRAL_NOT);
    fabric_source->addInput(id_A);
    fabric_source->addOutput(id_Q);
    fabric_source->connectPort(id_A, ctx->createNet(ctx->id("fabric_clock_source_input")));
    NetInfo *fabric_clock = ctx->createNet(ctx->id("fabric_clock"));
    fabric_source->connectPort(id_Q, fabric_clock);
    CellInfo *fabric_buffer = ctx->createCell(ctx->id("fabric_clock_buffer"), id_MISTRAL_CLKBUF);
    fabric_buffer->addInput(id_A);
    fabric_buffer->addOutput(id_Q);
    fabric_buffer->connectPort(id_A, fabric_clock);
    fabric_buffer->connectPort(id_Q, ctx->createNet(ctx->id("fabric_global_clock")));

    std::vector<BelId> fabric_lanes;
    int occupied_lanes = 0;
    for (BelId lane : ctx->hps_clock_bels[pin]) {
        bool fabric_lane = ctx->getBelType(lane) == id_MISTRAL_CLKENA && ctx->bel_data(lane).block_index == 2;
        if (fabric_lane) {
            fabric_lanes.push_back(lane);
            continue;
        }
        CellInfo *occupant = ctx->createCell(ctx->idf("occupied_hps_lane_%d", occupied_lanes++),
                                             ctx->getBelType(lane));
        ctx->bindBel(lane, occupant, STRENGTH_LOCKED);
    }
    ASSERT_GT(occupied_lanes, 0);
    ASSERT_FALSE(fabric_lanes.empty());

    EXPECT_THROW(ctx->pack(), log_execution_error_exception);
    EXPECT_EQ(hps_buffer->bel, BelId());
    for (BelId lane : fabric_lanes)
        EXPECT_TRUE(ctx->checkBelAvail(lane));
}

// The HPS reset and user clock outputs come from a single fixed HPS site, so
// a cart that instantiates it imports without the cell: the site belongs to
// the shell.
TEST(HpsClocksResets, CartImportSkipsFixedSite)
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
                                     ("nextpnr-hps-clocks-resets-cart-" +
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
        file << R"({"modules":{"cart":{"cells":{"hps_cr":{"type":"cyclonev_hps_interface_clocks_resets"}}}}})";
        ASSERT_TRUE(file.good());
    }

    ctx->merge_fes_cart(cart.path.string(), "test");

    EXPECT_EQ(ctx->cells.count(ctx->id("fes_cart$hps_cr")), 0);
    for (const auto &cell : ctx->cells)
        EXPECT_NE(cell.second->type, ctx->id("cyclonev_hps_interface_clocks_resets"));
}
