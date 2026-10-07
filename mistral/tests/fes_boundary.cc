#include <memory>

#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

class FesBoundaryTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *net, *other;
    WireId source, exit, sink;
    PipId first, last;

    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        source = ctx->add_wire(24, 41, ctx->id("boundary_source"));
        exit = ctx->add_wire(24, 42, ctx->id("boundary_exit"));
        sink = ctx->add_wire(25, 42, ctx->id("boundary_sink"));
        first = ctx->add_pip(source, exit);
        last = ctx->add_pip(exit, sink);
        net = ctx->createNet(ctx->id("video_plug_request[11]"));
        other = ctx->createNet(ctx->id("video_request[24]"));
        auto port = [&](const char *name, PortType direction, WireId wire) {
            auto id = ctx->id(name);
            auto pin = ctx->id("pin");
            ctx->createRegionPlug(id, ctx->id("test_plug"), Loc());
            ctx->addPlugPin(id, pin, direction, wire);
            auto cell = ctx->cells.at(id).get();
            cell->connectPort(pin, net);
            return cell;
        };
        port("shell", PORT_OUT, source)->belStrength = STRENGTH_LOCKED;
        port("cart", PORT_IN, sink)->attrs[ctx->id("FES_SLOT")] = std::string("video");
        ctx->note_fes_cram_region("1769,3442,2806,5162");
    }
};

TEST_F(FesBoundaryTest, VacantBoundaryOutputCanReachCart)
{
    EXPECT_TRUE(net->wires.empty());
    EXPECT_NO_THROW(ctx->fes_check_boundary_connectivity());
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(FesBoundaryTest, FrozenShellExitIsRejectedWithoutChangingOwnership)
{
    ctx->bindWire(exit, other, STRENGTH_LOCKED);
    EXPECT_THROW(ctx->fes_check_boundary_connectivity(), log_execution_error_exception);
    EXPECT_EQ(ctx->getBoundWireNet(exit), other);
    EXPECT_EQ(other->wires.at(exit).strength, STRENGTH_LOCKED);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(FesBoundaryTest, MovableCartRoutingDoesNotCauseFalseRejection)
{
    ctx->bindPip(first, other, STRENGTH_STRONG);
    EXPECT_NO_THROW(ctx->fes_check_boundary_connectivity());
    EXPECT_EQ(ctx->getBoundPipNet(first), other);
}

TEST_F(FesBoundaryTest, ExistingFrozenBranchOfSameNetCanBeExtended)
{
    ctx->bindWire(source, net, STRENGTH_LOCKED);
    ctx->bindPip(first, net, STRENGTH_LOCKED);
    EXPECT_NO_THROW(ctx->fes_check_boundary_connectivity());
    EXPECT_EQ(ctx->getBoundPipNet(first), net);
    EXPECT_EQ(ctx->getBoundPipNet(last), nullptr);
}

TEST_F(FesBoundaryTest, ReservedMuxSelectionIsRespected)
{
    auto alternate = ctx->add_wire(24, 41, ctx->id("alternate"));
    ctx->add_pip(alternate, exit);
    ctx->wires.at(exit).flags = WireInfo::RESERVED_ROUTE | 1;
    EXPECT_THROW(ctx->fes_check_boundary_connectivity(), log_execution_error_exception);
}

TEST_F(FesBoundaryTest, FrozenSelectionOfSameNetCannotBeReplaced)
{
    auto alternate = ctx->add_wire(24, 41, ctx->id("alternate"));
    auto fixed = ctx->add_pip(alternate, exit);
    ctx->bindPip(fixed, net, STRENGTH_LOCKED);
    EXPECT_THROW(ctx->fes_check_boundary_connectivity(), log_execution_error_exception);
    EXPECT_EQ(net->wires.at(exit).pip, fixed);
}

TEST_F(FesBoundaryTest, EveryPhysicalSinkMustBeReachable)
{
    auto isolated = ctx->add_wire(25, 43, ctx->id("isolated_sink"));
    auto name = ctx->id("second_cart_sink");
    auto pin = ctx->id("pin");
    ctx->createRegionPlug(name, ctx->id("test_plug"), Loc());
    ctx->addPlugPin(name, pin, PORT_IN, isolated);
    auto cell = ctx->cells.at(name).get();
    cell->attrs[ctx->id("FES_SLOT")] = std::string("video");
    cell->connectPort(pin, net);
    EXPECT_THROW(ctx->fes_check_boundary_connectivity(), log_execution_error_exception);
}

TEST_F(FesBoundaryTest, OutsidePhysicalMuxIsNotAnEscapeRoute)
{
    // Remove the synthetic shortcut. Synthetic edges themselves do not
    // configure CRAM, but this intervening physical mux does.
    ctx->wires.at(source).wires_downhill.clear();
    auto parent = WireId(CycloneV::rnode_coords(CycloneV::WM, 18, 1, 0));
    auto outside = WireId(CycloneV::rnode_coords(CycloneV::H14, 19, 1, 0));
    ctx->add_pip(source, parent);
    ctx->add_pip(outside, sink);
    ctx->note_fes_cram_region("0,0,1,1");
    ASSERT_FALSE(ctx->fes_pip_preserves_cram(PipId(parent.node, outside.node)));
    EXPECT_THROW(ctx->fes_check_boundary_connectivity(), log_execution_error_exception);
}
