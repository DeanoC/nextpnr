#include <array>
#include <map>
#include <memory>

#include "gtest/gtest.h"
#include "nextpnr.h"

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
        EXPECT_LT(fast.maxRiseDelay(), slow.minRiseDelay());
        EXPECT_LT(fast.maxFallDelay(), slow.minFallDelay());
    }
}

TEST(LutPlacementTiming, IsolatedCellEstimatesMatchBothPhysicalHalves)
{
    const std::array<IdString, 6> types{id_MISTRAL_BUF,   id_MISTRAL_ALUT2, id_MISTRAL_ALUT3,
                                        id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
    const std::array<IdString, 6> inputs{id_A, id_B, id_C, id_D, id_E, id_F};
    // The isolated estimate must agree with legalisation. Arrival permutation
    // is a separate option and does not select this estimate.
    for (int half : {0, 1}) {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        Context ctx(args);
        ctx.isolated_lut_placement = true;
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

TEST(LutPlacementTiming, DefaultRetainsPlacementPolicyUntilPinsAreLegalised)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    const std::array<IdString, 6> inputs{id_A, id_B, id_C, id_D, id_E, id_F};
    const std::array<IdString, 6> expected{id_F0, id_E0, id_D, id_C, id_B, id_A};
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
                EXPECT_EQ(cell->pin_data.at(inputs[i]).bel_pins, std::vector<IdString>{expected[i]});
                DelayQuad estimate, reference;
                ASSERT_TRUE(ctx.getCellDelay(cell, inputs[i], id_Q, estimate));
                ASSERT_TRUE(ctx.get_lut_pin_delay(expected[i], width == 6, reference));
                EXPECT_EQ(estimate.maxDelay(), reference.maxDelay());
            }
        }
    }
    ctx.lab_pre_route();
    // Retaining placement policy must not change physical timing reporting.
    for (int width : {5, 6}) {
        for (int half : {0, 1}) {
            auto *cell = ctx.cells.at(ctx.idf("lut%d_%d", width, half)).get();
            // Width 6 stays in logical order, so F is the fast tail. Width 5
            // retains the legal C/D, E, F, B, A pool, so the third input lands on F.
            const auto fast_port = width == 6 ? id_F : id_C;
            const auto slow_port = width == 6 ? id_A : id_E;
            EXPECT_EQ(cell->pin_data.at(id_A).bel_pins,
                      std::vector<IdString>{width == 6 ? id_A : half ? id_D : id_C});
            EXPECT_EQ(cell->pin_data.at(fast_port).bel_pins, std::vector<IdString>{half ? id_F1 : id_F0});
            if (width == 5) {
                EXPECT_EQ(cell->pin_data.at(id_E).bel_pins, std::vector<IdString>{id_A});
            }
            DelayQuad slow, fast;
            ASSERT_TRUE(ctx.getCellDelay(cell, slow_port, id_Q, slow));
            ASSERT_TRUE(ctx.getCellDelay(cell, fast_port, id_Q, fast));
            EXPECT_LT(fast.maxDelay(), slow.minDelay());
        }
    }
}

TEST(LutPlacementTiming, IsolatedSparseInputsMatchLegalisation)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    ctx.isolated_lut_placement = true;
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
            EXPECT_EQ(estimates[width - 5][i].maxDelay(), actual.maxDelay());
            EXPECT_EQ(estimates[width - 5][i].minDelay(), actual.minDelay());
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

TEST(LutPlacementTiming, ArrivalLeavesThePlacementEstimateOnTheDefaultScaffold)
{
    // Arrival permutes private pins after legalisation. Placement scoring stays
    // on the fastest-first scaffold unless --isolated-lut-placement is also set.
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
    EXPECT_EQ(base->pin_data.at(id_C).bel_pins, std::vector<IdString>{id_D});
    EXPECT_EQ(alone->pin_data.at(id_C).bel_pins, std::vector<IdString>{id_F0});
    EXPECT_EQ(alone->pin_data.at(id_E).bel_pins, std::vector<IdString>{id_A});
    EXPECT_EQ(base->pin_data.at(id_E).bel_pins, std::vector<IdString>{id_B});
    // The isolated legal map moves the third input to F and the last input
    // from B to A. Placement's default scaffold remains unchanged.
    DelayQuad base_e, isolated_e;
    ASSERT_TRUE(plain.getCellDelay(base, id_E, id_Q, base_e));
    ASSERT_TRUE(isolated.getCellDelay(alone, id_E, id_Q, isolated_e));
    EXPECT_NE(base_e.maxDelay(), isolated_e.maxDelay());
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

TEST(LutPlacementTiming, DirectDataFlipFlopDoesNotShareItsFabricPin)
{
    // sel_ef of a half is fed by that half's E and the other half's F.
    // Direct-data flip-flops reserve E or the other half's F. L5 private pins
    // retain the legal pool order while leaving those selector pins free.
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
    // Four inputs fit on C, F, B and A, leaving E0 for the flip-flop.
    EXPECT_EQ(pin(wide, id_A), id_C);
    EXPECT_EQ(pin(wide, id_B), id_F0);
    EXPECT_EQ(pin(wide, id_C), id_B);
    EXPECT_EQ(pin(wide, id_D), id_A);
    EXPECT_EQ(pin(mate, id_A), id_D);
    EXPECT_EQ(pin(mate, id_B), id_E1);
    // Five inputs need E and F. The other LUT leaves F0 for the flip-flop.
    EXPECT_EQ(pin(small, id_A), id_C);
    EXPECT_EQ(pin(small, id_B), id_E0);
    EXPECT_EQ(pin(full, id_A), id_D);
    EXPECT_EQ(pin(full, id_B), id_E1);
    EXPECT_EQ(pin(full, id_C), id_F1);
    EXPECT_EQ(pin(full, id_D), id_B);
    EXPECT_EQ(pin(full, id_E), id_A);
    // A route-through on the other half uses E1, leaving the full private pool.
    EXPECT_EQ(pin(chain, id_A), id_C);
    EXPECT_EQ(pin(chain, id_B), id_E0);
    EXPECT_EQ(pin(chain, id_C), id_F0);
    EXPECT_EQ(pin(chain, id_D), id_B);
    EXPECT_EQ(pin(chain, id_E), id_A);
    auto *buffer = ctx.cells.at(ctx.id("thru$ROUTETHRU")).get();
    EXPECT_EQ(buffer->type, id_MISTRAL_BUF);
    EXPECT_EQ(buffer->pin_data.at(id_A).bel_pins, std::vector<IdString>{id_E1});
    EXPECT_EQ(buffer->getPort(id_Q), thru->getPort(id_DATAIN));
}

TEST(LutPlacementTiming, SynchronousLoadKeepsASelectorInput)
{
    // sel_ef of a half is fed by that half's E and the other half's F.
    // Legality accepts SDATA when the other LUT has at most two inputs,
    // which used to leave that F free. The load keeps E when possible; a full
    // L5 can instead leave the other half's F free.
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
    // Two inputs fit on C and F, leaving E0 for SDATA. The other LUT still has E1.
    EXPECT_EQ(pin(top, id_A), id_C);
    EXPECT_EQ(pin(top, id_B), id_F0);
    EXPECT_FALSE(uses(top, id_E0));
    EXPECT_EQ(pin(bottom, id_A), id_D);
    EXPECT_EQ(pin(bottom, id_B), id_E1);
    // Five inputs need E0, so the other LUT leaves F1 for SDATA.
    EXPECT_TRUE(uses(full, id_E0));
    EXPECT_FALSE(uses(mate, id_F1));
    EXPECT_EQ(pin(mate, id_A), id_D);
    EXPECT_EQ(pin(mate, id_B), id_E1);
}
