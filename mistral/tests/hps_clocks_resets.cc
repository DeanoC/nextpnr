#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

namespace {

struct ScopedLogCapture
{
    std::string text;
    log_write_type previous = log_write_function;

    ScopedLogCapture() { log_write_function = [&](std::string message) { text += message; }; }
    ~ScopedLogCapture() { log_write_function = std::move(previous); }
};

} // namespace

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
    // The five clock/reset outputs. The bel's other pins (reset request/acknowledge handshake) are ordinary
    // fabric-routed pins, covered in hps_tie_offs.cc.
    static const char *const clock_outputs[] = {"h2f_rst_n", "h2f_cold_rst_n", "h2f_user0_clk", "h2f_user1_clk",
                                                "h2f_user2_clk"};
    int pins = 0;
    for (const char *name : clock_outputs) {
        const IdString pin = ctx->id(name);
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

    BelId fabric_lane;
    std::vector<BelId> alternative_lanes;
    for (BelId lane : ctx->hps_clock_bels[pin]) {
        bool is_fabric = ctx->getBelType(lane) == id_MISTRAL_CLKENA && ctx->bel_data(lane).block_index == 2;
        if (is_fabric) {
            ASSERT_EQ(fabric_lane, BelId());
            fabric_lane = lane;
        } else {
            alternative_lanes.push_back(lane);
        }
    }
    ASSERT_NE(fabric_lane, BelId());
    ASSERT_FALSE(alternative_lanes.empty());

    // Fill the HPS output's alternatives with connected clock-buffer cells and
    // call the allocation pass directly. This isolates the allocation outcome
    // from setup_clock_enables(), which validates MISTRAL_CLKENA cells earlier
    // in a full pack.
    int occupied_lanes = 0;
    for (BelId lane : alternative_lanes) {
        CellInfo *occupant = ctx->createCell(ctx->idf("occupied_hps_lane_%d", occupied_lanes++), id_MISTRAL_CLKBUF);
        occupant->addInput(id_A);
        occupant->addOutput(id_Q);
        occupant->connectPort(id_A, fabric_clock);
        occupant->connectPort(id_Q, ctx->createNet(ctx->idf("occupied_hps_lane_global_%d", occupied_lanes)));
        ctx->bindBel(lane, occupant, STRENGTH_LOCKED);
        EXPECT_FALSE(ctx->checkBelAvail(lane));
    }

    bool pack_failed = false;
    ScopedLogCapture captured;
    try {
        ctx->pack_hps_clocks();
    } catch (const log_execution_error_exception &) {
        pack_failed = true;
    }
    EXPECT_TRUE(pack_failed) << captured.text;
    EXPECT_NE(captured.text.find("'hps.h2f_rst_n': no free global clock buffer on its HPS_CLOCKS slots."),
              std::string::npos)
            << captured.text;
    EXPECT_EQ(hps_buffer->bel, BelId());
    EXPECT_TRUE(ctx->checkBelAvail(fabric_lane));
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

// h2f_pending_rst_req_n is routed through the fabric: the packer gives it no
// global clock buffer, while h2f_rst_n on the same cell still gets one.
TEST(HpsClocksResets, HandshakeOutputKeepsFabricRouting)
{
    ArchArgs args;
    args.device = "5CSEMA5F31C6";
    auto ctx = std::make_unique<Context>(args);

    CellInfo *hps = ctx->createCell(ctx->id("hps"), ctx->id("cyclonev_hps_interface_clocks_resets"));
    const IdString pending = ctx->id("h2f_pending_rst_req_n");
    const IdString reset = ctx->id("h2f_rst_n");
    hps->addOutput(pending);
    hps->addOutput(reset);
    NetInfo *pending_net = ctx->createNet(ctx->id("pending"));
    NetInfo *reset_net = ctx->createNet(ctx->id("reset"));
    hps->connectPort(pending, pending_net);
    hps->connectPort(reset, reset_net);

    CellInfo *ff = ctx->createCell(ctx->id("ff"), id_MISTRAL_FF);
    ff->addInput(id_DATAIN);
    ff->addInput(id_ACLR);
    ff->connectPort(id_DATAIN, pending_net);
    ff->connectPort(id_ACLR, reset_net);

    ASSERT_NO_THROW(ctx->pack_hps_clocks());
    ASSERT_EQ(pending_net->users.entries(), 1);
    for (const auto &user : pending_net->users) {
        EXPECT_EQ(user.cell, ff);
        EXPECT_EQ(user.port, id_DATAIN);
    }
    int buffers = 0;
    for (const auto &cell : ctx->cells) {
        if (cell.second->type != id_MISTRAL_CLKBUF)
            continue;
        ++buffers;
        EXPECT_EQ(cell.second->getPort(id_A), reset_net);
        EXPECT_NE(cell.second->bel, BelId());
    }
    EXPECT_EQ(buffers, 1);
}
