#include <map>
#include <memory>
#include <set>

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
