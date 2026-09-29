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
    std::string remap_report(bool stale = false)
    {
        auto inner = ctx->createCell(ctx->id("pre_enable"), id_MISTRAL_ALUT2);
        inner->params[id_LUT] = 0x8;
        inner->addInput(id_A); inner->addInput(id_B); inner->addOutput(id_Q);
        inner->connectPort(id_A, source_a->getPort(id_Q));
        inner->pin_data[id_B].state = PIN_1;
        auto net = ctx->createNet(ctx->id("intermediate"));
        inner->connectPort(id_Q, net);
        driver->disconnectPort(id_A); driver->connectPort(id_A, net);
        ctx->assignArchInfo(); place(inner, 24, 20);
        return std::string(R"({"critical_paths":[{"max_delay":1,"path":[
          {"type":"routing","delay":2,"net":"intermediate",
           "from":{"cell":"pre_enable","port":"Q","loc":[24,20]},
           "to":{"cell":"enable_logic","port":"A","loc":[24,20]}},
          {"type":"logic","delay":0.4,
           "from":{"cell":"enable_logic","port":"A","loc":[24,20]},
           "to":{"cell":"enable_logic","port":"Q","loc":[24,20]}},
          {"type":"routing","delay":2,"net":"enable",
           "from":{"cell":"enable_logic","port":"Q","loc":[24,20]},
           "to":{"cell":"near_a","port":"ENA","loc":[)") + (stale ? "31" : "30") + R"(,20]}},
          {"type":"setup","delay":0,
           "from":{"cell":"near_a","port":"ENA","loc":[30,20]},
           "to":{"cell":"near_a","port":"ENA","loc":[30,20]}}
        ]}]})";
    }
    void drive_clock(NetInfo *net)
    {
        auto source = ctx->createCell(ctx->idf("%s$source", net->name.c_str(ctx.get())), id_MISTRAL_CLKBUF);
        source->addInput(id_A); source->addOutput(id_Q); source->connectPort(id_Q, net);
        ctx->assignArchInfo();
        for (auto bel : ctx->getBels()) if (ctx->checkBelAvail(bel) && ctx->isValidBelForCellType(source->type, bel)) {
            ctx->bindBel(bel, source, STRENGTH_LOCKED); return;
        }
        FAIL() << "No clock source BEL";
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

TEST_F(EnableReplicationTest, LocalRemapListsWithoutMutation)
{
    auto report = remap_report();
    std::map<IdString, BelId> bels;
    std::map<std::pair<IdString, IdString>, std::pair<NetInfo *, int>> ports;
    for (auto &c : ctx->cells) {
        bels[c.first] = c.second->bel;
        for (auto &p : c.second->ports) ports[{c.first, p.first}] = {p.second.net, p.second.user_idx.idx()};
    }
    auto nc = ctx->cells.size(), nn = ctx->nets.size(), na = ctx->net_aliases.size();
    EXPECT_FALSE(ctx->remap_critical(report, -1));
    EXPECT_EQ(ctx->cells.size(), nc); EXPECT_EQ(ctx->nets.size(), nn); EXPECT_EQ(ctx->net_aliases.size(), na);
    for (auto &c : bels) EXPECT_EQ(ctx->cells.at(c.first)->bel, c.second);
    for (auto &p : ports) {
        auto actual = ctx->cells.at(p.first.first)->ports.at(p.first.second);
        EXPECT_EQ(actual.net, p.second.first); EXPECT_EQ(actual.user_idx.idx(), p.second.second);
    }
    ctx->check();
}

TEST_F(EnableReplicationTest, LocalRemapPreservesWholeGroupAndSideUsers)
{
    auto report = remap_report();
    ASSERT_TRUE(ctx->remap_critical(report, 0));
    auto net = near_a->getPort(id_ENA);
    ASSERT_NE(net, enable);
    EXPECT_EQ(near_b->getPort(id_ENA), net);
    EXPECT_EQ(near_b->get_pin_state(id_ENA), PIN_INV);
    EXPECT_EQ(remote->getPort(id_ENA), enable);
    EXPECT_EQ(driver->getPort(id_A)->name, ctx->id("intermediate"));
    EXPECT_EQ(net->driver.cell->type, id_MISTRAL_ALUT2);
    EXPECT_TRUE(ctx->isBelLocationValid(net->driver.cell->bel));
    ctx->check();
}

TEST_F(EnableReplicationTest, LocalRemapRejectsStaleReportAndProtectedGroup)
{
    auto report = remap_report(true);
    auto nc = ctx->cells.size(), nn = ctx->nets.size();
    EXPECT_THROW(ctx->remap_critical(report, 0), log_execution_error_exception);
    EXPECT_EQ(ctx->cells.size(), nc); EXPECT_EQ(ctx->nets.size(), nn);
    auto at = report.find("31,20"); report.replace(at, 5, "30,20");
    near_b->attrs[ctx->id("dont_touch")] = 1;
    EXPECT_FALSE(ctx->remap_critical(report, 0));
    EXPECT_EQ(ctx->cells.size(), nc); EXPECT_EQ(ctx->nets.size(), nn);
}

TEST_F(EnableReplicationTest, LocalRemapMovementPreservesRegisterState)
{
    auto report = remap_report();
    std::ostringstream evidence;
    log_streams.emplace_back(&evidence, LogLevel::INFO_MSG);
    ctx->remap_critical(report, -1);
    log_streams.pop_back();
    std::istringstream lines(evidence.str());
    std::string line;
    bool moving = false;
    int selected = -1;
    while (std::getline(lines, line)) {
        if (line.find("Local remap trial ") != std::string::npos)
            moving = line.find("shift=0,0 ") == std::string::npos;
        auto at = line.find("Local remap candidate ");
        if (moving && at != std::string::npos) { selected = std::stoi(line.substr(at + 22)); break; }
    }
    ASSERT_GE(selected, 0) << evidence.str();
    auto before = ctx->getBelLocation(near_a->bel);
    auto old_bel = near_b->bel;
    auto old_params = near_b->params;
    ASSERT_TRUE(ctx->remap_critical(report, selected));
    auto after = ctx->getBelLocation(near_a->bel);
    EXPECT_EQ(std::abs(after.x-before.x) + std::abs(after.y-before.y), 1);
    EXPECT_EQ(after.z, before.z);
    EXPECT_EQ(ctx->getBelLocation(near_b->bel).z, ctx->getBelLocation(old_bel).z);
    EXPECT_EQ(near_b->params, old_params);
    EXPECT_EQ(near_b->get_pin_state(id_ENA), PIN_INV);
    EXPECT_EQ(near_b->getPort(id_CLK), clock);
    for (auto &c : ctx->cells) EXPECT_TRUE(ctx->isBelLocationValid(c.second->bel));
}

TEST_F(EnableReplicationTest, LocalRemapInvalidIndexRestoresUserSlots)
{
    auto report = remap_report();
    auto saved = near_a->ports.at(id_ENA).user_idx;
    auto loads = source_a->getPort(id_Q)->users.entries();
    EXPECT_FALSE(ctx->remap_critical(report, 9999));
    EXPECT_EQ(near_a->getPort(id_ENA), enable);
    EXPECT_EQ(near_a->ports.at(id_ENA).user_idx, saved);
    EXPECT_EQ(source_a->getPort(id_Q)->users.entries(), loads);
    // Repeating the same attempt also catches dangling aliases/free-list damage.
    EXPECT_FALSE(ctx->remap_critical(report, 9999));
    EXPECT_EQ(near_a->ports.at(id_ENA).user_idx, saved);
    ctx->check();
}

TEST_F(EnableReplicationTest, LocalRemapSharedClockInputExcluded)
{
    auto report = remap_report();
    CellInfo *clock_user = ff("clock_user", nullptr);
    clock_user->disconnectPort(id_CLK);
    clock_user->connectPort(id_CLK, source_a->getPort(id_Q));
    ctx->assignArchInfo(); place(clock_user, 15, 20);
    auto count = ctx->cells.size();
    EXPECT_FALSE(ctx->remap_critical(report, 0));
    EXPECT_EQ(ctx->cells.size(), count);
    EXPECT_EQ(near_a->getPort(id_ENA), enable);
}

TEST_F(EnableReplicationTest, LocalRemapSharedHardInputExcluded)
{
    auto report = remap_report();
    auto hard = ctx->createCell(ctx->id("hard_boundary"), id_cyclonev_hps_interface_fpga2sdram);
    IdString pin = ctx->id("cmd_data_0[0]");
    hard->addInput(pin); hard->connectPort(pin, source_a->getPort(id_Q));
    ctx->assignArchInfo();
    for (auto bel : ctx->getBels()) if (ctx->isValidBelForCellType(hard->type, bel)) {
        ctx->bindBel(bel, hard, STRENGTH_WEAK); break;
    }
    auto count = ctx->cells.size();
    EXPECT_FALSE(ctx->remap_critical(report, 0));
    EXPECT_EQ(ctx->cells.size(), count);
}

TEST_F(EnableReplicationTest, LocalRemapKeepsBoundarySourceRegistersFixed)
{
    auto report = remap_report();
    auto hard = ctx->createCell(ctx->id("hard_boundary"), id_cyclonev_hps_interface_fpga2sdram);
    IdString pin = ctx->id("cmd_data_0[0]");
    hard->addInput(pin); hard->connectPort(pin, near_a->getPort(id_Q));
    ctx->assignArchInfo();
    for (auto bel : ctx->getBels()) if (ctx->isValidBelForCellType(hard->type, bel)) {
        ctx->bindBel(bel, hard, STRENGTH_WEAK); break;
    }
    std::ostringstream evidence;
    log_streams.emplace_back(&evidence, LogLevel::INFO_MSG);
    ctx->remap_critical(report, -1);
    log_streams.pop_back();
    std::istringstream lines(evidence.str()); std::string line;
    int trials = 0;
    while (std::getline(lines, line)) if (line.find("Local remap trial ") != std::string::npos) {
        EXPECT_NE(line.find("shift=0,0 "), std::string::npos) << line;
        ++trials;
    }
    EXPECT_GT(trials, 0);
    ctx->check();
}

TEST_F(EnableReplicationTest, LocalRemapAllowsGuardedTimedHardDataLoad)
{
    auto report = remap_report();
    drive_clock(clock);
    auto hard = ctx->createCell(ctx->id("timed_boundary"), id_cyclonev_hps_interface_fpga2sdram);
    IdString pin = ctx->id("cmd_valid_0"), cp = ctx->id("cmd_port_clk_0");
    hard->addInput(pin); hard->connectPort(pin, source_a->getPort(id_Q));
    hard->addInput(cp); hard->connectPort(cp, clock);
    ctx->assignArchInfo();
    for (auto bel : ctx->getBels()) if (ctx->isValidBelForCellType(hard->type, bel)) {
        ctx->bindBel(bel, hard, STRENGTH_WEAK); break;
    }
    auto bel = hard->bel; auto net = hard->getPort(pin);
    TimingAnalyser before(ctx.get()); before.setup(false, false, true);
    float slack = before.get_setup_slack(CellPortKey(hard->name, pin));
    hard->attrs[ctx->id("dont_touch")] = 1;
    EXPECT_FALSE(ctx->remap_critical(report, 0));
    hard->attrs.erase(ctx->id("dont_touch"));
    ASSERT_TRUE(ctx->remap_critical(report, 0));
    EXPECT_EQ(hard->bel, bel); EXPECT_EQ(hard->getPort(pin), net);
    TimingAnalyser after(ctx.get()); after.setup(false, false, true);
    EXPECT_GE(after.get_setup_slack(CellPortKey(hard->name, pin)), slack);
}

TEST_F(EnableReplicationTest, LocalRemapRejectsUnrelatedHardCaptureClock)
{
    auto report = remap_report();
    auto other = ctx->createNet(ctx->id("unrelated_clock"));
    other->is_global = true;
    other->clkconstr = std::make_unique<ClockConstraint>();
    other->clkconstr->period = DelayPair(1000);
    other->clkconstr->high = other->clkconstr->low = DelayPair(500);
    drive_clock(other);
    auto hard = ctx->createCell(ctx->id("untimed_boundary"), id_cyclonev_hps_interface_fpga2sdram);
    IdString pin = ctx->id("cmd_valid_0"), cp = ctx->id("cmd_port_clk_0");
    hard->addInput(pin); hard->connectPort(pin, source_a->getPort(id_Q));
    hard->addInput(cp); hard->connectPort(cp, other);
    ctx->assignArchInfo();
    for (auto bel : ctx->getBels()) if (ctx->isValidBelForCellType(hard->type, bel)) {
        ctx->bindBel(bel, hard, STRENGTH_WEAK); break;
    }
    int clock_count = 0;
    ASSERT_EQ(ctx->getPortTimingClass(hard, pin, clock_count), TMG_REGISTER_INPUT);
    ASSERT_EQ(clock_count, 1);
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    EXPECT_EQ(timing.get_setup_slack(CellPortKey(hard->name, pin)), float(std::numeric_limits<delay_t>::max()));
    auto count = ctx->cells.size();
    EXPECT_FALSE(ctx->remap_critical(report, 0));
    EXPECT_EQ(ctx->cells.size(), count);
}
