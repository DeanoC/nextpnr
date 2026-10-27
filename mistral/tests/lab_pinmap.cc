#include <array>
#include <map>
#include <memory>

#include "gtest/gtest.h"
#include "nextpnr.h"
#include "timing.h"
#include "analogue_lut_branch.h"

USING_NEXTPNR_NAMESPACE

class LabPinmapTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *top, *bottom, *socket;
    BelId socket_bel;

    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        auto clock = ctx->createNet(ctx->id("clock"));
        clock->is_global = true;
        auto arithmetic = [&](const char *name) {
            auto cell = ctx->createCell(ctx->id(name), id_MISTRAL_ALUT_ARITH);
            for (IdString pin : {id_A, id_B, id_C, id_D0, id_D1, id_CI})
                cell->addInput(pin);
            for (IdString pin : {id_SO, id_CO})
                cell->addOutput(pin);
            cell->connectPort(id_B, ctx->createNet(ctx->idf("%s$input", name)));
            cell->connectPort(id_SO, ctx->createNet(ctx->idf("%s$sum", name)));
            return cell;
        };
        top = arithmetic("counter_bit6");
        bottom = arithmetic("counter_bit7");
        top->connectPorts(id_CO, bottom, id_CI);
        socket = ctx->createCell(ctx->id("socket"), id_MISTRAL_FF);
        for (IdString pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            socket->addInput(pin);
        socket->addOutput(id_Q);
        socket->connectPort(id_CLK, clock);
        socket->connectPort(id_DATAIN, ctx->createNet(ctx->id("socket_input")));
        socket->connectPort(id_Q, ctx->createNet(ctx->id("socket_output")));
        socket->pin_data[id_ENA].state = PIN_1;
        socket->pin_data[id_ACLR].state = PIN_1;
        socket->pin_data[id_SCLR].state = PIN_0;
        socket->pin_data[id_SLOAD].state = PIN_0;
        ctx->assignArchInfo();
        ctx->bindBel(ctx->getBelByLocation(Loc(24, 1, 18)), top, STRENGTH_STRONG);
        ctx->bindBel(ctx->getBelByLocation(Loc(24, 1, 19)), bottom, STRENGTH_STRONG);
        socket_bel = ctx->getBelByLocation(Loc(24, 1, 2));
        ctx->bindBel(socket_bel, socket, STRENGTH_USER);
    }

    WireId input_wire(CellInfo *cell)
    {
        return ctx->getBelPinWire(cell->bel, cell->pin_data.at(id_B).bel_pins.at(0));
    }
};

TEST_F(LabPinmapTest, UserLockedSocketStillLegalisesOtherArithmeticInputs)
{
    // The placement estimate maps both halves to E0. FES #263 used to
    // retain it because STRENGTH_USER is numerically above STRENGTH_LOCKED.
    ASSERT_EQ(input_wire(top), input_wire(bottom));
    ctx->lab_pre_route();
    EXPECT_NE(input_wire(top), input_wire(bottom));
    EXPECT_EQ(input_wire(top), ctx->getBelPinWire(top->bel, id_C));
    EXPECT_EQ(input_wire(bottom), ctx->getBelPinWire(bottom->bel, id_D));
    EXPECT_EQ(socket->bel, socket_bel);
    EXPECT_EQ(socket->belStrength, STRENGTH_USER);
    auto buffer = ctx->cells.at(ctx->id("socket$ROUTETHRU")).get();
    EXPECT_EQ(buffer->type, id_MISTRAL_BUF);
    EXPECT_EQ(buffer->getPort(id_Q), socket->getPort(id_DATAIN));
    EXPECT_EQ(buffer->getPort(id_A), ctx->nets.at(ctx->id("socket_input")).get());
    // Loc(24, 1, 2) is ALM 0 FF0, the top half. The route-through uses F0.
    EXPECT_EQ(buffer->pin_data.at(id_A).bel_pins, std::vector<IdString>{id_F0});
}

TEST_F(LabPinmapTest, RoutedBufferCacheRefreshPreservesGraphAndRestoresNativeLegality)
{
    // A peer with three inputs occupies the alternative E/F data path.
    // The stale cache then incorrectly demands a second FF fabric input.
    auto *peer = ctx->createCell(ctx->id("cache_refresh_peer"), id_MISTRAL_ALUT3);
    for (IdString port : {id_A, id_B, id_C}) {
        peer->addInput(port);
        peer->connectPort(port, ctx->createNet(ctx->idf("peer_%s", port.c_str(ctx.get()))));
    }
    peer->addOutput(id_Q);
    peer->connectPort(id_Q, ctx->createNet(ctx->id("peer_output")));
    peer->setParam(id_LUT, Property(0x96, 8));
    ctx->assign_comb_info(peer);
    ctx->assign_default_pinmap(peer);
    ctx->bindBel(ctx->getBelByLocation(Loc(24, 1, 1)), peer, STRENGTH_STRONG);
    auto add_lut = [&](const std::string &name, int width, int alm, PlaceStrength strength) {
        auto *lut = ctx->createCell(ctx->id(name), width == 6 ? id_MISTRAL_ALUT6 : id_MISTRAL_ALUT3);
        const std::array<IdString, 6> pins{id_A,id_B,id_C,id_D,id_E,id_F};
        for (int i = 0; i < width; ++i) {
            lut->addInput(pins[i]);
            lut->connectPort(pins[i], ctx->createNet(ctx->id(name + "_input" + std::to_string(i))));
        }
        lut->addOutput(id_Q);lut->connectPort(id_Q,ctx->createNet(ctx->id(name + "_output")));
        lut->setParam(id_LUT,Property(0x9696,width == 6 ? 64 : 8));
        ctx->assign_comb_info(lut);ctx->assign_default_pinmap(lut);
        ctx->bindBel(ctx->getBelByLocation(Loc(24,1,6*alm)),lut,strength);
        return lut;
    };
    auto *moved = add_lut("cache_order_move",3,1,STRENGTH_WEAK);
    for (int alm = 4; alm <= 8; ++alm) add_lut("cache_order_fill" + std::to_string(alm),6,alm,STRENGTH_STRONG);
    add_lut("cache_order_fill9",3,9,STRENGTH_STRONG);
    auto target = ctx->getBelByLocation(Loc(24,1,12));
    auto *logical_input = socket->getPort(id_DATAIN);
    const auto controls = socket->ffInfo.ctrlset;
    ctx->lab_pre_route();
    auto *buffer = ctx->cells.at(ctx->id("socket$ROUTETHRU")).get();
    ASSERT_EQ(buffer->getPort(id_A), logical_input);
    ASSERT_EQ(buffer->getPort(id_Q), socket->getPort(id_DATAIN));
    ASSERT_EQ(socket->ffInfo.datain, logical_input);
    const auto data = ctx->bel_data(socket_bel).lab_data;
    const auto before_count = ctx->labs.at(data.lab).alms.at(data.alm).unique_input_count;
    ASSERT_FALSE(ctx->is_alm_legal(data.lab, data.alm));
    auto lab_inputs = [&]() { int count = 0;for(const auto &a:ctx->labs.at(data.lab).alms)count+=a.unique_input_count;return count; };
    ASSERT_EQ(lab_inputs(),43);
    ASSERT_FALSE(analogue_lut_move_allowed(ctx.get(),moved,target));
    const auto native = routed_placement_snapshot(ctx.get());
    const auto top_wire = input_wire(top), bottom_wire = input_wire(bottom);
    const auto changed = lut_branch_detail::refresh_route_through_datain(ctx.get());
    ASSERT_EQ(changed.array_items().size(), 1u);
    EXPECT_EQ(socket->ffInfo.datain, buffer->getPort(id_Q));
    EXPECT_EQ(socket->getPort(id_DATAIN), buffer->getPort(id_Q));
    EXPECT_EQ(buffer->getPort(id_A), logical_input);
    EXPECT_EQ(socket->ffInfo.ctrlset, controls);
    EXPECT_EQ(routed_placement_snapshot(ctx.get()), native);
    EXPECT_EQ(input_wire(top), top_wire);
    EXPECT_EQ(input_wire(bottom), bottom_wire);
    EXPECT_EQ(ctx->labs.at(data.lab).alms.at(data.alm).unique_input_count, before_count - 1);
    EXPECT_TRUE(ctx->is_alm_legal(data.lab, data.alm));
    EXPECT_TRUE(ctx->isBelLocationValid(socket_bel));
    EXPECT_EQ(lab_inputs(),42);
    EXPECT_TRUE(analogue_lut_move_allowed(ctx.get(),moved,target));
    EXPECT_TRUE(lut_branch_detail::refresh_route_through_datain(ctx.get()).array_items().empty());
}

TEST_F(LabPinmapTest, RestoredScaffoldPreservesPinmapsAndDataInput)
{
    // A frozen shell can carry an alternate legal pin assignment. It must
    // not be replaced by the fresh-route allocation or a new route-through.
    top->pin_data[id_B].bel_pins = {id_A};
    bottom->pin_data[id_B].bel_pins = {id_B};
    auto top_wire = input_wire(top), bottom_wire = input_wire(bottom);
    auto data = socket->getPort(id_DATAIN);
    auto cell_count = ctx->cells.size();
    ctx->unbindBel(socket_bel);
    ctx->bindBel(socket_bel, socket, STRENGTH_LOCKED);
    ctx->lab_pre_route();
    EXPECT_EQ(input_wire(top), top_wire);
    EXPECT_EQ(input_wire(bottom), bottom_wire);
    EXPECT_EQ(socket->getPort(id_DATAIN), data);
    EXPECT_EQ(ctx->cells.size(), cell_count);
}

TEST(LutPlacementTiming, Lut6PhysicalFIsFasterThanE)
{
    // Independent Quartus asymmetric-LUT audit traces the launch registers
    // through decoded RBF routing: E0 is slow, F0 fast. The source LUT's
    // truth-table variable order must not exchange these physical delays.
    for (int half : {0, 1}) {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        Context ctx(args);
        auto *cell = ctx.createCell(ctx.id("lut6"), id_MISTRAL_ALUT6);
        for (IdString pin : {id_A, id_B, id_C, id_D, id_E, id_F}) {
            cell->addInput(pin);
            cell->connectPort(pin, ctx.createNet(pin));
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(id_Q));
        ctx.assignArchInfo();
        ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, half)), cell, STRENGTH_STRONG);
        ctx.lab_pre_route();
        EXPECT_EQ(cell->pin_data.at(id_E).bel_pins, std::vector<IdString>{half ? id_E1 : id_E0});
        EXPECT_EQ(cell->pin_data.at(id_F).bel_pins, std::vector<IdString>{half ? id_F1 : id_F0});
        DelayQuad slow, fast;
        ASSERT_TRUE(ctx.getCellDelay(cell, id_E, id_Q, slow));
        ASSERT_TRUE(ctx.getCellDelay(cell, id_F, id_Q, fast));
        // Compare the characterized LUT mux levels, excluding the separate
        // block-input estimate (whose conservative early bound is zero).
        slow -= ctx.get_lut_input_delay(cell, id_E);
        fast -= ctx.get_lut_input_delay(cell, id_F);
        EXPECT_LT(fast.maxRiseDelay(), slow.minRiseDelay());
        EXPECT_LT(fast.maxFallDelay(), slow.minFallDelay());
    }
}

TEST(LutPlacementTiming, IsolatedCellEstimatesMatchBothPhysicalHalves)
{
    const std::array<IdString, 6> types{id_MISTRAL_BUF,   id_MISTRAL_ALUT2, id_MISTRAL_ALUT3,
                                        id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
    const std::array<IdString, 6> inputs{id_A, id_B, id_C, id_D, id_E, id_F};
    // The isolated estimate must agree with legalisation. The default agrees
    // on every pin legalisation fixes: all LUT6 inputs and the last L5 input.
    // The other L5 inputs keep the fastest-first scaffold order. Arrival
    // permutation is a separate option and does not select either estimate.
    const std::array<IdString, 5> scaffold_rest{id_E0, id_D, id_C, id_B, id_A};
    for (int run = 0; run < 4; ++run) {
        const int half = run & 1;
        const bool isolated = run >= 2;
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        Context ctx(args);
        ctx.isolated_lut_placement = isolated;
        std::array<CellInfo *, 6> cells;
        std::array<std::array<DelayQuad, 6>, 6> hypothetical, unplaced, placed;
        for (int width = 1; width <= 6; ++width) {
            auto *cell = ctx.createCell(ctx.idf("lut%d", width), types[width - 1]);
            cells[width - 1] = cell;
            for (int input = 0; input < width; ++input)
                cell->addInput(inputs[input]);
            cell->addOutput(id_Q);
            // Remap planning also scores future LUTs before nets exist.
            for (int input = 0; input < width; ++input)
                ASSERT_TRUE(ctx.getCellDelay(cell, inputs[input], id_Q, hypothetical[width - 1][input]));
            for (int input = 0; input < width; ++input)
                cell->connectPort(inputs[input], ctx.createNet(ctx.idf("input%d_%d", width, input)));
            cell->connectPort(id_Q, ctx.createNet(ctx.idf("output%d", width)));
        }
        ctx.assignArchInfo();
        for (int width = 1; width <= 6; ++width) {
            auto *cell = cells[width - 1];
            for (int input = 0; input < width; ++input)
                ASSERT_TRUE(ctx.getCellDelay(cell, inputs[input], id_Q, unplaced[width - 1][input]));
            ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 6 * (width - 1) + half)), cell, STRENGTH_STRONG);
            for (int input = 0; input < width; ++input)
                ASSERT_TRUE(ctx.getCellDelay(cell, inputs[input], id_Q, placed[width - 1][input]));
        }
        ctx.lab_pre_route();
        for (int width = 1; width <= 6; ++width) {
            for (int input = 0; input < width; ++input) {
                SCOPED_TRACE(std::to_string(half) + "/" + std::to_string(width) + "/" + std::to_string(input));
                DelayQuad actual;
                ASSERT_TRUE(ctx.getCellDelay(cells[width - 1], inputs[input], id_Q, actual));
                if (!isolated && width < 6 && input != width - 1) {
                    ASSERT_TRUE(ctx.get_lut_pin_delay(scaffold_rest[input], false, actual));
                    actual += ctx.get_lut_input_delay(cells[width - 1], inputs[input]);
                }
                for (const auto &estimate : {hypothetical[width - 1][input], unplaced[width - 1][input],
                                             placed[width - 1][input]}) {
                    EXPECT_EQ(estimate.minRiseDelay(), actual.minRiseDelay());
                    EXPECT_EQ(estimate.maxRiseDelay(), actual.maxRiseDelay());
                    EXPECT_EQ(estimate.minFallDelay(), actual.minFallDelay());
                    EXPECT_EQ(estimate.maxFallDelay(), actual.maxFallDelay());
                }
            }
        }
    }
}

TEST(LutPlacementTiming, DefaultEstimateGivesTheLastInputF)
{
    // Yosys times the last ALUT input fastest, so abc9 gives it the critical
    // signal, and legalisation gives it F. Before legalisation, placement
    // times it on F too, and a LUT6 on its only legal order. The other L5
    // inputs keep the #206 fastest-first scaffold order on E, C/D, B.
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    const std::array<IdString, 6> inputs{id_A, id_B, id_C, id_D, id_E, id_F};
    const std::array<IdString, 5> l5{id_E0, id_D, id_C, id_B, id_F0};
    const std::array<IdString, 6> l6{id_A, id_B, id_C, id_D, id_E0, id_F0};
    for (int width : {5, 6}) {
        for (int half : {0, 1}) {
            auto *cell = ctx.createCell(ctx.idf("lut%d_%d", width, half),
                                        width == 6 ? id_MISTRAL_ALUT6 : id_MISTRAL_ALUT5);
            for (int i = 0; i < width; ++i) {
                cell->addInput(inputs[i]);
                cell->connectPort(inputs[i], ctx.createNet(ctx.idf("%s_%d_%d", ctx.nameOf(inputs[i]), width, half)));
            }
            cell->addOutput(id_Q);
            cell->connectPort(id_Q, ctx.createNet(ctx.idf("output_%d_%d", width, half)));
            ctx.assignArchInfo();
            ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 6 * (2 * (width - 5) + half) + half)),
                        cell, STRENGTH_STRONG);
            for (int i = 0; i < width; ++i) {
                const IdString expected = width == 6 ? l6[i] : l5[i];
                EXPECT_EQ(cell->pin_data.at(inputs[i]).bel_pins, std::vector<IdString>{expected});
                DelayQuad estimate, reference;
                ASSERT_TRUE(ctx.getCellDelay(cell, inputs[i], id_Q, estimate));
                ASSERT_TRUE(ctx.get_lut_pin_delay(expected, width == 6, reference));
                reference += ctx.get_lut_input_delay(cell, inputs[i]);
                EXPECT_EQ(estimate.maxDelay(), reference.maxDelay());
            }
        }
    }
    ctx.lab_pre_route();
    for (int width : {5, 6}) {
        for (int half : {0, 1}) {
            auto *cell = ctx.cells.at(ctx.idf("lut%d_%d", width, half)).get();
            const auto last = width == 6 ? id_F : id_E;
            EXPECT_EQ(cell->pin_data.at(id_A).bel_pins, std::vector<IdString>{id_A});
            EXPECT_EQ(cell->pin_data.at(last).bel_pins, std::vector<IdString>{half ? id_F1 : id_F0});
            DelayQuad slow, fast;
            ASSERT_TRUE(ctx.getCellDelay(cell, id_A, id_Q, slow));
            ASSERT_TRUE(ctx.getCellDelay(cell, last, id_Q, fast));
            EXPECT_LT(fast.maxDelay(), slow.minDelay());
        }
    }
}

TEST(LutPlacementTiming, IsolatedSparseInputsMatchLegalisation)
{
    for (bool isolated : {false, true}) {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        Context ctx(args);
        ctx.isolated_lut_placement = isolated;
        for (int width : {5, 6}) {
            auto *cell = ctx.createCell(ctx.idf("sparse%d", width), width == 5 ? id_MISTRAL_ALUT5 : id_MISTRAL_ALUT6);
            for (IdString input : {id_A, id_B, id_C, id_D, id_E})
                cell->addInput(input);
            if (width == 6)
                cell->addInput(id_F);
            // B/D are deliberately disconnected before the connected E input.
            for (IdString input : {id_A, id_C, id_E})
                cell->connectPort(input, ctx.createNet(ctx.idf("%s_%d", ctx.nameOf(input), width)));
            cell->addOutput(id_Q);
            cell->connectPort(id_Q, ctx.createNet(ctx.idf("output%d", width)));
        }
        ctx.assignArchInfo();
        std::array<std::array<DelayQuad, 3>, 2> estimates;
        const std::array<IdString, 3> connected{id_A, id_C, id_E};
        for (int width : {5, 6}) {
            auto *cell = ctx.cells.at(ctx.idf("sparse%d", width)).get();
            for (int i = 0; i < 3; ++i)
                ASSERT_TRUE(ctx.getCellDelay(cell, connected[i], id_Q, estimates[width - 5][i]));
            ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 6 * (width - 5))), cell, STRENGTH_STRONG);
        }
        ctx.lab_pre_route();
        for (int width : {5, 6}) {
            auto *cell = ctx.cells.at(ctx.idf("sparse%d", width)).get();
            for (int i = 0; i < 3; ++i) {
                DelayQuad actual;
                ASSERT_TRUE(ctx.getCellDelay(cell, connected[i], id_Q, actual));
                // By default the L5 inputs before the last connected one keep
                // the scaffold order, counted over connected inputs only.
                if (!isolated && width == 5 && i < 2) {
                    ASSERT_TRUE(ctx.get_lut_pin_delay(i == 0 ? id_E0 : id_D, false, actual));
                    actual += ctx.get_lut_input_delay(cell, connected[i]);
                }
                EXPECT_EQ(estimates[width - 5][i].maxDelay(), actual.maxDelay());
                EXPECT_EQ(estimates[width - 5][i].minDelay(), actual.minDelay());
            }
        }
    }
}

TEST(LutPlacementTiming, SharedAndPrivateL5PinsRemainLegal)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    const std::array<IdString, 5> ports{id_A, id_B, id_C, id_D, id_E};
    auto *shared_a = ctx.createNet(ctx.id("shared_a"));
    auto *shared_b = ctx.createNet(ctx.id("shared_b"));
    std::array<CellInfo *, 2> luts{};
    for (int half = 0; half < 2; ++half) {
        auto *cell = ctx.createCell(ctx.idf("lut%d", half), id_MISTRAL_ALUT5);
        luts[half] = cell;
        for (IdString port : ports) {
            cell->addInput(port);
            NetInfo *net = port == id_A ? shared_a : port == id_B ? shared_b
                                                                  : ctx.createNet(ctx.idf("private%d_%s", half, ctx.nameOf(port)));
            cell->connectPort(port, net);
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("output%d", half)));
    }
    ctx.assignArchInfo();
    for (int half = 0; half < 2; ++half)
        ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, half)), luts[half], STRENGTH_STRONG);
    ctx.lab_pre_route();

    std::map<IdString, NetInfo *> pin_nets;
    std::map<NetInfo *, IdString> net_pins;
    for (CellInfo *cell : luts) {
        for (IdString port : ports) {
            auto *net = cell->getPort(port);
            const auto &mapped = cell->pin_data.at(port).bel_pins;
            ASSERT_EQ(mapped.size(), 1);
            const IdString physical = mapped.front();
            EXPECT_TRUE(physical.in(id_A, id_B, id_C, id_D, id_E0, id_E1, id_F0, id_F1));
            auto pin = pin_nets.emplace(physical, net);
            EXPECT_TRUE(pin.second || pin.first->second == net);
            auto connection = net_pins.emplace(net, physical);
            EXPECT_TRUE(connection.second || connection.first->second == physical);
        }
    }
    EXPECT_EQ(luts[0]->pin_data.at(id_A).bel_pins, std::vector<IdString>{id_A});
    EXPECT_EQ(luts[1]->pin_data.at(id_A).bel_pins, std::vector<IdString>{id_A});
    EXPECT_EQ(luts[0]->pin_data.at(id_B).bel_pins, std::vector<IdString>{id_B});
    EXPECT_EQ(luts[1]->pin_data.at(id_B).bel_pins, std::vector<IdString>{id_B});
}

TEST(LutPlacementTiming, PinOptionsLeaveThePlacementEstimate)
{
    // Arrival permutes private pins after legalisation and leaves placement
    // scoring alone. --isolated-lut-placement only changes the L5 inputs
    // before the last one: both estimates time the last input on F.
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context plain(args), arrival_only(args), isolated(args);
    arrival_only.arrival_pin_assignment = true;
    isolated.isolated_lut_placement = true;
    const std::array<IdString, 5> inputs{id_A, id_B, id_C, id_D, id_E};
    auto make = [](Context &ctx, const char *name) {
        auto *cell = ctx.createCell(ctx.id(name), id_MISTRAL_ALUT5);
        for (IdString pin : {id_A, id_B, id_C, id_D, id_E}) {
            cell->addInput(pin);
            cell->connectPort(pin, ctx.createNet(ctx.idf("%s_%s", name, ctx.nameOf(pin))));
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", name)));
        ctx.assignArchInfo();
        return cell;
    };
    auto *base = make(plain, "plain");
    auto *arrived = make(arrival_only, "arrival");
    auto *alone = make(isolated, "isolated");
    for (IdString pin : inputs) {
        DelayQuad base_delay, arrival_delay;
        ASSERT_TRUE(plain.getCellDelay(base, pin, id_Q, base_delay));
        ASSERT_TRUE(arrival_only.getCellDelay(arrived, pin, id_Q, arrival_delay));
        EXPECT_EQ(base->pin_data.at(pin).bel_pins, arrived->pin_data.at(pin).bel_pins);
        EXPECT_EQ(base_delay.maxDelay(), arrival_delay.maxDelay());
        EXPECT_EQ(base_delay.minDelay(), arrival_delay.minDelay());
    }
    EXPECT_EQ(base->pin_data.at(id_A).bel_pins, std::vector<IdString>{id_E0});
    EXPECT_EQ(base->pin_data.at(id_B).bel_pins, std::vector<IdString>{id_D});
    EXPECT_EQ(base->pin_data.at(id_C).bel_pins, std::vector<IdString>{id_C});
    EXPECT_EQ(base->pin_data.at(id_D).bel_pins, std::vector<IdString>{id_B});
    EXPECT_EQ(base->pin_data.at(id_E).bel_pins, std::vector<IdString>{id_F0});
    EXPECT_EQ(alone->pin_data.at(id_A).bel_pins, std::vector<IdString>{id_A});
    EXPECT_EQ(alone->pin_data.at(id_B).bel_pins, std::vector<IdString>{id_B});
    EXPECT_EQ(alone->pin_data.at(id_C).bel_pins, std::vector<IdString>{id_C});
    EXPECT_EQ(alone->pin_data.at(id_D).bel_pins, std::vector<IdString>{id_E0});
    EXPECT_EQ(alone->pin_data.at(id_E).bel_pins, std::vector<IdString>{id_F0});
    // The last input is timed on F either way; the first input is not.
    DelayQuad base_e, isolated_e, base_a, isolated_a;
    ASSERT_TRUE(plain.getCellDelay(base, id_E, id_Q, base_e));
    ASSERT_TRUE(isolated.getCellDelay(alone, id_E, id_Q, isolated_e));
    ASSERT_TRUE(plain.getCellDelay(base, id_A, id_Q, base_a));
    ASSERT_TRUE(isolated.getCellDelay(alone, id_A, id_Q, isolated_a));
    EXPECT_EQ(base_e.maxDelay(), isolated_e.maxDelay());
    EXPECT_LT(base_a.maxDelay(), isolated_a.maxDelay());
}

TEST(LutPlacementTiming, SharedLastInputKeepsAFreePrivateFastPin)
{
    // abc9 gives a LUT's last input the critical signal. When that net is
    // also an input of the other half, the pair shares it on A/B. The half
    // whose last input it is keeps it on its own F when it has a free
    // private pin, and the other half still reads it on A. A half with no
    // free private pin keeps the shared pin.
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    const std::array<IdString, 5> inputs{id_A, id_B, id_C, id_D, id_E};
    auto make = [&](const char *name, IdString type, const std::vector<NetInfo *> &nets, uint32_t lut) {
        auto *cell = ctx.createCell(ctx.id(name), type);
        for (size_t k = 0; k < nets.size(); ++k) {
            cell->addInput(inputs[k]);
            cell->connectPort(inputs[k], nets[k]);
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", name)));
        cell->params[id_LUT] = Property(lut, 1 << nets.size());
        return cell;
    };
    auto net = [&](const char *name) { return ctx.createNet(ctx.id(name)); };
    auto *shared = net("shared");
    auto *roomy = make("roomy", id_MISTRAL_ALUT3, {net("p0"), net("p1"), shared}, 0x96);
    auto *reader = make("reader", id_MISTRAL_ALUT3, {shared, net("q0"), net("q1")}, 0xe8);
    auto *shared_full = net("shared_full");
    auto *full = make("full", id_MISTRAL_ALUT5, {net("r0"), net("r1"), net("r2"), net("r3"), shared_full}, 0x6996c33cU);
    auto *full_reader = make("full_reader", id_MISTRAL_ALUT3, {shared_full, net("s0"), net("s1")}, 0xe8);
    ctx.assignArchInfo();
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 0)), roomy, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 1)), reader, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 6)), full, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 7)), full_reader, STRENGTH_STRONG);
    ctx.lab_pre_route();
    auto pin = [](CellInfo *cell, IdString port) { return cell->pin_data.at(port).bel_pins.at(0); };
    EXPECT_EQ(pin(roomy, id_C), id_F0);
    EXPECT_EQ(pin(roomy, id_B), id_E0);
    EXPECT_EQ(pin(roomy, id_A), id_C);
    EXPECT_EQ(pin(reader, id_A), id_A);
    EXPECT_EQ(pin(reader, id_B), id_E1);
    EXPECT_EQ(pin(reader, id_C), id_F1);
    // Four private inputs fill F, E and C, so the shared last input stays on A.
    EXPECT_EQ(pin(full, id_E), id_A);
    EXPECT_EQ(pin(full_reader, id_A), id_A);
    DelayQuad fast, slow;
    ASSERT_TRUE(ctx.getCellDelay(roomy, id_C, id_Q, fast));
    ASSERT_TRUE(ctx.getCellDelay(full, id_E, id_Q, slow));
    EXPECT_LT(fast.maxDelay(), slow.minDelay());
    // Each half's mask implements its own function, and the half that left
    // A to the other one does not depend on it.
    const uint32_t lab = ctx.bel_data(roomy->bel).lab_data.lab;
    const std::array<std::array<CellInfo *, 2>, 2> alms{{{roomy, reader}, {full, full_reader}}};
    for (int alm = 0; alm < 2; ++alm) {
        const uint64_t mask = ctx.compute_lut_mask(lab, alm);
        for (int half = 0; half < 2; ++half) {
            CellInfo *cell = alms[alm][half];
            const int width = int(cell->ports.size()) - 1;
            bool uses_a = false;
            for (int k = 0; k < width; ++k)
                uses_a |= pin(cell, inputs[k]) == id_A;
            for (int bits = 0; bits < (1 << width); ++bits) {
                int physical_bits = 0;
                for (int k = 0; k < width; ++k) {
                    const auto &mapping = cell->pin_data.at(inputs[k]);
                    IdString phys = mapping.bel_pins.at(0);
                    int position = phys == id_A ? 0 : phys == id_B ? 1 : phys.in(id_C, id_D) ? 2 : phys.in(id_E0, id_E1) ? 3 : 4;
                    bool value = bool((bits >> k) & 1) ^ (mapping.state != PIN_INV);
                    physical_bits |= int(value) << position;
                }
                const bool expected = (cell->params.at(id_LUT).as_int64() >> bits) & 1;
                SCOPED_TRACE(std::string(ctx.nameOf(cell)) + "/" + std::to_string(bits));
                EXPECT_EQ(bool((mask >> (physical_bits + 32 * half)) & 1), !expected);
                if (!uses_a)
                    EXPECT_EQ(bool((mask >> ((physical_bits ^ 1) + 32 * half)) & 1), !expected);
            }
        }
    }
}

TEST(LutInputArrival, PrivateLateInputGetsFastPinAndSharedInputsStayFixed)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    ctx.arrival_pin_assignment = true;
    const std::array<IdString, 5> inputs{id_A, id_B, id_C, id_D, id_E};
    std::array<CellInfo *, 2> cells;
    auto shared = ctx.createNet(ctx.id("shared"));
    auto shared_b = ctx.createNet(ctx.id("shared_b"));
    for (int half = 0; half < 2; ++half) {
        auto cell = ctx.createCell(ctx.idf("lut%d", half), id_MISTRAL_ALUT5);
        cells[half] = cell;
        for (IdString pin : inputs) {
            cell->addInput(pin);
            cell->connectPort(pin, pin == id_A ? shared : pin == id_B ? shared_b : ctx.createNet(ctx.idf("input%d_%s", half, ctx.nameOf(pin))));
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("output%d", half)));
        cell->params[id_LUT] = Property(half ? 0x695ac3f0U : 0xc6935a0fU, 32);
        cell->pin_data[id_D].state = PIN_INV;
    }
    ctx.assignArchInfo();
    for (int half = 0; half < 2; ++half)
        ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, half)), cells[half], STRENGTH_STRONG);
    ctx.lab_pre_route();
    const uint32_t lab = ctx.bel_data(cells[0]->bel).lab_data.lab;
    dict<IdString, delay_t> arrival{{id_A, 10000}, {id_B, 100}, {id_C, 200}, {id_D, 4000}, {id_E, 300}};
    auto shared_pin = cells[0]->pin_data.at(id_A).bel_pins;
    auto mate_shared_pin = cells[1]->pin_data.at(id_A).bel_pins;
    auto check_function = [&]() {
        uint64_t mask = ctx.compute_lut_mask(lab, 0);
        for (int half = 0; half < 2; ++half)
            for (int bits = 0; bits < 32; ++bits) {
                int physical_bits = 0;
                for (int k = 0; k < 5; ++k) {
                    const auto &mapping = cells[half]->pin_data.at(inputs[k]);
                    IdString pin = mapping.bel_pins.at(0);
                    int position = pin == id_A ? 0 : pin == id_B ? 1 : pin.in(id_C, id_D) ? 2 : pin.in(id_E0, id_E1) ? 3 : 4;
                    bool value = bool((bits >> k) & 1) ^ (mapping.state != PIN_INV);
                    physical_bits |= int(value) << position;
                }
                bool expected = (cells[half]->params.at(id_LUT).as_int64() >> bits) & 1;
                EXPECT_EQ(bool((mask >> (physical_bits + 32 * half)) & 1), !expected);
            }
    };
    check_function();
    EXPECT_GT(ctx.optimise_private_lut_pins(lab, 0, 0, arrival), 0);
    EXPECT_EQ(cells[0]->pin_data.at(id_A).bel_pins, shared_pin);
    EXPECT_EQ(cells[1]->pin_data.at(id_A).bel_pins, mate_shared_pin);
    check_function();
    EXPECT_EQ(cells[0]->pin_data.at(id_D).bel_pins, std::vector<IdString>{id_F0});
    EXPECT_GT(ctx.optimise_private_lut_pins(lab, 0, 1, arrival), 0);
    EXPECT_EQ(cells[1]->pin_data.at(id_D).bel_pins, std::vector<IdString>{id_F1});
    EXPECT_EQ(cells[1]->pin_data.at(id_A).bel_pins, mate_shared_pin);
    check_function();
    auto ff = ctx.createCell(ctx.id("direct_input_ff"), id_MISTRAL_FF);
    ff->addInput(id_DATAIN);
    ff->addOutput(id_Q);
    ff->connectPort(id_DATAIN, ctx.createNet(ctx.id("direct_input")));
    ctx.createNet(ctx.id("$PACKER_VCC_NET"));
    ctx.assign_ff_info(ff);
    auto ff_bel = ctx.getBelByLocation(Loc(24, 1, 2));
    ctx.bindBel(ff_bel, ff, STRENGTH_STRONG);
    arrival[id_E] = 20000;
    EXPECT_EQ(ctx.optimise_private_lut_pins(lab, 0, 0, arrival), 0);
    ctx.unbindBel(ff_bel);
    EXPECT_GT(ctx.optimise_private_lut_pins(lab, 0, 0, arrival), 0);
    check_function();
    ctx.unbindBel(cells[0]->bel);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 0)), cells[0], STRENGTH_LOCKED);
    EXPECT_EQ(ctx.optimise_private_lut_pins(lab, 0, 0, arrival), 0);
}

TEST(LutInputArrival, HalfCycleDeadlineTakesPriorityOverLaterFullCycleArrival)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    ctx.settings[ctx.id("target_freq")] = 1e8;
    auto *gnd = ctx.createNet(ctx.id("$PACKER_GND_NET"));
    ctx.createNet(ctx.id("$PACKER_VCC_NET"));
    auto *clock = ctx.createNet(ctx.id("clock"));
    clock->is_global = true;
    clock->clkconstr = std::make_unique<ClockConstraint>();
    clock->clkconstr->period = DelayPair(10000);
    clock->clkconstr->high = clock->clkconstr->low = DelayPair(5000);
    auto ff = [&](const char *name, int y, int z, bool falling) {
        auto *cell = ctx.createCell(ctx.id(name), id_MISTRAL_FF);
        for (IdString pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clock);
        cell->connectPort(id_DATAIN, gnd);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s$q", name)));
        cell->pin_data[id_CLK].state = falling ? PIN_INV : PIN_SIG;
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        ctx.assign_ff_info(cell);
        ctx.bindBel(ctx.getBelByLocation(Loc(24, y, z)), cell, STRENGTH_WEAK);
        return cell;
    };
    auto *half = ff("half", 3, 2, true);
    auto *full = ff("full", 2, 2, false);
    auto *capture = ff("capture", 1, 8, false);
    auto lut = [&](const char *name, IdString type, int y, NetInfo *a, NetInfo *b) {
        auto *cell = ctx.createCell(ctx.id(name), type);
        cell->addInput(id_A);
        cell->connectPort(id_A, a);
        if (b) {
            cell->addInput(id_B);
            cell->connectPort(id_B, b);
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s$q", name)));
        cell->params[id_LUT] = Property(b ? 8 : 2, b ? 4 : 2);
        ctx.assign_comb_info(cell);
        ctx.bindBel(ctx.getBelByLocation(Loc(24, y, 0)), cell, STRENGTH_WEAK);
        return cell;
    };
    auto *buffer = lut("full_buffer", id_MISTRAL_BUF, 2, full->getPort(id_Q), nullptr);
    auto *root = lut("root", id_MISTRAL_ALUT2, 1, half->getPort(id_Q), buffer->getPort(id_Q));
    capture->disconnectPort(id_DATAIN);
    capture->connectPort(id_DATAIN, root->getPort(id_Q));
    ctx.assignArchInfo();
    ctx.lab_pre_route();
    EXPECT_EQ(root->pin_data.at(id_B).bel_pins, std::vector<IdString>{id_F0});
    TimingAnalyser before(&ctx);
    before.setup();
    delay_t half_arrival, full_arrival;
    ASSERT_TRUE(before.get_max_arrival({root->name, id_A}, half_arrival));
    ASSERT_TRUE(before.get_max_arrival({root->name, id_B}, full_arrival));
    ASSERT_GT(full_arrival, half_arrival);
    ASSERT_LT(before.get_setup_slack({root->name, id_A}), before.get_setup_slack({root->name, id_B}));
    ctx.arrival_pin_assignment = true;
    ctx.lab_pre_route();
    EXPECT_EQ(root->pin_data.at(id_A).bel_pins, std::vector<IdString>{id_F0});
    EXPECT_EQ(root->pin_data.at(id_B).bel_pins, std::vector<IdString>{id_E0});
    TimingAnalyser after(&ctx);
    after.setup();
    EXPECT_GT(after.get_setup_slack({capture->name, id_DATAIN}), before.get_setup_slack({capture->name, id_DATAIN}));
}

TEST(LutPlacementTiming, DirectDataFlipFlopDoesNotShareItsFabricPin)
{
    // sel_ef of a half is fed by that half's E and the other half's F.
    // The private pool starts at F, so a direct-data flip-flop keeps E when
    // the LUT can spare it. A 5-input LUT cannot, and the other half leaves F
    // free. The last private input takes the fastest pin left in the pool.
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    ctx.createNet(ctx.id("$PACKER_GND_NET"));
    ctx.createNet(ctx.id("$PACKER_VCC_NET"));
    auto *clock = ctx.createNet(ctx.id("clock"));
    clock->is_global = true;
    const std::array<IdString, 5> pins{id_A, id_B, id_C, id_D, id_E};
    const std::array<IdString, 5> types{id_MISTRAL_BUF, id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                        id_MISTRAL_ALUT5};
    auto make_lut = [&](const char *name, int width) {
        auto *cell = ctx.createCell(ctx.id(name), types.at(width - 1));
        for (int i = 0; i < width; ++i) {
            cell->addInput(pins.at(i));
            cell->connectPort(pins.at(i), ctx.createNet(ctx.idf("%s_%s", name, ctx.nameOf(pins.at(i)))));
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", name)));
        return cell;
    };
    auto make_ff = [&](const char *name) {
        auto *cell = ctx.createCell(ctx.id(name), id_MISTRAL_FF);
        for (IdString pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clock);
        cell->connectPort(id_DATAIN, ctx.createNet(ctx.idf("%s_d", name)));
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", name)));
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        return cell;
    };
    auto *wide = make_lut("wide", 4);
    auto *mate = make_lut("mate", 2);
    auto *direct = make_ff("direct");
    auto *small = make_lut("small", 2);
    auto *full = make_lut("full", 5);
    auto *full_ff = make_ff("full_ff");
    auto *chain = make_lut("chain", 5);
    auto *chain_ff = make_ff("chain_ff");
    auto *thru = make_ff("thru");
    ctx.assignArchInfo();
    // ALM n starts at z = n*6: two COMBs, then FF0, FF1, FF2, FF3.
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 0)), wide, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 1)), mate, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 2)), direct, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 6)), small, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 7)), full, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 10)), full_ff, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 12)), chain, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 14)), chain_ff, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 16)), thru, STRENGTH_STRONG);
    ctx.lab_pre_route();
    auto pin = [](CellInfo *cell, IdString port) { return cell->pin_data.at(port).bel_pins.at(0); };
    // Four inputs fit on F, C, B and A, leaving E0 for the flip-flop. The
    // last input gets F.
    EXPECT_EQ(pin(wide, id_A), id_A);
    EXPECT_EQ(pin(wide, id_B), id_B);
    EXPECT_EQ(pin(wide, id_C), id_C);
    EXPECT_EQ(pin(wide, id_D), id_F0);
    EXPECT_EQ(pin(mate, id_A), id_E1);
    EXPECT_EQ(pin(mate, id_B), id_F1);
    // Five inputs need E. The other LUT leaves F0 for the flip-flop.
    EXPECT_EQ(pin(small, id_A), id_C);
    EXPECT_EQ(pin(small, id_B), id_E0);
    EXPECT_EQ(pin(full, id_A), id_A);
    EXPECT_EQ(pin(full, id_B), id_B);
    EXPECT_EQ(pin(full, id_C), id_D);
    EXPECT_EQ(pin(full, id_D), id_E1);
    EXPECT_EQ(pin(full, id_E), id_F1);
    // A route-through on the other half would take F, so it uses E instead.
    EXPECT_EQ(pin(chain, id_A), id_A);
    EXPECT_EQ(pin(chain, id_B), id_B);
    EXPECT_EQ(pin(chain, id_C), id_C);
    EXPECT_EQ(pin(chain, id_D), id_E0);
    EXPECT_EQ(pin(chain, id_E), id_F0);
    auto *buffer = ctx.cells.at(ctx.id("thru$ROUTETHRU")).get();
    EXPECT_EQ(buffer->type, id_MISTRAL_BUF);
    EXPECT_EQ(buffer->pin_data.at(id_A).bel_pins, std::vector<IdString>{id_E1});
    EXPECT_EQ(buffer->getPort(id_Q), thru->getPort(id_DATAIN));
}

TEST(LutPlacementTiming, SynchronousLoadKeepsASelectorInput)
{
    // sel_ef of a half is fed by that half's E and the other half's F.
    // Legality accepts SDATA when the other LUT has at most two inputs,
    // which used to leave that F free. The private pool takes F first, so the
    // load has to keep E, or the other LUT has to leave F free.
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    ctx.createNet(ctx.id("$PACKER_GND_NET"));
    ctx.createNet(ctx.id("$PACKER_VCC_NET"));
    auto *clock = ctx.createNet(ctx.id("clock"));
    clock->is_global = true;
    const std::array<IdString, 5> pins{id_A, id_B, id_C, id_D, id_E};
    const std::array<IdString, 5> types{id_MISTRAL_BUF, id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                        id_MISTRAL_ALUT5};
    auto make_lut = [&](const char *name, int width) {
        auto *cell = ctx.createCell(ctx.id(name), types.at(width - 1));
        for (int i = 0; i < width; ++i) {
            cell->addInput(pins.at(i));
            cell->connectPort(pins.at(i), ctx.createNet(ctx.idf("%s_%s", name, ctx.nameOf(pins.at(i)))));
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", name)));
        return cell;
    };
    auto make_load = [&](const char *name, NetInfo *datain) {
        auto *cell = ctx.createCell(ctx.id(name), id_MISTRAL_FF);
        for (IdString pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clock);
        cell->connectPort(id_DATAIN, datain);
        cell->connectPort(id_SLOAD, ctx.createNet(ctx.idf("%s_sload", name)));
        cell->connectPort(id_SDATA, ctx.createNet(ctx.idf("%s_sdata", name)));
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", name)));
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        return cell;
    };
    auto uses = [](CellInfo *cell, IdString pin) {
        for (const auto &port : cell->pin_data)
            for (IdString physical : port.second.bel_pins)
                if (physical == pin)
                    return true;
        return false;
    };
    auto *top = make_lut("top", 2);
    auto *bottom = make_lut("bottom", 2);
    auto *load = make_load("load", top->getPort(id_Q));
    auto *full = make_lut("full", 5);
    auto *mate = make_lut("mate", 2);
    auto *full_load = make_load("full_load", full->getPort(id_Q));
    ctx.assignArchInfo();
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 0)), top, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 1)), bottom, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 2)), load, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 6)), full, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 7)), mate, STRENGTH_STRONG);
    ctx.bindBel(ctx.getBelByLocation(Loc(24, 1, 8)), full_load, STRENGTH_STRONG);
    const auto top_lab = ctx.bel_data(top->bel).lab_data;
    const auto full_lab = ctx.bel_data(full->bel).lab_data;
    EXPECT_TRUE(ctx.is_alm_legal(top_lab.lab, top_lab.alm));
    EXPECT_TRUE(ctx.is_alm_legal(full_lab.lab, full_lab.alm));
    ctx.lab_pre_route();
    auto pin = [](CellInfo *cell, IdString port) { return cell->pin_data.at(port).bel_pins.at(0); };
    // Two inputs fit on F and C, leaving E0 for SDATA. The other LUT still
    // uses F1, and each LUT gives F to its last input.
    EXPECT_EQ(pin(top, id_A), id_C);
    EXPECT_EQ(pin(top, id_B), id_F0);
    EXPECT_FALSE(uses(top, id_E0));
    EXPECT_EQ(pin(bottom, id_A), id_E1);
    EXPECT_EQ(pin(bottom, id_B), id_F1);
    // Five inputs need E0, so the other LUT leaves F1 for SDATA.
    EXPECT_TRUE(uses(full, id_E0));
    EXPECT_FALSE(uses(mate, id_F1));
    EXPECT_EQ(pin(mate, id_A), id_D);
    EXPECT_EQ(pin(mate, id_B), id_E1);
}

TEST(HardBlockPinmapTest, UnconnectedBusOutputMapsToNoPin)
{
    // fes_hps_ddr writes .wrack_data_0(), which yosys emits as an empty
    // connection and the loader keeps as a disconnected scalar port. The
    // fpga2sdram BEL has only wrack_data_0[0..9].
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    auto ctx = std::make_unique<Context>(args);
    auto cell = ctx->createCell(ctx->id("f2sdram"), id_cyclonev_hps_interface_fpga2sdram);
    cell->addOutput(ctx->id("wrack_data_0"));
    cell->addOutput(ctx->id("cmd_ready_3"));
    cell->addInput(ctx->id("wrack_ready_0"));
    cell->connectPort(ctx->id("wrack_ready_0"), ctx->createNet(ctx->id("ready")));
    ctx->assign_default_pinmap(cell);
    EXPECT_TRUE(cell->pin_data.at(ctx->id("wrack_data_0")).bel_pins.empty());
    // Disconnected scalar and connected pins keep their same-named BEL pin.
    EXPECT_EQ(cell->pin_data.at(ctx->id("cmd_ready_3")).bel_pins, std::vector<IdString>{ctx->id("cmd_ready_3")});
    EXPECT_EQ(cell->pin_data.at(ctx->id("wrack_ready_0")).bel_pins, std::vector<IdString>{ctx->id("wrack_ready_0")});
}
