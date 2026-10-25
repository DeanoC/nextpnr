#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "gtest/gtest.h"
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
