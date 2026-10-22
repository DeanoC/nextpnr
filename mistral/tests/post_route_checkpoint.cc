/* Live post-route trees and move metadata survive checkpoint serialization. SPDX-License-Identifier: ISC */
#include "analogue_repair.h"
#include "critical_cohort.h"
#include "json_frontend.h"
#include "jsonwrite.h"
#include "log.h"
#include "gtest/gtest.h"
#include <map>
#include <memory>
#include <sstream>
#include <string>

USING_NEXTPNR_NAMESPACE
namespace {
using Tree = std::map<std::string, std::pair<std::string, int>>;
Tree tree(Context *ctx, NetInfo *net)
{
    Tree result;
    for (const auto &entry : net->wires)
        result[ctx->getWireName(entry.first).str(ctx)] = {
            entry.second.pip == PipId() ? "" : ctx->getPipName(entry.second.pip).str(ctx), int(entry.second.strength)};
    return result;
}
std::string json(Context *ctx)
{
    std::ostringstream out; std::string filename = "post-route-test.json";
    if (!write_json_file(out, filename, ctx)) throw std::runtime_error("JSON write failed");
    return out.str();
}
json11::Json decode(const Property &p)
{
    std::string raw, error, value = p.as_string();
    for (size_t i = 0; i < value.size(); i += 2) raw += char(std::stoi(value.substr(i, 2), nullptr, 16));
    auto result = json11::Json::parse(raw, error);
    if (!error.empty()) throw std::runtime_error(error);
    return result;
}
class PostRouteCheckpoint : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *driver, *middle, *sink;
    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7"; ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e8;
        ctx->createNet(ctx->id("$PACKER_GND_NET")); ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        driver = lut("driver", Loc(24,3,0), 2); middle = lut("middle", Loc(24,3,6), 6); sink = lut("sink", Loc(24,3,12), 2);
        middle->connectPort(id_A,driver->getPort(id_Q)); middle->pin_data[id_A].state = PIN_INV;
        sink->connectPort(id_A,middle->getPort(id_Q)); sink->pin_data[id_A].state = PIN_SIG;
        ctx->assignArchInfo();
        for (auto *cell : {driver,middle,sink}) {
            const auto &at = ctx->bel_data(cell->bel).lab_data;
            ctx->assign_alm_lut_inputs(at.lab,at.alm);
        }
        int visits = 500000;
        for (auto *cell : {driver,middle,sink}) ASSERT_TRUE(critical_local_route_net(ctx.get(),cell->getPort(id_Q),visits));
        ctx->attrs[id_step] = std::string("route"); ctx->configure_bitstream();
        ctx->save_fes_pin_maps(); ctx->archInfoToAttributes();
    }
    CellInfo *lut(const char *name, Loc loc, int width)
    {
        auto *cell = ctx->createCell(ctx->id(name), width == 6 ? id_MISTRAL_ALUT6 : id_MISTRAL_ALUT2);
        cell->params[id_LUT] = width == 6 ? Property(int64_t(0x92ad6c1f58e3b407ULL),64) : Property(8,4);
        const IdString pins[] = {id_A,id_B,id_C,id_D,id_E,id_F};
        for (int p = 0; p < width; ++p) {cell->addInput(pins[p]);cell->pin_data[pins[p]].state = p == 1 ? PIN_1 : PIN_0;}
        cell->addOutput(id_Q);cell->connectPort(id_Q,ctx->createNet(ctx->idf("%s$q",name)));
        ctx->assign_comb_info(cell);ctx->assign_default_pinmap(cell);
        ctx->bindBel(ctx->getBelByLocation(loc),cell,STRENGTH_WEAK);return cell;
    }
    void expect_reload()
    {
        ArchArgs args; args.device = "5CSEBA6U23I7"; Context loaded(args);
        std::istringstream in(json(ctx.get())); ASSERT_TRUE(parse_json(in,"post-route-test.json",&loaded));
        // Restore the exact saved physical maps without scaffold locking or
        // placed-pin defaults. This test must preserve every original strength.
        for (const auto &entry : loaded.cells) {
            auto *cell = entry.second.get(); ASSERT_TRUE(loaded.restore_cell_pin_map(cell)); loaded.assign_comb_info(cell);
        }
        for (const auto &entry : ctx->cells) {
            auto *old = entry.second.get(), *now = loaded.cells.at(loaded.id(ctx->nameOf(old))).get();
            EXPECT_EQ(ctx->getBelName(old->bel).str(ctx.get()),loaded.getBelName(now->bel).str(&loaded));
            EXPECT_EQ(old->belStrength,now->belStrength);EXPECT_EQ(old->params,now->params);
            ASSERT_EQ(old->pin_data.size(),now->pin_data.size());
            for (const auto &pin : old->pin_data) {
                auto id = loaded.id(pin.first.str(ctx.get())); ASSERT_TRUE(now->pin_data.count(id));
                EXPECT_EQ(pin.second.state,now->pin_data.at(id).state);
                std::vector<std::string> a,b;
                for (auto p : pin.second.bel_pins) a.push_back(p.str(ctx.get()));
                for (auto p : now->pin_data.at(id).bel_pins) b.push_back(p.str(&loaded));
                EXPECT_EQ(a,b);
            }
        }
        for (const auto &entry : ctx->nets) {
            auto found = loaded.nets.find(loaded.id(entry.first.str(ctx.get())));
            if (found == loaded.nets.end()) { EXPECT_TRUE(entry.second->wires.empty()); continue; }
            EXPECT_EQ(tree(ctx.get(),entry.second.get()),tree(&loaded,found->second.get()));
        }
        auto saved = decode(loaded.attrs.at(loaded.id("FES_LABSTATE_V1")));
        ASSERT_EQ(saved["labs"].array_items().size(),ctx->labs.size());
        for (size_t l = 0; l < ctx->labs.size(); ++l) {
            const auto &state = saved["labs"][int(l)][3];
            EXPECT_EQ(state[0].int_value(),int(ctx->labs[l].aclr_used[0]));
            EXPECT_EQ(state[1].int_value(),int(ctx->labs[l].aclr_used[1]));
            for (size_t a = 0; a < 10; ++a) {
                const auto &alm = ctx->labs[l].alms[a];size_t p = 2 + 6*a;
                for (int value : {int(alm.l6_mode),int(alm.carry_mode),alm.clk_ena_idx[0],alm.clk_ena_idx[1],alm.aclr_idx[0],alm.aclr_idx[1]})
                    EXPECT_EQ(state[int(p++)].int_value(),value);
            }
        }
    }
};
TEST_F(PostRouteCheckpoint, AcceptedRouteTreeRefreshesRoutingAndReloadsExactly)
{
    auto *net = sink->getPort(id_Q);auto before = tree(ctx.get(),net);GpuRouteTree candidate;
    for (const auto &wire : net->wires) candidate.wires.emplace_back(wire.first,wire.second.pip);
    bool added = false;
    for (const auto &wire : net->wires) {
        for (auto pip : ctx->getPipsDownhill(wire.first)) if (ctx->checkPipAvailForNet(pip,net) && ctx->checkWireAvail(ctx->getPipDstWire(pip))) {
            candidate.wires.emplace_back(ctx->getPipDstWire(pip),pip);added = true;break;
        }
        if (added) break;
    }
    ASSERT_TRUE(added);const auto stale = json(ctx.get());
    ASSERT_TRUE(critical_route_trial(ctx.get(),net,candidate,[]{return true;}));
    ASSERT_NE(tree(ctx.get(),net),before);EXPECT_EQ(json(ctx.get()),stale);
    ctx->refresh_post_route_repair_checkpoint();EXPECT_EQ(json(ctx.get()),stale); // both budgets absent/zero
    ctx->critical_route_budget = 1;ctx->configure_bitstream();ctx->refresh_post_route_repair_checkpoint();
    EXPECT_NE(json(ctx.get()),stale);expect_reload();
    const auto current = json(ctx.get());ctx->refresh_post_route_repair_checkpoint();EXPECT_EQ(json(ctx.get()),current);
}
TEST_F(PostRouteCheckpoint, AcceptedMoveRefreshesBelPinsLabModesAndRoutes)
{
    const auto old_bel = middle->bel;const auto old_json = json(ctx.get());
    auto target = ctx->getBelByLocation(Loc(30,3,6));
    ASSERT_TRUE(critical_routed_move_trial(ctx.get(),middle,target,500000,[]{return true;}));
    EXPECT_EQ(json(ctx.get()),old_json);EXPECT_EQ(middle->bel,target);
    ctx->critical_move_route_budget = 1;ctx->configure_bitstream();ctx->refresh_post_route_repair_checkpoint();
    EXPECT_NE(json(ctx.get()),old_json);EXPECT_EQ(middle->attrs.at(ctx->id("NEXTPNR_BEL")).as_string(),ctx->getBelName(target).str(ctx.get()));
    EXPECT_EQ(middle->pin_data.at(id_A).state,PIN_INV);
    const auto &old_at = ctx->bel_data(old_bel).lab_data, &new_at = ctx->bel_data(target).lab_data;
    EXPECT_FALSE(ctx->labs.at(old_at.lab).alms.at(old_at.alm).l6_mode);EXPECT_TRUE(ctx->labs.at(new_at.lab).alms.at(new_at.alm).l6_mode);
    expect_reload();
}
TEST_F(PostRouteCheckpoint, LiveGuardReadsMovedMetadataAndRestoresAllCachedAttributes)
{
    // A genuine native timed endpoint keeps the contract check from passing
    // merely because this small fixture has an empty timing inventory.
    auto *clock = ctx->createNet(ctx->id("clk")); clock->is_global = true;
    clock->clkconstr = std::make_unique<ClockConstraint>();
    clock->clkconstr->period = DelayPair(10000);
    clock->clkconstr->high = clock->clkconstr->low = DelayPair(5000);
    auto *ff = ctx->createCell(ctx->id("timed_ff"),id_MISTRAL_FF);
    for (auto pin : {id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN}) ff->addInput(pin);
    ff->addOutput(id_Q); ff->connectPort(id_CLK,clock);
    ff->pin_data[id_ENA].state = ff->pin_data[id_ACLR].state = PIN_1;
    ff->pin_data[id_SCLR].state = ff->pin_data[id_SLOAD].state = PIN_0;
    ff->connectPort(id_Q,ctx->createNet(ctx->id("timed_ff$q")));
    ff->connectPort(id_DATAIN,ff->getPort(id_Q));
    ctx->assign_ff_info(ff);ctx->assign_default_pinmap(ff);
    ctx->bindBel(ctx->getBelByLocation(Loc(30,20,8)),ff,STRENGTH_WEAK);
    auto *io = ctx->createCell(ctx->id("unmodelled_output"),id_MISTRAL_IO);
    io->addInput(id_I);io->connectPort(id_I,middle->getPort(id_Q));
    ctx->save_fes_pin_maps();ctx->archInfoToAttributes();
    const auto stale_bel = middle->attrs.at(ctx->id("NEXTPNR_BEL"));
    auto target = ctx->getBelByLocation(Loc(30,3,6));ctx->unbindBel(middle->bel);ctx->bindBel(target,middle,STRENGTH_WEAK);
    middle->pin_data[id_A].state = PIN_SIG;
    const auto cached = json(ctx.get());
    TimingAnalyser timing(ctx.get());timing.setup(false,false,true);
    const auto old = critical_swap_endpoint_snapshot(ctx.get(),timing,timing);
    const auto live = critical_swap_live_endpoint_snapshot(ctx.get(),timing,timing);
    EXPECT_EQ(json(ctx.get()),cached);EXPECT_EQ(middle->attrs.at(ctx->id("NEXTPNR_BEL")),stale_bel);
    const auto key = json11::Json(json11::Json::array{"unmodelled_output","I"}).dump();
    ASSERT_TRUE(old["unsupported_details"].object_items().count(key));
    EXPECT_EQ(old["unsupported_details"][key]["driver"]["attrs"]["NEXTPNR_BEL"].string_value(),stale_bel.as_string());
    EXPECT_EQ(live["unsupported_details"][key]["driver"]["attrs"]["NEXTPNR_BEL"].string_value(),ctx->getBelName(target).str(ctx.get()));
    EXPECT_NE(old["unsupported_details"][key]["driver"]["attrs"]["FES_PINMAP_V1"],live["unsupported_details"][key]["driver"]["attrs"]["FES_PINMAP_V1"]);
    ASSERT_FALSE(old["endpoints"].object_items().empty());
    ASSERT_FALSE(old["clocks"].object_items().empty());
    std::string reason;
    EXPECT_FALSE(critical_route_snapshot_safe(old,live,&reason));
    EXPECT_EQ(reason,"contract changed: unsupported_details");
    ctx->settings[ctx->id("timing/reset_contracts")] = std::string("{");const auto invalid = json(ctx.get());
    EXPECT_THROW(critical_swap_live_endpoint_snapshot(ctx.get(),timing,timing),log_execution_error_exception);
    EXPECT_EQ(json(ctx.get()),invalid);had_nonfatal_error = false;
}
} // namespace
