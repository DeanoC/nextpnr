#include "gtest/gtest.h"
#include "heap_hard_blocks.h"
#include <memory>
#include <set>

USING_NEXTPNR_NAMESPACE

class HeapHardBlocksTest : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    std::unique_ptr<PlacerHeapCfg> cfg;
    NetInfo *clock, *phase, *constant;
    CellInfo *launch, *capture, *ram;
    dict<IdString, HeapHardBlockLocation> locations;

    NetInfo *clock_net(const std::string &name, int shift)
    {
        auto *net = ctx->createNet(ctx->id(name)); net->is_global = true;
        net->clkconstr = std::make_unique<ClockConstraint>();
        net->clkconstr->period = DelayPair(10000);
        net->clkconstr->high = net->clkconstr->low = DelayPair(5000);
        net->clkconstr->phase_group = ctx->id("fixture_phase_group");
        net->clkconstr->phase_shift = shift;
        return net;
    }
    CellInfo *ff(const std::string &name, NetInfo *clk)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_DATAIN, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA}) cell->addInput(pin);
        cell->addOutput(id_Q); cell->connectPort(id_CLK, clk);
        cell->pin_data[id_ENA].state = PIN_1; cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0; cell->pin_data[id_SLOAD].state = PIN_0;
        cell->connectPort(id_Q, ctx->createNet(ctx->id(name + "$q")));
        return cell;
    }
    CellInfo *memory(const std::string &name)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_M10K);
        cell->params[id_CFG_ABITS] = 12; cell->params[id_CFG_DBITS] = 1; cell->params[id_CFG_DUAL_CLOCK] = 1;
        for (auto pin : {id_CLK1, id_CLK2, id_B1EN, id_ACLR0, id_ACLR1, ctx->id("B1ADDR[0]")}) cell->addInput(pin);
        cell->connectPort(id_CLK1, clock); cell->connectPort(id_CLK2, phase);
        cell->connectPort(id_B1EN, constant);
        cell->connectPort(id_ACLR0, constant); cell->connectPort(id_ACLR1, constant);
        cell->pin_data[id_B1EN].state = PIN_1;
        cell->pin_data[id_ACLR0].state = PIN_0; cell->pin_data[id_ACLR1].state = PIN_0;
        cell->connectPort(ctx->id("B1ADDR[0]"), launch->getPort(id_Q));
        cell->addOutput(id_B1DATA); cell->connectPort(id_B1DATA, ctx->createNet(ctx->id(name + "$q")));
        return cell;
    }
    bool plan(std::vector<HeapHardBlockAssignment> &result, std::string &reason)
    {
        ctx->assignArchInfo();
        TimingAnalyser timing(ctx.get()); timing.setup_only = true; timing.setup();
        return plan_heap_hard_blocks(ctx.get(), *cfg, timing, locations, result, reason);
    }
    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7"; ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e8;
        ctx->settings[ctx->id("placerHeap/alpha")] = std::string("0.1");
        ctx->settings[ctx->id("placerHeap/timingWeight")] = 10;
        ctx->settings[ctx->id("timing_driven")] = true;
        cfg = std::make_unique<PlacerHeapCfg>(ctx.get());
        cfg->hardBlockWindows = true; cfg->hardBlockBucket = id_MISTRAL_M10K;
        cfg->hpwl_scale_x = 1; cfg->hpwl_scale_y = 2;
        cfg->hardBlockCellEligible = [](const CellInfo *c) {
            return c->type == id_MISTRAL_M10K && c->constr_children.empty() &&
                   c->constr_x == 0 && c->constr_y == 0 && c->constr_z == 0 && !c->constr_abs_z;
        };
        cfg->hardBlockStaticInput = [](const CellInfo *c, IdString pin) {
            return c->get_pin_state(pin) == PIN_0 || c->get_pin_state(pin) == PIN_1 ||
                   (c->getPort(pin) && c->getPort(pin)->driver.cell && c->getPort(pin)->driver.cell->type == id_MISTRAL_CONST);
        };
        clock = clock_net("source_clock", 0); phase = clock_net("memory_clock", 2500);
        constant = ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        auto *constant_driver = ctx->createCell(ctx->id("constant_driver"), id_MISTRAL_CONST);
        constant_driver->params[id_LUT] = 0; constant_driver->addOutput(id_Q); constant_driver->connectPort(id_Q, constant);
        launch = ff("launch", clock); launch->connectPort(id_DATAIN, launch->getPort(id_Q));
        ram = memory("memory"); capture = ff("capture", clock); capture->connectPort(id_DATAIN, ram->getPort(id_B1DATA));
        locations[ram->name] = {26, 30, true};
        locations[launch->name] = {14, 30, false}; locations[capture->name] = {41, 30, false};
    }
};

TEST_F(HeapHardBlocksTest, NativeCaptureAndReturnWindowsBothAffectAssignment)
{
    std::vector<HeapHardBlockAssignment> result, repeat; std::string reason;
    ASSERT_TRUE(plan(result, reason)) << reason; ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(ctx->getBelLocation(result[0].bel).x, 14);
    ASSERT_TRUE(plan(repeat, reason)); EXPECT_EQ(result[0].bel, repeat[0].bel);
    phase->clkconstr->phase_shift = 7500;
    ASSERT_TRUE(plan(result, reason)) << reason;
    EXPECT_EQ(ctx->getBelLocation(result[0].bel).x, 41);
    EXPECT_EQ(ram->bel, BelId()); EXPECT_EQ(ram->belStrength, STRENGTH_NONE);
}

TEST_F(HeapHardBlocksTest, RejectsCompleteBucketWhenOneMemberIsProtectedOrInteracting)
{
    std::vector<HeapHardBlockAssignment> result; std::string reason;
    for (const char *attr : {"keep", "dont_touch", "BEL", "LOC", "FES_SLOT"}) {
        ram->attrs[ctx->id(attr)] = 1;
        EXPECT_FALSE(plan(result, reason)) << attr; EXPECT_TRUE(result.empty());
        ram->attrs.erase(ctx->id(attr));
    }
    ram->constr_x = 1; EXPECT_FALSE(plan(result, reason)); ram->constr_x = 0;
    ram->constr_abs_z = true; EXPECT_FALSE(plan(result, reason)); ram->constr_abs_z = false;
    ram->constr_children.push_back(capture); EXPECT_FALSE(plan(result, reason)); ram->constr_children.clear();
    locations[ram->name].movable = false; EXPECT_FALSE(plan(result, reason)); locations[ram->name].movable = true;
    auto *other = memory("second_memory"); locations[other->name] = {26, 31, true};
    other->disconnectPort(ctx->id("B1ADDR[0]")); other->connectPort(ctx->id("B1ADDR[0]"), ram->getPort(id_B1DATA));
    EXPECT_FALSE(plan(result, reason)); EXPECT_TRUE(result.empty());
    EXPECT_NE(reason.find("direct movable"), std::string::npos) << reason;
}

TEST_F(HeapHardBlocksTest, MultiCaptureOutputKeepsEveryWindowAndRejectsHiddenUnsupportedBranch)
{
    auto *logic = ctx->createCell(ctx->id("return_logic"), id_MISTRAL_ALUT2);
    logic->params[id_LUT] = Property(int64_t(8), 4);
    logic->addInput(id_A); logic->addInput(id_B); logic->addOutput(id_Q);
    logic->connectPort(id_A, ram->getPort(id_B1DATA)); logic->connectPort(id_B, ram->getPort(id_B1DATA));
    logic->connectPort(id_Q, ctx->createNet(ctx->id("return_logic$q")));
    capture->disconnectPort(id_DATAIN); capture->connectPort(id_DATAIN, logic->getPort(id_Q));
    auto *second_clock = clock_net("second_capture_clock", 5000);
    auto *second = ff("second_capture", second_clock); second->connectPort(id_DATAIN, logic->getPort(id_Q));
    locations[logic->name] = {41, 30, false}; locations[second->name] = {41, 31, false};
    ctx->assignArchInfo(); TimingAnalyser timing(ctx.get()); timing.setup_only = true; timing.setup();
    std::vector<TimingAnalyser::SetupWindowHint> rows;
    ASSERT_TRUE(timing.get_setup_window_hints(CellPortKey(logic->name, id_A), rows));
    std::set<delay_t> windows; for (const auto &row : rows) windows.insert(row.window.value());
    EXPECT_EQ(windows, (std::set<delay_t>{2500, 7500}));
    std::vector<HeapHardBlockAssignment> result; std::string reason;
    ASSERT_TRUE(plan(result, reason)) << reason;
    auto *unknown = ctx->createCell(ctx->id("ignored_branch"), id_MISTRAL_OB);
    unknown->addInput(id_I); unknown->connectPort(id_I, logic->getPort(id_Q));
    locations[unknown->name] = {41, 32, false};
    // The ignored branch contributes no tag: the surviving known tags alone
    // must not be mistaken for complete incident timing coverage.
    ctx->assignArchInfo(); TimingAnalyser incomplete(ctx.get()); incomplete.setup_only = true; incomplete.setup();
    ASSERT_TRUE(incomplete.get_setup_window_hints(CellPortKey(logic->name, id_A), rows));
    EXPECT_FALSE(plan_heap_hard_blocks(ctx.get(), *cfg, incomplete, locations, result, reason));
    EXPECT_TRUE(result.empty());
}

TEST_F(HeapHardBlocksTest, NativeTransactionRollsBackEveryPostBindLegalityFailure)
{
    auto *second = memory("second_memory");
    auto *third = memory("third_memory");
    std::vector<CellInfo *> cells{ram, second, third};
    std::vector<BelId> bels;
    for (auto bel : ctx->getBels()) if (ctx->getBelBucketForBel(bel) == id_MISTRAL_M10K) bels.push_back(bel);
    ASSERT_GE(bels.size(), cells.size());
    std::vector<HeapHardBlockAssignment> assignment;
    for (size_t i = 0; i < cells.size(); ++i) assignment.push_back({cells[i], bels[i]});
    std::string reason;
    for (size_t rejected = 0; rejected < cells.size(); ++rejected) {
        ctx->fes_bel_region[bels[rejected]] = ctx->id("reserved_test_region");
        EXPECT_FALSE(bind_heap_hard_blocks(ctx.get(), *cfg, assignment, reason));
        EXPECT_EQ(reason, "native assignment legality");
        for (size_t i = 0; i < cells.size(); ++i) {
            EXPECT_EQ(cells[i]->bel, BelId()); EXPECT_EQ(cells[i]->belStrength, STRENGTH_NONE);
            EXPECT_TRUE(ctx->checkBelAvail(bels[i]));
        }
        ctx->fes_bel_region.clear();
    }
    ASSERT_TRUE(bind_heap_hard_blocks(ctx.get(), *cfg, assignment, reason)) << reason;
    for (size_t i = 0; i < cells.size(); ++i) {
        EXPECT_EQ(cells[i]->bel, bels[i]); EXPECT_EQ(cells[i]->belStrength, STRENGTH_WEAK);
        EXPECT_TRUE(ctx->isBelLocationValid(bels[i])); ctx->unbindBel(bels[i]);
    }
    assignment[1].bel = assignment[0].bel;
    EXPECT_FALSE(bind_heap_hard_blocks(ctx.get(), *cfg, assignment, reason));
    for (auto *cell : cells) EXPECT_EQ(cell->bel, BelId());
}

TEST_F(HeapHardBlocksTest, DeepNativeConeFallsBackBeforeRecursionBecomesUnbounded)
{
    auto *previous = ram->getPort(id_B1DATA);
    for (int i = 0; i < 150; ++i) {
        auto *cell = ctx->createCell(ctx->idf("buffer_%03d", i), id_MISTRAL_BUF);
        cell->addInput(id_A); cell->addOutput(id_Q); cell->connectPort(id_A, previous);
        previous = ctx->createNet(ctx->idf("buffer_net_%03d", i)); cell->connectPort(id_Q, previous);
        locations[cell->name] = {30, 30, false};
    }
    capture->disconnectPort(id_DATAIN); capture->connectPort(id_DATAIN, previous);
    std::vector<HeapHardBlockAssignment> result; std::string reason;
    EXPECT_FALSE(plan(result, reason)); EXPECT_TRUE(result.empty());
    EXPECT_EQ(reason, "cone depth bound");
}

TEST_F(HeapHardBlocksTest, RejectsRegisteredBoundaryWithAdditionalNativeCombinationalArcs)
{
    auto *dsp = ctx->createCell(ctx->id("hybrid_dsp"), id_MISTRAL_MUL9X9);
    auto a = ctx->id("A[0]"), y = ctx->id("Y[0]");
    dsp->params[id_INREG_CTRL_AX] = 1; dsp->params[id_OREG_CTRL] = 1;
    dsp->addInput(a); dsp->addInput(id_CLK); dsp->addOutput(y);
    dsp->connectPort(a, ram->getPort(id_B1DATA)); dsp->connectPort(id_CLK, clock);
    dsp->connectPort(y, ctx->createNet(ctx->id("hybrid_dsp$q")));
    capture->disconnectPort(id_DATAIN); capture->connectPort(id_DATAIN, dsp->getPort(y));
    locations[dsp->name] = {41, 30, false};
    int count = 0; DelayQuad delay;
    ASSERT_EQ(ctx->getPortTimingClass(dsp, a, count), TMG_REGISTER_INPUT);
    ASSERT_EQ(ctx->getPortTimingClass(dsp, y, count), TMG_REGISTER_OUTPUT);
    ASSERT_TRUE(ctx->getCellDelay(dsp, a, y, delay));
    std::vector<HeapHardBlockAssignment> result; std::string reason;
    EXPECT_FALSE(plan(result, reason)); EXPECT_TRUE(result.empty());
    // Reverse direction: a RAM address driven by a hybrid registered output.
    dsp->disconnectPort(a); dsp->connectPort(a, launch->getPort(id_Q));
    ram->disconnectPort(ctx->id("B1ADDR[0]")); ram->connectPort(ctx->id("B1ADDR[0]"), dsp->getPort(y));
    capture->disconnectPort(id_DATAIN); capture->connectPort(id_DATAIN, ram->getPort(id_B1DATA));
    EXPECT_FALSE(plan(result, reason)); EXPECT_TRUE(result.empty());
}
