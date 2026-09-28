#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include "gtest/gtest.h"
#include "json_frontend.h"
#include "nextpnr.h"
NEXTPNR_NAMESPACE_BEGIN
void diagnostic_placed_timeout(Context *, const char *);
NEXTPNR_NAMESPACE_END
USING_NEXTPNR_NAMESPACE
TEST(PlacedTimeoutTest, DisabledIsIdentifierNeutral)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    auto before = ctx.id("timeout-test-before");
    diagnostic_placed_timeout(&ctx, nullptr);
    diagnostic_placed_timeout(&ctx, "");
    auto after = ctx.id("timeout-test-after");
    EXPECT_EQ(after.index, before.index + 1);
    EXPECT_TRUE(ctx.cells.empty());
    EXPECT_TRUE(ctx.nets.empty());
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
    ctx.assignArchInfo();
    for (auto &e : ctx.cells)
        if (e.second->bel != BelId())
            ASSERT_TRUE(ctx.isBelLocationValid(e.second->bel)) << e.first.str(&ctx);
    auto count = ctx.cells.size();
    const char *prefix = std::getenv("MISTRAL_TIMEOUT_TEST_PREFIX");
    ASSERT_TRUE(prefix && *prefix);
    diagnostic_placed_timeout(&ctx, prefix);
    EXPECT_EQ(ctx.cells.size(), count + 35);
    for (auto &e : ctx.cells)
        if (e.second->bel != BelId())
            EXPECT_TRUE(ctx.isBelLocationValid(e.second->bel)) << e.first.str(&ctx);
}
