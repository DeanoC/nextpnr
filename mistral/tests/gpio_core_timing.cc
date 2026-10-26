#include "nextpnr.h"
#include "log.h"
#include "timing.h"
#include "gpio_core_reference_data.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>
#include <vector>

USING_NEXTPNR_NAMESPACE

class GpioCoreTimingTest : public ::testing::Test
{
  protected:
    using CV=mistral::CycloneV;
    std::unique_ptr<Context> ctx;
    CellInfo *gpio;
    NetInfo *clock;
    NetInfo *driven(const char *name) {
        auto *net=ctx->createNet(ctx->id(name));
        auto *driver=ctx->createCell(ctx->idf("%s$driver",name),id_MISTRAL_BUF);
        driver->addOutput(id_Q); driver->connectPort(id_Q,net);
        return net;
    }
    void SetUp() override {
        ArchArgs args; args.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(args);
        ctx->createNet(ctx->id("$PACKER_GND_NET"));ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        clock=driven("clock");clock->clkconstr=std::make_unique<ClockConstraint>();
        clock->clkconstr->period=DelayPair{20000};
        clock->clkconstr->high=clock->clkconstr->low=DelayPair{10000};
        gpio=ctx->createCell(ctx->id("gpio"),id_MISTRAL_SDRIO);
        for(auto key:{"IOREG_IN","IOREG_OUT","IOREG_OE"})gpio->params[ctx->id(key)]=1;
        for(auto p:{id_CLK,id_CLKIN,id_I,id_OE}) {
            gpio->addInput(p);gpio->pin_data[p].bel_pins={p};gpio->pin_data[p].state=PIN_SIG;
        }
        gpio->addOutput(id_Q);gpio->pin_data[id_Q].bel_pins={id_Q};
        gpio->connectPort(id_CLK,clock);gpio->connectPort(id_CLKIN,clock);
        gpio->connectPort(id_I,driven("data"));gpio->connectPort(id_OE,driven("enable"));
        gpio->connectPort(id_Q,ctx->createNet(ctx->id("q")));
        gpio->attrs[id_LOC]=std::string("PIN_V12");
        gpio->attrs[ctx->id("NEXTPNR_GPIO_TIMING_PROFILE")]=std::string("QUARTUS_17_0_2_RAMTEST");
        gpio->attrs[ctx->id("BOARD_MODEL_FAR_C")]=std::string("30p");
    }
    void enable() {ctx->settings[ctx->id("mistral/gpio_core_reference")]=Property::State::S1;}
    TimingClockingInfo info(IdString p=id_Q) {return ctx->getPortClockingInfo(gpio,p,0);}
    void reject() {EXPECT_THROW(info(),log_execution_error_exception);}
    void physical() {
        // Actual independent Quartus RBF, not a configuration synthesized
        // from the generated expectations under test.
        auto path=std::filesystem::path(__FILE__).parent_path()/"gpio-core/fixture/fitted.rbf.gz";
        std::ifstream file(path,std::ios::binary);ASSERT_TRUE(file.good());
        boost::iostreams::filtering_istream stream;
        stream.push(boost::iostreams::gzip_decompressor());stream.push(file);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});ASSERT_FALSE(bytes.empty());
        ctx->cyclonev->rbf_load(bytes.data(),bytes.size());
        auto *package=ctx->cyclonev->pin_find_name(gpio->attrs.at(id_LOC).as_string().substr(4));
        ASSERT_NE(package,nullptr);auto bel=ctx->get_io_pin_bel(package);ctx->bindBel(bel,gpio,STRENGTH_USER);
        for(auto p:{id_I,id_OE,id_Q,id_CLK,id_CLKIN})
            if(gpio->getPort(p))ctx->bindWire(ctx->getBelPinWire(bel,p),gpio->getPort(p),STRENGTH_USER);
        ctx->bitstream_configured=true;
    }
    void bit(CV::bmux_type_t field,bool value) {
        auto lane=ctx->cyclonev->p2p_to(CV::pnode_coords{CV::GPIO,gpio->bel.pos,CV::PNONE,0,-1});
        ASSERT_TRUE(ctx->cyclonev->bmux_b_set(CV::DQS16,lane.p(),field,lane.bi(),value));
    }
};

TEST_F(GpioCoreTimingTest, DefaultRetainsHistoricalFabricAndPadModels) {
    physical();auto q=info(),d=info(id_I);
    EXPECT_EQ(q.clockToQ.minDelay(),0);EXPECT_EQ(q.clockToQ.maxDelay(),870);
    EXPECT_EQ(d.setup.maxDelay(),120);EXPECT_EQ(d.hold.maxDelay(),310);
    auto pads=ctx->getRegisteredIoTiming(gpio,id_PAD,true);ASSERT_EQ(pads.size(),1);
    EXPECT_EQ(pads[0].clocking.setup.maxDelay(),1910);EXPECT_EQ(pads[0].clocking.hold.maxDelay(),-310);
}
TEST_F(GpioCoreTimingTest, NativeLocalInterconnectIsIncluded) {
    enable();physical();auto q=info(),d=info(id_I),oe=info(id_OE);
    EXPECT_EQ(q.clock_port,id_CLKIN);EXPECT_EQ(q.edge,RISING_EDGE);
    EXPECT_GT(q.clockToQ.minDelay(),0);EXPECT_GT(q.clockToQ.maxDelay(),1500);
    EXPECT_EQ(d.setup.maxDelay(),4);EXPECT_EQ(d.hold.maxDelay(),468);
    EXPECT_EQ(oe.setup.maxDelay(),31);EXPECT_EQ(oe.hold.maxDelay(),469);
    EXPECT_EQ(q.clockToQ.minRiseDelay(),1734);EXPECT_EQ(q.clockToQ.maxFallDelay(),2010);
}
TEST_F(GpioCoreTimingTest, BypassPinUsesItsOwnMeasuredLocalTiming) {
    enable();gpio->attrs[id_LOC]=std::string("PIN_AE23");physical();auto q=info();
    EXPECT_EQ(q.clockToQ.maxRiseDelay(),4389);EXPECT_EQ(info(id_I).hold.maxDelay(),-83);
    auto wire=ctx->getBelPinWire(gpio->bel,id_Q);
    EXPECT_EQ(ctx->cyclonev->rnode_to_pnode(ctx->cyclonev->rc2ri(wire.node)).bt(),CV::HMC);
}
TEST_F(GpioCoreTimingTest, PadChecksMoveToTheSameNativeClockFrame) {
    physical();auto before=ctx->getRegisteredIoTiming(gpio,id_PAD,false);
    auto before_clock=ctx->getPrimitiveClockRequirements(gpio);enable();
    auto after=ctx->getRegisteredIoTiming(gpio,id_PAD,false);
    auto capture=ctx->getRegisteredIoTiming(gpio,id_PAD,true);
    ASSERT_EQ(before.size(),2);ASSERT_EQ(after.size(),2);ASSERT_EQ(capture.size(),1);
    EXPECT_GT(after[0].clocking.clockToQ.maxDelay(),before[0].clocking.clockToQ.maxDelay());
    EXPECT_GT(after[1].clocking.clockToQ.maxDelay(),before[1].clocking.clockToQ.maxDelay());
    EXPECT_EQ(after[0].clocking.clockToQ.minDelay(),0);
    EXPECT_LT(capture[0].clocking.setup.maxDelay(),1910);EXPECT_GT(capture[0].clocking.hold.maxDelay(),-310);
    auto after_clock=ctx->getPrimitiveClockRequirements(gpio);ASSERT_EQ(after_clock.size(),2);
    for(size_t i=0;i<after_clock.size();i++) {
        EXPECT_EQ(after_clock[i].min_period,10000);
        EXPECT_GT(after_clock[i].min_high,before_clock[i].min_high);
        EXPECT_GT(after_clock[i].min_low,before_clock[i].min_low);
    }
}
TEST_F(GpioCoreTimingTest, MissingAndUnsupportedPhysicalClocksRejected) {
    enable();clock->clkconstr.reset();EXPECT_EQ(info().clock_port,id_CLKIN);
    clock->clkconstr=std::make_unique<ClockConstraint>();
    clock->clkconstr->period=DelayPair{8000};clock->clkconstr->high=clock->clkconstr->low=DelayPair{4000};reject();
    clock->clkconstr->period=DelayPair{20000};clock->clkconstr->high=DelayPair{9000};clock->clkconstr->low=DelayPair{11000};reject();
}
TEST_F(GpioCoreTimingTest, UnmeasuredControlsAndModesRejected) {
    enable();gpio->params[ctx->id("IOREG_IN_DDR")]=1;reject();gpio->params[ctx->id("IOREG_IN_DDR")]=0;
    gpio->params[ctx->id("IOREG_OE_CE")]=1;reject();gpio->params[ctx->id("IOREG_OE_CE")]=0;
    gpio->addInput(id_ACLR);gpio->connectPort(id_ACLR,driven("clear"));reject();gpio->disconnectPort(id_ACLR);
    gpio->params[ctx->id("IOREG_OUT")]=0;reject();
}
TEST_F(GpioCoreTimingTest, UnmeasuredPinElectricalAndClockPolarityRejected) {
    enable();gpio->attrs[id_LOC]=std::string("PIN_AD20");reject();gpio->attrs[id_LOC]=std::string("PIN_V12");
    gpio->attrs[ctx->id("D3_DELAY")]=1;reject();gpio->attrs.erase(ctx->id("D3_DELAY"));
    gpio->pin_data[id_CLKIN].state=PIN_INV;reject();
}
TEST_F(GpioCoreTimingTest, ActualPhysicalSelectorsAndOwnerAreRequired) {
    enable();physical();bit(CV::CE_OEREG_TIEOFF_EN,true);reject();bit(CV::CE_OEREG_TIEOFF_EN,false);
    bit(CV::OEREG_CLK_INV,true);reject();bit(CV::OEREG_CLK_INV,false);
    gpio->attrs[id_LOC]=std::string("PIN_W12");reject();
}
TEST_F(GpioCoreTimingTest, NativePortMappingAndOwnedCutAreRequired) {
    enable();physical();auto clockwire=ctx->getBelPinWire(gpio->bel,id_CLKIN);
    ctx->unbindWire(clockwire);reject();ctx->bindWire(clockwire,clock,STRENGTH_USER);
    gpio->pin_data[id_Q].bel_pins={id_Q_L};reject();
}
TEST_F(GpioCoreTimingTest, IncompleteAnalogueRouteFailsClosed) {
    enable();physical();auto *sink=ctx->createCell(ctx->id("sink"),id_MISTRAL_FF);
    sink->addInput(id_DATAIN);sink->pin_data[id_DATAIN].bel_pins={id_DATAIN};
    sink->connectPort(id_DATAIN,gpio->getPort(id_Q));ctx->assign_ff_info(sink);
    ctx->bindBel(ctx->labs[0].alms[0].ff_bels[0],sink,STRENGTH_USER);
    PortRef endpoint;endpoint.cell=sink;endpoint.port=id_DATAIN;DelayQuad delay;
    EXPECT_THROW(ctx->analogue_arc_delay(gpio->getPort(id_Q),endpoint,delay,nullptr),log_execution_error_exception);
}
TEST_F(GpioCoreTimingTest, CompleteAnalogueRoutePropagatesNativeGpioWave) {
    enable();physical();
    const gpio_core_reference::Site *fixture=nullptr;
    for (const auto &candidate : gpio_core_reference::sites)
        if (std::string_view(candidate.pin)=="V12") fixture=&candidate;
    ASSERT_NE(fixture,nullptr);
    const auto q=info(),data=info(id_I),oe=info(id_OE);
    const auto &q_ref=fixture->arcs[0][2],&data_ref=fixture->arcs[0][0],&oe_ref=fixture->arcs[0][1];
    EXPECT_EQ(q.clockToQ.minRiseDelay(),q_ref.a);EXPECT_EQ(q.clockToQ.maxRiseDelay(),q_ref.b);
    EXPECT_EQ(q.clockToQ.minFallDelay(),q_ref.c);EXPECT_EQ(q.clockToQ.maxFallDelay(),q_ref.d);
    EXPECT_EQ(data.setup.minDelay(),data_ref.a);EXPECT_EQ(data.setup.maxDelay(),data_ref.a);
    EXPECT_EQ(data.hold.minDelay(),data_ref.b);EXPECT_EQ(data.hold.maxDelay(),data_ref.b);
    EXPECT_EQ(oe.setup.minDelay(),oe_ref.a);EXPECT_EQ(oe.setup.maxDelay(),oe_ref.a);
    EXPECT_EQ(oe.hold.minDelay(),oe_ref.b);EXPECT_EQ(oe.hold.maxDelay(),oe_ref.b);
#ifndef MISTRAL_GPIO_SOURCE_WAVE
    GTEST_SKIP() << "Mistral lacks GPIO source-wave support";
#else
    auto source=ctx->getNetinfoSourceWire(gpio->getPort(id_Q));
    ASSERT_EQ(source.node.t(),CV::GIN);
    const auto source_ri=ctx->cyclonev->rc2ri(source.node);
    ASSERT_EQ(ctx->cyclonev->rnode_timing_get_mode(source_ri),CV::RTM_CIRCUIT);

    mistral::AnalogSim probe;
    int input=-1;
    std::vector<std::pair<CV::rnode_index,int>> outputs;
    ctx->cyclonev->rnode_timing_build_circuit(source_ri,CV::T_100,CV::DELAY_MAX,CV::RF_RISE,
                                               probe,input,outputs);
    ASSERT_GE(input,0);
    ASSERT_FALSE(outputs.empty());
    PipId route_pip;
    WireId destination;
    for (auto output : outputs) {
        auto wire=WireId{ctx->cyclonev->ri2rc(output.first)};
        for (auto pip : ctx->getPipsDownhill(source)) {
            if (ctx->getPipDstWire(pip)==wire) {
                route_pip=pip;
                destination=wire;
                break;
            }
        }
        if (route_pip!=PipId())
            break;
    }
    ASSERT_NE(route_pip,PipId());
    ctx->cyclonev->rnode_link(source_ri,ctx->cyclonev->rc2ri(destination.node));

    auto net=gpio->getPort(id_Q);
    auto sink_id=ctx->id("complete_route_sink"),sink_pin=ctx->id("pin");
    ctx->createRegionPlug(sink_id,ctx->id("test_plug"),Loc());
    ctx->addPlugPin(sink_id,sink_pin,PORT_IN,destination);
    auto sink=ctx->cells.at(sink_id).get();
    sink->connectPort(sink_pin,net);
    ctx->bindPip(route_pip,net,STRENGTH_USER);

    PortRef endpoint;endpoint.cell=sink;endpoint.port=sink_pin;
    DelayQuad routed;
    ASSERT_TRUE(ctx->analogue_arc_delay(net,endpoint,routed,nullptr));
    EXPECT_GT(routed.maxDelay(),0);
    EXPECT_LE(routed.minDelay(),routed.maxDelay());
#endif
}

TEST_F(GpioCoreTimingTest, StandaloneSdrOutputUsesItsOwnPhysicalMode) {
    enable();gpio->type=id_MISTRAL_SDROUT;gpio->attrs[id_LOC]=std::string("PIN_Y11");
    for(auto p:{id_Q,id_OE,id_CLKIN})gpio->disconnectPort(p);
    physical();auto timing=info(id_I);
    EXPECT_EQ(timing.clock_port,id_CLK);EXPECT_EQ(timing.setup.maxDelay(),39);EXPECT_EQ(timing.hold.maxDelay(),345);
}

TEST_F(GpioCoreTimingTest, EndpointDiscoveryDoesNotWaiveLaterClockValidation) {
    enable();physical();clock->clkconstr.reset();
    EXPECT_EQ(ctx->getRegisteredIoTiming(gpio,id_PAD,true).size(),1);
    gpio->attrs.erase(ctx->id("NEXTPNR_GPIO_TIMING_PROFILE"));
    EXPECT_TRUE(ctx->getRegisteredIoTiming(gpio,id_PAD,true).empty());
    auto requirements=ctx->getPrimitiveClockRequirements(gpio);ASSERT_EQ(requirements.size(),2);
    EXPECT_EQ(requirements[0].min_period,10000);
    TimingAnalyser timing(ctx.get());
    EXPECT_THROW(timing.setup(),log_execution_error_exception);
}

TEST_F(GpioCoreTimingTest, UnsupportedRegisteredInputsDoNotFallBack) {
    enable();gpio->type=id_MISTRAL_SDRIN;reject();
    gpio->type=id_MISTRAL_DDRIN;
    EXPECT_THROW(info(id_Q_H),log_execution_error_exception);
    gpio->type=id_MISTRAL_DDRBIDIR;
    EXPECT_THROW(info(id_Q_L),log_execution_error_exception);
    gpio->type=id_MISTRAL_DDROUT;
    EXPECT_THROW(info(id_D_H),log_execution_error_exception);
}
