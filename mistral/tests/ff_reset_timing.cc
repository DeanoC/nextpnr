#include <memory>
#include "gtest/gtest.h"
#include "nextpnr.h"
#include "log.h"

USING_NEXTPNR_NAMESPACE

class FfResetTimingTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *ff;
    NetInfo *clock;
    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ff = ctx->createCell(ctx->id("ff"), id_MISTRAL_FF);
        for (auto p : {id_CLK, id_ACLR, id_DATAIN}) ff->addInput(p);
        clock = ctx->createNet(ctx->id("clock"));
        ff->connectPort(id_CLK, clock);
        ff->connectPort(id_ACLR, driven("clear"));
        ff->pin_data[id_CLK].state = PIN_SIG;
        ff->pin_data[id_ACLR].state = PIN_SIG;
        ff->pin_data[id_ENA].state = PIN_1;
        ff->pin_data[id_SCLR].state = PIN_0;
        ff->pin_data[id_SLOAD].state = PIN_0;
        waveform(20000,10000,10000);
    }
    NetInfo *driven(const char *name)
    {
        auto *net = ctx->createNet(ctx->id(name));
        auto *driver = ctx->createCell(ctx->idf("%s$driver",name), id_MISTRAL_NOT);
        driver->addOutput(id_Q); driver->connectPort(id_Q,net);
        return net;
    }
    void waveform(delay_t period, delay_t high, delay_t low)
    {
        clock->clkconstr = std::make_unique<ClockConstraint>();
        clock->clkconstr->period = DelayPair{period,period};
        clock->clkconstr->high = DelayPair{high,high};
        clock->clkconstr->low = DelayPair{low,low};
    }
    void enable() { ctx->settings[ctx->id("mistral/ff_reset_reference")] = Property::State::S1; }
    TimingClockingInfo info(IdString port=id_ACLR) { return ctx->getPortClockingInfo(ff,port,0); }
    void expect_reject() { EXPECT_THROW(info(),log_execution_error_exception); }
};

TEST_F(FfResetTimingTest, DefaultRetainsHistoricalApproximation)
{
    EXPECT_EQ(info().setup.maxDelay(),-196); EXPECT_EQ(info().hold.minDelay(),270);
}
TEST_F(FfResetTimingTest, OptInTightensRecoveryAndPreservesHoldFloor)
{
    enable(); auto v=info(); EXPECT_EQ(v.setup.minDelay(),170); EXPECT_EQ(v.setup.maxDelay(),170);
    EXPECT_EQ(v.hold.minDelay(),270); EXPECT_EQ(v.hold.maxDelay(),270);
    EXPECT_EQ(v.clock_port,id_CLK); EXPECT_EQ(v.edge,RISING_EDGE);
    EXPECT_EQ(info(id_DATAIN).setup.maxDelay(),-196);
}
TEST_F(FfResetTimingTest, ExactObservedClockRates)
{
    enable(); for (auto period : {10000,13468,20000}) {
        waveform(period,period/2,period/2); EXPECT_EQ(info().setup.maxDelay(),170);
    }
}
TEST_F(FfResetTimingTest, DynamicEnableBothPolarities)
{
    enable(); ff->addInput(id_ENA); ff->connectPort(id_ENA,driven("enable"));
    for (auto state : {PIN_SIG,PIN_INV}) { ff->pin_data[id_ENA].state=state; EXPECT_EQ(info().setup.maxDelay(),170); }
}
TEST_F(FfResetTimingTest, InvertedClockRejected)
{
    enable(); ff->pin_data[id_CLK].state=PIN_INV; expect_reject();
}
TEST_F(FfResetTimingTest, InvertedClearRejected)
{
    enable(); ff->pin_data[id_ACLR].state=PIN_INV; expect_reject();
}
TEST_F(FfResetTimingTest, UndrivenClearRejected)
{
    enable(); ff->disconnectPort(id_ACLR); ff->connectPort(id_ACLR,ctx->createNet(ctx->id("undriven"))); expect_reject();
}
TEST_F(FfResetTimingTest, MissingClearRejected)
{
    enable(); ff->disconnectPort(id_ACLR); expect_reject();
}
TEST_F(FfResetTimingTest, InactiveOrUndrivenEnableRejected)
{
    enable(); ff->pin_data[id_ENA].state=PIN_0; expect_reject();
    ff->pin_data[id_ENA].state=PIN_SIG; ff->addInput(id_ENA);
    ff->connectPort(id_ENA,ctx->createNet(ctx->id("undriven_ena"))); expect_reject();
}
TEST_F(FfResetTimingTest, OtherOverridesRejected)
{
    enable(); for (auto port : {id_SCLR,id_SLOAD}) {
        ff->addInput(port); ff->connectPort(port,driven(port==id_SCLR?"sclr":"sload"));
        ff->pin_data[port].state=PIN_SIG; expect_reject();
        ff->disconnectPort(port); ff->pin_data[port].state=PIN_0;
    }
}
TEST_F(FfResetTimingTest, MissingOrUnsupportedWaveformRejected)
{
    enable(); clock->clkconstr.reset(); expect_reject();
    waveform(8000,4000,4000); expect_reject();
    waveform(20000,4000,16000); expect_reject();
    waveform(20000,10000,9000); expect_reject();
    waveform(20000,10000,10000); clock->clkconstr->period=DelayPair{19999,20000}; expect_reject();
}

TEST_F(FfResetTimingTest, InactiveClearCalibrationDoesNotClaimAResetModel)
{
    enable(); ff->disconnectPort(id_ACLR); ff->pin_data[id_ACLR].state=PIN_1;
    // A live synchronous override is irrelevant to the absent clear port.
    ff->addInput(id_SCLR); ff->connectPort(id_SCLR,driven("synchronous_reset"));
    ff->pin_data[id_SCLR].state=PIN_SIG;
    EXPECT_EQ(info().setup.maxDelay(),-196); EXPECT_EQ(info().hold.minDelay(),270);
}
