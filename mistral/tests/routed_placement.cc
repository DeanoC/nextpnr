/* Native observation coverage and identity. SPDX-License-Identifier: ISC */
#include "routed_placement.h"
#include "critical_cohort_model.h"
#include "critical_cohort.h"
#include "gtest/gtest.h"
#include "log.h"

USING_NEXTPNR_NAMESPACE
namespace {
class RoutedPlacement : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clk;
    CellInfo *launch, *capture;
    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 50e6;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        clk = ctx->createNet(ctx->id("clock"));
        clk->is_global = true;
        clk->clkconstr = std::make_unique<ClockConstraint>();
        clk->clkconstr->period = DelayPair(20000);
        clk->clkconstr->high = clk->clkconstr->low = DelayPair(10000);
        launch = ff("launch", 12, 10, 2);
        capture = ff("capture", 15, 10, 8);
        capture->connectPort(id_DATAIN, launch->getPort(id_Q));
        launch->connectPort(id_DATAIN, capture->getPort(id_Q));
        ctx->assignArchInfo();
    }
    CellInfo *ff(const char *name, int x, int y, int z)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clk);
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        cell->pin_data[id_SDATA].state = PIN_0;
        cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", name)));
        ctx->assignArchInfo();
        ctx->bindBel(ctx->getBelByLocation(Loc(x, y, z)), cell, STRENGTH_WEAK);
        return cell;
    }
    CellInfo *ram()
    {
        auto *cell = ctx->createCell(ctx->id("ram"), id_MISTRAL_M10K);
        cell->params[id_CFG_TDP] = 1;
        cell->params[id_CFG_ABITS] = 10;
        cell->params[id_CFG_DBITS] = 10;
        for (auto pin : {id_CLK1, id_CLK2, id_A1EN, id_B1EN, id_A1WE, id_B1WE})
            cell->addInput(pin);
        cell->connectPort(id_CLK1, clk);
        cell->connectPort(id_CLK2, clk);
        cell->pin_data[id_CLK1].bel_pins = {ctx->id("CLKIN[0]")};
        cell->pin_data[id_CLK2].bel_pins = {ctx->id("CLKIN[1]")};
        cell->pin_data[id_A1EN].bel_pins = {ctx->id("ENABLE[1]")};
        cell->pin_data[id_B1EN].bel_pins = {ctx->id("ENABLE[0]")};
        cell->pin_data[id_A1WE].bel_pins = {ctx->id("WREN[0]")};
        cell->pin_data[id_B1WE].bel_pins = {ctx->id("WREN[1]")};
        auto *one = ctx->createCell(ctx->id("ram_one"), id_MISTRAL_CONST);
        one->params[id_LUT] = 1;
        one->addOutput(id_Q);
        one->connectPort(id_Q, ctx->createNet(ctx->id("ram_one_net")));
        for (auto pin : {id_A1EN, id_B1EN, id_A1WE, id_B1WE})
            cell->connectPort(pin, one->getPort(id_Q));
        for (char side : {'A', 'B'}) {
            auto data = ctx->idf("%c1DATA[8]", side), q = ctx->idf("%c1Q[8]", side);
            cell->addInput(data);
            cell->addOutput(q);
            cell->pin_data[data].bel_pins = {ctx->idf("DATA%cIN[8]", side), ctx->idf("DATA%cIN[18]", side)};
            cell->pin_data[q].bel_pins = {ctx->idf("DATA%cOUT[8]", side)};
            cell->connectPort(q, ctx->createNet(ctx->idf("ram$%c$q", side)));
        }
        for (auto bel : ctx->getBels())
            if (ctx->isValidBelForCellType(cell->type, bel) && ctx->checkBelAvail(bel)) {
                ctx->bindBel(bel, cell, STRENGTH_WEAK);
                break;
            }
        EXPECT_NE(cell->bel, BelId());
        return cell;
    }
    json11::Json entry(const json11::Json &inventory, const char *group, CellInfo *cell, IdString pin)
    {
        for (const auto &row : inventory[group].array_items())
            if (row["cell"].string_value() == cell->name.str(ctx.get()) &&
                row["port"].string_value() == pin.str(ctx.get()))
                return row;
        return {};
    }
};

TEST_F(RoutedPlacement, PostRouteLutReceiptRejectsUndeclaredCellsClocksAndRegisters)
{
    using J = json11::Json;
    auto *lut = ctx->createCell(ctx->id("movable_lut"), id_MISTRAL_ALUT2);
    lut->addInput(id_A); lut->addInput(id_B); lut->addOutput(id_Q);
    lut->params[id_LUT] = 6;
    lut->connectPort(id_A, launch->getPort(id_Q));
    lut->connectPort(id_B, capture->getPort(id_Q));
    lut->connectPort(id_Q, ctx->createNet(ctx->id("lut_q")));
    ctx->assign_comb_info(lut);
    auto source = ctx->getBelByLocation(Loc(24, 20, 0));
    auto target = ctx->getBelByLocation(Loc(25, 20, 0));
    ctx->bindBel(source, lut, STRENGTH_WEAK);
    auto parent = routed_placement_snapshot(ctx.get());
    ASSERT_TRUE(apply_critical_cohort_placement(ctx.get(), {{lut, target}}));
    auto final = routed_placement_snapshot(ctx.get());
    auto move = J::object{{"cell", "movable_lut"}, {"from_bel", ctx->getBelName(source).str(ctx.get())},
        {"to_bel", ctx->getBelName(target).str(ctx.get())}, {"strength", int(STRENGTH_WEAK)}};
    auto receipt = J::object{{"version", 1}, {"parent", parent}, {"target", final}, {"moves", J::array{move}}};
    EXPECT_NO_THROW(check_post_route_lut_moves(receipt, final));
    auto bad = receipt;
    bad["moves"] = J::array{};
    EXPECT_THROW(check_post_route_lut_moves(bad, final), log_execution_error_exception);
    bad = receipt; bad["moves"] = J::array{move, move};
    EXPECT_THROW(check_post_route_lut_moves(bad, final), log_execution_error_exception);
    for (const char *key : {"design", "clocks", "architecture", "ordering", "rng", "timing_settings"}) {
        auto changed = final.object_items(); changed[key] = J();
        bad = receipt; bad["target"] = changed;
        EXPECT_THROW(check_post_route_lut_moves(bad, changed), log_execution_error_exception) << key;
    }
    auto placement = final["placement"].object_items();
    placement["capture"] = J::array{"MISTRAL_FF.24.20.2", int(STRENGTH_WEAK)};
    auto changed = final.object_items(); changed["placement"] = placement;
    bad = receipt; bad["target"] = changed;
    EXPECT_THROW(check_post_route_lut_moves(bad, changed), log_execution_error_exception);
    auto ff_move = move; ff_move["cell"] = "capture";
    bad = receipt; bad["moves"] = J::array{ff_move};
    EXPECT_THROW(check_post_route_lut_moves(bad, final), log_execution_error_exception);
    auto wrong_strength = move; wrong_strength["strength"] = int(STRENGTH_LOCKED);
    bad = receipt; bad["moves"] = J::array{wrong_strength};
    EXPECT_THROW(check_post_route_lut_moves(bad, final), log_execution_error_exception);
}

TEST_F(RoutedPlacement, GroupPlanReplaysRegistersWithoutInventingTiming)
{
    ctx->routed_placement_group_moves = true;
    auto origin = routed_placement_snapshot(ctx.get());
    auto target = origin.object_items();
    auto placements = target["placement"].object_items();
    auto bel = ctx->getBelByLocation(Loc(13, 10, 2));
    placements[launch->name.str(ctx.get())] = json11::Json::array{ctx->getBelName(bel).str(ctx.get()), int(launch->belStrength)};
    target["placement"] = placements;
    auto plan = make_routed_placement_group_plan(origin, origin, target);
    EXPECT_EQ(plan["version"], json11::Json(3));
    EXPECT_TRUE(plan["predicted"].is_null());
    EXPECT_TRUE(plan["data_routes"].is_null());
    EXPECT_NO_THROW(apply_routed_placement_plan(ctx.get(), plan, origin));
    EXPECT_EQ(routed_placement_snapshot(ctx.get()), json11::Json(target));
}

TEST_F(RoutedPlacement, GroupPlanRequiresItsExplicitModeAndEvaluationContract)
{
    auto ordinary = routed_placement_snapshot(ctx.get());
    auto group_plan = make_routed_placement_group_plan(ordinary, ordinary, ordinary);
    EXPECT_THROW(apply_routed_placement_plan(ctx.get(), group_plan, ordinary), log_execution_error_exception);
    ctx->routed_placement_preserve_routes = ctx->routed_placement_group_moves = true;
    auto origin = routed_placement_snapshot(ctx.get());
    auto plan = make_routed_placement_plan(origin, origin, origin, 0, 0);
    EXPECT_THROW(apply_routed_placement_plan(ctx.get(), plan, origin), log_execution_error_exception);
    auto invalid = make_routed_placement_group_plan(origin, origin, origin).object_items();
    invalid["evaluation"] = "already_qualified";
    EXPECT_THROW(apply_routed_placement_plan(ctx.get(), invalid, origin), log_execution_error_exception);
    EXPECT_EQ(routed_placement_snapshot(ctx.get()), origin);
}

TEST_F(RoutedPlacement, GroupPlanCannotMoveAProtectedRegister)
{
    ctx->routed_placement_preserve_routes = ctx->routed_placement_group_moves = true;
    launch->attrs[ctx->id("dont_touch")] = 1;
    auto origin = routed_placement_snapshot(ctx.get());
    auto target = origin.object_items();
    auto placements = target["placement"].object_items();
    placements[launch->name.str(ctx.get())] = json11::Json::array{
        ctx->getBelName(ctx->getBelByLocation(Loc(13, 10, 2))).str(ctx.get()), int(launch->belStrength)};
    target["placement"] = placements;
    auto plan = make_routed_placement_group_plan(origin, origin, target);
    EXPECT_THROW(apply_routed_placement_plan(ctx.get(), plan, origin), log_execution_error_exception);
    EXPECT_EQ(routed_placement_snapshot(ctx.get()), origin);
}

TEST_F(RoutedPlacement, SnapshotPreservesTypedParametersAndDoesNotMutate)
{
    auto first = routed_placement_snapshot(ctx.get());
    EXPECT_EQ(first, routed_placement_snapshot(ctx.get()));
    launch->params[ctx->id("sample")] = Property(1, 8);
    auto numeric = routed_placement_snapshot(ctx.get());
    launch->params[ctx->id("sample")] = Property("10000000");
    EXPECT_NE(numeric, routed_placement_snapshot(ctx.get()));
}

TEST_F(RoutedPlacement, NativeEndpointSetupPathsHaveExactClockPairsAndSums)
{
    TimingAnalyser timing(ctx.get());
    timing.with_clock_skew = true;
    timing.setup(false, false, true);
    std::vector<EndpointClockPairTiming> rows;
    std::vector<CriticalPath> paths;
    ASSERT_TRUE(timing.get_endpoint_clock_pair_timings({capture->name, id_DATAIN}, rows));
    ASSERT_TRUE(timing.get_endpoint_setup_paths({capture->name, id_DATAIN}, paths));
    ASSERT_EQ(paths.size(), rows.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        EXPECT_EQ(paths[i].clock_pair.start.clock, rows[i].launch.clock);
        EXPECT_EQ(paths[i].clock_pair.end.clock, rows[i].capture.clock);
        int64_t sum = 0;
        for (const auto &segment : paths[i].segments) sum += segment.delay;
        EXPECT_EQ(sum, rows[i].max_path_delay);
        EXPECT_EQ(paths[i].segments.back().to, std::make_pair(capture->name, id_DATAIN));
    }
}

TEST_F(RoutedPlacement, NativeEndpointSetupPathsRejectAnUnanalysedFrame)
{
    TimingAnalyser timing(ctx.get());
    std::vector<CriticalPath> paths(1);
    EXPECT_FALSE(timing.get_endpoint_setup_paths({capture->name, id_DATAIN}, paths));
    EXPECT_TRUE(paths.empty());
}

TEST_F(RoutedPlacement, GroupLegalityProbeRestoresARejectedPlacement)
{
    ctx->routed_placement_group_moves = true;
    launch->attrs[ctx->id("dont_touch")] = 1;
    const auto origin = routed_placement_snapshot(ctx.get());
    auto target = origin.object_items(), placement = target["placement"].object_items();
    placement[launch->name.str(ctx.get())] = json11::Json::array{
        ctx->getBelName(ctx->getBelByLocation(Loc(13, 10, 2))).str(ctx.get()), int(launch->belStrength)};
    target["placement"] = placement;
    auto plan = make_routed_placement_group_plan(origin, origin, target);
    EXPECT_FALSE(apply_routed_placement_plan(ctx.get(), plan, origin, true));
    EXPECT_EQ(routed_placement_snapshot(ctx.get()), origin);
}

TEST_F(RoutedPlacement, SnapshotIncludesTopLevelPortDirectionAndNet)
{
    PortInfo port;
    port.name = ctx->id("external");
    port.type = PORT_IN;
    port.net = clk;
    ctx->ports[port.name] = port;
    const auto original = routed_placement_snapshot(ctx.get());
    ctx->ports.at(port.name).type = PORT_OUT;
    EXPECT_NE(original["design"], routed_placement_snapshot(ctx.get())["design"]);
    ctx->ports.at(port.name).type = PORT_IN;
    ctx->ports.at(port.name).net = capture->getPort(id_Q);
    EXPECT_NE(original["design"], routed_placement_snapshot(ctx.get())["design"]);
}

TEST_F(RoutedPlacement, ExecutionBackendDoesNotChangeReplayIdentityButRouterTuningDoes)
{
    const auto original = routed_placement_snapshot(ctx.get());
    ctx->settings[ctx->id("gpurouter/cpu")] = 1;
    ctx->settings[ctx->id("gpurouter/device")] = -1;
    ctx->settings[ctx->id("gpurouter/required")] = 1;
    EXPECT_EQ(original, routed_placement_snapshot(ctx.get()));
    ctx->settings[ctx->id("gpurouter/maxBatches")] = 7;
    EXPECT_NE(original, routed_placement_snapshot(ctx.get()));
}

TEST_F(RoutedPlacement, NativeRowsAreEnumeratedAndPredictionsAreNotFinalObservations)
{
    auto result = routed_placement_endpoints(ctx.get());
    EXPECT_TRUE(result["analysis_complete"].bool_value());
    EXPECT_FALSE(result["final_analogue_model"].bool_value());
    EXPECT_FALSE(result["complete"].bool_value());
    EXPECT_EQ(result["endpoints"].array_items().size(), 2U);
    EXPECT_TRUE(result["unsupported"].array_items().empty());
    for (const auto &endpoint : result["endpoints"].array_items()) {
        ASSERT_EQ(endpoint["rows"].array_items().size(), 1U);
        auto row = endpoint["rows"][0];
        EXPECT_EQ(row["launch"][0].string_value(), "clock");
        EXPECT_EQ(row["capture"][0].string_value(), "clock");
        EXPECT_TRUE(row["setup_timed"].bool_value());
        EXPECT_TRUE(row["hold_related"].bool_value());
        EXPECT_TRUE(row["setup_margin_ps"].is_number());
        EXPECT_TRUE(row["hold_margin_ps"].is_number());
    }
}

TEST_F(RoutedPlacement, ConsumedUserBelAttributeRetainsItsProtectedNativeIdentity)
{
    const auto bel = launch->bel;
    ctx->unbindBel(bel);
    ctx->bindBel(bel, launch, STRENGTH_USER);
    launch->attrs[ctx->id("BEL")] = ctx->getBelName(bel).str(ctx.get());
    const auto original = routed_placement_snapshot(ctx.get());
    ctx->archInfoToAttributes();
    ASSERT_FALSE(launch->attrs.count(ctx->id("BEL")));
    EXPECT_EQ(original, routed_placement_snapshot(ctx.get()));
    ctx->unbindBel(bel);
    ctx->bindBel(ctx->getBelByLocation(Loc(18, 10, 2)), launch, STRENGTH_USER);
    EXPECT_NE(original["design"], routed_placement_snapshot(ctx.get())["design"]);
}

TEST_F(RoutedPlacement, DynamicAsynchronousControlIsUnsupportedEvenWhenUnchanged)
{
    capture->pin_data[id_ACLR].state = PIN_SIG;
    capture->connectPort(id_ACLR, launch->getPort(id_Q));
    ctx->assignArchInfo();
    auto result = routed_placement_endpoints(ctx.get());
    ASSERT_FALSE(result["unsupported"].array_items().empty());
    bool found = false;
    for (const auto &endpoint : result["unsupported"].array_items())
        found |= endpoint["cell"] == json11::Json("capture") && endpoint["port"] == json11::Json("ACLR");
    EXPECT_TRUE(found);
    EXPECT_FALSE(result["complete"].bool_value());
}

TEST_F(RoutedPlacement, StaticInputsHaveProofRatherThanInventedTimingRows)
{
    auto result = routed_placement_endpoints(ctx.get());
    EXPECT_FALSE(result["static_endpoints"].array_items().empty());
    for (const auto &entry : result["static_endpoints"].array_items()) {
        EXPECT_FALSE(entry["proof"].array_items().empty());
        EXPECT_TRUE(entry["rows"].is_null());
    }
}

TEST_F(RoutedPlacement, NativeHoldRowsRetainClockEdges)
{
    capture->pin_data[id_CLK].state = PIN_INV;
    ctx->assignArchInfo();
    auto result = routed_placement_endpoints(ctx.get());
    ASSERT_EQ(result["endpoints"].array_items().size(), 2U);
    for (const auto &entry : result["endpoints"].array_items()) {
        auto row = entry["rows"][0];
        EXPECT_NE(row["launch"][1], row["capture"][1]);
        EXPECT_EQ(row["setup_window_ps"].number_value(), 10000);
    }
}

TEST_F(RoutedPlacement, UnconnectedTdpPaddingHasNativeProofOnlyWhenBothReadOutputsAreUnobserved)
{
    auto *memory = ram();
    auto data = ctx->id("A1DATA[8]"), q = ctx->id("B1Q[8]");
    auto unused = routed_placement_endpoints(ctx.get());
    auto proof = entry(unused, "inactive_endpoints", memory, data);
    ASSERT_EQ(proof["proof"][0].string_value(), "m10k_unobserved_data_bit");
    EXPECT_TRUE(proof["rows"].is_null());
    capture->disconnectPort(id_DATAIN);
    capture->connectPort(id_DATAIN, memory->getPort(q));
    auto live = routed_placement_endpoints(ctx.get());
    EXPECT_TRUE(entry(live, "inactive_endpoints", memory, data).is_null());
    EXPECT_FALSE(entry(live, "unsupported", memory, data).is_null());
}

TEST_F(RoutedPlacement, UnconnectedTdpDataWithATopLevelOrClockReadResultIsNotUnobserved)
{
    auto *memory = ram();
    auto data = ctx->id("A1DATA[8]"), q = ctx->id("A1Q[8]");
    PortInfo top;
    top.name = ctx->id("external");
    top.type = PORT_OUT;
    top.net = memory->getPort(q);
    ctx->ports[top.name] = top;
    EXPECT_TRUE(entry(routed_placement_endpoints(ctx.get()), "inactive_endpoints", memory, data).is_null());
    ctx->ports.clear();
    memory->getPort(q)->clkconstr = std::make_unique<ClockConstraint>(*clk->clkconstr);
    EXPECT_TRUE(entry(routed_placement_endpoints(ctx.get()), "inactive_endpoints", memory, data).is_null());
}

TEST_F(RoutedPlacement, DisabledTdpWriteProofHonorsConstantValueAndInputInversion)
{
    auto *memory = ram();
    auto data = ctx->id("A1DATA[8]");
    memory->connectPort(data, launch->getPort(id_Q));
    auto *constant = ctx->createCell(ctx->id("enable_value"), id_MISTRAL_CONST);
    constant->params[id_LUT] = 1;
    constant->addOutput(id_Q);
    constant->connectPort(id_Q, ctx->createNet(ctx->id("enable_net")));
    memory->disconnectPort(id_A1WE);
    auto *invert = ctx->createCell(ctx->id("invert_enable"), id_MISTRAL_NOT);
    invert->addInput(id_A);
    invert->addOutput(id_Q);
    invert->connectPort(id_A, constant->getPort(id_Q));
    invert->connectPort(id_Q, ctx->createNet(ctx->id("inverted_enable")));
    memory->connectPort(id_A1WE, invert->getPort(id_Q));
    auto disabled = routed_placement_endpoints(ctx.get());
    ASSERT_EQ(entry(disabled, "inactive_endpoints", memory, data)["proof"][0].string_value(), "m10k_write_disabled");
    constant->params[id_LUT] = 0;
    auto enabled = routed_placement_endpoints(ctx.get());
    EXPECT_TRUE(entry(enabled, "inactive_endpoints", memory, data).is_null());
    EXPECT_FALSE(entry(enabled, "endpoints", memory, data).is_null());
    memory->pin_data[id_A1WE].state = PIN_INV;
    EXPECT_TRUE(entry(routed_placement_endpoints(ctx.get()), "inactive_endpoints", memory, data).is_null());
    memory->pin_data[id_A1WE].state = PIN_SIG;
    constant->params[id_LUT] = Property(Property::Sx);
    EXPECT_TRUE(entry(routed_placement_endpoints(ctx.get()), "inactive_endpoints", memory, data).is_null());
}

TEST_F(RoutedPlacement, MixedWidthTdpRequiresItsOwnCoverageProof)
{
    auto *memory = ram();
    memory->params[id_CFG_MIXED_WIDTH] = 1;
    memory->params[id_CFG_RD_DBITS] = 10;
    memory->params[id_CFG_RD_ABITS] = 10;
    EXPECT_TRUE(entry(routed_placement_endpoints(ctx.get()), "inactive_endpoints", memory,
                      ctx->id("A1DATA[8]")).is_null());
}

TEST_F(RoutedPlacement, UnmodeledHardIpOutputCannotHideInAMixedRegisteredInputCone)
{
    auto *hps = ctx->createCell(ctx->id("hard_ip"), id_cyclonev_hps_interface_mpu_general_purpose);
    auto out = ctx->id("gp_out[0]");
    hps->addOutput(out);
    hps->connectPort(out, ctx->createNet(ctx->id("unknown_source")));
    auto *gate = ctx->createCell(ctx->id("mixed"), id_MISTRAL_ALUT2);
    gate->params[id_LUT] = Property(6, 4);
    gate->addInput(id_A);
    gate->addInput(id_B);
    gate->addOutput(id_Q);
    gate->connectPort(id_A, launch->getPort(id_Q));
    gate->connectPort(id_B, hps->getPort(out));
    gate->connectPort(id_Q, ctx->createNet(ctx->id("mixed_result")));
    capture->disconnectPort(id_DATAIN);
    capture->connectPort(id_DATAIN, gate->getPort(id_Q));
    ctx->assignArchInfo();
    ctx->assign_default_pinmap(gate);
    ctx->bindBel(ctx->getBelByLocation(Loc(12, 10, 0)), gate, STRENGTH_WEAK);
    auto result = routed_placement_endpoints(ctx.get());
    EXPECT_FALSE(entry(result, "endpoints", capture, id_DATAIN).is_null());
    auto missing = entry(result, "unsupported", hps, out);
    EXPECT_EQ(missing["reason"].string_value(), "unmodeled_launch_boundary");
    EXPECT_EQ(missing["direction"].string_value(), "output");
    gate->disconnectPort(id_B);
    PortInfo top;
    top.name = ctx->id("hard_ip_output");
    top.type = PORT_OUT;
    top.net = hps->getPort(out);
    ctx->ports[top.name] = top;
    EXPECT_FALSE(entry(routed_placement_endpoints(ctx.get()), "unsupported", hps, out).is_null());
    ctx->ports.clear();
    EXPECT_TRUE(entry(routed_placement_endpoints(ctx.get()), "unsupported", hps, out).is_null());
}

TEST_F(RoutedPlacement, ExplicitSwapReplaysAllOccupantsAndPreservesStrengths)
{
    auto original = routed_placement_snapshot(ctx.get());
    auto a = launch->bel, b = capture->bel;
    ASSERT_TRUE(apply_critical_cohort_placement(ctx.get(), {{launch, b}, {capture, a}}));
    auto target = routed_placement_snapshot(ctx.get());
    auto plan = make_routed_placement_plan(original, original, target, -100, -50);
    ASSERT_EQ(plan["moves"].array_items().size(), 2U);
    ASSERT_TRUE(apply_critical_cohort_placement(ctx.get(), {{launch, a}, {capture, b}}));
    ASSERT_EQ(original, routed_placement_snapshot(ctx.get()));
    apply_routed_placement_plan(ctx.get(), plan, original);
    EXPECT_EQ(target, routed_placement_snapshot(ctx.get()));
}

TEST_F(RoutedPlacement, CompleteRoutePlanReplaysItsExactTarget)
{
    ctx->routed_placement_preserve_routes = true;
    ctx->routed_placement_routes = "{}"; // This test exercises placement, before route import.
    auto origin = routed_placement_snapshot(ctx.get());
    auto a = launch->bel, b = capture->bel;
    ASSERT_TRUE(apply_critical_cohort_placement(ctx.get(), {{launch, b}, {capture, a}}));
    auto target = routed_placement_snapshot(ctx.get());
    auto routing = json11::Json::object{{"joint_trees", true}, {"native_analogue", true}, {"nets", json11::Json::object{}}};
    auto plan = make_routed_placement_plan(origin, origin, target, -100, -50, routing);
    EXPECT_EQ(plan["version"].int_value(), 2);
    ASSERT_TRUE(apply_critical_cohort_placement(ctx.get(), {{launch, a}, {capture, b}}));
    apply_routed_placement_plan(ctx.get(), plan, origin);
    EXPECT_EQ(routed_placement_snapshot(ctx.get()), target);
}

TEST_F(RoutedPlacement, StaleRouteTargetCannotMoveTheFreshPlacement)
{
    using J = json11::Json;
    ctx->routed_placement_preserve_routes = true;
    ctx->routed_placement_routes = "{}";
    auto origin = routed_placement_snapshot(ctx.get());
    auto routing = J::object{{"joint_trees", true}, {"native_analogue", true}, {"nets", J::object{}}};
    auto plan = make_routed_placement_plan(origin, origin, origin, -100, -50, routing).object_items();
    auto carried = plan["data_routes"].object_items();
    auto target = carried["target"].object_items();
    target["rng"] = "stale";
    carried["target"] = target;
    plan["data_routes"] = carried;
    EXPECT_THROW(apply_routed_placement_plan(ctx.get(), plan, origin), log_execution_error_exception);
    EXPECT_EQ(routed_placement_snapshot(ctx.get()), origin);
}

TEST_F(RoutedPlacement, RoutePlanRequiresPreservationAndParentWitness)
{
    using J = json11::Json;
    ctx->routed_placement_preserve_routes = true;
    auto origin = routed_placement_snapshot(ctx.get());
    auto routing = J::object{{"joint_trees", true}, {"native_analogue", true}, {"nets", J::object{}}};
    auto plan = make_routed_placement_plan(origin, origin, origin, -100, -50, routing);
    EXPECT_THROW(apply_routed_placement_plan(ctx.get(), plan, origin), log_execution_error_exception);
    ctx->routed_placement_routes = "{}";
    ctx->routed_placement_preserve_routes = false;
    EXPECT_THROW(apply_routed_placement_plan(ctx.get(), plan, origin), log_execution_error_exception);
    ctx->routed_placement_preserve_routes = true;
    EXPECT_EQ(routed_placement_snapshot(ctx.get()), origin);
}

TEST_F(RoutedPlacement, ProtectedOrMissingOccupantAndDuplicateTargetsAreRejectedWithoutChange)
{
    auto original = routed_placement_snapshot(ctx.get());
    auto a = launch->bel, b = capture->bel;
    EXPECT_FALSE(apply_critical_cohort_placement(ctx.get(), {{launch, b}}));
    EXPECT_FALSE(apply_critical_cohort_placement(ctx.get(), {{launch, a}, {capture, a}}));
    EXPECT_FALSE(apply_critical_cohort_placement(ctx.get(), {{launch, b}, {launch, a}}));
    EXPECT_EQ(original, routed_placement_snapshot(ctx.get()));
    launch->attrs[ctx->id("keep")] = 1;
    original = routed_placement_snapshot(ctx.get());
    EXPECT_FALSE(apply_critical_cohort_placement(ctx.get(), {{launch, b}, {capture, a}}));
    EXPECT_EQ(original, routed_placement_snapshot(ctx.get()));
}

TEST_F(RoutedPlacement, StalePlanCannotMoveTheFreshPlacement)
{
    auto original = routed_placement_snapshot(ctx.get());
    auto plan = make_routed_placement_plan(original, original, original, -100, -50).object_items();
    launch->params[ctx->id("changed")] = 1;
    auto current = routed_placement_snapshot(ctx.get());
    EXPECT_THROW(apply_routed_placement_plan(ctx.get(), plan, current), log_execution_error_exception);
    EXPECT_EQ(current, routed_placement_snapshot(ctx.get()));
}
} // namespace
