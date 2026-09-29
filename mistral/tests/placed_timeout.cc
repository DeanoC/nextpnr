#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include "gtest/gtest.h"
#include "json_frontend.h"
#include "log.h"
#include "nextpnr.h"
#include "placed_timeout_region.h"
#include "retained_enable_policy.h"
NEXTPNR_NAMESPACE_BEGIN
void diagnostic_placed_timeout(Context *, const char *);
void diagnostic_retained_enable(Context *, const char *, bool);
NEXTPNR_NAMESPACE_END
USING_NEXTPNR_NAMESPACE
TEST(PlacedTimeoutRegionTest, OldRootNeighborhoodsExpandOnlyRequestedDomain)
{
    TimeoutRegion region{{33, 21}, {{{27, 16}, {39, 26}}}, false};
    EXPECT_FALSE(region.contains(27, 16));
    EXPECT_FALSE(region.contains(39, 26));
    EXPECT_TRUE(region.contains(33, 21));
    EXPECT_TRUE(region.contains(39, 21));
    EXPECT_FALSE(region.contains(40, 21));
    region.expanded = true;
    EXPECT_TRUE(region.contains(27, 16));
    EXPECT_TRUE(region.contains(39, 26));
    EXPECT_TRUE(region.contains(21, 16));
    EXPECT_TRUE(region.contains(45, 26));
    EXPECT_FALSE(region.contains(20, 16));
    EXPECT_FALSE(region.contains(46, 26));
    EXPECT_TRUE(region.contains(33, 21));
    EXPECT_EQ(region.midpoint_distance(27, 16), 11);
    EXPECT_EQ(region.midpoint_distance(39, 26), 11);
}
TEST(PlacedTimeoutRegionTest, ExpandedDomainIsExactUnion)
{
    TimeoutRegion region{{33, 21}, {{{27, 16}, {39, 26}}}, true};
    for (int x = 15; x <= 50; ++x)
        for (int y = 5; y <= 40; ++y) {
            bool expected = std::abs(x - 33) + std::abs(y - 21) <= 6 || std::abs(x - 27) + std::abs(y - 16) <= 6 ||
                            std::abs(x - 39) + std::abs(y - 26) <= 6;
            EXPECT_EQ(region.contains(x, y), expected);
        }
}
TEST(RetainedEnablePolicyTest, ProtectedWholeGroupNeverMoves)
{
    std::vector<int> before{2000, 1900}, after{1000, 900};
    EXPECT_EQ(retained_enable_policy::group_gain(true, before, after), 0);
    EXPECT_EQ(retained_enable_policy::group_gain(false, before, after), 1000);
    EXPECT_EQ(retained_enable_policy::group_gain(false, before, {1000, 1800}), 0);
}
TEST(RetainedEnablePolicyTest, PriorInputLoadsRemainDisjoint)
{
    EXPECT_TRUE(retained_enable_policy::disjoint(std::set<int>{1, 2}, std::set<int>{3, 4}));
    EXPECT_FALSE(retained_enable_policy::disjoint(std::set<int>{1, 2}, std::set<int>{2, 4}));
}
TEST(RetainedEnablePolicyTest, ExistingCopyConsumesConfiguredBudget)
{
    EXPECT_TRUE(retained_enable_policy::budget_available(4, 1));
    EXPECT_FALSE(retained_enable_policy::budget_available(1, 1));
    EXPECT_FALSE(retained_enable_policy::budget_available(0, 0));
}
TEST(PlacedTimeoutTest, DisabledIsIdentifierNeutral)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    auto before = ctx.id("timeout-test-before");
    const char *old_region = std::getenv("NEXTPNR_MISTRAL_PLACED_TIMEOUT_REGION");
    const std::string saved_region = old_region ? old_region : "";
    bool had_region = old_region != nullptr;
    setenv("NEXTPNR_MISTRAL_PLACED_TIMEOUT_REGION", "invalid-but-prefix-disabled", 1);
    diagnostic_placed_timeout(&ctx, nullptr);
    diagnostic_placed_timeout(&ctx, "");
    diagnostic_retained_enable(&ctx, nullptr, false);
    diagnostic_retained_enable(&ctx, "", false);
    if (had_region)
        setenv("NEXTPNR_MISTRAL_PLACED_TIMEOUT_REGION", saved_region.c_str(), 1);
    else
        unsetenv("NEXTPNR_MISTRAL_PLACED_TIMEOUT_REGION");
    auto after = ctx.id("timeout-test-after");
    EXPECT_EQ(after.index, before.index + 1);
    EXPECT_TRUE(ctx.cells.empty());
    EXPECT_TRUE(ctx.nets.empty());
}
TEST(PlacedTimeoutTest, MissingFixtureReportsNameBeforeMutation)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    // The first generated root may still exist after a synthesis change.
    ctx.createCell(ctx.id("ddr0_test.cmd_end_MISTRAL_ALUT2_B_Q_MISTRAL_ALUT3_B"), id_MISTRAL_ALUT3);
    const size_t before_cells = ctx.cells.size();
    std::string message;
    auto previous_writer = log_write_function;
    log_write_function = [&](std::string text) { message += text; };
    EXPECT_THROW(diagnostic_placed_timeout(&ctx, "/tmp/nextpnr-missing-timeout-fixture"),
                 log_execution_error_exception);
    log_write_function = previous_writer;
    EXPECT_NE(message.find("Timeout fixture mismatch: missing cell"), std::string::npos);
    EXPECT_NE(message.find("ddr0_test.burst_end_MISTRAL_ALUT3_A"), std::string::npos);
    EXPECT_EQ(ctx.cells.size(), before_cells);
    EXPECT_TRUE(ctx.nets.empty());
}
TEST(RetainedEnableTest, MissingFixtureReportsNameBeforeMutation)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    std::string message;
    auto previous_writer = log_write_function;
    log_write_function = [&](std::string text) { message += text; };
    EXPECT_THROW(diagnostic_retained_enable(&ctx, "/tmp/nextpnr-missing-retained-fixture", false),
                 log_execution_error_exception);
    log_write_function = previous_writer;
    EXPECT_NE(message.find("Retained-enable fixture mismatch: missing cell"), std::string::npos);
    EXPECT_NE(message.find("ddr1_nack_MISTRAL_FF_Q_ENA"), std::string::npos);
    EXPECT_TRUE(ctx.cells.empty());
    EXPECT_TRUE(ctx.nets.empty());
}
// Test-only reconstruction of pack.cc constrain_carries(). The generic JSON
// writer preserves BELs but omits cluster and relative-placement metadata.
static void restore_snapshot_carries(Context &ctx, const std::string &prefix)
{
    std::set<IdString> visited;
    int arithmetic_count = 0, chains = 0;
    for (const auto &entry : ctx.cells) {
        CellInfo *cell = entry.second.get();
        if (cell->type != id_MISTRAL_ALUT_ARITH)
            continue;
        ++arithmetic_count;
        ASSERT_EQ(cell->cluster, ClusterId());
        ASSERT_TRUE(cell->constr_children.empty());
    }
    for (const auto &entry : ctx.cells) {
        CellInfo *cell = entry.second.get();
        if (cell->type != id_MISTRAL_ALUT_ARITH)
            continue;
        auto cin = cell->getPort(id_CI);
        if (cin && cin->driver.cell)
            continue;
        std::vector<CellInfo *> chain;
        CellInfo *cursor = cell;
        while (true) {
            ASSERT_EQ(cursor->type, id_MISTRAL_ALUT_ARITH);
            ASSERT_TRUE(visited.insert(cursor->name).second);
            chain.push_back(cursor);
            auto co = cursor->getPort(id_CO);
            if (!co || co->users.empty())
                break;
            ASSERT_EQ(co->users.entries(), 1);
            auto user = *co->users.begin();
            ASSERT_EQ(user.port, id_CI);
            cursor = user.cell;
        }
        ++chains;
        cell->constr_abs_z = true;
        cell->constr_z = 0;
        cell->cluster = cell->name;
        ASSERT_NE(cell->bel, BelId());
        auto base = ctx.getBelLocation(cell->bel);
        for (int i = 0; i < int(chain.size()); ++i) {
            auto c = chain.at(i);
            if (i > 0) {
                c->constr_x = 0;
                c->constr_y = -(i / 20);
                c->constr_z = ((i / 2) % 10) * 6 + (i % 2);
                c->constr_abs_z = true;
                c->cluster = cell->name;
                cell->constr_children.push_back(c);
            }
            ASSERT_NE(c->bel, BelId());
            auto actual = ctx.getBelLocation(c->bel);
            ASSERT_EQ(actual.x, base.x);
            ASSERT_EQ(actual.y, base.y - i / 20);
            ASSERT_EQ(actual.z, ((i / 2) % 10) * 6 + (i % 2));
        }
    }
    ASSERT_EQ(visited.size(), size_t(arithmetic_count));
    ASSERT_GT(chains, 0);
    std::ofstream evidence(prefix + ".carry-import.json");
    evidence << "{\"arithmetic_cells\":" << arithmetic_count << ",\"carry_chains\":" << chains
             << ",\"all_cells_covered\":true,\"all_relative_bels_match\":true,"
                "\"scope\":\"Test-only reconstruction of constrain_carries cluster and relative-layout fields\"}\n";
    evidence.close();
    ASSERT_TRUE(evidence.good());
}
TEST(PlacedTimeoutTest, ActualSnapshotPreflight)
{
    const char *path = std::getenv("MISTRAL_TIMEOUT_TEST_SNAPSHOT");
    if (!path || !*path)
        GTEST_SKIP() << "Optional diagnostic import, not a route replay";
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    std::ifstream json(path);
    ASSERT_TRUE(parse_json(json, path, &ctx));
    std::ifstream sidecar(std::string(path) + ".pins.tsv");
    ASSERT_TRUE(sidecar.good());
    std::string line;
    std::getline(sidecar, line);
    while (std::getline(sidecar, line)) {
        std::istringstream row(line);
        std::string name, port, state;
        ASSERT_TRUE(bool(std::getline(row, name, '\t')));
        ASSERT_TRUE(bool(std::getline(row, port, '\t')));
        ASSERT_TRUE(bool(std::getline(row, state)));
        auto c = ctx.cells.at(ctx.id(name)).get();
        if (!c->ports.count(ctx.id(port)) && c->ports.count(ctx.id(port + "[0]")))
            c->renamePort(ctx.id(port + "[0]"), ctx.id(port));
        c->pin_data[ctx.id(port)].state = CellPinState(std::stoi(state));
    }
    // Known diagnostic JSON scalar/index collision; restore packed PLL output.
    auto pll = ctx.cells.at(ctx.id("ram_clock.pll")).get();
    auto second = ctx.nets.at(ctx.id("ram_clock.pll_outclk_1")).get();
    ASSERT_FALSE(pll->ports.count(ctx.id("outclk[1]")));
    ASSERT_EQ(second->driver.cell, nullptr);
    pll->addOutput(ctx.id("outclk[1]"));
    pll->connectPort(ctx.id("outclk[1]"), second);
    const char *prefix = std::getenv("MISTRAL_TIMEOUT_TEST_PREFIX");
    ASSERT_TRUE(prefix && *prefix);
    ASSERT_NO_FATAL_FAILURE(restore_snapshot_carries(ctx, prefix));
    ctx.assignArchInfo();
    for (auto &e : ctx.cells)
        if (e.second->bel != BelId())
            ASSERT_TRUE(ctx.isBelLocationValid(e.second->bel)) << e.first.str(&ctx);
    auto count = ctx.cells.size();
    diagnostic_placed_timeout(&ctx, prefix);
    EXPECT_EQ(ctx.cells.size(), count + 35);
    const char *retained = std::getenv("MISTRAL_RETAINED_ENABLE_TEST_PREFIX");
    diagnostic_retained_enable(&ctx, retained, false);
    EXPECT_EQ(ctx.cells.size(), count + 35 + int(retained && *retained));
    for (auto &e : ctx.cells)
        if (e.second->bel != BelId())
            EXPECT_TRUE(ctx.isBelLocationValid(e.second->bel)) << e.first.str(&ctx);
}
