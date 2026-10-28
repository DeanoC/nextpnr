#include "nextpnr.h"
#include "log.h"
#include "lut_route_minimum.h"
#include <gtest/gtest.h>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <filesystem>
#include <fstream>

USING_NEXTPNR_NAMESPACE

TEST(LutRouteMinimum, SignedHopsEncloseBothIntervalsBeforeSummation)
{
    mistral::AnalogSim::time_interval positive{100, 200}, negative{-90, -30};
    ASSERT_TRUE(lut_route_minimum::enclose_minimum(0.9, positive));
    ASSERT_TRUE(lut_route_minimum::enclose_minimum(0.9, negative));
    EXPECT_DOUBLE_EQ(positive.mi, 90); EXPECT_DOUBLE_EQ(positive.mx, 200);
    EXPECT_DOUBLE_EQ(negative.mi, -90); EXPECT_DOUBLE_EQ(negative.mx, -30);
    mistral::AnalogSim::time_interval tight_negative{-90, -89};
    ASSERT_TRUE(lut_route_minimum::enclose_minimum(0.9, tight_negative));
    EXPECT_DOUBLE_EQ(tight_negative.mi, -90); EXPECT_DOUBLE_EQ(tight_negative.mx, -81);
    // Scaling the old total (100-90)*0.9 would incorrectly give +9.
    EXPECT_DOUBLE_EQ(positive.mi + negative.mi, 0);
}

TEST(LutRouteMinimum, InvalidIntervalsAndFactorsReject)
{
    for (double factor : {-0.1, 0.0, 1.1, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()}) {
        mistral::AnalogSim::time_interval value{1, 2};
        EXPECT_FALSE(lut_route_minimum::enclose_minimum(factor, value));
        EXPECT_EQ(value.mi, 1); EXPECT_EQ(value.mx, 2);
    }
    mistral::AnalogSim::time_interval backwards{2, 1};
    EXPECT_FALSE(lut_route_minimum::enclose_minimum(0.9, backwards));
}

TEST(LutRouteMinimum, OutwardRoundingPreservesNegativeAdvances)
{
    delay_t value;
    ASSERT_TRUE(lut_route_minimum::outward_ps(-0.25e-12, true, value)); EXPECT_EQ(value, -1);
    ASSERT_TRUE(lut_route_minimum::outward_ps(-0.25e-12, false, value)); EXPECT_EQ(value, 0);
    ASSERT_TRUE(lut_route_minimum::outward_ps(0.25e-12, true, value)); EXPECT_EQ(value, 0);
    ASSERT_TRUE(lut_route_minimum::outward_ps(0.25e-12, false, value)); EXPECT_EQ(value, 1);
    EXPECT_FALSE(lut_route_minimum::outward_ps(std::numeric_limits<double>::infinity(), true, value));
    EXPECT_FALSE(lut_route_minimum::outward_ps(double(std::numeric_limits<delay_t>::max()) * 1e-12 + 1, false, value));
}

TEST(LutRouteMinimum, ClockRoutingResourcesAreOutsideTheDataDomain)
{
    using CV = mistral::CycloneV;
    for (auto type : {CV::GIN, CV::H3, CV::H6, CV::TD, CV::GOUT})
        EXPECT_TRUE(lut_route_minimum::data_element(type));
    for (auto type : {CV::GCLK, CV::RCLK, CV::SCLK, CV::TCLK, CV::HCLK, CV::WM})
        EXPECT_FALSE(lut_route_minimum::data_element(type));
}

class LutRouteMinimumTest : public ::testing::Test
{
  protected:
    using CV = mistral::CycloneV;
    std::unique_ptr<Context> ctx;
    NetInfo *data;
    CellInfo *sink;
    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->createNet(ctx->id("$PACKER_GND_NET")); ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        data = ctx->createNet(ctx->id("data"));
        auto *source = ctx->createCell(ctx->id("launch"), id_MISTRAL_FF);
        source->addOutput(id_Q); source->connectPort(id_Q, data);
        source->pin_data[id_Q].bel_pins = {id_Q};
        for (auto port : {id_CLK, id_DATAIN, id_ENA, id_SCLR, id_ACLR, id_SLOAD, id_SDATA}) {
            source->addInput(port); source->pin_data[port].bel_pins = {port};
            source->pin_data[port].state = port.in(id_ENA, id_ACLR) ? PIN_1 : PIN_0;
        }
        ctx->assign_ff_info(source);
        sink = ctx->createCell(ctx->id("sink"), id_MISTRAL_ALUT6);
        sink->addInput(id_A); sink->connectPort(id_A, data);
        sink->addOutput(id_Q); sink->connectPort(id_Q, ctx->createNet(ctx->id("output")));
        sink->params[id_LUT] = Property(int64_t(0x6996966996696996ULL), 64);
        ctx->assign_comb_info(sink);
        sink->pin_data[id_A].bel_pins = {id_D}; sink->pin_data[id_Q].bel_pins = {id_COMBOUT};
        auto lab = std::find_if(ctx->labs.begin(), ctx->labs.end(), [](const LABInfo &v) {
            return !v.is_mlab && v.alms[0].lut_bels[0].pos == CV::xycoords(29, 2);
        });
        auto mlab = std::find_if(ctx->labs.begin(), ctx->labs.end(), [](const LABInfo &v) {
            return v.is_mlab && v.alms[0].lut_bels[0].pos == CV::xycoords(28, 2);
        });
        ASSERT_NE(lab, ctx->labs.end()); ASSERT_NE(mlab, ctx->labs.end());
        ctx->bindBel(lab->alms[0].ff_bels[0], source, STRENGTH_USER);
        ctx->bindBel(mlab->alms[9].lut_bels[1], sink, STRENGTH_USER);
        std::vector<CV::rnode_coords> route{{CV::GIN,29,2,0}, {CV::H6,24,2,33},
                                          {CV::TD,28,2,6}, {CV::GOUT,28,2,77}};
        const auto source_wire = ctx->getNetinfoSourceWire(data);
        ASSERT_TRUE(source_wire.is_nextpnr_created());
        const auto &user = data->users.at(sink->ports.at(id_A).user_idx);
        EXPECT_EQ(ctx->getNetinfoSinkWire(data, user, 0).node, route.back());
        ctx->bindWire(source_wire, data, STRENGTH_USER);
        ctx->bindPip(PipId(source_wire.node, route.front()), data, STRENGTH_USER);
        for (size_t i=1; i<route.size(); ++i)
            ctx->bindPip(PipId(route[i-1], route[i]), data, STRENGTH_USER);
        auto path = std::filesystem::path(__FILE__).parent_path() / "route-minimum/fitted.rbf.gz";
        std::ifstream file(path, std::ios::binary); ASSERT_TRUE(file.good());
        boost::iostreams::filtering_istream stream;
        stream.push(boost::iostreams::gzip_decompressor()); stream.push(file);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
        ASSERT_FALSE(bytes.empty()); ctx->cyclonev->rbf_load(bytes.data(), bytes.size());
        ctx->use_lut_core_reference = true;
        ctx->settings[ctx->id("mistral/routed_delay_ranges")] = true;
        ctx->bitstream_configured = true;
    }
    DelayQuad total()
    {
        const auto &user = data->users.at(sink->ports.at(id_A).user_idx);
        DelayQuad route;
        EXPECT_TRUE(ctx->getArcDelayOverride(data, user, route));
        return route + ctx->get_lut_input_delay(sink, id_A);
    }
};

TEST_F(LutRouteMinimumTest, FittedFlipFlopRouteUsesTheNativeScaleWithoutChangingTheRbf)
{
    std::vector<uint8_t> before, after; ctx->cyclonev->rbf_save(before);
    auto baseline = total();
    // Independently retained native FF-route trace at Mistral 5e311528d8.
    // These also expose an engine change that would double-apply this opt-in.
    EXPECT_EQ(baseline.minRiseDelay(), 569); EXPECT_EQ(baseline.maxRiseDelay(), 633);
    EXPECT_EQ(baseline.minFallDelay(), 540); EXPECT_EQ(baseline.maxFallDelay(), 576);
    ctx->use_lut_route_min_reference = true;
    auto measured = total();
    EXPECT_EQ(measured.minRiseDelay(), 525); EXPECT_EQ(measured.maxRiseDelay(), 634);
    EXPECT_EQ(measured.minFallDelay(), 498); EXPECT_EQ(measured.maxFallDelay(), 577);
    EXPECT_LT(measured.minRiseDelay(), baseline.minRiseDelay());
    EXPECT_LT(measured.minFallDelay(), baseline.minFallDelay());
    EXPECT_GE(measured.maxRiseDelay(), baseline.maxRiseDelay());
    EXPECT_GE(measured.maxFallDelay(), baseline.maxFallDelay());
    EXPECT_LE(measured.maxRiseDelay()-baseline.maxRiseDelay(), 1);
    EXPECT_LE(measured.maxFallDelay()-baseline.maxFallDelay(), 1);
    ctx->use_lut_route_min_reference = false;
    auto restored = total();
    EXPECT_EQ(restored.minRiseDelay(), baseline.minRiseDelay());
    EXPECT_EQ(restored.maxFallDelay(), baseline.maxFallDelay());
    ctx->cyclonev->rbf_save(after); EXPECT_EQ(before, after);
}

TEST_F(LutRouteMinimumTest, DeviceAndDependencyGatesReject)
{
    ctx->use_lut_route_min_reference = true;
    EXPECT_NO_THROW(ctx->validate_lut_route_min_reference());
    ctx->use_lut_core_reference = false;
    EXPECT_THROW(ctx->validate_lut_route_min_reference(), log_execution_error_exception);
    ctx->use_lut_core_reference = true;
    ctx->settings[ctx->id("mistral/routed_delay_ranges")] = false;
    EXPECT_THROW(ctx->validate_lut_route_min_reference(), log_execution_error_exception);
    ctx->settings[ctx->id("mistral/routed_delay_ranges")] = true;
    ctx->args.device = "uncharacterized";
    EXPECT_THROW(ctx->validate_lut_route_min_reference(), log_execution_error_exception);
}

TEST_F(LutRouteMinimumTest, ADisconnectedRouteCannotFallBack)
{
    ctx->use_lut_route_min_reference = true;
    ctx->unbindPip(PipId(CV::rnode_coords(CV::TD,28,2,6), CV::rnode_coords(CV::GOUT,28,2,77)));
    EXPECT_THROW(total(), log_execution_error_exception);
}
