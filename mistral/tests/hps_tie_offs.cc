// HPS interface fabric inputs: the boot / debug-APB / trace BELs, the clocks_resets reset-handshake pins, and how
// bitgen encodes constant and driven HPS inputs. An unrouted HPS input reads 1; its inversion bit turns that into
// 0. The inversion defaults differ per block (the boot-select inputs default to inverted), so bitgen writes the bit
// for every input of a placed HPS cell.

#include <memory>
#include <string>
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

BelId only_bel(Context *ctx, const char *type)
{
    const IdString id = ctx->id(type);
    std::vector<BelId> sites;
    for (BelId bel : ctx->getBels())
        if (ctx->getBelType(bel) == id)
            sites.push_back(bel);
    return sites.size() == 1 ? sites.front() : BelId();
}

struct Inv
{
    bool found = false, value = false, is_default = false;
};

// Inversion bit behind a BEL input pin.
Inv inversion(Context *ctx, BelId bel, IdString pin)
{
    Inv result;
    WireId wire = ctx->getBelPinWire(bel, pin);
    if (wire == WireId())
        return result;
    const auto ri = ctx->cyclonev->rc2ri(wire.node);
    for (const auto &inv : ctx->cyclonev->inv_get())
        if (inv.node == ri) {
            result.found = true;
            result.value = inv.value;
            result.is_default = inv.def;
        }
    return result;
}

void expect_pins(Context *ctx, BelId bel, const char *name, PortType dir, int count)
{
    for (int bit = 0; bit < std::max(count, 1); bit++) {
        IdString pin = count == 0 ? ctx->id(name) : ctx->idf("%s[%d]", name, bit);
        EXPECT_EQ(ctx->getBelPinType(bel, pin), dir) << pin.c_str(ctx);
        EXPECT_NE(ctx->getBelPinWire(bel, pin), WireId()) << pin.c_str(ctx);
    }
}

} // namespace

TEST(HpsFabricBels, BootFromFpgaInputs)
{
    auto ctx = make_context();
    const BelId bel = only_bel(ctx.get(), "cyclonev_hps_interface_boot_from_fpga");
    ASSERT_NE(bel, BelId());
    expect_pins(ctx.get(), bel, "boot_from_fpga_ready", PORT_IN, 0);
    expect_pins(ctx.get(), bel, "boot_from_fpga_on_failure", PORT_IN, 0);
    expect_pins(ctx.get(), bel, "bsel_en", PORT_IN, 0);
    expect_pins(ctx.get(), bel, "csel_en", PORT_IN, 0);
    expect_pins(ctx.get(), bel, "csel", PORT_IN, 2);
    expect_pins(ctx.get(), bel, "bsel", PORT_IN, 3);
    int pins = 0;
    for (IdString pin : ctx->getBelPins(bel)) {
        (void)pin;
        ++pins;
    }
    EXPECT_EQ(pins, 9);
}

TEST(HpsFabricBels, TraceAndDebugApbPins)
{
    auto ctx = make_context();
    const BelId tpiu = only_bel(ctx.get(), "cyclonev_hps_interface_tpiu_trace");
    ASSERT_NE(tpiu, BelId());
    expect_pins(ctx.get(), tpiu, "traceclk_ctl", PORT_IN, 0);
    expect_pins(ctx.get(), tpiu, "trace_data", PORT_OUT, 32);

    const BelId apb = only_bel(ctx.get(), "cyclonev_hps_interface_dbg_apb");
    ASSERT_NE(apb, BelId());
    expect_pins(ctx.get(), apb, "DBG_APB_DISABLE", PORT_IN, 0);
    expect_pins(ctx.get(), apb, "P_CLK_EN", PORT_IN, 0);
    expect_pins(ctx.get(), apb, "P_RDATA", PORT_IN, 32);
    expect_pins(ctx.get(), apb, "P_READY", PORT_IN, 0);
    expect_pins(ctx.get(), apb, "P_SLV_ERR", PORT_IN, 0);
    expect_pins(ctx.get(), apb, "P_ADDR", PORT_OUT, 18);
    expect_pins(ctx.get(), apb, "P_ADDR_31", PORT_OUT, 0);
    expect_pins(ctx.get(), apb, "P_ENABLE", PORT_OUT, 0);
    expect_pins(ctx.get(), apb, "P_SEL", PORT_OUT, 0);
    expect_pins(ctx.get(), apb, "P_WDATA", PORT_OUT, 32);
    expect_pins(ctx.get(), apb, "P_WRITE", PORT_OUT, 0);
    expect_pins(ctx.get(), apb, "P_RESET_N", PORT_OUT, 0);
}

TEST(HpsFabricBels, ClocksResetsHandshakePins)
{
    auto ctx = make_context();
    const BelId bel = only_bel(ctx.get(), "cyclonev_hps_interface_clocks_resets");
    ASSERT_NE(bel, BelId());
    for (const char *pin : {"f2h_cold_rst_req_n", "f2h_warm_rst_req_n", "f2h_dbg_rst_req_n", "f2h_pending_rst_ack"})
        expect_pins(ctx.get(), bel, pin, PORT_IN, 0);
    expect_pins(ctx.get(), bel, "h2f_pending_rst_req_n", PORT_OUT, 0);
    int pins = 0;
    for (IdString pin : ctx->getBelPins(bel)) {
        (void)pin;
        ++pins;
    }
    EXPECT_EQ(pins, 10) << "5 clock/reset outputs + 4 handshake inputs + h2f_pending_rst_req_n";
}

// The packer folds constants on HPS inputs into the cell (no routing); bitgen then writes inversion 1 for a 0-tie
// and 0 for a 1-tie, clears the inversion of a logic-driven input, and leaves unconnected inputs at their default.
TEST(HpsTieOffs, ConstantsAndDrivenInputsSetInversionExplicitly)
{
    auto ctx = make_context();
    const BelId bel = only_bel(ctx.get(), "cyclonev_hps_interface_boot_from_fpga");
    ASSERT_NE(bel, BelId());

    // The boot-select inputs default to inverted (they read 0 unconfigured); otherwise this test proves nothing
    // about clearing the bit.
    const IdString csel0 = ctx->id("csel[0]"), bsel1 = ctx->id("bsel[1]"), bsel2 = ctx->id("bsel[2]");
    {
        ctx->configure_bitstream();
        Inv d = inversion(ctx.get(), bel, csel0);
        ASSERT_TRUE(d.found);
        ASSERT_TRUE(d.value && d.is_default) << "csel[0] inversion no longer defaults to 1";
    }

    NetInfo *gnd = ctx->createNet(ctx->id("$gnd_net"));
    NetInfo *vcc = ctx->createNet(ctx->id("$vcc_net"));
    CellInfo *gnd_cell = ctx->createCell(ctx->id("$gnd"), id_GND);
    gnd_cell->addOutput(id_Y);
    gnd_cell->connectPort(id_Y, gnd);
    CellInfo *vcc_cell = ctx->createCell(ctx->id("$vcc"), id_VCC);
    vcc_cell->addOutput(id_Y);
    vcc_cell->connectPort(id_Y, vcc);

    CellInfo *boot = ctx->createCell(ctx->id("boot"), ctx->id("cyclonev_hps_interface_boot_from_fpga"));
    const IdString bsel_en = ctx->id("bsel_en");
    for (IdString port : {bsel_en, csel0, bsel1, bsel2})
        boot->addInput(port);
    boot->connectPort(bsel_en, gnd);  // tie to 0
    boot->connectPort(csel0, vcc);    // tie to 1
    NetInfo *signal = ctx->createNet(ctx->id("bsel1_signal"));
    boot->connectPort(bsel1, signal); // driven (the test binds the wire instead of routing a driver)
                                      // bsel[2] stays unconnected

    // A 0-tie on an input whose inversion defaults to 0 (trace clock control), so the 1 is visible.
    const BelId tpiu_bel = only_bel(ctx.get(), "cyclonev_hps_interface_tpiu_trace");
    ASSERT_NE(tpiu_bel, BelId());
    const IdString traceclk_ctl = ctx->id("traceclk_ctl");
    CellInfo *tpiu = ctx->createCell(ctx->id("tpiu"), ctx->id("cyclonev_hps_interface_tpiu_trace"));
    tpiu->addInput(traceclk_ctl);
    tpiu->connectPort(traceclk_ctl, gnd);

    ASSERT_TRUE(ctx->pack());
    EXPECT_EQ(tpiu->get_pin_state(traceclk_ctl), PIN_0);
    EXPECT_EQ(boot->getPort(bsel_en), nullptr) << "a constant HPS input must not be routed";
    EXPECT_EQ(boot->getPort(csel0), nullptr) << "a constant HPS input must not be routed";
    EXPECT_EQ(boot->get_pin_state(bsel_en), PIN_0);
    EXPECT_EQ(boot->get_pin_state(csel0), PIN_1);

    // pack() leaves its soft-constant driver cells unplaced and unused here; bitgen expects every cell placed.
    std::vector<IdString> leftover;
    for (auto &cell : ctx->cells)
        if (cell.second.get() != boot && cell.second.get() != tpiu)
            leftover.push_back(cell.first);
    for (IdString name : leftover) {
        CellInfo *ci = ctx->cells.at(name).get();
        for (auto &port : ci->ports)
            if (port.second.net != nullptr)
                ci->disconnectPort(port.first);
        ctx->cells.erase(name);
    }

    ctx->bindBel(bel, boot, STRENGTH_USER);
    ctx->bindBel(tpiu_bel, tpiu, STRENGTH_USER);
    ctx->bindWire(ctx->getBelPinWire(bel, bsel1), signal, STRENGTH_WEAK);
    ctx->configure_bitstream();

    Inv tied0 = inversion(ctx.get(), bel, bsel_en), tied1 = inversion(ctx.get(), bel, csel0);
    Inv driven = inversion(ctx.get(), bel, bsel1), unused = inversion(ctx.get(), bel, bsel2);
    ASSERT_TRUE(tied0.found && tied1.found && driven.found && unused.found);
    EXPECT_TRUE(tied0.value) << "0-tie: inversion 1";
    Inv trace = inversion(ctx.get(), tpiu_bel, traceclk_ctl);
    ASSERT_TRUE(trace.found);
    EXPECT_TRUE(trace.value && !trace.is_default) << "0-tie on a default-0 input: inversion 1";
    EXPECT_FALSE(tied1.value) << "1-tie: inversion 0, even though the default is 1";
    EXPECT_FALSE(driven.value) << "driven input: true polarity";
    EXPECT_TRUE(unused.is_default) << "unconnected input keeps its default";
}
