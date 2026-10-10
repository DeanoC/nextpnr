#include <array>
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
    const std::array<IdString, 6> types{id_MISTRAL_BUF, id_MISTRAL_ALUT2, id_MISTRAL_ALUT3,
                                       id_MISTRAL_ALUT4, id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
    const std::array<IdString, 6> inputs{id_A, id_B, id_C, id_D, id_E, id_F};
    for (int half : {0, 1}) {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        Context ctx(args);
        ctx.arrival_pin_assignment = true;
        std::array<CellInfo *, 6> cells;
        std::array<std::array<DelayQuad, 6>, 6> unplaced, placed;
        for (int width = 1; width <= 6; ++width) {
            auto *cell = ctx.createCell(ctx.idf("lut%d", width), types[width - 1]);
            cells[width - 1] = cell;
            for (int input = 0; input < width; ++input) {
                cell->addInput(inputs[input]);
                cell->connectPort(inputs[input], ctx.createNet(ctx.idf("input%d_%d", width, input)));
            }
            cell->addOutput(id_Q);
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
                for (const auto &estimate : {unplaced[width - 1][input], placed[width - 1][input]}) {
                    EXPECT_EQ(estimate.minRiseDelay(), actual.minRiseDelay());
                    EXPECT_EQ(estimate.maxRiseDelay(), actual.maxRiseDelay());
                    EXPECT_EQ(estimate.minFallDelay(), actual.minFallDelay());
                    EXPECT_EQ(estimate.maxFallDelay(), actual.maxFallDelay());
                }
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
