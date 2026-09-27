#include <memory>
#include <map>
#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"
#include <sstream>

USING_NEXTPNR_NAMESPACE

class EnableReplicationTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *driver, *source_a, *source_b, *near_a, *near_b, *remote;
    NetInfo *clock, *enable;
    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e9;
        clock = ctx->createNet(ctx->id("clock"));
        clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>();
        clock->clkconstr->period = DelayPair(1000);
        clock->clkconstr->high = clock->clkconstr->low = DelayPair(500);
        // Required by FF control-set handling even if this fixture has no SCLR.
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        source_a = ff("source_a", nullptr);
        source_b = ff("source_b", nullptr);
        driver = ctx->createCell(ctx->id("enable_logic"), id_MISTRAL_ALUT3);
        driver->params[id_LUT] = 0x96;
        for (IdString pin : {id_A, id_B, id_C}) driver->addInput(pin);
        driver->addOutput(id_Q);
        driver->connectPort(id_A, source_a->getPort(id_Q));
        driver->connectPort(id_B, source_b->getPort(id_Q));
        driver->pin_data[id_A].state = PIN_INV;
        driver->pin_data[id_C].state = PIN_1;
        enable = ctx->createNet(ctx->id("enable"));
        driver->connectPort(id_Q, enable);
        near_a = ff("near_a", enable);
        near_b = ff("near_b", enable);
        remote = ff("remote", enable);
        near_b->pin_data[id_ENA].state = PIN_INV;
        ctx->assignArchInfo();
        place(source_a, 30, 20);
        place(source_b, 30, 20);
        place(driver, 24, 20);
        place(near_a, 30, 20);
        place(near_b, 30, 20);
        place(remote, 24, 20);
    }
    CellInfo *ff(const char *name, NetInfo *ena)
    {
        CellInfo *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (IdString pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clock);
        if (ena) cell->connectPort(id_ENA, ena);
        else cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        auto output = ctx->createNet(ctx->idf("%s$q", name));
        cell->connectPort(id_Q, output);
        cell->connectPort(id_DATAIN, output);
        return cell;
    }
    void place(CellInfo *cell, int x, int y)
    {
        for (auto bel : ctx->getBelsByTile(x, y)) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel)) continue;
            ctx->bindBel(bel, cell, STRENGTH_WEAK);
            if (ctx->isBelLocationValid(bel)) return;
            ctx->unbindBel(bel);
        }
        FAIL() << "No legal fixture BEL for " << cell->name.str(ctx.get()) << " at " << x << "," << y;
    }
    CellInfo *clone() { auto i = ctx->cells.find(ctx->id("enable_logic$enable_replica")); return i == ctx->cells.end() ? nullptr : i->second.get(); }
};

TEST_F(EnableReplicationTest, WholeLabAndPinStatesPreserved)
{
    std::map<IdString, BelId> placement;
    for (const auto &entry : ctx->cells) placement[entry.first] = entry.second->bel;
    ctx->replicate_enables(1);
    auto copy = clone();
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->type, driver->type);
    EXPECT_EQ(copy->params.at(id_LUT), driver->params.at(id_LUT));
    for (IdString pin : {id_A, id_B, id_C}) {
        EXPECT_EQ(copy->getPort(pin), driver->getPort(pin));
        EXPECT_EQ(copy->get_pin_state(pin), driver->get_pin_state(pin));
    }
    EXPECT_EQ(near_a->getPort(id_ENA), copy->getPort(id_Q));
    EXPECT_EQ(near_b->getPort(id_ENA), copy->getPort(id_Q));
    EXPECT_EQ(near_b->get_pin_state(id_ENA), PIN_INV);
    EXPECT_EQ(remote->getPort(id_ENA), enable);
    for (const auto &entry : placement) EXPECT_EQ(ctx->cells.at(entry.first)->bel, entry.second);
}

TEST_F(EnableReplicationTest, DisabledIsNoOp)
{
    auto count = ctx->cells.size();
    ctx->replicate_enables(0);
    EXPECT_EQ(ctx->cells.size(), count);
    EXPECT_EQ(clone(), nullptr);
    EXPECT_EQ(near_a->getPort(id_ENA), enable);
}

TEST_F(EnableReplicationTest, ProtectedAndMixedConsumersExcluded)
{
    driver->attrs[ctx->id("dont_touch")] = 1;
    ctx->replicate_enables(1);
    EXPECT_EQ(clone(), nullptr);
    driver->attrs.erase(ctx->id("dont_touch"));
    source_a->disconnectPort(id_DATAIN);
    source_a->connectPort(id_DATAIN, enable);
    ctx->assignArchInfo();
    ctx->replicate_enables(1);
    EXPECT_EQ(clone(), nullptr);
}

TEST_F(EnableReplicationTest, FrozenSinkExcluded)
{
    BelId frozen = near_a->bel;
    ctx->unbindBel(frozen);
    ctx->bindBel(frozen, near_a, STRENGTH_LOCKED);
    ctx->replicate_enables(1);
    EXPECT_EQ(clone(), nullptr);
}

TEST_F(EnableReplicationTest, InputRegressionRollsBackTemporaryClone)
{
    // One source remains near the original LUT. A candidate near the distant
    // sinks would improve output delay while making this input worse.
    ctx->unbindBel(source_b->bel);
    place(source_b, 24, 20);
    auto cells = ctx->cells.size(), nets = ctx->nets.size();
    auto users_a = source_a->getPort(id_Q)->users.entries();
    auto users_b = source_b->getPort(id_Q)->users.entries();
    std::ostringstream evidence;
    log_streams.emplace_back(&evidence, LogLevel::INFO_MSG);
    ctx->replicate_enables(1);
    log_streams.pop_back();
    EXPECT_NE(evidence.str().find("Enable replication considering 'enable_logic'"), std::string::npos);
    EXPECT_EQ(clone(), nullptr);
    EXPECT_EQ(ctx->cells.size(), cells);
    EXPECT_EQ(ctx->nets.size(), nets);
    EXPECT_EQ(source_a->getPort(id_Q)->users.entries(), users_a);
    EXPECT_EQ(source_b->getPort(id_Q)->users.entries(), users_b);
    EXPECT_EQ(near_a->getPort(id_ENA), enable);
    EXPECT_EQ(near_b->getPort(id_ENA), enable);
    EXPECT_FALSE(ctx->net_aliases.count(ctx->id("enable_logic$enable_replica$Q")));
    // Retry the same rejected candidate: no dangling alias may assert.
    ctx->replicate_enables(1);
    EXPECT_EQ(clone(), nullptr);
}

TEST_F(EnableReplicationTest, MemoryCapableTileCanHoldOrdinaryLogic)
{
    ctx->unbindBel(driver->bel);
    ctx->unbindBel(remote->bel);
    place(driver, 15, 20);
    place(remote, 15, 20);
    ASSERT_EQ(ctx->getBelType(driver->bel), id_MISTRAL_MCOMB);
    ctx->replicate_enables(1);
    ASSERT_NE(clone(), nullptr);
}

TEST_F(EnableReplicationTest, ExistingAliasPreventsNameCollision)
{
    IdString alias = ctx->id("enable_logic$enable_replica$Q");
    ctx->net_aliases[alias] = enable->name;
    auto count = ctx->cells.size();
    ctx->replicate_enables(1);
    EXPECT_EQ(clone(), nullptr);
    EXPECT_EQ(ctx->cells.size(), count);
    EXPECT_EQ(ctx->getNetByAlias(alias), enable);
}

TEST_F(EnableReplicationTest, SharedHardBlockInputNetExcluded)
{
    CellInfo *hard = ctx->createCell(ctx->id("hard_boundary"), id_cyclonev_hps_interface_fpga2sdram);
    IdString pin = ctx->id("cmd_data_0[0]");
    hard->addInput(pin);
    hard->connectPort(pin, source_a->getPort(id_Q));
    ctx->assignArchInfo();
    bool placed = false;
    for (auto bel : ctx->getBels())
        if (ctx->isValidBelForCellType(hard->type, bel)) {
            ctx->bindBel(bel, hard, STRENGTH_WEAK); placed = true; break;
        }
    ASSERT_TRUE(placed);
    ctx->replicate_enables(1);
    EXPECT_EQ(clone(), nullptr);
}

TEST_F(EnableReplicationTest, SharedClockInputNetExcluded)
{
    CellInfo *clock_user = ff("clock_user", nullptr);
    clock_user->disconnectPort(id_CLK);
    clock_user->connectPort(id_CLK, source_a->getPort(id_Q));
    ctx->assignArchInfo();
    place(clock_user, 15, 20);
    ASSERT_NE(clock_user->bel, BelId());
    ctx->replicate_enables(1);
    EXPECT_EQ(clone(), nullptr);
}
