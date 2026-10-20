#include <memory>
#include "gtest/gtest.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

// M10K read clock-to-output classes, from the TimeQuest characterization in
// mistral/tests/m10k/clock-to-out (Slow 1100mV 100C).
class M10kTimingTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;

    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
    }

    CellInfo *ram(const std::string &name, std::initializer_list<std::pair<IdString, int>> params)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_M10K);
        for (auto &param : params)
            cell->params[param.first] = Property(param.second);
        for (IdString port : {id_CLK1, id_CLK2, id_B1EN}) {
            cell->addInput(port);
            cell->connectPort(port, ctx->createNet(ctx->idf("%s$%s", name.c_str(), port.c_str(ctx.get()))));
        }
        for (IdString port : {id_ACLR0, id_ACLR1})
            cell->pin_data[port].state = PIN_0;
        return cell;
    }

    DelayQuad clock_to_out(CellInfo *cell, const std::string &port)
    {
        IdString id = ctx->id(port);
        cell->addOutput(id);
        int count = 0;
        EXPECT_EQ(ctx->getPortTimingClass(cell, id, count), TMG_REGISTER_OUTPUT) << port;
        EXPECT_EQ(count, 1) << port;
        return ctx->getPortClockingInfo(cell, id, 0).clockToQ;
    }

    static void expect(const DelayQuad &delay, delay_t min, delay_t max)
    {
        EXPECT_EQ(delay.minRiseDelay(), min);
        EXPECT_EQ(delay.minFallDelay(), min);
        EXPECT_EQ(delay.maxRiseDelay(), max);
        EXPECT_EQ(delay.maxFallDelay(), max);
    }
};

TEST_F(M10kTimingTest, SimpleDualPortRead)
{
    auto *flow = ram("flow", {{id_CFG_ABITS, 9}, {id_CFG_DBITS, 20}});
    expect(clock_to_out(flow, "B1DATA[3]"), 1004, 2620);
    auto *one = ram("one", {{id_CFG_ABITS, 13}, {id_CFG_DBITS, 1}});
    expect(clock_to_out(one, "B1DATA"), 1004, 2620);
    auto *reg = ram("reg", {{id_CFG_ABITS, 9}, {id_CFG_DBITS, 20}, {id_CFG_OUT_REG_B, 1}});
    expect(clock_to_out(reg, "B1DATA[3]"), 566, 709);
}

TEST_F(M10kTimingTest, FortyBitRead)
{
    // Either port 40 bits wide takes TimeQuest's slower 40-bit read path.
    auto *flow = ram("flow", {{id_CFG_ABITS, 8}, {id_CFG_DBITS, 40}});
    expect(clock_to_out(flow, "B1DATA[5]"), 1004, 2704);
    expect(clock_to_out(flow, "B1DATA[25]"), 1004, 2704);
    auto *wide_read = ram("wide_read", {{id_CFG_ABITS, 10}, {id_CFG_DBITS, 10}, {id_CFG_MIXED_WIDTH, 1},
                                        {id_CFG_RD_ABITS, 8}, {id_CFG_RD_DBITS, 40}, {id_CFG_DUAL_CLOCK, 1}});
    expect(clock_to_out(wide_read, "B1DATA[30]"), 1004, 2704);
    auto *wide_write = ram("wide_write", {{id_CFG_ABITS, 8}, {id_CFG_DBITS, 40}, {id_CFG_MIXED_WIDTH, 1},
                                          {id_CFG_RD_ABITS, 10}, {id_CFG_RD_DBITS, 10}, {id_CFG_DUAL_CLOCK, 1}});
    expect(clock_to_out(wide_write, "B1DATA[2]"), 1004, 2704);
    // Either output-register request registers both halves of the word.
    auto *reg = ram("reg", {{id_CFG_ABITS, 8}, {id_CFG_DBITS, 40}, {id_CFG_OUT_REG_A, 1}});
    expect(clock_to_out(reg, "B1DATA[5]"), 574, 749);
    expect(clock_to_out(reg, "B1DATA[25]"), 574, 749);
}

TEST_F(M10kTimingTest, OutputClearSelectsTheRegister)
{
    // ACLR0 clears the A half, which carries B1DATA[19:0] of a 40-bit read.
    auto *wide = ram("wide", {{id_CFG_ABITS, 8}, {id_CFG_DBITS, 40}});
    wide->pin_data[id_ACLR0].state = PIN_SIG;
    expect(clock_to_out(wide, "B1DATA[5]"), 574, 749);
    expect(clock_to_out(wide, "B1DATA[25]"), 1004, 2704);
    EXPECT_TRUE(ctx->m10k_output_registered(wide, true));
    EXPECT_FALSE(ctx->m10k_output_registered(wide, false));
    // A narrow SDP read only uses the B half, so ACLR0 leaves it unregistered.
    auto *narrow = ram("narrow", {{id_CFG_ABITS, 10}, {id_CFG_DBITS, 10}});
    narrow->pin_data[id_ACLR0].state = PIN_INV;
    expect(clock_to_out(narrow, "B1DATA[1]"), 1004, 2620);
    EXPECT_FALSE(ctx->m10k_output_registered(narrow, true));
    narrow->pin_data[id_ACLR1].state = PIN_1;
    expect(clock_to_out(narrow, "B1DATA[1]"), 566, 709);
}

TEST_F(M10kTimingTest, TrueDualPortRead)
{
    auto *flow = ram("flow", {{id_CFG_TDP, 1}, {id_CFG_ABITS, 10}, {id_CFG_DBITS, 10}});
    expect(clock_to_out(flow, "A1Q[0]"), 1004, 2610);
    expect(clock_to_out(flow, "B1Q[9]"), 1004, 2581);
    auto *reg_a = ram("reg_a", {{id_CFG_TDP, 1}, {id_CFG_ABITS, 9}, {id_CFG_DBITS, 20}, {id_CFG_OUT_REG_A, 1}});
    expect(clock_to_out(reg_a, "A1Q[0]"), 592, 744);
    expect(clock_to_out(reg_a, "B1Q[0]"), 1004, 2581);
    auto *clear_b = ram("clear_b", {{id_CFG_TDP, 1}, {id_CFG_ABITS, 9}, {id_CFG_DBITS, 20}});
    clear_b->pin_data[id_ACLR1].state = PIN_SIG;
    expect(clock_to_out(clear_b, "A1Q[0]"), 1004, 2610);
    expect(clock_to_out(clear_b, "B1Q[0]"), 569, 709);
    auto *one = ram("one", {{id_CFG_TDP, 1}, {id_CFG_ABITS, 13}, {id_CFG_DBITS, 1}});
    expect(clock_to_out(one, "A1Q"), 1004, 2610);
    expect(clock_to_out(one, "B1Q"), 1004, 2581);
}
