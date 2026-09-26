#include <memory>

#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

class FesCramTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *net;
    WireId root, parent, outside, inside;
    PipId protected_pip, removable_pip;

    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        net = ctx->createNet(ctx->id("shell"));
        auto wire = [&](CycloneV::rnode_type_t type, int x, int y, int z) {
            return WireId(CycloneV::rnode_coords(type, x, y, z));
        };
        root = wire(CycloneV::V12, 18, 0, 5);
        parent = wire(CycloneV::WM, 18, 1, 0);
        outside = wire(CycloneV::H14, 19, 1, 0);
        inside = wire(CycloneV::H3, 27, 1, 4);
        protected_pip = PipId(parent.node, outside.node);
        removable_pip = PipId(outside.node, inside.node);
        ctx->bindWire(root, net, STRENGTH_USER);
        ctx->bindPip(PipId(root.node, parent.node), net, STRENGTH_USER);
        ctx->bindPip(protected_pip, net, STRENGTH_USER);
        ctx->bindPip(removable_pip, net, STRENGTH_USER);
    }

    void fence() { ctx->note_fes_cram_region("1769,32,2806,7024"); }

    CellInfo *port(const char *name, PortType direction, WireId wire)
    {
        auto id = ctx->id(name);
        auto pin = ctx->id("pin");
        ctx->createRegionPlug(id, ctx->id("test_plug"), Loc());
        ctx->addPlugPin(id, pin, direction, wire);
        auto cell = ctx->cells.at(id).get();
        cell->connectPort(pin, net);
        return cell;
    }
};

TEST_F(FesCramTest, OrphanPreservesOutsideMuxAndAncestors)
{
    fence();
    ctx->fes_trim_net_orphans(net);
    EXPECT_EQ(ctx->getBoundWireNet(root), net);
    EXPECT_EQ(ctx->getBoundWireNet(parent), net);
    EXPECT_EQ(ctx->getBoundPipNet(protected_pip), net);
    EXPECT_EQ(ctx->getBoundPipNet(removable_pip), nullptr);
    ctx->lockNetRouting(net->name);
    ctx->fes_lock_protected_routing();
    EXPECT_EQ(net->wires.at(root).strength, STRENGTH_LOCKED);
    EXPECT_EQ(net->wires.at(outside).strength, STRENGTH_LOCKED);
    EXPECT_NO_THROW(ctx->fes_validate_cram_routing());
}

TEST_F(FesCramTest, UnfencedOrphanIsRemoved)
{
    ctx->fes_trim_net_orphans(net);
    EXPECT_TRUE(net->wires.empty());
}

TEST_F(FesCramTest, WholeChipFenceAllowsOrphanRemoval)
{
    ctx->note_fes_cram_region("0,0,7605,7024");
    ctx->fes_trim_net_orphans(net);
    EXPECT_TRUE(net->wires.empty());
    EXPECT_NO_THROW(ctx->fes_validate_cram_routing());
}

TEST_F(FesCramTest, MissingFrozenSelectionIsRejected)
{
    fence();
    ctx->unbindWire(outside);
    EXPECT_THROW(ctx->fes_validate_cram_routing(), log_execution_error_exception);
}

TEST_F(FesCramTest, ReplacedFrozenSelectionIsRejected)
{
    fence();
    ctx->unbindWire(outside);
    auto alternative = WireId(CycloneV::rnode_coords(CycloneV::WM, 18, 1, 1));
    ctx->bindPip(PipId(alternative.node, outside.node), net, STRENGTH_USER);
    EXPECT_THROW(ctx->fes_validate_cram_routing(), log_execution_error_exception);
}

TEST_F(FesCramTest, ChangedFrozenOwnerIsRejected)
{
    fence();
    ctx->unbindWire(outside);
    auto other = ctx->createNet(ctx->id("cart"));
    ctx->bindPip(protected_pip, other, STRENGTH_USER);
    EXPECT_THROW(ctx->fes_validate_cram_routing(), log_execution_error_exception);
}

TEST_F(FesCramTest, RoutedDesignAllowsLockedStubButRejectsUnlockedOrDisconnectedStub)
{
    port("source", PORT_OUT, root);
    port("sink", PORT_IN, parent);
    fence();
    ctx->fes_trim_net_orphans(net);
    ctx->fes_lock_protected_routing();
    EXPECT_TRUE(ctx->checkRoutedDesign());
    net->wires.at(outside).strength = STRENGTH_STRONG;
    EXPECT_FALSE(ctx->checkRoutedDesign());
    net->wires.at(outside).strength = STRENGTH_LOCKED;
    ctx->unbindWire(parent);
    EXPECT_FALSE(ctx->checkRoutedDesign());
}

TEST_F(FesCramTest, RoutedDesignAllowsLockedSinklessNetButRequiresConnectedDriver)
{
    auto source = port("source", PORT_OUT, root);
    fence();
    ctx->fes_trim_net_orphans(net);
    ctx->fes_lock_protected_routing();
    EXPECT_TRUE(ctx->checkRoutedDesign());
    net->wires.at(outside).strength = STRENGTH_STRONG;
    EXPECT_FALSE(ctx->checkRoutedDesign());
    net->wires.at(outside).strength = STRENGTH_LOCKED;
    auto disconnected = WireId(CycloneV::rnode_coords(CycloneV::WM, 18, 1, 1));
    ctx->bindWire(disconnected, net, STRENGTH_LOCKED);
    EXPECT_FALSE(ctx->checkRoutedDesign());
    ctx->unbindWire(disconnected);
    source->disconnectPort(ctx->id("pin"));
    EXPECT_FALSE(ctx->checkRoutedDesign());
}
