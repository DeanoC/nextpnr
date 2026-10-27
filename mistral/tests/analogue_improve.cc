#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include "gtest/gtest.h"
#include "analogue_repair.h"
#include "analogue_lut_pins.h"
#include "analogue_lut_moves.h"
#include "analogue_lut_pairs.h"
#include "analogue_routing.h"
#include "critical_cohort.h"
#include "nextpnr.h"
#include "timing.h"
#include "routed_placement.h"

USING_NEXTPNR_NAMESPACE

TEST(AnalogueRouting, CellConnectionsProtectSelectorsWhileAllowingFabricChanges)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    auto ff = ctx.getBelByLocation(Loc(24, 1, 3)); // FF1 has general and local output muxes.
    auto lut = ctx.getBelByLocation(Loc(24, 1, 0));
    ASSERT_NE(ff, BelId()); ASSERT_NE(lut, BelId());
    using Tree = std::vector<std::pair<WireId, PipId>>;
    for (auto source : {ctx.getBelPinWire(ff, id_Q), ctx.getBelPinWire(lut, id_COMBOUT)}) {
        ASSERT_TRUE(source.is_nextpnr_created());
        std::vector<PipId> outputs;
        for (auto pip : ctx.getPipsDownhill(source))
            if (!ctx.getPipDstWire(pip).is_nextpnr_created()) outputs.push_back(pip);
        ASSERT_GE(outputs.size(), 2u);
        const auto first = ctx.getPipDstWire(outputs[0]), second = ctx.getPipDstWire(outputs[1]);
        Tree parent{{source, PipId()}, {first, outputs[0]}};
        Tree changed{{source, PipId()}, {second, outputs[1]}};
        const auto expected = analogue_cell_connections(&ctx, parent);
        EXPECT_NE(expected, analogue_cell_connections(&ctx, changed));
        changed = parent;
        changed.emplace_back(second, outputs[1]);
        EXPECT_NE(expected, analogue_cell_connections(&ctx, changed));
        changed = Tree{{source, PipId()}};
        EXPECT_NE(expected, analogue_cell_connections(&ctx, changed));
        PipId fabric;
        for (auto pip : ctx.getPipsDownhill(first))
            if (!ctx.getPipDstWire(pip).is_nextpnr_created()) { fabric = pip; break; }
        ASSERT_NE(fabric, PipId());
        changed = parent;
        changed.emplace_back(ctx.getPipDstWire(fabric), fabric);
        EXPECT_EQ(expected, analogue_cell_connections(&ctx, changed));
    }
    const auto terminal = ctx.getBelPinWire(ff, id_DATAIN);
    ASSERT_TRUE(terminal.is_nextpnr_created());
    std::vector<PipId> inputs;
    for (auto pip : ctx.getPipsUphill(terminal)) inputs.push_back(pip);
    ASSERT_GE(inputs.size(), 2u);
    EXPECT_NE(analogue_cell_connections(&ctx, Tree{{terminal, inputs[0]}}),
              analogue_cell_connections(&ctx, Tree{{terminal, inputs[1]}}));
}

TEST(AnalogueRouting, ParentCellPipScopeConstrainsSearchAndRestoresOnExit)
{
    ArchArgs args; args.device = "5CSEBA6U23I7"; Context ctx(args);
    const auto ff = ctx.getBelByLocation(Loc(24, 1, 3)), lut = ctx.getBelByLocation(Loc(24, 1, 0));
    auto bindings = [&](NetInfo *net) {
        std::map<WireId, std::pair<PipId, PlaceStrength>> result;
        for (const auto &wire : net->wires) result.emplace(wire.first, std::make_pair(wire.second.pip, wire.second.strength));
        return result;
    };
    int index = 0;
    for (auto source : {ctx.getBelPinWire(ff, id_Q), ctx.getBelPinWire(lut, id_COMBOUT)}) {
        auto *net = ctx.createNet(ctx.idf("cell_scope_owner_%d", index++));
        std::vector<PipId> outputs;
        for (auto pip : ctx.getPipsDownhill(source))
            if (!ctx.getPipDstWire(pip).is_nextpnr_created() && ctx.checkPipAvail(pip)) outputs.push_back(pip);
        ASSERT_GE(outputs.size(), 2u);
        const auto kept = outputs[0], extra = outputs[1];
        PipId fabric;
        for (auto pip : ctx.getPipsDownhill(ctx.getPipDstWire(kept)))
            if (!ctx.getPipDstWire(pip).is_nextpnr_created() && ctx.checkPipAvail(pip)) { fabric = pip; break; }
        ASSERT_NE(fabric, PipId());
        ctx.bindWire(source, net, STRENGTH_WEAK); ctx.bindPip(kept, net, STRENGTH_WEAK);
        const auto before = bindings(net);
        const auto native = routed_placement_snapshot(&ctx);
        const auto allowed = analogue_parent_cell_pips(&ctx);
        EXPECT_TRUE(allowed.count(kept)); EXPECT_FALSE(allowed.count(extra)); EXPECT_FALSE(allowed.count(fabric));
        ASSERT_TRUE(ctx.checkPipAvail(extra)); ASSERT_TRUE(ctx.checkPipAvail(fabric));
        {
            AnalogueCellPipScope scope(&ctx, allowed);
            EXPECT_TRUE(ctx.checkPipAvailForNet(kept, net)); EXPECT_TRUE(ctx.pipAllowedForNet(kept, net));
            EXPECT_FALSE(ctx.checkPipAvail(extra)); EXPECT_FALSE(ctx.checkPipAvailForNet(extra, net));
            EXPECT_FALSE(ctx.pipAllowedForNet(extra, net));
            EXPECT_STREQ(ctx.pip_rejection_reason(extra, net), "analogue_trial_requires_original_cell_connection");
            EXPECT_TRUE(ctx.checkPipAvail(fabric)); EXPECT_TRUE(ctx.pipAllowedForNet(fabric, net));
            EXPECT_EQ(bindings(net), before); EXPECT_EQ(routed_placement_snapshot(&ctx), native);
        }
        EXPECT_EQ(ctx.analogue_cell_pip_allowlist, nullptr);
        EXPECT_TRUE(ctx.checkPipAvail(extra)); EXPECT_TRUE(ctx.pipAllowedForNet(extra, net));
        struct Cancelled {};
        EXPECT_THROW(([&]() { AnalogueCellPipScope scope(&ctx, allowed); throw Cancelled{}; })(), Cancelled);
        EXPECT_EQ(ctx.analogue_cell_pip_allowlist, nullptr); EXPECT_TRUE(ctx.checkPipAvail(extra));
        EXPECT_EQ(bindings(net), before); EXPECT_EQ(routed_placement_snapshot(&ctx), native);
        ctx.ripupNet(net->name);
    }
    // Entering a virtual FF data terminal is constrained as well as
    // leaving a virtual driver. Ordinary fabric remains unrestricted.
    const auto sink = ctx.getBelPinWire(ff, id_DATAIN);
    std::vector<PipId> inputs;
    for (auto pip : ctx.getPipsUphill(sink)) if (ctx.checkPipAvail(pip)) inputs.push_back(pip);
    ASSERT_GE(inputs.size(), 2u);
    auto *data = ctx.createNet(ctx.id("cell_scope_ff_data"));
    ctx.bindWire(ctx.getPipSrcWire(inputs[0]), data, STRENGTH_WEAK);
    ctx.bindPip(inputs[0], data, STRENGTH_WEAK);
    const auto allowed = analogue_parent_cell_pips(&ctx);
    {
        AnalogueCellPipScope scope(&ctx, allowed);
        EXPECT_TRUE(ctx.checkPipAvailForNet(inputs[0], data));
        EXPECT_FALSE(ctx.pipAllowedForNet(inputs[1], data));
    }
    EXPECT_EQ(ctx.analogue_cell_pip_allowlist, nullptr);
    EXPECT_TRUE(ctx.pipAllowedForNet(inputs[1], data));
}

TEST(AnalogueRouting, WholeInputSourcePrefixKeepsCellEgressesAndReleasesFabric)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    const auto ff = ctx.getBelByLocation(Loc(24, 1, 3));
    const auto lut = ctx.getBelByLocation(Loc(24, 1, 0));
    using Tree = std::vector<std::pair<WireId, PipId>>;
    for (auto source : {ctx.getBelPinWire(ff, id_Q), ctx.getBelPinWire(lut, id_COMBOUT)}) {
        std::vector<PipId> outputs;
        for (auto pip : ctx.getPipsDownhill(source))
            if (!ctx.getPipDstWire(pip).is_nextpnr_created()) outputs.push_back(pip);
        ASSERT_GE(outputs.size(), 2u);
        const auto first = ctx.getPipDstWire(outputs[0]), second = ctx.getPipDstWire(outputs[1]);
        Tree parent{{source, PipId()}, {first, outputs[0]}, {second, outputs[1]}};
        Tree expected = parent;
        for (auto pip : ctx.getPipsUphill(ctx.getBelPinWire(ff, id_DATAIN)))
            if (ctx.getPipSrcWire(pip) == source) {
                parent.emplace_back(ctx.getPipDstWire(pip), pip);
                expected.emplace_back(ctx.getPipDstWire(pip), pip);
            }
        PipId fabric;
        for (auto pip : ctx.getPipsDownhill(first))
            if (!ctx.getPipDstWire(pip).is_nextpnr_created() && ctx.getPipDstWire(pip) != second) {
                fabric = pip; break;
            }
        ASSERT_NE(fabric, PipId());
        parent.emplace_back(ctx.getPipDstWire(fabric), fabric);
        auto prefix = analogue_source_prefix(&ctx, source, parent);
        ASSERT_TRUE(prefix);
        const std::map<WireId, PipId> actual(prefix->begin(), prefix->end()), wanted(expected.begin(), expected.end());
        EXPECT_EQ(actual, wanted);
        // A real hard-block source reserves no fabric edge, even when
        // that edge was part of its previous complete route.
        auto real = analogue_source_prefix(&ctx, first, Tree{{first, PipId()}, {ctx.getPipDstWire(fabric), fabric}});
        ASSERT_TRUE(real); ASSERT_EQ(real->size(), 1u);
        EXPECT_EQ(real->front(), std::make_pair(first, PipId()));
        auto bad = parent; bad.erase(bad.begin());
        EXPECT_FALSE(analogue_source_prefix(&ctx, source, bad));
        bad = parent; bad.push_back(parent.front());
        EXPECT_FALSE(analogue_source_prefix(&ctx, source, bad));
        bad = parent; bad[1].first = ctx.getBelPinWire(ff, id_DATAIN);
        EXPECT_FALSE(analogue_source_prefix(&ctx, source, bad));
        auto other = ctx.getBelPinWire(ctx.getBelByLocation(Loc(24, 1, 8)), id_DATAIN);
        bad = parent; bad.emplace_back(other, PipId());
        EXPECT_FALSE(analogue_source_prefix(&ctx, source, bad));
        for (auto pip : ctx.getPipsUphill(other)) {
            if (std::any_of(parent.begin(), parent.end(), [&](const auto &entry) {
                    return entry.first == ctx.getPipSrcWire(pip);
                })) continue;
            bad = parent; bad.emplace_back(other, pip);
            EXPECT_FALSE(analogue_source_prefix(&ctx, source, bad));
            break;
        }
    }
}

TEST(AnalogueRouting, RetainUnusedSourceEgressesWithoutReplacingConnectionsOrTakingWires)
{
    ArchArgs args; args.device = "5CSEBA6U23I7";
    Context ctx(args);
    using Tree = std::vector<std::pair<WireId, PipId>>;
    const auto ff = ctx.getBelByLocation(Loc(24, 1, 3)), lut = ctx.getBelByLocation(Loc(24, 1, 0));
    int index = 0;
    for (auto source : {ctx.getBelPinWire(ff, id_Q), ctx.getBelPinWire(lut, id_COMBOUT)}) {
        auto *net = ctx.createNet(ctx.idf("prefix_owner_%d", index));
        auto *other = ctx.createNet(ctx.idf("prefix_obstacle_%d", index++));
        std::vector<PipId> outputs;
        for (auto pip : ctx.getPipsDownhill(source))
            if (!ctx.getPipDstWire(pip).is_nextpnr_created()) outputs.push_back(pip);
        ASSERT_GE(outputs.size(), 2u);
        const auto first = ctx.getPipDstWire(outputs[0]), second = ctx.getPipDstWire(outputs[1]);
        Tree prefix{{source, PipId()}, {first, outputs[0]}, {second, outputs[1]}};
        PipId fabric;
        for (auto pip : ctx.getPipsDownhill(first)) {
            auto dst = ctx.getPipDstWire(pip);
            if (!dst.is_nextpnr_created() && dst != first && dst != second) { fabric = pip; break; }
        }
        ASSERT_NE(fabric, PipId());
        auto snapshot = [&](NetInfo *ni) {
            std::map<WireId, std::pair<PipId, PlaceStrength>> result;
            for (const auto &entry : ni->wires)
                result.emplace(entry.first, std::make_pair(entry.second.pip, entry.second.strength));
            return result;
        };
        ctx.bindWire(source, net, STRENGTH_STRONG);
        ctx.bindPip(outputs[0], net, STRENGTH_WEAK);
        ctx.bindPip(fabric, net, STRENGTH_WEAK);
        const auto before = snapshot(net);
        auto retained = analogue_retain_source_prefix(&ctx, net, source, prefix);
        ASSERT_TRUE(retained); EXPECT_EQ(*retained, 1u);
        for (const auto &entry : before) EXPECT_EQ(snapshot(net).at(entry.first), entry.second);
        EXPECT_EQ(net->wires.at(second).pip, outputs[1]);
        EXPECT_EQ(net->wires.at(second).strength, STRENGTH_PLACER);
        const auto complete = snapshot(net);
        retained = analogue_retain_source_prefix(&ctx, net, source, prefix);
        ASSERT_TRUE(retained); EXPECT_EQ(*retained, 0u); EXPECT_EQ(snapshot(net), complete);
        // Retention is cell-adjacent only; it cannot restore an arbitrary
        // fabric path to conceal a protected-route mismatch.
        auto too_wide = prefix; too_wide.emplace_back(ctx.getPipDstWire(fabric), fabric);
        EXPECT_FALSE(analogue_retain_source_prefix(&ctx, net, source, too_wide));
        EXPECT_EQ(snapshot(net), complete);
        ctx.unbindWire(second);
        ctx.bindWire(second, other, STRENGTH_LOCKED);
        const auto blocked = snapshot(net), obstacle = snapshot(other);
        EXPECT_FALSE(analogue_retain_source_prefix(&ctx, net, source, prefix));
        EXPECT_EQ(snapshot(net), blocked); EXPECT_EQ(snapshot(other), obstacle);
        ctx.unbindWire(second);
        // An existing, different connection must be rejected unchanged.
        ctx.bindWire(second, net, STRENGTH_WEAK);
        const auto different = snapshot(net);
        EXPECT_FALSE(analogue_retain_source_prefix(&ctx, net, source, prefix));
        EXPECT_EQ(snapshot(net), different);
        ctx.unbindWire(second);
        ctx.unbindWire(ctx.getPipDstWire(fabric)); ctx.unbindWire(first);
        retained = analogue_retain_source_prefix(&ctx, net, source, prefix);
        ASSERT_TRUE(retained); EXPECT_EQ(*retained, 2u);
        EXPECT_EQ(analogue_cell_connections(&ctx, prefix), analogue_cell_connections(&ctx,
            Tree{{source, net->wires.at(source).pip}, {first, net->wires.at(first).pip},
                 {second, net->wires.at(second).pip}}));
        EXPECT_FALSE(analogue_retain_source_prefix(&ctx, other, source, prefix));
        ctx.ripupNet(net->name);
    }
}

TEST(AnalogueLutInputs, StaticIndependentFfBypassNeedsExplicitOptInAndCompleteNativeResources)
{
    ArchArgs args; args.device = "5CSEBA6U23I7"; Context ctx(args);
    ctx.createNet(ctx.id("$PACKER_GND_NET")); ctx.createNet(ctx.id("$PACKER_VCC_NET"));
    auto *pad = ctx.createCell(ctx.id("ff_static_clock_pad"), id_MISTRAL_IB);
    auto *buffer = ctx.createCell(ctx.id("ff_static_clock_buffer"), id_MISTRAL_CLKBUF);
    auto *input_clock = ctx.createNet(ctx.id("ff_static_input_clock"));
    auto *clock = ctx.createNet(ctx.id("ff_static_clock"));
    pad->addOutput(id_O); pad->connectPort(id_O, input_clock);
    buffer->addInput(id_A); buffer->connectPort(id_A, input_clock);
    buffer->addOutput(id_Q); buffer->connectPort(id_Q, clock);
    clock->clkconstr = std::make_unique<ClockConstraint>(); clock->clkconstr->period = DelayPair(10000);
    clock->clkconstr->high = DelayPair(5000); clock->clkconstr->low = DelayPair(5000);
    const std::array<IdString, 5> ports{id_A,id_B,id_C,id_D,id_E};
    const std::array<IdString, 4> types{id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5};
    const std::array<uint64_t, 4> masks{0x2,0xd2,0x35f0,0x987a35f0};
    std::array<int, 2> lab_index{-1,-1};
    for (auto bel : ctx.getBels()) {
        if (!ctx.isValidBelForCellType(id_MISTRAL_ALUT3, bel)) continue;
        const auto where = ctx.bel_data(bel).lab_data;
        int kind = ctx.labs.at(where.lab).is_mlab ? 1 : 0;
        if (lab_index[kind] < 0) lab_index[kind] = where.lab;
        if (lab_index[0] >= 0 && lab_index[1] >= 0) break;
    }
    ASSERT_GE(lab_index[0],0); ASSERT_GE(lab_index[1],0);
    int index = 0, total = 0;
    for (int kind : {0,1}) for (int half : {0,1}) for (int width = 2; width <= 5; ++width) {
        SCOPED_TRACE("kind="+std::to_string(kind)+" half="+std::to_string(half)+" width="+std::to_string(width));
        const int alm_index = half*4+width-2;
        auto &alm = ctx.labs.at(lab_index[kind]).alms.at(alm_index);
        auto *lut = ctx.createCell(ctx.idf("ff_static_lut_%d",index),types.at(width-2));
        for (int i = 0; i < width; ++i) {
            lut->addInput(ports[i]); lut->connectPort(ports[i],ctx.createNet(ctx.idf("ff_static_in_%d_%d",index,i)));
        }
        lut->addOutput(id_Q); lut->connectPort(id_Q,ctx.createNet(ctx.idf("ff_static_out_%d",index)));
        lut->params[id_LUT] = Property(int64_t(masks.at(width-2)),1<<width);
        ctx.assign_comb_info(lut); ctx.assign_default_pinmap(lut);
        ctx.bindBel(alm.lut_bels.at(half),lut,STRENGTH_WEAK);
        std::vector<IdString> physical;
        if (width >= 4) physical.push_back(id_A);
        if (width >= 5) physical.push_back(id_B);
        if (width >= 3) physical.push_back(half ? id_D : id_C);
        physical.push_back(half ? id_E1 : id_E0); physical.push_back(half ? id_F1 : id_F0);
        ASSERT_EQ(physical.size(),size_t(width));
        for (int i = 0; i < width; ++i) {
            lut->pin_data[ports[i]].state = i == 1 ? PIN_INV : PIN_SIG;
            lut->pin_data[ports[i]].bel_pins = {physical.at(i)};
            ctx.bindWire(ctx.getBelPinWire(lut->bel,physical.at(i)),lut->getPort(ports[i]),STRENGTH_WEAK);
        }
        auto *data = ctx.createNet(ctx.idf("ff_static_data_%d",index));
        auto *ff = ctx.createCell(ctx.idf("ff_static_register_%d",index),id_MISTRAL_FF);
        for (auto port : {id_CLK,id_DATAIN,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA}) ff->addInput(port);
        ff->addOutput(id_Q); ff->connectPort(id_Q,ctx.createNet(ctx.idf("ff_static_q_%d",index)));
        ff->connectPort(id_CLK,clock); ff->connectPort(id_DATAIN,data);
        ff->pin_data[id_ENA].state = PIN_1; ff->pin_data[id_ACLR].state = PIN_1;
        ff->pin_data[id_SCLR].state = PIN_0; ff->pin_data[id_SLOAD].state = PIN_0;
        ctx.assign_ff_info(ff); ctx.assign_default_pinmap(ff);
        const int ff_index = 2*half;
        ctx.bindBel(alm.ff_bels.at(ff_index),ff,STRENGTH_WEAK);
        const auto source = ctx.getBelPinWire(lut->bel,half ? id_F0 : id_F1);
        const auto ef = alm.sel_ef.at(half), input = alm.ff_in.at(ff_index);
        auto pip_between = [&](WireId a, WireId b) {
            for (auto pip : ctx.getPipsDownhill(a)) if (ctx.getPipDstWire(pip)==b) return pip;
            return PipId();
        };
        const auto into_ef = pip_between(source,ef), into_ff = pip_between(ef,input);
        ASSERT_NE(into_ef,PipId()); ASSERT_NE(into_ff,PipId());
        ctx.bindWire(source,data,STRENGTH_WEAK); ctx.bindPip(into_ef,data,STRENGTH_WEAK);
        ctx.bindPip(into_ff,data,STRENGTH_WEAK);
        ASSERT_TRUE(ctx.isBelLocationValid(lut->bel));
        const auto first = ports.at(width >= 3 ? width-3 : 0), second = ports.at(width-2+(width==2));
        auto check = [&](bool expected) {
            auto before = routed_placement_snapshot(&ctx);
            EXPECT_EQ(analogue_lut_pin_swap_allowed(&ctx,lut,first,second,false,true),expected);
            EXPECT_EQ(routed_placement_snapshot(&ctx),before);
        };
        EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx,lut,first,second)); check(true);
        EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx,lut,first,second,true,true));
        const auto saved = lut->pin_data; const uint64_t original = ctx.compute_lut_mask(lab_index[kind],alm_index);
        ASSERT_TRUE(analogue_swap_lut_pins(&ctx,lut,first,second,false,true));
        const uint64_t mask = ctx.compute_lut_mask(lab_index[kind],alm_index);
        const uint64_t other_half = half ? 0xffffffffULL : 0xffffffff00000000ULL;
        EXPECT_EQ((mask^original)&other_half,0u);
        for (unsigned address=unsigned(half*32);address<unsigned((half+1)*32);++address) {
            unsigned row=0;
            for (int i=0;i<width;++i) {
                const auto &pin=lut->pin_data.at(ports[i]); auto p=pin.bel_pins.front();
                int bit=p==id_A?0:p==id_B?1:p.in(id_C,id_D)?2:p.in(id_E0,id_E1)?3:4;
                row|=unsigned(bool((address>>bit)&1)^(pin.state!=PIN_INV))<<i;
            }
            unsigned storage=address;
            if(kind) { auto bit=[&](int i){return(address>>i)&1;}; storage=(bit(5)<<5)|(bit(3)<<4)|
                ((bit(2)^bit(3))<<3)|((bit(1)^bit(3))<<2)|(bit(4)<<1)|(bit(0)^bit(3)); }
            EXPECT_EQ(bool((mask>>storage)&1),!bool((masks.at(width-2)>>row)&1));
        }
        EXPECT_EQ(ctx.getBoundWireNet(source),data); EXPECT_EQ(data->wires.at(ef).pip,into_ef);
        EXPECT_EQ(data->wires.at(input).pip,into_ff);
        // A changed pin map without corresponding routing is not a valid parent contract.
        check(false); lut->pin_data=saved; check(true);
        for (auto port : {id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA}) {
            ff->connectPort(port,data); check(false); ff->disconnectPort(port);
        }
        ff->belStrength=STRENGTH_LOCKED;check(false);ff->belStrength=STRENGTH_WEAK;
        ff->attrs[ctx.id("keep")]=1;check(false);ff->attrs.erase(ctx.id("keep"));
        const auto clock_type=pad->type;pad->type=id_MISTRAL_NOT;check(false);pad->type=clock_type;
        ctx.unbindWire(input);ctx.bindWire(input,data,STRENGTH_WEAK);check(false);
        ctx.unbindWire(input);ctx.bindPip(into_ff,data,STRENGTH_WEAK);
        const auto secondary=alm.ff_bels.at(ff_index+1); const auto secondary_input=alm.ff_in.at(ff_index+1);
        const auto secondary_pip=pip_between(ef,secondary_input);ASSERT_NE(secondary_pip,PipId());
        ctx.unbindWire(input);ctx.unbindBel(ff->bel);ctx.bindBel(secondary,ff,STRENGTH_WEAK);
        ctx.bindPip(secondary_pip,data,STRENGTH_WEAK);EXPECT_FALSE(ctx.isBelLocationValid(lut->bel));check(false);
        ctx.unbindWire(secondary_input);ctx.unbindBel(ff->bel);ctx.bindBel(alm.ff_bels.at(ff_index),ff,STRENGTH_WEAK);
        ctx.bindPip(into_ff,data,STRENGTH_WEAK);check(true);
        ++index;++total;
    }
    EXPECT_EQ(total,16);
}

TEST(AnalogueLutPairs, NativeCapacityDisplacementAndPhysicalFunctionsRetainTheOriginalContracts)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    const std::array<IdString, 6> ports{id_A, id_B, id_C, id_D, id_E, id_F};
    const std::array<IdString, 5> types{id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                       id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
    auto *shared = ctx.createNet(ctx.id("pair_shared"));
    auto lut = [&](const char *name, int count, Loc location, NetInfo *last = nullptr) {
        auto *cell = ctx.createCell(ctx.id(name), types.at(count - 2));
        for (int i = 0; i < count; ++i) {
            cell->addInput(ports[i]);
            auto *net = i == 0 ? shared : i == count - 1 && last ? last :
                ctx.createNet(ctx.idf("%s_%d", name, i));
            cell->connectPort(ports[i], net);
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", name)));
        cell->params[id_LUT] = Property(int64_t(0x52c4b6e1987a35f0ULL), 64);
        cell->pin_data[id_B].state = PIN_INV;
        ctx.assign_comb_info(cell); ctx.assign_default_pinmap(cell);
        ctx.bindBel(ctx.getBelByLocation(location), cell, STRENGTH_WEAK);
        return cell;
    };
    auto *moving = lut("pair_moving", 3, Loc(25, 20, 1));
    auto *peer = lut("pair_peer", 5, Loc(24, 20, 0), moving->getPort(id_Q));
    for (int i = 1; i <= 5; ++i) lut(("pair_fill_" + std::to_string(i)).c_str(), 6, Loc(24, 20, i * 6));
    lut("pair_fill_four", 4, Loc(24, 20, 36));
    auto *cold = lut("pair_cold", 3, Loc(24, 20, 42));
    ctx.lab_pre_route();
    const auto target = ctx.getBelByLocation(Loc(24, 20, 1));
    const auto cold_target = ctx.getBelByLocation(Loc(25, 20, 6));
    const auto from = moving->bel, cold_from = cold->bel;
    const auto peer_where = ctx.bel_data(peer->bel).lab_data, source_where = ctx.bel_data(from).lab_data;
    auto counters = [&]() {
        std::vector<int> values;
        for (const auto &lab : ctx.labs) for (const auto &alm : lab.alms) values.push_back(alm.unique_input_count);
        return values;
    };
    const auto original_counters = counters();
    const auto original = routed_placement_snapshot(&ctx);
    const auto source_mask = ctx.compute_lut_mask(source_where.lab, source_where.alm);
    const auto peer_mask = ctx.compute_lut_mask(peer_where.lab, peer_where.alm);
    const auto moving_pins = moving->pin_data, peer_pins = peer->pin_data, cold_pins = cold->pin_data;
    ASSERT_TRUE(ctx.check_lab_input_count(peer_where.lab)); // 5 + 5*6 + 4 + 3 = 42.
    ASSERT_EQ(analogue_lut_pair_target(&ctx, moving, peer), target);
    EXPECT_FALSE(analogue_lut_move_allowed(&ctx, moving, target)); // Ordinary moves still require an empty ALM.
    EXPECT_FALSE(analogue_lut_pair_capacity_preview(&ctx, moving, peer));
    EXPECT_EQ(counters(), original_counters);
    EXPECT_FALSE(apply_critical_cohort_placement(&ctx, {{moving, target}}));
    EXPECT_EQ(routed_placement_snapshot(&ctx), original);
    EXPECT_EQ(counters(), original_counters);
    ASSERT_TRUE(analogue_lut_move_allowed(&ctx, cold, cold_target));
    EXPECT_TRUE(analogue_lut_pair_capacity_preview(&ctx, moving, peer, cold, cold_target));
    EXPECT_EQ(counters(), original_counters);
    for (auto *cell : {moving, peer}) for (const char *attr : {"keep", "dont_touch", "BEL", "FES_SLOT"}) {
        cell->attrs[ctx.id(attr)] = 1;
        EXPECT_EQ(analogue_lut_pair_target(&ctx, moving, peer), BelId());
        cell->attrs.erase(ctx.id(attr));
    }
    ASSERT_TRUE(apply_critical_cohort_placement(&ctx, {{moving, target}, {cold, cold_target}}));
    EXPECT_TRUE(analogue_lut_pair_input_capacity(&ctx, moving, peer));
    const auto mask = ctx.compute_lut_mask(peer_where.lab, peer_where.alm);
    auto storage = [&](uint32_t lab, unsigned address) {
        if (!ctx.labs.at(lab).is_mlab) return address;
        auto bit = [&](int i) { return (address >> i) & 1; };
        return (bit(5) << 5) | (bit(3) << 4) | ((bit(2) ^ bit(3)) << 3) |
               ((bit(1) ^ bit(3)) << 2) | (bit(4) << 1) | (bit(0) ^ bit(3));
    };
    for (auto *cell : {moving, peer}) {
        const unsigned half = cell == moving ? 1 : 0;
        const auto old_lab = cell == moving ? source_where.lab : peer_where.lab;
        const auto old_mask = cell == moving ? source_mask : peer_mask;
        for (unsigned address = half * 32; address < (half + 1) * 32; ++address) {
            unsigned row = 0;
            for (int i = 0; i < cell->combInfo.lut_input_count; ++i) {
                const auto &pin = cell->pin_data.at(ports[i]);
                const auto p = pin.bel_pins.front();
                const int bit = p == id_A ? 0 : p == id_B ? 1 : p.in(id_C, id_D) ? 2 :
                                p.in(id_E0, id_E1) ? 3 : 4;
                row |= unsigned(bool((address >> bit) & 1) ^ (pin.state != PIN_INV)) << i;
            }
            const bool expected = !bool((uint64_t(cell->params.at(id_LUT).as_int64()) >> row) & 1);
            EXPECT_EQ(bool((old_mask >> storage(old_lab, address)) & 1), expected);
            EXPECT_EQ(bool((mask >> storage(peer_where.lab, address)) & 1), expected);
        }
    }
    for (auto *cell : {moving, peer, cold}) {
        const auto &saved = cell == moving ? moving_pins : cell == peer ? peer_pins : cold_pins;
        for (const auto &pin : saved) {
            EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
        }
    }
    ASSERT_TRUE(apply_critical_cohort_placement(&ctx, {{moving, from}, {cold, cold_from}}));
    EXPECT_EQ(routed_placement_snapshot(&ctx), original);
    EXPECT_EQ(counters(), original_counters);
    // A shared physical input cannot silently acquire a second net.
    const auto old_pin = moving->pin_data.at(id_A).bel_pins;
    moving->pin_data.at(id_A).bel_pins = peer->pin_data.at(id_B).bel_pins;
    EXPECT_EQ(analogue_lut_pair_target(&ctx, moving, peer), BelId());
    moving->pin_data.at(id_A).bel_pins = old_pin;
    ctx.createNet(ctx.id("$PACKER_GND_NET")); ctx.createNet(ctx.id("$PACKER_VCC_NET"));
    auto *ff = ctx.createCell(ctx.id("pair_ff"), id_MISTRAL_FF);
    ctx.assignArchInfo();
    for (auto bel : ctx.labs.at(peer_where.lab).alms.at(peer_where.alm).ff_bels) {
        ctx.bindBel(bel, ff, STRENGTH_WEAK);
        EXPECT_EQ(analogue_lut_pair_target(&ctx, moving, peer), BelId());
        ctx.unbindBel(bel);
    }
}

TEST(AnalogueLutMoves, CapacityPreviewUsesTheNativeLimitAndRestoresBothCounters)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    const std::array<IdString, 6> pins{id_A, id_B, id_C, id_D, id_E, id_F};
    auto lut = [&](const char *name, int count, Loc location) {
        auto *cell = ctx.createCell(ctx.id(name), count == 2 ? id_MISTRAL_ALUT2 : id_MISTRAL_ALUT6);
        for (int i = 0; i < count; ++i) {
            cell->addInput(pins[i]);
            cell->connectPort(pins[i], ctx.createNet(ctx.idf("%s_%d", name, i)));
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", name)));
        cell->params[id_LUT] = Property(int64_t(0x52c4b6e1987a35f0ULL), 64);
        ctx.assign_comb_info(cell);
        ctx.assign_default_pinmap(cell);
        ctx.bindBel(ctx.getBelByLocation(location), cell, STRENGTH_WEAK);
        return cell;
    };
    auto *moving = lut("capacity_move", 2, Loc(24, 20, 0));
    std::vector<CellInfo *> fillers;
    for (int i = 1; i <= 7; ++i)
        fillers.push_back(lut(("capacity_fill_" + std::to_string(i)).c_str(), 6, Loc(25, 20, i * 6)));
    ctx.lab_pre_route();
    const auto from = ctx.bel_data(moving->bel).lab_data;
    const auto target = ctx.getBelByLocation(Loc(25, 20, 0));
    const auto to = ctx.bel_data(target).lab_data;
    ASSERT_TRUE(ctx.check_lab_input_count(to.lab)); // Seven real LUT6s consume all 42 inputs.
    auto &a = ctx.labs.at(from.lab).alms.at(from.alm).unique_input_count;
    auto &b = ctx.labs.at(to.lab).alms.at(to.alm).unique_input_count;
    ASSERT_EQ(a, 2); ASSERT_EQ(b, 0);
    EXPECT_FALSE(analogue_lut_move_allowed(&ctx, moving, target));
    EXPECT_EQ(a, 2); EXPECT_EQ(b, 0);
    EXPECT_FALSE(apply_critical_cohort_placement(&ctx, {{moving, target}}));
    // Native rejected placement restores the LAB vector; acquire fresh references below.
    ctx.unbindBel(fillers.back()->bel);
    EXPECT_TRUE(analogue_lut_move_allowed(&ctx, moving, target));
    EXPECT_EQ(ctx.labs.at(from.lab).alms.at(from.alm).unique_input_count, 2);
    EXPECT_EQ(ctx.labs.at(to.lab).alms.at(to.alm).unique_input_count, 0);
    EXPECT_TRUE(apply_critical_cohort_placement(&ctx, {{moving, target}}));
    EXPECT_EQ(moving->bel, target);
}

TEST(AnalogueLutMoves, NativeRelocationPreservesTruthAcrossTileKindsAndRejectsProtectedOrOccupiedAlms)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    const std::array<IdString, 6> ports{id_A, id_B, id_C, id_D, id_E, id_F};
    const std::array<IdString, 5> types{id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                       id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
    for (bool from_mlab : {false, true}) for (int half : {0, 1}) for (int count = 2; count <= 6; ++count) {
        auto *cell = ctx.createCell(ctx.idf("move_%d_%d_%d", int(from_mlab), half, count), types[count - 2]);
        for (int i = 0; i < count; ++i) {
            cell->addInput(ports[i]);
            cell->connectPort(ports[i], ctx.createNet(ctx.idf("%s_%d", ctx.nameOf(cell), i)));
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", ctx.nameOf(cell))));
        cell->params[id_LUT] = Property(int64_t(0x52c4b6e1987a35f0ULL), 64);
        cell->pin_data[id_B].state = PIN_INV;
        ctx.assign_comb_info(cell);
        ctx.assign_default_pinmap(cell);
        BelId source, target;
        for (auto bel : ctx.getBels()) {
            if (!ctx.isValidBelForCellType(cell->type, bel) || !ctx.checkBelAvail(bel)) continue;
            const auto &where = ctx.bel_data(bel).lab_data;
            if (where.idx != half) continue;
            const auto &alm = ctx.labs.at(where.lab).alms.at(where.alm);
            if (ctx.getBoundBelCell(alm.lut_bels[1 - half])) continue;
            if (ctx.labs.at(where.lab).is_mlab == from_mlab && source == BelId()) source = bel;
            if (ctx.labs.at(where.lab).is_mlab != from_mlab && target == BelId()) target = bel;
            if (source != BelId() && target != BelId()) break;
        }
        ASSERT_NE(source, BelId()); ASSERT_NE(target, BelId());
        ctx.bindBel(source, cell, STRENGTH_WEAK);
        ctx.lab_pre_route();
        ASSERT_TRUE(analogue_lut_move_allowed(&ctx, cell, target));
        const auto original_pins = cell->pin_data;
        const auto original_params = cell->params;
        for (const char *attr : {"keep", "dont_touch", "BEL", "FES_SLOT"}) {
            cell->attrs[ctx.id(attr)] = 1;
            EXPECT_FALSE(analogue_lut_move_allowed(&ctx, cell, target));
            cell->attrs.erase(ctx.id(attr));
        }
        const auto to = ctx.bel_data(target).lab_data;
        auto *peer = ctx.createCell(ctx.idf("%s_peer", ctx.nameOf(cell)), id_MISTRAL_FF);
        auto ff_bel = ctx.labs.at(to.lab).alms.at(to.alm).ff_bels[0];
        ctx.bindBel(ff_bel, peer, STRENGTH_WEAK);
        EXPECT_FALSE(analogue_lut_move_allowed(&ctx, cell, target));
        ctx.unbindBel(ff_bel);
        ASSERT_TRUE(apply_critical_cohort_placement(&ctx, {{cell, target}}));
        const auto from = ctx.bel_data(source).lab_data;
        ctx.assign_alm_lut_inputs(from.lab, from.alm);
        ctx.assign_alm_lut_inputs(to.lab, to.alm);
        cell->pin_data = original_pins;
        ASSERT_TRUE(ctx.isBelLocationValid(target));
        const auto mask = ctx.compute_lut_mask(to.lab, to.alm);
        for (int bits = 0; bits < (1 << count); ++bits) {
            std::map<IdString, bool> values;
            for (int i = 0; i < count; ++i) {
                const auto &pin = cell->pin_data.at(ports[i]);
                values[pin.bel_pins.front()] = bool((bits >> i) & 1) ^ (pin.state != PIN_INV);
            }
            auto e = half ? id_E1 : id_E0, f = half ? id_F1 : id_F0;
            int physical = int(values[id_A]) | (int(values[id_B]) << 1) | (int(values[f]) << 4);
            if (count == 6) {
                physical |= (int(values[e]) << 5) | (int(values[values[e] ? id_D : id_C]) << 2) |
                            (int(values[values[e] ? id_C : id_D]) << 3);
            } else {
                physical |= (half << 5) | (int(values[e]) << 3) | (int(values[half ? id_D : id_C]) << 2);
            }
            if (ctx.labs.at(to.lab).is_mlab) {
                auto bit = [&](int index) { return (physical >> index) & 1; };
                physical = (bit(5) << 5) | (bit(3) << 4) | ((bit(2) ^ bit(3)) << 3) |
                           ((bit(1) ^ bit(3)) << 2) | (bit(4) << 1) | (bit(0) ^ bit(3));
            }
            const bool expected = (uint64_t(cell->params.at(id_LUT).as_int64()) >> bits) & 1;
            EXPECT_EQ(bool((mask >> physical) & 1), !expected)
                << "inputs=" << count << " from_mlab=" << from_mlab << " half=" << half << " bits=" << bits;
        }
        EXPECT_EQ(cell->params, original_params);
        EXPECT_EQ(cell->bel, target);
        for (int i = 0; i < count; ++i)
            EXPECT_EQ(cell->pin_data.at(ports[i]).bel_pins, original_pins.at(ports[i]).bel_pins);
    }
}

TEST(AnalogueLutInputs, EveryPermutationPreservesTheNativeFunctionOnBothTileKindsAndHalves)
{
    const std::array<IdString, 6> ports{id_A, id_B, id_C, id_D, id_E, id_F};
    for (bool mlab : {false, true}) for (int half : {0, 1}) {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        Context ctx(args);
        auto *cell = ctx.createCell(ctx.id("local_lut"), id_MISTRAL_ALUT6);
        for (auto port : ports) {
            cell->addInput(port);
            cell->connectPort(port, ctx.createNet(port));
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx.createNet(id_Q));
        cell->params[id_LUT] = Property(int64_t(0x52c4b6e1987a35f0ULL), 64);
        cell->pin_data[id_D].state = PIN_INV;
        ctx.assignArchInfo();
        BelId bel;
        for (auto candidate : ctx.getBels()) {
            auto kind = ctx.getBelType(candidate);
            if (kind != id_MISTRAL_COMB && kind != id_MISTRAL_MCOMB) continue;
            const auto &where = ctx.bel_data(candidate).lab_data;
            if (where.idx == half && ctx.labs.at(where.lab).is_mlab == mlab) { bel = candidate; break; }
        }
        ASSERT_NE(bel, BelId());
        ctx.bindBel(bel, cell, STRENGTH_WEAK);
        ctx.lab_pre_route();
        ASSERT_TRUE(ctx.isBelLocationValid(bel));
        const auto where = ctx.bel_data(bel).lab_data;
        const auto original_pins = cell->pin_data;
        const auto original_params = cell->params;
        std::array<int, 6> order{0, 1, 2, 3, 4, 5};
        int permutations = 0;
        do {
            cell->pin_data = original_pins;
            for (size_t i = 0; i < ports.size(); ++i) {
                IdString wanted = original_pins.at(ports[order[i]]).bel_pins.front();
                for (size_t j = i; j < ports.size(); ++j)
                    if (cell->pin_data.at(ports[j]).bel_pins.front() == wanted) {
                        if (i != j) {
                            ASSERT_TRUE(analogue_swap_lut_pins(&ctx, cell, ports[i], ports[j]));
                        }
                        break;
                    }
            }
            const uint64_t mask = ctx.compute_lut_mask(where.lab, where.alm);
            for (int bits = 0; bits < 64; ++bits) {
                std::map<IdString, bool> values;
                for (size_t i = 0; i < ports.size(); ++i) {
                    const auto &pin = cell->pin_data.at(ports[i]);
                    values[pin.bel_pins.front()] = bool((bits >> i) & 1) ^ (pin.state != PIN_INV);
                }
                auto e = half ? id_E1 : id_E0, f = half ? id_F1 : id_F0;
                int physical = int(values[id_A]) | (int(values[id_B]) << 1) |
                               (int(values[f]) << 4) | (int(values[e]) << 5);
                physical |= int(values[values[e] ? id_D : id_C]) << 2;
                physical |= int(values[values[e] ? id_C : id_D]) << 3;
                if (mlab) {
                    // Decode the MLAB storage address with bit equations,
                    // independently of compute_lut_mask's permutation table.
                    auto bit = [&](int index) { return (physical >> index) & 1; };
                    int encoded = (bit(5) << 5) | (bit(3) << 4) | ((bit(2) ^ bit(3)) << 3) |
                                  ((bit(1) ^ bit(3)) << 2) | (bit(4) << 1) | (bit(0) ^ bit(3));
                    physical = encoded;
                }
                bool expected = (uint64_t(cell->params.at(id_LUT).as_int64()) >> bits) & 1;
                ASSERT_EQ(bool((mask >> physical) & 1), !expected)
                        << "mlab=" << mlab << " half=" << half << " permutation=" << permutations << " bits=" << bits;
            }
            ++permutations;
        } while (std::next_permutation(order.begin(), order.end()));
        EXPECT_EQ(permutations, 720);
        EXPECT_EQ(cell->params, original_params);
        EXPECT_EQ(cell->bel, bel);
        for (auto port : ports) EXPECT_EQ(cell->getPort(port)->name, port);
        cell->attrs[ctx.id("keep")] = 1;
        EXPECT_FALSE(analogue_swap_lut_pins(&ctx, cell, id_A, id_F));
        cell->attrs.erase(ctx.id("keep"));
        auto saved = cell->pin_data;
        cell->pin_data[id_A].bel_pins = cell->pin_data[id_B].bel_pins;
        EXPECT_FALSE(analogue_swap_lut_pins(&ctx, cell, id_A, id_F));
        cell->pin_data = saved;
        ctx.unbindBel(bel);
        ctx.bindBel(bel, cell, STRENGTH_LOCKED);
        EXPECT_FALSE(analogue_swap_lut_pins(&ctx, cell, id_A, id_F));
    }
}

TEST(AnalogueLutInputs, PrivateL5SwapsPreserveBothHalfFunctionsAndRejectSharedOrProtectedPins)
{
    const std::array<IdString, 5> ports{id_A, id_B, id_C, id_D, id_E};
    const std::array<IdString, 4> types{id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5};
    const std::array<uint64_t, 4> masks{0x2, 0xd2, 0x35f0, 0x987a35f0};
    for (bool mlab : {false, true}) for (int half : {0, 1})
      for (int count = 2; count <= 5; ++count) for (bool shared : {false, true}) {
        if (shared && count != 5) continue;
        SCOPED_TRACE("mlab=" + std::to_string(mlab) + " half=" + std::to_string(half) +
                     " inputs=" + std::to_string(count) + " shared=" + std::to_string(shared));
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        Context ctx(args);
        auto make = [&](const char *name, int width, uint64_t mask) {
            auto *cell = ctx.createCell(ctx.id(name), types.at(width - 2));
            for (int i = 0; i < width; ++i) {
                cell->addInput(ports[i]);
                cell->connectPort(ports[i], ctx.createNet(ctx.idf("%s_%d", name, i)));
            }
            cell->addOutput(id_Q);
            cell->connectPort(id_Q, ctx.createNet(ctx.idf("%s_q", name)));
            cell->params[id_LUT] = Property(int64_t(mask), 1 << width);
            return cell;
        };
        auto *lut = make("private_l5", count, masks.at(count - 2));
        const int peer_inputs = shared ? 5 : 3;
        auto *peer = make("opposite_lut", peer_inputs, shared ? 0x7694ae35 : 0xd2);
        if (shared) for (auto pin : {id_A, id_B}) {
            peer->disconnectPort(pin);
            peer->connectPort(pin, lut->getPort(pin));
        }
        peer->pin_data[id_B].state = PIN_INV;
        ctx.assignArchInfo();
        BelId bel;
        for (auto candidate : ctx.getBels()) {
            if (!ctx.isValidBelForCellType(lut->type, candidate)) continue;
            const auto where = ctx.bel_data(candidate).lab_data;
            if (where.idx == half && ctx.labs.at(where.lab).is_mlab == mlab) { bel = candidate; break; }
        }
        ASSERT_NE(bel, BelId());
        const auto where = ctx.bel_data(bel).lab_data;
        const auto &alm = ctx.labs.at(where.lab).alms.at(where.alm);
        auto peer_bel = alm.lut_bels[1 - half];
        ctx.bindBel(bel, lut, STRENGTH_WEAK);
        ctx.bindBel(peer_bel, peer, STRENGTH_WEAK);
        ctx.lab_pre_route();
        ASSERT_TRUE(ctx.isBelLocationValid(bel));
        ASSERT_TRUE(ctx.isBelLocationValid(peer_bel));
        auto original = lut->pin_data, peer_original = peer->pin_data;
        const auto first = ports.at(std::max(0, count - 3)), second = ports.at(count - 1);
        if (shared) for (auto pin : {id_A, id_B})
            ASSERT_EQ(lut->pin_data.at(pin).bel_pins, peer->pin_data.at(pin).bel_pins);
        ASSERT_TRUE(analogue_swap_lut_pins(&ctx, lut, first, second));
        const uint64_t mask = ctx.compute_lut_mask(where.lab, where.alm);
        // Check every physical address of the whole ALM, including unused
        // pins and the opposite half, against the two original functions.
        for (unsigned address = 0; address < 64; ++address) {
            const int selected_half = address >> 5;
            auto *cell = selected_half == half ? lut : peer;
            const int active_inputs = cell == lut ? count : peer_inputs;
            unsigned row = 0;
            for (int i = 0; i < active_inputs; ++i) {
                const auto &pin = cell->pin_data.at(ports[i]);
                auto p = pin.bel_pins.front();
                int bit = p == id_A ? 0 : p == id_B ? 1 : p.in(id_C, id_D) ? 2 :
                          p.in(id_E0, id_E1) ? 3 : 4;
                row |= unsigned(bool((address >> bit) & 1) ^ (pin.state != PIN_INV)) << i;
            }
            unsigned physical = address;
            if (mlab) {
                auto bit = [&](int i) { return (address >> i) & 1; };
                physical = (bit(5) << 5) | (bit(3) << 4) | ((bit(2) ^ bit(3)) << 3) |
                    ((bit(1) ^ bit(3)) << 2) | (bit(4) << 1) | (bit(0) ^ bit(3));
            }
            EXPECT_EQ(bool((mask >> physical) & 1), !bool((uint64_t(cell->params.at(id_LUT).as_int64()) >> row) & 1));
        }
        for (int i = 0; i < peer_inputs; ++i) {
            const auto port = ports[i];
            EXPECT_EQ(peer->pin_data.at(port).bel_pins, peer_original.at(port).bel_pins);
            EXPECT_EQ(peer->pin_data.at(port).state, peer_original.at(port).state);
        }
        EXPECT_EQ(peer->bel, peer_bel);
        EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, first, first));
        EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, first, id_F));
        if (count >= 4) EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, id_A, second));
        for (const char *attr : {"keep", "dont_touch", "BEL", "FES_SLOT"}) {
            peer->attrs[ctx.id(attr)] = 1;
            EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, first, second));
            peer->attrs.erase(ctx.id(attr));
            lut->attrs[ctx.id(attr)] = 1;
            EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, first, second));
            lut->attrs.erase(ctx.id(attr));
        }
        peer->pin_data.at(id_A).bel_pins = {half ? id_E1 : id_E0};
        EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, first, second));
        peer->pin_data = peer_original;
        auto *ff = ctx.createCell(ctx.id("occupied_ff"), id_MISTRAL_FF);
        ctx.bindBel(alm.ff_bels[0], ff, STRENGTH_WEAK);
        EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, first, second));
        ctx.unbindBel(ff->bel);
        ASSERT_TRUE(analogue_swap_lut_pins(&ctx, lut, first, second));
        for (int i = 0; i < count; ++i)
            EXPECT_EQ(lut->pin_data.at(ports[i]).bel_pins, original.at(ports[i]).bel_pins);
    }
}

TEST(AnalogueLutInputs, SoleL5PermutationsPreserveFunctionAndRequireTheWholeAlmToBeEmpty)
{
    const std::array<IdString, 5> ports{id_A, id_B, id_C, id_D, id_E};
    const std::array<IdString, 4> types{id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5};
    const std::array<uint64_t, 4> masks{0x2, 0xd2, 0x35f0, 0x987a35f0};
    for (bool mlab : {false, true}) for (int half : {0, 1}) for (int count = 2; count <= 5; ++count) {
        SCOPED_TRACE("mlab=" + std::to_string(mlab) + " half=" + std::to_string(half) +
                     " inputs=" + std::to_string(count));
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        Context ctx(args);
        auto *lut = ctx.createCell(ctx.id("sole_l5"), types.at(count - 2));
        for (int i = 0; i < count; ++i) {
            lut->addInput(ports[i]);
            lut->connectPort(ports[i], ctx.createNet(ports[i]));
        }
        lut->addOutput(id_Q);
        lut->connectPort(id_Q, ctx.createNet(id_Q));
        lut->params[id_LUT] = Property(int64_t(masks.at(count - 2)), 1 << count);
        lut->pin_data[id_B].state = PIN_INV;
        auto *peer = ctx.createCell(ctx.id("sole_l5_peer"), id_MISTRAL_ALUT2);
        ctx.createNet(ctx.id("$PACKER_GND_NET"));
        ctx.createNet(ctx.id("$PACKER_VCC_NET"));
        auto *ff = ctx.createCell(ctx.id("sole_l5_ff"), id_MISTRAL_FF);
        ctx.assignArchInfo();
        BelId bel;
        for (auto candidate : ctx.getBels()) {
            if (!ctx.isValidBelForCellType(lut->type, candidate)) continue;
            const auto where = ctx.bel_data(candidate).lab_data;
            if (where.idx == half && ctx.labs.at(where.lab).is_mlab == mlab) { bel = candidate; break; }
        }
        ASSERT_NE(bel, BelId());
        ctx.bindBel(bel, lut, STRENGTH_WEAK);
        ctx.lab_pre_route();
        ASSERT_TRUE(ctx.isBelLocationValid(bel));
        const auto where = ctx.bel_data(bel).lab_data;
        const auto &alm = ctx.labs.at(where.lab).alms.at(where.alm);
        const auto original = lut->pin_data;
        const auto params = lut->params;
        std::vector<int> order;
        for (int i = 0; i < count; ++i) order.push_back(i);
        do {
            lut->pin_data = original;
            for (int i = 0; i < count; ++i) {
                auto wanted = original.at(ports[order[i]]).bel_pins.front();
                for (int j = i; j < count; ++j) {
                    if (lut->pin_data.at(ports[j]).bel_pins.front() != wanted) continue;
                    if (i != j) {
                        const auto a = lut->pin_data.at(ports[i]).bel_pins.front();
                        const auto b = lut->pin_data.at(ports[j]).bel_pins.front();
                        if (a.in(id_A, id_B) || b.in(id_A, id_B))
                            EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, ports[i], ports[j]));
                        ASSERT_TRUE(analogue_swap_lut_pins(&ctx, lut, ports[i], ports[j], true));
                    }
                    break;
                }
            }
            const uint64_t mask = ctx.compute_lut_mask(where.lab, where.alm);
            // All 32 physical addresses of the occupied half, including
            // unused pin combinations; MLAB storage uses a different layout.
            for (unsigned address = half * 32; address < unsigned((half + 1) * 32); ++address) {
                unsigned row = 0;
                for (int i = 0; i < count; ++i) {
                    const auto &pin = lut->pin_data.at(ports[i]);
                    auto p = pin.bel_pins.front();
                    int bit = p == id_A ? 0 : p == id_B ? 1 : p.in(id_C, id_D) ? 2 :
                              p.in(id_E0, id_E1) ? 3 : 4;
                    row |= unsigned(bool((address >> bit) & 1) ^ (pin.state != PIN_INV)) << i;
                }
                unsigned physical = address;
                if (mlab) {
                    auto bit = [&](int i) { return (address >> i) & 1; };
                    physical = (bit(5) << 5) | (bit(3) << 4) | ((bit(2) ^ bit(3)) << 3) |
                               ((bit(1) ^ bit(3)) << 2) | (bit(4) << 1) | (bit(0) ^ bit(3));
                }
                EXPECT_EQ(bool((mask >> physical) & 1), !bool((masks.at(count - 2) >> row) & 1));
            }
            EXPECT_EQ(lut->params, params);
            EXPECT_EQ(lut->bel, bel);
            for (int i = 0; i < count; ++i) EXPECT_EQ(lut->pin_data.at(ports[i]).state, original.at(ports[i]).state);
        } while (std::next_permutation(order.begin(), order.end()));
        lut->pin_data = original;
        ASSERT_TRUE(analogue_lut_pin_swap_allowed(&ctx, lut, id_A, id_B, true));
        ctx.bindBel(alm.lut_bels[1 - half], peer, STRENGTH_WEAK);
        EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, id_A, id_B, true));
        ctx.unbindBel(peer->bel);
        for (auto ff_bel : alm.ff_bels) {
            ctx.bindBel(ff_bel, ff, STRENGTH_WEAK);
            EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, id_A, id_B, true));
            ctx.unbindBel(ff->bel);
        }
        for (const char *attr : {"keep", "dont_touch", "BEL", "FES_SLOT"}) {
            lut->attrs[ctx.id(attr)] = 1;
            EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, id_A, id_B, true));
            lut->attrs.erase(ctx.id(attr));
        }
        lut->pin_data.at(id_A).bel_pins = {half ? id_C : id_D};
        EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, id_A, id_B, true));
        lut->pin_data = original;
        lut->pin_data.at(id_A).bel_pins = lut->pin_data.at(id_B).bel_pins;
        EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, id_A, id_B, true));
        lut->pin_data = original;
        ctx.unbindBel(bel);
        ctx.bindBel(bel, lut, STRENGTH_LOCKED);
        EXPECT_FALSE(analogue_lut_pin_swap_allowed(&ctx, lut, id_A, id_B, true));
    }
}

// Analogue improvement of a passing design scores a sink by its setup slack
// margin over the worst path of its own clock pairs, so that the critical
// paths of every clock rank alike and no clock's critical path is slowed.
class SetupSlackMarginTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;

    NetInfo *clock(const std::string &name, delay_t period)
    {
        auto *net = ctx->createNet(ctx->id(name));
        net->is_global = true;
        net->clkconstr = std::make_unique<ClockConstraint>();
        net->clkconstr->period = DelayPair(period);
        net->clkconstr->high = DelayPair(period / 2);
        net->clkconstr->low = DelayPair(period - period / 2);
        return net;
    }

    CellInfo *ff(const std::string &name, NetInfo *clk, int x, int y)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clk);
        cell->pin_data[id_CLK].state = PIN_SIG;
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        ctx->assignArchInfo();
        // The first LAB at or right of (x, y); some columns hold no LABs
        for (int lab_x = x; lab_x < x + 8; lab_x++)
            for (auto bel : ctx->getBelsByTile(lab_x, y)) {
                if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel))
                    continue;
                ctx->bindBel(bel, cell, STRENGTH_USER);
                if (ctx->isBelLocationValid(bel))
                    return cell;
                ctx->unbindBel(bel);
            }
        ADD_FAILURE() << "No legal FF BEL for " << name << " near " << x << ',' << y;
        return cell;
    }

    void connect(CellInfo *from, std::initializer_list<CellInfo *> to)
    {
        auto *net = ctx->createNet(ctx->id(from->name.str(ctx.get()) + "$q"));
        from->connectPort(id_Q, net);
        for (auto *cell : to)
            cell->connectPort(id_DATAIN, net);
    }

    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e8;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
    }
};

TEST_F(SetupSlackMarginTest, RanksTheCriticalPathOfEveryClockAlike)
{
    // A 10 ns clock launches one net with a slow and a fast sink; a 40 ns
    // clock has a single path. Each FF sits in its own LAB.
    auto *fast_clk = clock("fast_clk", 10000), *slow_clk = clock("slow_clk", 40000);
    auto *a = ff("a", fast_clk, 10, 10), *a_slow = ff("a_slow", fast_clk, 12, 10);
    auto *a_fast = ff("a_fast", fast_clk, 14, 10);
    auto *b = ff("b", slow_clk, 10, 20), *b_end = ff("b_end", slow_clk, 12, 20);
    connect(a, {a_slow, a_fast});
    connect(b, {b_end});

    TimingAnalyser tmg(ctx.get());
    tmg.setup();
    const CellPortKey slow(a_slow->name, id_DATAIN), fast(a_fast->name, id_DATAIN), other(b_end->name, id_DATAIN);
    auto time = [&](delay_t fast_delay) {
        tmg.set_route_delay(slow, DelayPair(3000));
        tmg.set_route_delay(fast, DelayPair(fast_delay));
        tmg.set_route_delay(other, DelayPair(5000));
        tmg.run(false, false, false, true);
        return AnalogueClockTiming(tmg.get_timing_result());
    };
    const auto before = time(1000);

    // The slow sink and the 40 ns path are each their clock pair's critical
    // path, although the 40 ns path has far more slack.
    EXPECT_EQ(tmg.get_setup_slack_margin(slow), 0);
    EXPECT_EQ(tmg.get_setup_slack_margin(other), 0);
    EXPECT_GT(tmg.get_setup_slack(other), tmg.get_setup_slack(slow) + 25000);
    // The fast sink of the same net is its route delay difference away from
    // that critical path.
    EXPECT_EQ(tmg.get_setup_slack_margin(fast), 2000);
    EXPECT_EQ(tmg.get_setup_slack(fast) - tmg.get_setup_slack(slow), 2000);

    // Slowing the fast sink by less than its margin leaves every clock's
    // worst slack and Fmax as they were; by more, its clock loses both.
    EXPECT_FALSE(time(2900).worse_than(before));
    const auto slower = time(3100);
    EXPECT_TRUE(slower.worse_than(before));
    EXPECT_LT(slower.setup.at(fast_clk->name), before.setup.at(fast_clk->name));
    EXPECT_LT(slower.fmax.at(fast_clk->name), before.fmax.at(fast_clk->name));
    EXPECT_EQ(slower.setup.at(slow_clk->name), before.setup.at(slow_clk->name));
}

TEST_F(SetupSlackMarginTest, PortOnlyOnAnUntimedCrossingHasNoMargin)
{
    // Two unrelated clocks: the crossing is not timed, so its endpoint is on
    // no timed pair.
    auto *clk1 = clock("clk1", 10000), *clk2 = clock("clk2", 13000);
    auto *a = ff("a", clk1, 10, 10), *c = ff("c", clk2, 12, 10);
    connect(a, {c});
    a->connectPort(id_DATAIN, a->getPort(id_Q));
    TimingAnalyser tmg(ctx.get());
    tmg.setup();
    EXPECT_EQ(tmg.get_setup_slack_margin(CellPortKey(c->name, id_DATAIN)), std::numeric_limits<delay_t>::max());
    EXPECT_EQ(tmg.get_setup_slack_margin(CellPortKey(a->name, id_DATAIN)), 0);
}

namespace {
AnalogueClockTiming clocks(Context *ctx, std::initializer_list<std::tuple<const char *, delay_t, delay_t, float>> rows)
{
    AnalogueClockTiming t;
    for (auto &row : rows) {
        IdString clock = ctx->id(std::get<0>(row));
        t.setup[clock] = std::get<1>(row);
        t.hold[clock] = std::get<2>(row);
        t.fmax[clock] = std::get<3>(row);
    }
    return t;
}
} // namespace

TEST(AnalogueClockTiming, AnyClockLosingSetupSlackFmaxOrHoldSlackIsWorse)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    auto before = clocks(&ctx, {{"sys", 300, 100, 52.5f}, {"pixel", 4000, -50, 110.0f}});
    EXPECT_FALSE(before.worse_than(before));
    EXPECT_FALSE(before.better_than(before));
    // One clock gaining is better and not worse.
    auto gain = clocks(&ctx, {{"sys", 900, 100, 54.0f}, {"pixel", 4000, -50, 110.0f}});
    EXPECT_FALSE(gain.worse_than(before));
    EXPECT_TRUE(gain.better_than(before));
    // A gain on one clock does not pay for a loss on another.
    EXPECT_TRUE(clocks(&ctx, {{"sys", 900, 100, 54.0f}, {"pixel", 3999, -50, 110.0f}}).worse_than(before));
    EXPECT_TRUE(clocks(&ctx, {{"sys", 900, 100, 54.0f}, {"pixel", 4000, -50, 109.9f}}).worse_than(before));
    // The hold check fails at zero: positive hold slack may shrink but must
    // stay positive, and hold slack at or below zero may not get any worse.
    EXPECT_FALSE(clocks(&ctx, {{"sys", 300, 1, 52.5f}, {"pixel", 4000, -50, 110.0f}}).worse_than(before));
    EXPECT_TRUE(clocks(&ctx, {{"sys", 300, 0, 52.5f}, {"pixel", 4000, -50, 110.0f}}).worse_than(before));
    EXPECT_TRUE(clocks(&ctx, {{"sys", 300, -1, 52.5f}, {"pixel", 4000, -50, 110.0f}}).worse_than(before));
    EXPECT_TRUE(clocks(&ctx, {{"sys", 300, 100, 52.5f}, {"pixel", 4000, -51, 110.0f}}).worse_than(before));
    EXPECT_FALSE(clocks(&ctx, {{"sys", 300, 100, 52.5f}, {"pixel", 4000, -40, 110.0f}}).worse_than(before));
    auto at_zero = clocks(&ctx, {{"sys", 300, 0, 52.5f}});
    EXPECT_FALSE(at_zero.worse_than(at_zero));
    EXPECT_TRUE(clocks(&ctx, {{"sys", 300, -1, 52.5f}}).worse_than(at_zero));
    // A clock that is no longer timed is worse; a new one is ignored.
    EXPECT_TRUE(clocks(&ctx, {{"sys", 900, 100, 54.0f}}).worse_than(before));
    auto extra = clocks(&ctx, {{"sys", 300, 100, 52.5f}, {"pixel", 4000, -50, 110.0f}, {"audio", -10, -10, 1.0f}});
    EXPECT_FALSE(extra.worse_than(before));
    EXPECT_FALSE(extra.better_than(before));
}
