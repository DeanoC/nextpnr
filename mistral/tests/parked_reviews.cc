#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

namespace {

struct CapturedLog
{
    std::ostringstream stream;
    explicit CapturedLog(LogLevel level) { log_streams.emplace_back(&stream, level); }
    ~CapturedLog() { log_streams.pop_back(); }
    std::string text() const { return stream.str(); }
};

std::unique_ptr<Context> device()
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    return std::make_unique<Context>(args);
}

void read_sdc(Context *ctx, const std::string &text)
{
    std::istringstream stream(text);
    ctx->read_sdc(stream);
}

} // namespace

// Issue 132: an SCLR group of 21–40 flip-flops fits one LAB when --mistral-ff4 is on.
TEST(ParkedReviews, Ff4SclrGroupUsesFortyFlipFlopsPerLab)
{
    auto ctx = device();
    ctx->createNet(ctx->id("$PACKER_GND_NET"));
    ctx->createNet(ctx->id("$PACKER_VCC_NET"));
    BelId lab_bel;
    for (const auto &lab : ctx->labs) {
        lab_bel = lab.alms[0].lut_bels[0];
        if (lab_bel != BelId())
            break;
    }
    ASSERT_NE(lab_bel, BelId());
    ctx->fes_region_bels[ctx->id("cart")].insert(lab_bel);

    NetInfo *sclr = ctx->createNet(ctx->id("shared_sclr"));
    std::vector<CellInfo *> ffs;
    for (int i = 0; i < 41; i++) {
        auto *ff = ctx->createCell(ctx->idf("ff%d", i), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            ff->addInput(pin);
        ff->addOutput(id_Q);
        ff->pin_data[id_ENA].state = PIN_1;
        ff->pin_data[id_ACLR].state = PIN_1;
        ff->pin_data[id_SLOAD].state = PIN_0;
        ff->pin_data[id_SCLR].state = PIN_SIG;
        ff->connectPort(id_SCLR, sclr);
        ffs.push_back(ff);
    }
    ctx->assignArchInfo();
    ctx->lab_ff4 = true;
    auto report = [&](int count) {
        std::vector<CellInfo *> group(ffs.begin(), ffs.begin() + count);
        ctx->fes_report_slot_capacity(ctx->id("cart"), group);
    };
    EXPECT_NO_THROW(report(21));
    EXPECT_NO_THROW(report(40));
    {
        CapturedLog errors(LogLevel::ERROR_MSG);
        EXPECT_THROW(report(41), log_execution_error_exception);
        EXPECT_NE(errors.text().find("flip-flops exceed"), std::string::npos) << errors.text();
    }
    had_nonfatal_error = false;
    ctx->lab_ff4 = false;
    {
        CapturedLog errors(LogLevel::ERROR_MSG);
        EXPECT_THROW(report(21), log_execution_error_exception);
        EXPECT_NE(errors.text().find("does not fit"), std::string::npos) << errors.text();
    }
    had_nonfatal_error = false;
}

// Issue 133: a path multiplier must consume the whole token. Issue 143: an empty -group is rejected.
TEST(ParkedReviews, SdcMultiplierAndEmptyClockGroupAreRejected)
{
    auto ctx = device();
    auto reject = [&](const std::string &text, const char *expected) {
        CapturedLog errors(LogLevel::ERROR_MSG);
        EXPECT_THROW(read_sdc(ctx.get(), text), log_execution_error_exception) << text;
        EXPECT_NE(errors.text().find(expected), std::string::npos) << errors.text();
    };
    reject("set_multicycle_path -setup 2junk -from [get_clocks {clk}] -to [get_clocks {clk}]\n",
           "invalid path multiplier");
    reject("set_multicycle_path 2junk -from [get_clocks {clk}] -to [get_clocks {clk}]\n", "invalid path multiplier");
    reject("set_multicycle_path -setup 0 -from [get_clocks {clk}] -to [get_clocks {clk}]\n", "at least 1");
    reject("set_clock_groups -asynchronous -group {}\n", "empty");
    reject("set_clock_groups -asynchronous -group [get_clocks {}]\n", "empty");
    EXPECT_TRUE(ctx->sdc_clock_exceptions.empty());
    EXPECT_NO_THROW(read_sdc(ctx.get(), "set_multicycle_path -setup 2 -from [get_clocks {clk}] -to [get_clocks {clk}]\n"));
    ASSERT_EQ(ctx->sdc_clock_exceptions.size(), 1u);
    EXPECT_EQ(ctx->sdc_clock_exceptions[0].setup_multiplier, 2);
    EXPECT_NO_THROW(read_sdc(ctx.get(), "set_clock_groups -asynchronous -group {clk}\n"));
    ASSERT_EQ(ctx->sdc_clock_groups.size(), 1u);
    ASSERT_EQ(ctx->sdc_clock_groups[0].size(), 1u);
    ASSERT_EQ(ctx->sdc_clock_groups[0][0].size(), 1u);
    EXPECT_EQ(ctx->sdc_clock_groups[0][0][0], "clk");
}

// Issue 129: D5 on a registered SDRIO output is encoded; drive settings still need OE.
TEST(ParkedReviews, SdrioRegisteredOutputAcceptsD5Delay)
{
    auto make = [](const char *name) {
        auto ctx = device();
        auto *pad = ctx->createCell(ctx->id(name), id_MISTRAL_SDRIO);
        pad->addInout(id_PAD);
        return std::make_pair(std::move(ctx), pad);
    };
    {
        auto [ctx, pad] = make("registered_output");
        auto *oe = ctx->createNet(ctx->id("oe"));
        pad->addInput(id_OE);
        pad->connectPort(id_OE, oe);
        pad->params[ctx->id("IOREG_OUT")] = Property(1);
        pad->attrs[ctx->id("D5_DELAY")] = Property(4);
        pad->attrs[ctx->id("CURRENT_STRENGTH_NEW")] = Property("8MA");
        pad->attrs[ctx->id("SLEW_RATE")] = Property("0");
        CapturedLog info(LogLevel::INFO_MSG);
        EXPECT_NO_THROW(ctx->check_io_electrical());
        EXPECT_EQ(info.text().find("do not apply"), std::string::npos) << info.text();
    }
    {
        auto [ctx, pad] = make("registered_output_without_oe");
        pad->params[ctx->id("IOREG_OUT")] = Property(1);
        pad->attrs[ctx->id("D5_DELAY")] = Property(4);
        pad->attrs[ctx->id("CURRENT_STRENGTH_NEW")] = Property("8MA");
        CapturedLog info(LogLevel::INFO_MSG);
        EXPECT_NO_THROW(ctx->check_io_electrical());
        EXPECT_NE(info.text().find("do not apply"), std::string::npos) << info.text();
    }
    {
        auto [ctx, pad] = make("no_output_path");
        pad->attrs[ctx->id("D5_DELAY")] = Property(4);
        CapturedLog errors(LogLevel::ERROR_MSG);
        EXPECT_THROW(ctx->check_io_electrical(), log_execution_error_exception);
        EXPECT_NE(errors.text().find("D5_DELAY"), std::string::npos) << errors.text();
    }
    {
        auto ctx = device();
        auto *pad = ctx->createCell(ctx->id("input_only"), id_MISTRAL_IB);
        pad->addInput(id_PAD);
        pad->attrs[ctx->id("CURRENT_STRENGTH_NEW")] = Property("8MA");
        CapturedLog info(LogLevel::INFO_MSG);
        EXPECT_NO_THROW(ctx->check_io_electrical());
        EXPECT_NE(info.text().find("do not apply"), std::string::npos) << info.text();
    }
    {
        auto [ctx, pad] = make("d1_on_sdrio");
        pad->attrs[ctx->id("D1_DELAY")] = Property(4);
        CapturedLog errors(LogLevel::ERROR_MSG);
        EXPECT_THROW(ctx->check_io_electrical(), log_execution_error_exception);
        EXPECT_NE(errors.text().find("not encoded"), std::string::npos) << errors.text();
    }
}

// Issue 138: FAST_OUTPUT_ENABLE_REGISTER on a pad with no OE is a warning, not a failed FF check.
TEST(ParkedReviews, OutputEnableRegisterWithoutOeIsIgnored)
{
    const bool saved_warn = log_warn_as_error;
    log_warn_as_error = false;
    auto pad = [](Context *ctx) {
        auto *io = ctx->createCell(ctx->id("dq"), id_MISTRAL_IO);
        io->addInout(id_PAD);
        io->addInput(id_I);
        io->addOutput(id_O);
        io->attrs[ctx->id("LOC")] = Property("PIN_AH27");
        io->attrs[ctx->id("FAST_OUTPUT_ENABLE_REGISTER")] = Property("ON");
        return io;
    };
    {
        auto ctx = device();
        pad(ctx.get());
        CapturedLog warnings(LogLevel::WARNING_MSG);
        EXPECT_TRUE(ctx->pack());
        EXPECT_NE(warnings.text().find("ignored"), std::string::npos) << warnings.text();
        EXPECT_NE(warnings.text().find("no output enable"), std::string::npos) << warnings.text();
    }
    {
        auto ctx = device();
        auto *io = pad(ctx.get());
        auto *lut = ctx->createCell(ctx->id("oe_lut"), id_MISTRAL_ALUT2);
        lut->addOutput(id_Q);
        auto *oe = ctx->createNet(ctx->id("oe"));
        lut->connectPort(id_Q, oe);
        io->addInput(id_OE);
        io->connectPort(id_OE, oe);
        CapturedLog errors(LogLevel::ERROR_MSG);
        EXPECT_THROW(ctx->pack(), log_execution_error_exception);
        EXPECT_NE(errors.text().find("MISTRAL_FF"), std::string::npos) << errors.text();
    }
    log_warn_as_error = saved_warn;
    had_nonfatal_error = false;
}
