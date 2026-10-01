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
