#include "ready_shortest.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
USING_NEXTPNR_NAMESPACE
TEST(ReadyShortestTest, ExactWeightedOptimumObstaclesAndZeroCycle)
{
    using namespace ready_shortest;
    std::map<uint32_t, std::vector<Edge>> graph{{0, {{1, 9, true}, {2, 1, true}, {4, 0, false}}},
                                                {1, {{3, 1, true}}},
                                                {2, {{0, 0, true}, {1, 1, true}, {3, 20, true}}},
                                                {4, {{3, 0, true}}}};
    auto result = search(0, 3, 0, [&](uint32_t n) { return graph[n]; });
    ASSERT_TRUE(result.reachable);
    EXPECT_EQ(result.distance, 3);
    EXPECT_EQ(result.nodes.at(3).predecessor, 1);
    EXPECT_EQ(result.nodes.at(1).predecessor, 2);
    EXPECT_EQ(result.nodes.at(0).predecessor, -1);
    EXPECT_FALSE(result.nodes.at(3).expanded);
    EXPECT_EQ(result.nodes.count(4), 0);
    EXPECT_EQ(result.nodes.at(2).settled_index, 1);
}
TEST(ReadyShortestTest, StableTiesAndUnreachable)
{
    using namespace ready_shortest;
    std::map<uint32_t, std::vector<Edge>> graph{
            {0, {{2, 0, true}, {1, 0, true}}}, {1, {{2, 0, true}, {3, 2, true}}}, {2, {{1, 0, true}, {3, 2, true}}}};
    auto a = search(0, 3, 0, [&](uint32_t n) { return graph[n]; });
    EXPECT_EQ(a.nodes.at(3).predecessor, 1);
    EXPECT_EQ(a.nodes.at(1).predecessor, 0);
    EXPECT_EQ(a.nodes.at(2).predecessor, 0);
    auto b = search(0, 9, 0, [&](uint32_t n) { return graph[n]; });
    EXPECT_FALSE(b.reachable);
    EXPECT_EQ(b.nodes.size(), 4);
}
TEST(ReadyShortestTest, RejectsNegativeAndOverflowCosts)
{
    using namespace ready_shortest;
    EXPECT_THROW(search(0, 1, 0, [](uint32_t) { return std::vector<Edge>{{1, -1, true}}; }), std::runtime_error);
    EXPECT_THROW(search(0, 1, INT64_MAX - 1, [](uint32_t) { return std::vector<Edge>{{1, 2, true}}; }),
                 std::runtime_error);
}

#include <cstdlib>
#include <fstream>
#include "ready_shortest_backend.h"
TEST(ReadyShortestTest, BackendRespectsForeignWireAndPreservesTargetBinding)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto target = ctx.createNet(ctx.id("target")), other = ctx.createNet(ctx.id("other"));
    auto src = ctx.add_wire(1, 1, ctx.id("source")), old = ctx.add_wire(2, 1, ctx.id("old"));
    auto block = ctx.add_wire(2, 2, ctx.id("blocked")), alt = ctx.add_wire(2, 3, ctx.id("alternative"));
    auto dst = ctx.add_wire(3, 1, ctx.id("sink"));
    auto p0 = ctx.add_pip(src, old), p1 = ctx.add_pip(old, dst);
    auto b0 = ctx.add_pip(src, block);
    ctx.add_pip(block, dst);
    auto a0 = ctx.add_pip(src, alt), a1 = ctx.add_pip(alt, dst);
    // Real backend observed delays distinguish the optimum from both the old
    // path and an apparently shorter path whose destination is occupied.
    ctx.pip_delay_calibrated = true;
    ctx.pip_delay_observed[p0] = 20;
    ctx.pip_delay_observed[p1] = 20;
    ctx.pip_delay_observed[a0] = 4;
    ctx.pip_delay_observed[a1] = 5;
    ctx.pip_delay_observed[b0] = 0;
    ctx.bindWire(src, target, STRENGTH_WEAK);
    ctx.bindPip(p0, target, STRENGTH_WEAK);
    ctx.bindPip(p1, target, STRENGTH_WEAK);
    ctx.bindWire(block, other, STRENGTH_WEAK);
    auto result = ready_shortest_candidate(&ctx, target, src, dst, "/tmp/mistral-ready-shortest-test");
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].route_delay, 9);
    EXPECT_EQ(result[0].variant, 200);
    ASSERT_EQ(result[0].wires.size(), 3);
    EXPECT_EQ(result[0].wires[1].first, alt);
    EXPECT_EQ(ctx.getBoundWireNet(block), other);
    EXPECT_EQ(ctx.getBoundWireNet(old), target);
    EXPECT_EQ(ctx.getBoundWireNet(alt), nullptr);
    EXPECT_EQ(target->wires.at(dst).pip, p1);
    ctx.ripupNet(target->name);
    ctx.bindWire(src, target, STRENGTH_WEAK);
    for (size_t i = 1; i < result[0].wires.size(); ++i) {
        auto p = result[0].wires[i].second;
        ASSERT_TRUE(ctx.checkPipAvailForNet(p, target));
        ASSERT_TRUE(ctx.checkWireAvail(ctx.getPipDstWire(p)));
        ctx.bindPip(p, target, STRENGTH_WEAK);
    }
    EXPECT_EQ(target->wires.at(dst).pip, a1);
    ctx.ripupNet(target->name);
    ctx.bindWire(src, target, STRENGTH_WEAK);
    ctx.bindPip(p0, target, STRENGTH_WEAK);
    ctx.bindPip(p1, target, STRENGTH_WEAK);
    EXPECT_EQ(target->wires.at(dst).pip, p1);
    EXPECT_EQ(ctx.getBoundWireNet(block), other);
}
TEST(ReadyShortestTest, DisabledPrefixIgnoresModeBeforeIdentifierAllocation)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    const char *old = std::getenv("NEXTPNR_MISTRAL_READY_SHORTEST");
    std::string saved = old ? old : "";
    bool existed = old;
    const char *old_relaxed = std::getenv("NEXTPNR_MISTRAL_READY_RELAXED");
    std::string saved_relaxed = old_relaxed ? old_relaxed : "";
    bool relaxed_existed = old_relaxed;
    setenv("NEXTPNR_MISTRAL_READY_RELAXED", "invalid-relaxed-mode-ignored", 1);
    setenv("NEXTPNR_MISTRAL_READY_SHORTEST", "invalid-mode-ignored", 1);
    auto before = ctx.id("before-mode-only");
    ctx.ready_fallback_pass(nullptr);
    ctx.ready_fallback_pass("");
    EXPECT_EQ(ctx.id("after-mode-only").index, before.index + 1);
    if (existed)
        setenv("NEXTPNR_MISTRAL_READY_SHORTEST", saved.c_str(), 1);
    else
        unsetenv("NEXTPNR_MISTRAL_READY_SHORTEST");
    if (relaxed_existed)
        setenv("NEXTPNR_MISTRAL_READY_RELAXED", saved_relaxed.c_str(), 1);
    else
        unsetenv("NEXTPNR_MISTRAL_READY_RELAXED");
}

TEST(ReadyShortestTest, RelaxedOccupancyRetainsStaticReservationAndEveryBinding)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto target = ctx.createNet(ctx.id("relaxed-target")), other = ctx.createNet(ctx.id("relaxed-other"));
    auto src = ctx.add_wire(1, 1, ctx.id("src")), old = ctx.add_wire(2, 1, ctx.id("old"));
    auto occupied = ctx.add_wire(2, 2, ctx.id("occupied")), reserved = ctx.add_wire(2, 3, ctx.id("reserved"));
    auto gate = ctx.add_wire(1, 3, ctx.id("reservation-source")), dst = ctx.add_wire(3, 1, ctx.id("dst"));
    auto p0 = ctx.add_pip(src, old), p1 = ctx.add_pip(old, dst);
    auto q0 = ctx.add_pip(src, occupied), q1 = ctx.add_pip(occupied, dst);
    auto r0 = ctx.add_pip(src, reserved), r1 = ctx.add_pip(reserved, dst);
    ctx.add_pip(gate, reserved);
    auto &reserved_info = ctx.wires.at(reserved);
    auto found = std::find(reserved_info.wires_uphill.begin(), reserved_info.wires_uphill.end(), gate);
    ASSERT_NE(found, reserved_info.wires_uphill.end());
    reserved_info.flags |= WireInfo::RESERVED_ROUTE | (found - reserved_info.wires_uphill.begin());
    ASSERT_TRUE(ctx.is_pip_blocked(r0));
    ctx.pip_delay_calibrated = true;
    for (auto p : {p0, p1})
        ctx.pip_delay_observed[p] = 20;
    ctx.pip_delay_observed[q0] = 3;
    ctx.pip_delay_observed[q1] = 4;
    ctx.pip_delay_observed[r0] = 0;
    ctx.pip_delay_observed[r1] = 0;
    ctx.bindWire(src, target, STRENGTH_WEAK);
    ctx.bindPip(p0, target, STRENGTH_WEAK);
    ctx.bindPip(p1, target, STRENGTH_WEAK);
    // Foreign destination and pip ownership: bindPip deliberately establishes
    // this collector fixture without a logical source; it is not a legal FPGA.
    ctx.bindPip(q0, other, STRENGTH_WEAK);
    ASSERT_EQ(ctx.getBoundPipNet(q0), other);
    auto observed = ctx.pip_delay_observed;
    auto exact = ready_shortest_candidate(&ctx, target, src, dst, "/tmp/mistral-ready-relaxed-fixed");
    ASSERT_EQ(exact.size(), 1);
    EXPECT_EQ(exact[0].route_delay, 40);
    auto relaxed = ready_shortest_candidate(&ctx, target, src, dst, "/tmp/mistral-ready-relaxed-test", true);
    ASSERT_EQ(relaxed.size(), 1);
    EXPECT_EQ(relaxed[0].route_delay, 7);
    EXPECT_EQ(relaxed[0].variant, 201);
    ASSERT_EQ(relaxed[0].wires.size(), 3);
    EXPECT_EQ(relaxed[0].wires[1].first, occupied);
    EXPECT_EQ(ctx.getBoundWireNet(occupied), other);
    EXPECT_EQ(ctx.getBoundPipNet(q0), other);
    EXPECT_EQ(ctx.getBoundPipNet(p0), target);
    EXPECT_EQ(ctx.getBoundPipNet(p1), target);
    EXPECT_EQ(ctx.getBoundWireNet(reserved), nullptr);
    EXPECT_EQ(ctx.getBoundPipNet(r0), nullptr);
    EXPECT_EQ(ctx.pip_delay_observed, observed);
    EXPECT_EQ(target->wires.size(), 3);
    EXPECT_EQ(other->wires.size(), 1);
}
