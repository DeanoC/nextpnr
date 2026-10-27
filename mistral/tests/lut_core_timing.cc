#include "nextpnr.h"
#include "log.h"
#include <gtest/gtest.h>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <filesystem>
#include <fstream>

USING_NEXTPNR_NAMESPACE

class LutCoreTimingTest : public ::testing::Test
{
  protected:
    using CV=mistral::CycloneV;
    std::unique_ptr<Context> ctx;
    CellInfo *lut;
    BelId bel;
    uint32_t lab;
    int half=0;
    const std::array<IdString,6> logical{id_A,id_B,id_C,id_D,id_E,id_F};
    void make(int n=6) {
        ArchArgs args;args.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(args);
        ctx->createNet(ctx->id("$PACKER_GND_NET"));ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        IdString types[]{id_MISTRAL_BUF,id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5,id_MISTRAL_ALUT6};
        lut=ctx->createCell(ctx->id("measured"),types[n-1]);
        for(int i=0;i<n;++i) {
            lut->addInput(logical[i]);
            auto *driver=ctx->createCell(ctx->idf("source%d",i),id_MISTRAL_BUF);
            driver->addInput(id_A);driver->addOutput(id_Q);
            auto *net=ctx->createNet(ctx->idf("data%d",i));
            driver->connectPort(id_Q,net);lut->connectPort(logical[i],net);
            ctx->assign_comb_info(driver);
        }
        lut->addOutput(id_Q);lut->connectPort(id_Q,ctx->createNet(ctx->id("out")));
        lut->params[id_LUT]=Property(int64_t(0x6996966996696996ULL),64);
        ctx->assign_comb_info(lut);
        lut->pin_data[id_Q].bel_pins={id_COMBOUT};
    }
    void SetUp() override {make();}
    void enable() {ctx->use_lut_core_reference=true;}
    DelayQuad local(IdString from=id_A) {
        DelayQuad d;EXPECT_TRUE(ctx->lut_core_delay(lut,from,id_Q,d));return d;
    }
    void physical(bool mlab=false,int lane=0,int n=6,int logical_width=0) {
        make(logical_width?logical_width:n);
        enable();half=lane;
        const auto pos=CV::xycoords(mlab?15:29,mlab?26:77);
        auto it=std::find_if(ctx->labs.begin(),ctx->labs.end(),[&](const LABInfo &v){return v.is_mlab==mlab && v.alms[0].lut_bels[0].pos==pos;});
        ASSERT_NE(it,ctx->labs.end());lab=std::distance(ctx->labs.begin(),it);
        bel=it->alms[0].lut_bels[lane];ctx->bindBel(bel,lut,STRENGTH_USER);
        const char *kind=n==6?(lane?"l6-bottom":"l6-top"):"l5";
        auto path=std::filesystem::path(__FILE__).parent_path()/"lut-carry/fixtures"/
                  (std::string(mlab?"mlab-":"lab-")+kind)/"fitted.rbf.gz";
        std::ifstream file(path,std::ios::binary);ASSERT_TRUE(file.good());
        boost::iostreams::filtering_istream stream;
        stream.push(boost::iostreams::gzip_decompressor());stream.push(file);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});
        ASSERT_FALSE(bytes.empty());ctx->cyclonev->rbf_load(bytes.data(),bytes.size());
        const std::vector<IdString> pins=n==6?std::vector<IdString>{id_A,id_B,id_C,id_D,lane?id_E1:id_E0,lane?id_F1:id_F0}:
            std::vector<IdString>{id_A,id_B,lane?id_D:id_C,lane?id_E1:id_E0,lane?id_F1:id_F0};
        for(int i=0;i<(logical_width?logical_width:n);++i) {
            lut->pin_data[logical[i]].bel_pins={pins[i]};
            ctx->bindWire(ctx->getBelPinWire(bel,pins[i]),lut->getPort(logical[i]),STRENGTH_USER);
        }
        ctx->bindWire(ctx->getBelPinWire(bel,id_COMBOUT),lut->getPort(id_Q),STRENGTH_USER);
        ctx->bitstream_configured=true;
    }
    void reject() {EXPECT_THROW(local(),log_execution_error_exception);}
};

TEST_F(LutCoreTimingTest, DefaultKeepsHistoricalModel)
{
    DelayQuad d;EXPECT_FALSE(ctx->lut_core_delay(lut,id_A,id_Q,d));
    ASSERT_TRUE(ctx->getCellDelay(lut,id_A,id_Q,d));
    EXPECT_EQ(d.minRiseDelay(),592);EXPECT_EQ(d.maxRiseDelay(),780); // Includes 0..175 ps planning mux.
    EXPECT_EQ(d.minFallDelay(),567);EXPECT_EQ(d.maxFallDelay(),748);
}
TEST_F(LutCoreTimingTest, UnplacedBoundsRetainIndependentEarlyAndLateValues)
{
    enable();auto d=local();EXPECT_LT(d.minRiseDelay(),d.maxRiseDelay());
    EXPECT_LT(d.minRiseDelay(),592);EXPECT_GT(d.maxRiseDelay(),590);
}
TEST_F(LutCoreTimingTest, FittedLabLut6UsesPhysicalReference)
{
    physical();auto a=local();
    EXPECT_EQ(a.minRiseDelay(),451);EXPECT_EQ(a.maxRiseDelay(),602);
    EXPECT_EQ(a.minFallDelay(),434);EXPECT_EQ(a.maxFallDelay(),569);
    auto f=local(id_F);EXPECT_EQ(f.minRiseDelay(),55);EXPECT_EQ(f.maxRiseDelay(),93);
}
TEST_F(LutCoreTimingTest, FittedMlabUsesItsOwnReference)
{
    physical(true);auto a=local();
    EXPECT_EQ(a.minRiseDelay(),450);EXPECT_EQ(a.maxRiseDelay(),584);
    EXPECT_EQ(a.minFallDelay(),431);EXPECT_EQ(a.maxFallDelay(),560);
}
TEST_F(LutCoreTimingTest, BottomLut5ResolvesItsPhysicalDInput)
{
    physical(false,1,5);auto d=local(id_C);
    EXPECT_EQ(d.minRiseDelay(),320);EXPECT_EQ(d.maxRiseDelay(),463);
    EXPECT_EQ(d.minFallDelay(),310);EXPECT_EQ(d.maxFallDelay(),477);
}
TEST_F(LutCoreTimingTest, SmallLogicalLutUsesItsActualNativeSixInputMode)
{
    physical(false,0,6,2);auto d=local();
    EXPECT_EQ(d.minRiseDelay(),451);EXPECT_EQ(d.maxRiseDelay(),602);
}
TEST_F(LutCoreTimingTest, BufAndNotUseTheNativeInputEnvelope)
{
    physical(false,0,5,1);auto buffer=local();
    lut->type=id_MISTRAL_NOT;auto invert=local();
    EXPECT_EQ(buffer.minRiseDelay(),invert.minRiseDelay());
    EXPECT_EQ(buffer.maxFallDelay(),invert.maxFallDelay());
}
TEST_F(LutCoreTimingTest, WrongDeviceAndUnknownTruthReject)
{
    enable();ctx->args.device="uncharacterized";reject();ctx->args.device="5CSEBA6U23I7";
    lut->params.erase(id_LUT);reject();lut->params[id_LUT]=Property(Property::Sx);reject();
}
TEST_F(LutCoreTimingTest, ConfiguredOwnerIsRequired)
{
    enable();ctx->bitstream_configured=true;reject();
}
TEST_F(LutCoreTimingTest, MissingPinAndWrongHalfReject)
{
    physical();lut->pin_data[id_A].bel_pins.clear();reject();
    lut->pin_data[id_A].bel_pins={id_E1};reject();
}
TEST_F(LutCoreTimingTest, MissingNetOwnershipRejects)
{
    physical();ctx->unbindWire(ctx->getBelPinWire(bel,id_A));reject();
}
TEST_F(LutCoreTimingTest, FeedbackAndArithmeticModesReject)
{
    physical();ASSERT_TRUE(ctx->cyclonev->bmux_m_set(CV::LAB,bel.pos,CV::TMODE,0,CV::D_F));reject();
    ASSERT_TRUE(ctx->cyclonev->bmux_m_set(CV::LAB,bel.pos,CV::TMODE,0,CV::D_E));
    ASSERT_TRUE(ctx->cyclonev->bmux_m_set(CV::LAB,bel.pos,CV::ARITH_SEL,0,CV::ADDER));reject();
}
TEST_F(LutCoreTimingTest, WrongOutputDriverRejects)
{
    physical();lut->getPort(id_Q)->driver.port=id_A;reject();
}
TEST_F(LutCoreTimingTest, FoldedLogicalInversionKeepsPhysicalInputTiming)
{
    physical();auto a=local();lut->pin_data[id_A].state=PIN_INV;auto b=local();
    EXPECT_EQ(a.minRiseDelay(),b.minRiseDelay());EXPECT_EQ(a.maxFallDelay(),b.maxFallDelay());
}
TEST_F(LutCoreTimingTest, InputBufferIsChargedExactlyOnce)
{
    physical();auto *net=lut->getPort(id_A);const auto &user=net->users.at(lut->ports.at(id_A).user_idx);
    auto &entry=ctx->analogue_arc_cache[&user];entry.ok=true;
    entry.lut_input_delay=DelayQuad(20,30);entry.delay=DelayQuad(120,330);ctx->analogue_cache_valid=true;
    DelayQuad route,cell;ASSERT_TRUE(ctx->getArcDelayOverride(net,user,route));ASSERT_TRUE(ctx->getCellDelay(lut,id_A,id_Q,cell));
    EXPECT_EQ(route.minRiseDelay(),100);EXPECT_EQ(route.maxRiseDelay(),300);
    EXPECT_EQ(cell.minRiseDelay(),471);EXPECT_EQ(cell.maxRiseDelay(),632);
    EXPECT_EQ(route.minRiseDelay()+cell.minRiseDelay(),571);EXPECT_EQ(route.maxRiseDelay()+cell.maxRiseDelay(),932);
}
TEST_F(LutCoreTimingTest, MissingAnalogueBufferCannotFallBackToEstimate)
{
    physical();ctx->analogue_cache_valid=true;DelayQuad d;
    EXPECT_THROW(ctx->getCellDelay(lut,id_A,id_Q,d),log_execution_error_exception);
}
TEST_F(LutCoreTimingTest, CarryModelIsUnchanged)
{
    enable();auto *carry=ctx->createCell(ctx->id("carry"),id_MISTRAL_ALUT_ARITH);DelayQuad d;
    ASSERT_TRUE(ctx->getCellDelay(carry,id_A,id_SO,d));
    EXPECT_EQ(d.minRiseDelay(),1300);EXPECT_EQ(d.maxRiseDelay(),1342);
}
