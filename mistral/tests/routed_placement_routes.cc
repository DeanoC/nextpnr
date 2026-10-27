/* Native physical-tree preservation and fail-closed imports. SPDX-License-Identifier: ISC */
#include "routed_placement.h"
#include "analogue_routing.h"
#include "critical_cohort_route_cost.h"
#include <algorithm>
#include "gtest/gtest.h"
#include "log.h"

USING_NEXTPNR_NAMESPACE
namespace {
using J = json11::Json;
class RoutedPlacementRoutes : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *driver, *sink_cell;
    NetInfo *net, *other;
    WireId source, middle, sink, spare;
    PipId first, last;
    J parent, witness;
    CellInfo *moved_sink = nullptr;
    WireId moved_old, moved_new;
    PipId reconnect;
    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        source = ctx->add_wire(24, 20, ctx->id("preserve_source"));
        middle = ctx->add_wire(24, 20, ctx->id("preserve_middle"));
        sink = ctx->add_wire(30, 20, ctx->id("preserve_sink"));
        spare = ctx->add_wire(25, 20, ctx->id("preserve_spare"));
        first = ctx->add_pip(source, middle);
        last = ctx->add_pip(middle, sink);
        net = ctx->createNet(ctx->id("data"));
        other = ctx->createNet(ctx->id("other"));
        driver = ctx->createCell(ctx->id("driver"), id_MISTRAL_ALUT2);
        driver->addOutput(id_Q);
        driver->params[id_LUT] = 6;
        driver->connectPort(id_Q, net);
        driver->pin_data[id_Q].bel_pins = {ctx->id("PRESERVE_Q")};
        ctx->assign_comb_info(driver);
        sink_cell = ctx->createCell(ctx->id("sink"), id_MISTRAL_ALUT2);
        sink_cell->addInput(id_A);
        sink_cell->params[id_LUT] = 6;
        sink_cell->connectPort(id_A, net);
        sink_cell->pin_data[id_A].bel_pins = {ctx->id("PRESERVE_A")};
        ctx->assign_comb_info(sink_cell);
        BelId a = ctx->getBelByLocation(Loc(24, 20, 0)), b = ctx->getBelByLocation(Loc(30, 20, 0));
        ctx->add_bel_pin(a, ctx->id("PRESERVE_Q"), PORT_OUT, source);
        ctx->add_bel_pin(b, ctx->id("PRESERVE_A"), PORT_IN, sink);
        ctx->bindBel(a, driver, STRENGTH_WEAK);
        ctx->bindBel(b, sink_cell, STRENGTH_WEAK);
        parent = routed_placement_snapshot(ctx.get());
        ctx->bindWire(source, net, STRENGTH_WEAK);
        ctx->bindPip(first, net, STRENGTH_WEAK);
        ctx->bindPip(last, net, STRENGTH_WEAK);
        witness = snapshot_routed_placement_routes(ctx.get(), parent);
        ASSERT_EQ(witness["nets"].object_items().size(), 1);
        for (auto wire : {sink, middle, source}) ctx->unbindWire(wire);
    }
    J replace_tree(J::object tree)
    {
        auto result = witness.object_items();
        result["nets"] = J::object{{"data", tree}};
        return result;
    }
    void stationary_branch_case()
    {
        ctx->routed_placement_group_moves = true;
        ctx->routed_placement_preserve_routes = true;
        ctx->routed_placement_preserve_branches = true;
        moved_old = ctx->add_wire(31, 20, ctx->id("branch_old_sink"));
        moved_new = ctx->add_wire(32, 20, ctx->id("branch_new_sink"));
        auto old_pip = ctx->add_pip(middle, moved_old);
        reconnect = ctx->add_pip(middle, moved_new);
        moved_sink = ctx->createCell(ctx->id("moved_sink"), id_MISTRAL_ALUT2);
        moved_sink->addInput(id_A);
        moved_sink->connectPort(id_A, net);
        moved_sink->pin_data[id_A].bel_pins = {ctx->id("PRESERVE_A")};
        ctx->assign_comb_info(moved_sink);
        auto old_bel = ctx->getBelByLocation(Loc(31, 20, 0));
        auto new_bel = ctx->getBelByLocation(Loc(32, 20, 0));
        ctx->add_bel_pin(old_bel, ctx->id("PRESERVE_A"), PORT_IN, moved_old);
        ctx->add_bel_pin(new_bel, ctx->id("PRESERVE_A"), PORT_IN, moved_new);
        ctx->bindBel(old_bel, moved_sink, STRENGTH_WEAK);
        parent = routed_placement_snapshot(ctx.get());
        ctx->bindWire(source, net, STRENGTH_WEAK);
        ctx->bindPip(first, net, STRENGTH_WEAK);
        ctx->bindPip(last, net, STRENGTH_WEAK);
        ctx->bindPip(old_pip, net, STRENGTH_WEAK);
        witness = snapshot_routed_placement_routes(ctx.get(), parent);
        for (auto wire : {moved_old, sink, middle, source}) ctx->unbindWire(wire);
        ctx->unbindBel(old_bel);
        ctx->bindBel(new_bel, moved_sink, STRENGTH_WEAK);
        ctx->routed_placement_current = routed_placement_snapshot(ctx.get()).dump();
    }
    J cost_source(const std::map<PipId, int> &values = {})
    {
        auto result = CriticalCohortRouteCosts::snapshot(ctx.get(), parent).object_items();
        result["routes"] = witness;
        J::array background;
        for (const auto &binding : witness["nets"]["data"]["wires"].array_items())
            if (!binding[1].string_value().empty()) background.push_back(binding[1]);
        result["background"] = J::object{{"data", background}};
        result["calibrated"] = true;
        J::object observed;
        for (const auto &entry : values)
            observed[ctx->getPipName(entry.first).str(ctx.get())] = entry.second;
        result["observed"] = observed;
        return result;
    }
    void change_driver_source(WireId wire)
    {
        auto pin = ctx->id("MOVED_SOURCE_Q");
        ctx->add_bel_pin(driver->bel, pin, PORT_OUT, wire);
        driver->pin_data[id_Q].bel_pins = {pin};
        ASSERT_EQ(ctx->getNetinfoSourceWire(net), wire);
    }
    J handoff(bool alternate = true)
    {
        ctx->routed_placement_preserve_routes = true;
        auto target = routed_placement_snapshot(ctx.get());
        ctx->routed_placement_current = target.dump();
        J wires = witness["nets"]["data"]["wires"];
        if (alternate) {
            auto a = ctx->add_pip(source, spare), b = ctx->add_pip(spare, sink);
            wires = J::array{J::array{ctx->getWireName(source).str(ctx.get()), ""},
                J::array{ctx->getWireName(spare).str(ctx.get()), ctx->getPipName(a).str(ctx.get())},
                J::array{ctx->getWireName(sink).str(ctx.get()), ctx->getPipName(b).str(ctx.get())}};
        }
        J request = J::object{{"source", ctx->getWireName(source).str(ctx.get())},
            {"sinks", J::array{ctx->getWireName(sink).str(ctx.get())}}, {"wires", wires}};
        return J::object{{"version", 1}, {"target", target}, {"nets", J::object{{"data", request}}}};
    }
    J branch_source(const std::map<PipId, int> &values, const std::vector<PipId> &extra)
    {
        auto result = cost_source(values).object_items();
        auto routes = witness.object_items();
        auto record = routes["nets"]["data"].object_items();
        auto wires = record["wires"].array_items();
        auto background = result["background"]["data"].array_items();
        for (auto pip : extra) {
            wires.push_back(J::array{ctx->getWireName(ctx->getPipDstWire(pip)).str(ctx.get()),
                                     ctx->getPipName(pip).str(ctx.get())});
            background.push_back(ctx->getPipName(pip).str(ctx.get()));
        }
        record["wires"] = wires;
        routes["nets"] = J::object{{"data", record}};
        result["routes"] = routes;
        result["background"] = J::object{{"data", background}};
        return result;
    }
};

TEST_F(RoutedPlacementRoutes, MovedSourceRetainsDownstreamParentsAndCannotReuseTheOldDriver)
{
    auto joined = ctx->add_pip(spare, middle);
    auto false_driver = ctx->add_pip(spare, source);
    CriticalCohortRouteCosts costs(ctx.get(), cost_source({{first, 1}, {last, 20}, {joined, 50}, {false_driver, 1}}), parent);
    const auto old_tree = costs.parent_tree(net->name);
    change_driver_source(spare);
    int budget = 100;
    auto tree = costs.reconnect_source(ctx.get(), net, spare, {middle}, {{sink, last}}, {net->name}, {}, budget);
    ASSERT_EQ(tree.size(), 3);
    EXPECT_EQ(tree.at(spare), PipId());
    EXPECT_EQ(tree.at(middle), joined);
    EXPECT_EQ(tree.at(sink), last);
    EXPECT_EQ(tree.count(source), 0);
    EXPECT_EQ(costs.parent_tree(net->name), old_tree);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, MovedSourceCanReleaseTheTrunkAndReconnectAtTheActualSink)
{
    auto joined = ctx->add_pip(spare, middle);
    auto direct = ctx->add_pip(spare, sink);
    CriticalCohortRouteCosts costs(ctx.get(), cost_source({{last, 20}, {joined, 50}, {direct, 5}}), parent);
    const auto old_tree = costs.parent_tree(net->name);
    change_driver_source(spare);
    int budget = 100;
    auto local = costs.reconnect_source(ctx.get(), net, spare, {middle}, {{sink, last}},
                                        {net->name}, {}, budget);
    ASSERT_EQ(local.size(), 3);
    EXPECT_EQ(local.at(middle), joined);
    EXPECT_EQ(local.at(sink), last);
    budget = 100;
    auto trunk = costs.reconnect_source(ctx.get(), net, spare, {sink}, {}, {net->name}, {}, budget);
    ASSERT_EQ(trunk.size(), 2);
    EXPECT_EQ(trunk.at(spare), PipId());
    EXPECT_EQ(trunk.at(sink), direct);
    EXPECT_EQ(trunk.count(middle), 0);
    EXPECT_EQ(trunk.count(source), 0);
    EXPECT_EQ(costs.parent_tree(net->name), old_tree);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, MovedSourceRejectsIncompleteOrChangedForestAndAnUndeclaredDriver)
{
    auto joined = ctx->add_pip(spare, middle);
    auto wrong_parent = ctx->add_pip(spare, sink);
    CriticalCohortRouteCosts costs(ctx.get(), cost_source({{joined, 10}, {wrong_parent, 10}}), parent);
    int budget = 100;
    EXPECT_TRUE(costs.reconnect_source(ctx.get(), net, spare, {middle}, {{sink, last}},
                                       {net->name}, {}, budget).empty()); // Actual driver has not moved.
    change_driver_source(spare);
    EXPECT_TRUE(costs.reconnect_source(ctx.get(), net, spare, {middle}, {}, {net->name}, {}, budget).empty());
    EXPECT_TRUE(costs.reconnect_source(ctx.get(), net, spare, {middle}, {{sink, wrong_parent}},
                                       {net->name}, {}, budget).empty());
    EXPECT_TRUE(costs.reconnect_source(ctx.get(), net, spare, {source}, {{middle, first}, {sink, last}},
                                       {net->name}, {}, budget).empty());
    EXPECT_EQ(budget, 100);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, MovedSourceCannotUseAnotherOwnersWireOrInventAPathOnBudgetExpiry)
{
    auto joined = ctx->add_pip(spare, middle);
    CriticalCohortRouteCosts costs(ctx.get(), cost_source({{joined, 10}}), parent);
    change_driver_source(spare);
    int budget = 0;
    EXPECT_TRUE(costs.reconnect_source(ctx.get(), net, spare, {middle}, {{sink, last}},
                                       {net->name}, {}, budget).empty());
    ctx->bindWire(middle, other, STRENGTH_STRONG);
    budget = 100;
    EXPECT_TRUE(costs.reconnect_source(ctx.get(), net, spare, {middle}, {{sink, last}},
                                       {net->name, other->name}, {}, budget).empty());
    EXPECT_EQ(ctx->getBoundWireNet(middle), other);
    EXPECT_EQ(other->wires.at(middle).strength, STRENGTH_STRONG);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, MovedSourceConnectsEveryForestRootWithOneActualSource)
{
    auto root2 = ctx->add_wire(27, 20, ctx->id("second_root"));
    auto sink2 = ctx->add_wire(32, 20, ctx->id("second_sink"));
    auto fork = ctx->add_pip(source, root2), tail = ctx->add_pip(root2, sink2);
    auto joined = ctx->add_pip(spare, middle), joined2 = ctx->add_pip(spare, root2);
    auto *consumer = ctx->createCell(ctx->id("second_consumer"), id_MISTRAL_ALUT2);
    consumer->addInput(id_A);
    consumer->params[id_LUT] = 6;
    consumer->connectPort(id_A, net);
    auto pin = ctx->id("SECOND_A");
    consumer->pin_data[id_A].bel_pins = {pin};
    ctx->assign_comb_info(consumer);
    auto bel = ctx->getBelByLocation(Loc(32, 20, 0));
    ctx->add_bel_pin(bel, pin, PORT_IN, sink2);
    ctx->bindBel(bel, consumer, STRENGTH_WEAK);
    parent = routed_placement_snapshot(ctx.get());
    ctx->bindWire(source, net, STRENGTH_WEAK);
    for (auto pip : {first, last, fork, tail}) ctx->bindPip(pip, net, STRENGTH_WEAK);
    witness = snapshot_routed_placement_routes(ctx.get(), parent);
    ctx->ripupNet(net->name);
    CriticalCohortRouteCosts costs(ctx.get(), cost_source({{joined, 10}, {joined2, 20}}), parent);
    change_driver_source(spare);
    int budget = 100;
    auto tree = costs.reconnect_source(ctx.get(), net, spare, {middle, root2}, {{sink, last}, {sink2, tail}},
                                       {net->name}, {}, budget);
    ASSERT_EQ(tree.size(), 5);
    EXPECT_EQ(tree.at(spare), PipId());
    EXPECT_EQ(tree.at(middle), joined);
    EXPECT_EQ(tree.at(root2), joined2);
    EXPECT_EQ(tree.at(sink), last);
    EXPECT_EQ(tree.at(sink2), tail);
    EXPECT_EQ(tree.count(source), 0);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, BranchRepairKeepsOtherBranchesAndPricesTheCompleteFanout)
{
    auto held = ctx->add_wire(26, 20, ctx->id("held_sink"));
    auto held_pip = ctx->add_pip(middle, held);
    auto a = ctx->add_pip(middle, spare), b = ctx->add_pip(spare, sink);
    CriticalCohortRouteCosts costs(ctx.get(),
        branch_source({{first, 100}, {last, 100}, {held_pip, 75}, {a, 125}, {b, 125}}, {held_pip}), parent);
    auto choices = costs.branch_alternatives(ctx.get(), net->name, sink, {sink, held});
    ASSERT_EQ(choices.size(), 1);
    EXPECT_EQ(choices.front(), last);
    int budget = 100;
    std::map<WireId, PipId> tree;
    auto values = costs.repair_branch(ctx.get(), net, source, {sink, held}, sink, last,
                                      {net->name}, {}, budget, nullptr, &tree);
    ASSERT_EQ(values.size(), 2);
    EXPECT_EQ(values.at(sink), 350 + 3 * ctx->getDelayEpsilon());
    EXPECT_EQ(values.at(held), 175 + 2 * ctx->getDelayEpsilon());
    EXPECT_EQ(tree.at(middle), first);
    EXPECT_EQ(tree.at(held), held_pip);
    EXPECT_EQ(tree.at(sink), b);
    EXPECT_EQ(tree.at(spare), a);
    EXPECT_EQ(costs.parent_tree(net->name).at(sink), last);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, BranchRepairCannotRemoveAnotherSinkOrRepairAnInternalSink)
{
    auto a = ctx->add_pip(source, spare), b = ctx->add_pip(spare, sink);
    CriticalCohortRouteCosts costs(ctx.get(), cost_source({{a, 10}, {b, 10}}), parent);
    EXPECT_TRUE(costs.branch_alternatives(ctx.get(), net->name, middle, {middle, sink}).empty());
    auto choices = costs.branch_alternatives(ctx.get(), net->name, sink, {middle, sink});
    ASSERT_EQ(choices.size(), 1);
    EXPECT_EQ(choices.front(), last);
    int budget = 100;
    std::map<WireId, PipId> tree;
    EXPECT_TRUE(costs.repair_branch(ctx.get(), net, source, {middle, sink}, sink, first,
                                   {net->name}, {}, budget, nullptr, &tree).empty());
    EXPECT_EQ(budget, 100);
    auto values = costs.repair_branch(ctx.get(), net, source, {middle, sink}, sink, last,
                                      {net->name}, {}, budget, nullptr, &tree);
    ASSERT_EQ(values.size(), 2);
    EXPECT_EQ(tree.at(middle), first);
    EXPECT_EQ(tree.at(sink), b);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, BranchRepairHonorsOwnershipAndSearchBudget)
{
    auto a = ctx->add_pip(source, spare), b = ctx->add_pip(spare, sink);
    CriticalCohortRouteCosts costs(ctx.get(), cost_source({{a, 10}, {b, 10}}), parent);
    ctx->bindWire(spare, other, STRENGTH_LOCKED);
    int budget = 100;
    std::map<WireId, PipId> tree;
    EXPECT_TRUE(costs.repair_branch(ctx.get(), net, source, {sink}, sink, last,
                                   {net->name}, {}, budget, nullptr, &tree).empty());
    EXPECT_EQ(ctx->getBoundWireNet(spare), other);
    ctx->unbindWire(spare);
    budget = 0;
    EXPECT_TRUE(costs.repair_branch(ctx.get(), net, source, {sink}, sink, last,
                                   {net->name}, {}, budget, nullptr, &tree).empty());
    EXPECT_EQ(budget, 0);
    budget = 100;
    ASSERT_EQ(costs.repair_branch(ctx.get(), net, source, {sink}, sink, last,
                                  {net->name}, {}, budget, nullptr, &tree).size(), 1);
    EXPECT_EQ(tree.at(sink), b);
    EXPECT_EQ(tree.count(middle), 0);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, BranchRepairHonorsCapturedNativeReservations)
{
    auto a = ctx->add_pip(source, spare), b = ctx->add_pip(spare, middle);
    ctx->reserve_route(source, middle);
    auto saved = cost_source({{a, 10}, {b, 10}});
    ctx->wires.at(middle).flags = 0;
    CriticalCohortRouteCosts costs(ctx.get(), saved, parent);
    EXPECT_TRUE(costs.branch_alternatives(ctx.get(), net->name, sink, {sink}).empty());
    int budget = 100;
    std::map<WireId, PipId> tree;
    EXPECT_TRUE(costs.repair_branch(ctx.get(), net, source, {sink}, sink, first,
                                   {net->name}, {}, budget, nullptr, &tree).empty());
    EXPECT_EQ(ctx->wires.at(middle).flags, 0);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, ProposedTreeReplacesTheMatchingParentAndStaysProtected)
{
    auto request = handoff();
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent, request);
    EXPECT_TRUE(result["preserved"].array_items().empty());
    ASSERT_EQ(result["proposed"].array_items().size(), 1);
    EXPECT_EQ(net->wires.count(middle), 0);
    EXPECT_EQ(ctx->getBoundWireNet(spare), net);
    for (const auto &wire : net->wires) EXPECT_EQ(wire.second.strength, STRENGTH_USER);
    EXPECT_FALSE(analogue_net_is_movable(net));
    EXPECT_NO_THROW(check_routed_placement_routes(ctx.get(), result["handoff"], result["proposed"]));
}

TEST_F(RoutedPlacementRoutes, ProposedStaleTargetOrFanoutDoesNotBindAnyTree)
{
    auto request = handoff().object_items();
    auto target = request["target"].object_items();
    target["rng"] = "stale";
    request["target"] = target;
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), witness, parent, request), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
    request = handoff(false).object_items();
    auto tree = request["nets"]["data"].object_items();
    tree["sinks"] = J::array{ctx->getWireName(spare).str(ctx.get())};
    request["nets"] = J::object{{"data", tree}};
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), witness, parent, request), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, ProposedNativeConflictKeepsTheExistingOwner)
{
    auto request = handoff();
    ctx->bindWire(spare, other, STRENGTH_LOCKED);
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), witness, parent, request), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
    EXPECT_EQ(ctx->getBoundWireNet(spare), other);
    EXPECT_EQ(other->wires.at(spare).strength, STRENGTH_LOCKED);
}

TEST_F(RoutedPlacementRoutes, ProposedTreeCannotDisplaceAnUnboundRetainedParentTree)
{
    auto other_sink = ctx->add_wire(26, 20, ctx->id("other_sink"));
    auto *a = ctx->createCell(ctx->id("other_driver"), id_MISTRAL_ALUT2);
    auto *b = ctx->createCell(ctx->id("other_sink_cell"), id_MISTRAL_ALUT2);
    a->addOutput(id_Q); b->addInput(id_A);
    a->params[id_LUT] = b->params[id_LUT] = 6;
    a->connectPort(id_Q, other); b->connectPort(id_A, other);
    auto out = ctx->id("OTHER_Q"), in = ctx->id("OTHER_A");
    a->pin_data[id_Q].bel_pins = {out}; b->pin_data[id_A].bel_pins = {in};
    ctx->assign_comb_info(a); ctx->assign_comb_info(b);
    auto abel = ctx->getBelByLocation(Loc(24, 20, 6)), bbel = ctx->getBelByLocation(Loc(30, 20, 6));
    ctx->add_bel_pin(abel, out, PORT_OUT, spare); ctx->add_bel_pin(bbel, in, PORT_IN, other_sink);
    ctx->bindBel(abel, a, STRENGTH_WEAK); ctx->bindBel(bbel, b, STRENGTH_WEAK);
    auto edge = ctx->add_pip(spare, other_sink);
    ctx->routed_placement_preserve_routes = true;
    parent = routed_placement_snapshot(ctx.get());
    ctx->bindWire(source, net, STRENGTH_WEAK); ctx->bindPip(first, net, STRENGTH_WEAK);
    ctx->bindPip(last, net, STRENGTH_WEAK);
    ctx->bindWire(spare, other, STRENGTH_WEAK); ctx->bindPip(edge, other, STRENGTH_WEAK);
    witness = snapshot_routed_placement_routes(ctx.get(), parent);
    for (auto wire : {sink, middle, source, other_sink, spare}) ctx->unbindWire(wire);
    auto request = handoff(); // The new data tree also wants the retained other's root.
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), witness, parent, request), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
    EXPECT_TRUE(other->wires.empty());
    EXPECT_EQ(ctx->getBoundWireNet(spare), nullptr);
}

TEST_F(RoutedPlacementRoutes, InvalidLaterProposalCannotPartlyApplyAnEarlierOne)
{
    auto request = handoff().object_items();
    auto nets = request["nets"].object_items();
    nets["zz_unknown"] = nets.at("data");
    request["nets"] = nets;
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), witness, parent, request), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
    EXPECT_EQ(ctx->getBoundWireNet(spare), nullptr);
}

TEST_F(RoutedPlacementRoutes, ProposedIncompleteTreeAndNativePrefixAreRejected)
{
    auto request = handoff().object_items();
    auto tree = request["nets"]["data"].object_items();
    auto wires = tree["wires"].array_items();
    wires.pop_back();
    tree["wires"] = wires;
    request["nets"] = J::object{{"data", tree}};
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), witness, parent, request), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
    request = handoff(false).object_items();
    ctx->bindWire(source, net, STRENGTH_STRONG);
    ctx->bindPip(PipId(source.node, spare.node), net, STRENGTH_STRONG);
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), witness, parent, request), log_execution_error_exception);
    EXPECT_EQ(net->wires.size(), 2);
    EXPECT_EQ(net->wires.at(source).strength, STRENGTH_STRONG);
}

TEST_F(RoutedPlacementRoutes, ProposedFinalCheckRejectsPhysicalContractChanges)
{
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent, handoff());
    driver->params[id_LUT] = 8;
    EXPECT_THROW(check_routed_placement_routes(ctx.get(), result["handoff"], result["proposed"]),
                 log_execution_error_exception);
}

TEST_F(RoutedPlacementRoutes, RoutingCostsReuseNativeCalibrationAndRestoreContext)
{
    ctx->pip_delay_calibrated = false;
    ctx->pip_delay_prior = 1.75f;
    ctx->pip_delay_observed[first] = 777;
    ctx->pip_type_calibration[0] = {100, 500, 2};
    auto costs = CriticalCohortRouteCosts(ctx.get(), cost_source({{first, 100}, {last, 200}}), parent);
    int expected = 300 + 2 * ctx->getDelayEpsilon();
    EXPECT_EQ(costs.reference(net->name, source, {sink}), expected);
    EXPECT_FALSE(costs.reference(net->name, spare, {sink}).has_value());
    int budget = 20;
    auto result = costs.estimate(ctx.get(), net, source, {middle, sink}, {}, {}, budget);
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result.at(sink), expected);
    EXPECT_EQ(result.at(middle), 100 + ctx->getDelayEpsilon());
    EXPECT_EQ(budget, 17);
    EXPECT_FALSE(ctx->pip_delay_calibrated);
    EXPECT_EQ(ctx->pip_delay_prior, 1.75f);
    ASSERT_EQ(ctx->pip_delay_observed.size(), 1);
    EXPECT_EQ(ctx->pip_delay_observed.at(first), 777);
    EXPECT_EQ(ctx->pip_type_calibration[0].analogue_ps, 500);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, RoutingCostsAvoidHeldCorridorAndReleaseOnlyNamedTrees)
{
    auto target = ctx->add_wire(26, 20, ctx->id("cost_target"));
    auto detour = ctx->add_wire(27, 20, ctx->id("cost_detour"));
    auto a = ctx->add_pip(spare, middle), b = ctx->add_pip(middle, target);
    auto c = ctx->add_pip(spare, detour), d = ctx->add_pip(detour, target);
    CriticalCohortRouteCosts costs(ctx.get(), cost_source({{a, 10}, {b, 10}, {c, 250}, {d, 250}}), parent);
    int budget = 20;
    auto held = costs.estimate(ctx.get(), other, spare, {target}, {other->name}, {}, budget);
    ASSERT_EQ(held.size(), 1);
    EXPECT_EQ(held.at(target), 500 + 2 * ctx->getDelayEpsilon());
    budget = 20;
    auto released = costs.estimate(ctx.get(), other, spare, {target}, {net->name}, {}, budget);
    ASSERT_EQ(released.size(), 1);
    EXPECT_EQ(released.at(target), 20 + 2 * ctx->getDelayEpsilon());
    ctx->bindWire(middle, net, STRENGTH_USER);
    budget = 20;
    auto bound = costs.estimate(ctx.get(), other, spare, {target}, {net->name}, {}, budget);
    EXPECT_EQ(bound, held);
    budget = 20;
    EXPECT_TRUE(costs.estimate(ctx.get(), other, spare, {target}, {net->name}, {{target, net->name}}, budget).empty());
}

TEST_F(RoutedPlacementRoutes, RoutingCostsReportMissingBranchesAndBoundSearchWork)
{
    CriticalCohortRouteCosts costs(ctx.get(), cost_source({{first, 100}, {last, 200}}), parent);
    int budget = 2;
    auto partial = costs.estimate(ctx.get(), net, source, {middle, sink}, {}, {}, budget);
    EXPECT_EQ(budget, 0);
    EXPECT_EQ(partial.size(), 1);
    EXPECT_EQ(partial.count(sink), 0);
    budget = 20;
    auto unreachable = costs.estimate(ctx.get(), net, source, {sink, spare}, {}, {}, budget);
    EXPECT_EQ(unreachable.size(), 1);
    EXPECT_EQ(unreachable.count(spare), 0);
    EXPECT_FALSE(costs.reference(net->name, source, {sink, spare}).has_value());
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, RoutingCostsRejectStaleTreeAndMalformedCalibration)
{
    auto data = cost_source().object_items();
    auto stale = parent.object_items();
    stale["rng"] = "different";
    EXPECT_THROW(CriticalCohortRouteCosts(ctx.get(), data, stale), log_execution_error_exception);
    data["prior"] = 0;
    EXPECT_THROW(CriticalCohortRouteCosts(ctx.get(), data, parent), log_execution_error_exception);
    data = cost_source().object_items();
    data["types"] = J::array{};
    EXPECT_THROW(CriticalCohortRouteCosts(ctx.get(), data, parent), log_execution_error_exception);
    data = cost_source().object_items();
    data["observed"] = J::object{{ctx->getPipName(first).str(ctx.get()), -1}};
    EXPECT_THROW(CriticalCohortRouteCosts(ctx.get(), data, parent), log_execution_error_exception);
    data = cost_source().object_items();
    data["background"] = J::object{};
    EXPECT_THROW(CriticalCohortRouteCosts(ctx.get(), data, parent), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, RoutingCostsRequireShortestPathProofAfterFrontiersMeet)
{
    auto target = ctx->add_wire(26, 20, ctx->id("cost_target"));
    auto a = ctx->add_wire(27, 20, ctx->id("cost_a"));
    auto b = ctx->add_wire(28, 20, ctx->id("cost_b"));
    auto sa = ctx->add_pip(spare, a), at = ctx->add_pip(a, target);
    auto sb = ctx->add_pip(spare, b), bt = ctx->add_pip(b, target);
    CriticalCohortRouteCosts costs(ctx.get(), cost_source({{sa, 1}, {at, 100}, {sb, 50}, {bt, 50}}), parent);
    int budget = 2;
    EXPECT_TRUE(costs.estimate(ctx.get(), other, spare, {target}, {}, {}, budget).empty());
    EXPECT_EQ(budget, 0);
    budget = 20;
    auto complete = costs.estimate(ctx.get(), other, spare, {target}, {}, {}, budget);
    ASSERT_EQ(complete.size(), 1);
    EXPECT_EQ(complete.at(target), 100 + 2 * ctx->getDelayEpsilon());
}

TEST_F(RoutedPlacementRoutes, RoutingCostsReserveTheCompleteTreeAgainstLaterNets)
{
    auto *second = ctx->createNet(ctx->id("second"));
    auto root = ctx->add_wire(26, 20, ctx->id("second_root"));
    auto shared = ctx->add_wire(27, 20, ctx->id("shared"));
    auto target_a = ctx->add_wire(28, 20, ctx->id("target_a"));
    auto target_b = ctx->add_wire(29, 20, ctx->id("target_b"));
    auto detour = ctx->add_wire(31, 20, ctx->id("detour"));
    std::map<PipId, int> values;
    for (auto edge : {ctx->add_pip(spare, shared), ctx->add_pip(root, shared),
                      ctx->add_pip(shared, target_a), ctx->add_pip(shared, target_b)}) values[edge] = 10;
    values[ctx->add_pip(root, detour)] = 250;
    values[ctx->add_pip(detour, target_b)] = 250;
    CriticalCohortRouteCosts costs(ctx.get(), cost_source(values), parent);
    int budget = 100;
    std::map<WireId, IdString> owners{{spare, other->name}, {root, second->name}};
    std::map<WireId, PipId> first_tree, second_tree;
    auto first = costs.estimate(ctx.get(), other, spare, {target_a}, {}, owners, budget, nullptr, &first_tree);
    ASSERT_EQ(first.size(), 1);
    EXPECT_EQ(first.at(target_a), 20 + 2 * ctx->getDelayEpsilon());
    ASSERT_TRUE(first_tree.count(shared));
    for (const auto &wire : first_tree) owners.emplace(wire.first, other->name);
    auto next = costs.estimate(ctx.get(), second, root, {target_b}, {}, owners, budget, nullptr, &second_tree);
    ASSERT_EQ(next.size(), 1);
    EXPECT_EQ(next.at(target_b), 500 + 2 * ctx->getDelayEpsilon());
    EXPECT_EQ(second_tree.count(shared), 0);
    EXPECT_EQ(owners.at(shared), other->name);
    EXPECT_TRUE(other->wires.empty());
    EXPECT_TRUE(second->wires.empty());
}

TEST_F(RoutedPlacementRoutes, RoutingCostsProduceConnectedFanoutWithOneParentPerWire)
{
    auto a = ctx->add_wire(26, 20, ctx->id("branch_a"));
    auto b = ctx->add_wire(27, 20, ctx->id("branch_b"));
    auto shared = ctx->add_wire(28, 20, ctx->id("shared"));
    auto target_a = ctx->add_wire(29, 20, ctx->id("target_a"));
    auto target_b = ctx->add_wire(31, 20, ctx->id("target_b"));
    std::map<PipId, int> values;
    for (auto edge : {ctx->add_pip(spare, a), ctx->add_pip(spare, b), ctx->add_pip(a, shared),
                      ctx->add_pip(b, shared), ctx->add_pip(shared, target_a), ctx->add_pip(shared, target_b)}) values[edge] = 10;
    CriticalCohortRouteCosts costs(ctx.get(), cost_source(values), parent);
    int budget = 100;
    std::map<WireId, PipId> tree;
    json11::Json::object trace;
    auto result = costs.estimate(ctx.get(), other, spare, {target_a, target_b}, {}, {}, budget, &trace, &tree);
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(trace.size(), 2);
    for (auto target : {target_a, target_b}) {
        std::set<WireId> seen;
        auto cursor = target;
        while (cursor != spare) {
            ASSERT_TRUE(seen.insert(cursor).second);
            ASSERT_TRUE(tree.count(cursor));
            auto pip = tree.at(cursor);
            EXPECT_EQ(ctx->getPipDstWire(pip), cursor);
            cursor = ctx->getPipSrcWire(pip);
        }
    }
    EXPECT_EQ(tree.at(spare), PipId());
}

TEST_F(RoutedPlacementRoutes, RoutingCostsRetainArchitectureBackgroundDespiteLogicalRelease)
{
    auto root = ctx->add_wire(26, 20, ctx->id("root"));
    auto end = ctx->add_wire(27, 20, ctx->id("helper_end"));
    auto target = ctx->add_wire(28, 20, ctx->id("target"));
    auto detour = ctx->add_wire(29, 20, ctx->id("detour"));
    auto helper = ctx->add_pip(spare, end);
    auto a = ctx->add_pip(root, spare), b = ctx->add_pip(spare, target);
    auto c = ctx->add_pip(root, detour), d = ctx->add_pip(detour, target);
    auto source = cost_source({{a, 10}, {b, 10}, {c, 250}, {d, 250}}).object_items();
    auto background = source["background"].object_items();
    background["architecture_helper"] = J::array{ctx->getPipName(helper).str(ctx.get())};
    source["background"] = background;
    CriticalCohortRouteCosts costs(ctx.get(), source, parent);
    int budget = 100;
    std::map<WireId, PipId> tree;
    auto result = costs.estimate(ctx.get(), other, root, {target}, {ctx->id("architecture_helper")}, {}, budget, nullptr, &tree);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.at(target), 500 + 2 * ctx->getDelayEpsilon());
    EXPECT_EQ(tree.count(spare), 0);
    EXPECT_TRUE(other->wires.empty());
}

TEST_F(RoutedPlacementRoutes, CapturedNativeReservationsSurviveTheUnpreparedContext)
{
    auto gate = ctx->add_wire(26, 20, ctx->id("reserved_gate"));
    auto target = ctx->add_wire(27, 20, ctx->id("target"));
    auto detour = ctx->add_wire(28, 20, ctx->id("detour"));
    auto forbidden = ctx->add_pip(spare, gate);
    ctx->add_pip(source, gate);
    auto tail = ctx->add_pip(gate, target);
    auto a = ctx->add_pip(spare, detour), b = ctx->add_pip(detour, target);
    ctx->reserve_route(source, gate);
    auto saved = cost_source({{forbidden, 10}, {tail, 10}, {a, 100}, {b, 100}});
    EXPECT_EQ(saved["reservations"][ctx->getWireName(gate).str(ctx.get())],
              J(ctx->getWireName(source).str(ctx.get())));
    ctx->wires.at(gate).flags = 0;
    EXPECT_TRUE(ctx->checkPipAvailForNet(forbidden, other));
    CriticalCohortRouteCosts costs(ctx.get(), saved, parent);
    int budget = 100;
    std::map<WireId, PipId> tree;
    auto result = costs.estimate(ctx.get(), other, spare, {target}, {}, {}, budget, nullptr, &tree);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.at(target), 200 + 2 * ctx->getDelayEpsilon());
    EXPECT_EQ(tree.count(gate), 0);
    EXPECT_EQ(ctx->wires.at(gate).flags, 0);
}

TEST_F(RoutedPlacementRoutes, CompleteTreeRetainsPhysicalPinsAndProtectsExistingPrefix)
{
    ctx->bindWire(source, net, STRENGTH_STRONG);
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    ASSERT_EQ(result["preserved"].array_items().size(), 1);
    EXPECT_EQ(ctx->getBoundPipNet(first), net);
    EXPECT_EQ(ctx->getBoundPipNet(last), net);
    for (const auto &binding : net->wires) EXPECT_EQ(binding.second.strength, STRENGTH_USER);
    EXPECT_NO_THROW(check_routed_placement_routes(ctx.get(), witness, result["preserved"]));
    EXPECT_FALSE(analogue_net_is_movable(net));
}

TEST_F(RoutedPlacementRoutes, StationaryFanoutKeepsItsSharedTrunkAndReleasesTheMovedLeaf)
{
    stationary_branch_case();
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    const auto &proof = result["branches"];
    ASSERT_EQ(proof["nets"].object_items().size(), 1);
    EXPECT_TRUE(result["preserved"].array_items().empty());
    EXPECT_EQ(proof["nets"]["data"]["source"], witness["nets"]["data"]);
    EXPECT_EQ(proof["nets"]["data"]["wires"].array_items().size(), 3);
    EXPECT_EQ(proof["nets"]["data"]["sinks"].array_items().size(), 1);
    EXPECT_EQ(ctx->getBoundPipNet(first), net);
    EXPECT_EQ(ctx->getBoundPipNet(last), net);
    EXPECT_EQ(ctx->getBoundWireNet(moved_old), nullptr);
    EXPECT_EQ(ctx->getBoundWireNet(moved_new), nullptr);
    ctx->bindPip(reconnect, net, STRENGTH_WEAK);
    EXPECT_NO_THROW(check_routed_placement_branches(ctx.get(), proof, parent, routed_placement_snapshot(ctx.get())));
    EXPECT_FALSE(analogue_net_is_movable(net));
}

TEST_F(RoutedPlacementRoutes, StationaryBranchesRejectMovedSourcesAndChangedTruth)
{
    stationary_branch_case();
    driver->params[id_LUT] = 8;
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_TRUE(result["branches"]["nets"].object_items().empty());
    EXPECT_TRUE(net->wires.empty());
    driver->params[id_LUT] = 6;
    auto moved = ctx->getBelByLocation(Loc(25, 20, 0));
    ctx->add_bel_pin(moved, ctx->id("PRESERVE_Q"), PORT_OUT, spare);
    ctx->unbindBel(driver->bel);
    ctx->bindBel(moved, driver, STRENGTH_WEAK);
    result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_TRUE(result["branches"]["nets"].object_items().empty());
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, StationaryBranchesRejectPinStateChangesAndUndeclaredMoves)
{
    stationary_branch_case();
    sink_cell->pin_data[id_A].state = PIN_INV;
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_TRUE(result["branches"]["nets"].object_items().empty());
    EXPECT_TRUE(net->wires.empty());
    sink_cell->pin_data[id_A].state = PIN_SIG;
    ctx->routed_placement_current = parent.dump();
    result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_TRUE(result["branches"]["nets"].object_items().empty());
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, StationaryBranchesKeepNativeOwnersAndRejectFinalEdgeChanges)
{
    stationary_branch_case();
    ctx->bindWire(middle, other, STRENGTH_LOCKED);
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_TRUE(result["branches"]["nets"].object_items().empty());
    EXPECT_EQ(ctx->getBoundWireNet(middle), other);
    EXPECT_TRUE(net->wires.empty());
    ctx->unbindWire(middle);
    result = apply_routed_placement_routes(ctx.get(), witness, parent);
    ASSERT_EQ(result["branches"]["nets"].object_items().size(), 1);
    ctx->unbindWire(sink);
    EXPECT_THROW(check_routed_placement_branches(ctx.get(), result["branches"], parent,
                                               routed_placement_snapshot(ctx.get())), log_execution_error_exception);
}

TEST_F(RoutedPlacementRoutes, StationaryBranchModeIsDefaultOff)
{
    stationary_branch_case();
    ctx->routed_placement_preserve_branches = false;
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_TRUE(result["branches"].is_null());
    EXPECT_TRUE(net->wires.empty());
    EXPECT_EQ(result["changed"].array_items().size(), 1);
}

TEST_F(RoutedPlacementRoutes, AnalogueReroutingHonorsEveryBindingStrength)
{
    EXPECT_FALSE(analogue_net_is_movable(net));
    ctx->bindWire(source, net, STRENGTH_WEAK);
    EXPECT_TRUE(analogue_net_is_movable(net));
    ctx->bindPip(first, net, STRENGTH_USER);
    EXPECT_FALSE(analogue_net_is_movable(net));
    net->wires.at(middle).strength = STRENGTH_STRONG;
    EXPECT_TRUE(analogue_net_is_movable(net));
    net->wires.at(source).strength = STRENGTH_LOCKED;
    EXPECT_FALSE(analogue_net_is_movable(net));
    net->wires.at(source).strength = STRENGTH_WEAK;
    net->is_global = true;
    EXPECT_FALSE(analogue_net_is_movable(net));
}

TEST_F(RoutedPlacementRoutes, ChangedCellTruthOrPinStateReleasesTheWholeTree)
{
    driver->params[id_LUT] = 8;
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_TRUE(result["preserved"].array_items().empty());
    EXPECT_EQ(result["changed"].array_items().size(), 1);
    EXPECT_TRUE(net->wires.empty());
    driver->params[id_LUT] = 6;
    sink_cell->pin_data[id_A].state = PIN_INV;
    result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_EQ(result["changed"].array_items().size(), 1);
    EXPECT_TRUE(net->wires.empty());
    sink_cell->pin_data[id_A].state = PIN_SIG;
    driver->attrs[ctx->id("keep")] = 1;
    result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_EQ(result["changed"].array_items().size(), 1);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, AddedPhysicalSinkReleasesTheWholeTree)
{
    sink_cell->addInput(id_B);
    sink_cell->connectPort(id_B, net);
    sink_cell->pin_data[id_B].bel_pins = {ctx->id("PRESERVE_B")};
    ctx->add_bel_pin(sink_cell->bel, ctx->id("PRESERVE_B"), PORT_IN, spare);
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_EQ(result["changed"].array_items().size(), 1);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, NonexistentArchitectureEdgeIsRejectedBeforeBinding)
{
    auto tree = witness["nets"]["data"].object_items();
    auto wires = tree["wires"].array_items();
    for (auto &wire : wires)
        if (wire[0].string_value() == ctx->getWireName(source).str(ctx.get()))
            wire = J::array{wire[0], ctx->getPipName(PipId(spare.node, source.node)).str(ctx.get())};
    tree["wires"] = wires;
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), replace_tree(tree), parent), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, ChangedBelReleasesTheWholeTree)
{
    BelId moved = ctx->getBelByLocation(Loc(25, 20, 0));
    ctx->add_bel_pin(moved, ctx->id("PRESERVE_Q"), PORT_OUT, spare);
    ctx->unbindBel(driver->bel);
    ctx->bindBel(moved, driver, STRENGTH_WEAK);
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_EQ(result["changed"].array_items().size(), 1);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, NewNativeResourceConflictReleasesTreeAndPreservesOwner)
{
    ctx->bindWire(middle, other, STRENGTH_LOCKED);
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    EXPECT_EQ(result["blocked"].array_items().size(), 1);
    EXPECT_TRUE(net->wires.empty());
    EXPECT_EQ(ctx->getBoundWireNet(middle), other);
    EXPECT_EQ(other->wires.at(middle).strength, STRENGTH_LOCKED);
}

TEST_F(RoutedPlacementRoutes, StaleParentAndDuplicateWireAreRejectedBeforeBinding)
{
    auto stale = parent.object_items();
    stale["rng"] = "different";
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), witness, stale), log_execution_error_exception);
    auto tree = witness["nets"]["data"].object_items();
    auto wires = tree["wires"].array_items();
    wires.push_back(wires.front());
    tree["wires"] = wires;
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), replace_tree(tree), parent), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, CyclicAndIncompleteTreesAreRejectedBeforeBinding)
{
    auto tree = witness["nets"]["data"].object_items();
    auto wires = tree["wires"].array_items();
    PipId back = ctx->add_pip(sink, source);
    for (auto &wire : wires)
        if (wire[0].string_value() == ctx->getWireName(source).str(ctx.get()))
            wire = J::array{wire[0], ctx->getPipName(back).str(ctx.get())};
    tree["wires"] = wires;
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), replace_tree(tree), parent), log_execution_error_exception);
    wires = witness["nets"]["data"]["wires"].array_items();
    wires.erase(std::remove_if(wires.begin(), wires.end(), [&](const J &wire) {
        return wire[0].string_value() == ctx->getWireName(sink).str(ctx.get());
    }), wires.end());
    tree["wires"] = wires;
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), replace_tree(tree), parent), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(RoutedPlacementRoutes, InvalidLaterTreeDoesNotApplyAnEarlierValidTree)
{
    auto extra = witness["nets"]["data"].object_items();
    auto physical = extra["physical"].object_items();
    physical["name"] = "other";
    extra["physical"] = physical;
    auto invalid = witness.object_items();
    invalid["nets"] = J::object{{"data", witness["nets"]["data"]}, {"other", extra}};
    EXPECT_THROW(apply_routed_placement_routes(ctx.get(), invalid, parent), log_execution_error_exception);
    EXPECT_TRUE(net->wires.empty());
    EXPECT_TRUE(other->wires.empty());
}

TEST_F(RoutedPlacementRoutes, FinalCheckRejectsAnExtraBranchOnAPreservedTree)
{
    auto result = apply_routed_placement_routes(ctx.get(), witness, parent);
    ctx->bindPip(ctx->add_pip(source, spare), net, STRENGTH_WEAK);
    EXPECT_THROW(check_routed_placement_routes(ctx.get(), witness, result["preserved"]), log_execution_error_exception);
}
} // namespace
