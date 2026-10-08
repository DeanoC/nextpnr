/* Real LAB geometry, cluster closure and transactional rollback. SPDX-License-Identifier: ISC */
#include "critical_cohort.h"
#include <memory>
#include <sstream>
#include <stdexcept>
#include "critical_cohort_model.h"
#include "gtest/gtest.h"
#include "json11.hpp"
#include "log.h"
#include "place_common.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE
namespace {
class CriticalCohort : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    int serial = 0;
    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e8;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
    }
    BelId bel(int x, int y, int z) { return ctx->getBelByLocation(Loc(x, y, z)); }
    CellInfo *lut(int x, int y, int z, PlaceStrength strength = STRENGTH_WEAK)
    {
        auto *cell = ctx->createCell(ctx->idf("lut%d", serial++), id_MISTRAL_ALUT2);
        cell->addInput(id_A);
        cell->addInput(id_B);
        cell->addOutput(id_Q);
        cell->connectPort(id_A, ctx->nets.at(ctx->id("$PACKER_GND_NET")).get());
        cell->connectPort(id_B, ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get());
        cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", ctx->nameOf(cell))));
        ctx->assign_comb_info(cell);
        ctx->assign_default_pinmap(cell);
        ctx->bindBel(bel(x, y, z), cell, strength);
        return cell;
    }
    std::vector<CellInfo *> carry(int x, int y, int length)
    {
        std::vector<CellInfo *> result;
        for (int i = 0; i < length; ++i) {
            auto *cell = ctx->createCell(ctx->idf("carry%d", serial++), id_MISTRAL_ALUT_ARITH);
            for (IdString pin : {id_A, id_B, id_C, id_D0, id_D1, id_CI})
                cell->addInput(pin);
            cell->addOutput(id_SO);
            cell->addOutput(id_CO);
            cell->connectPort(id_SO, ctx->createNet(ctx->idf("%s$so", ctx->nameOf(cell))));
            cell->connectPort(id_CO, ctx->createNet(ctx->idf("%s$co", ctx->nameOf(cell))));
            if (i)
                cell->connectPort(id_CI, result.back()->getPort(id_CO));
            cell->cluster = i ? result.front()->name : cell->name;
            cell->constr_abs_z = true;
            cell->constr_z = ((i / 2) % 10) * 6 + i % 2;
            cell->constr_y = -(i / 20);
            if (i)
                result.front()->constr_children.push_back(cell);
            ctx->bindBel(bel(x, y - i / 20, cell->constr_z), cell, STRENGTH_STRONG);
            result.push_back(cell);
        }
        ctx->assignArchInfo();
        return result;
    }
    NetInfo *clock(const char *name, int period)
    {
        auto *net = ctx->createNet(ctx->id(name));
        net->is_global = true;
        net->clkconstr = std::make_unique<ClockConstraint>();
        net->clkconstr->period = DelayPair(period);
        net->clkconstr->high = net->clkconstr->low = DelayPair(period / 2);
        return net;
    }
    CellInfo *ff(NetInfo *clk, int x, int y, int z, NetInfo *data = nullptr)
    {
        auto *cell = ctx->createCell(ctx->idf("ff%d", serial++), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clk);
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", ctx->nameOf(cell))));
        cell->connectPort(id_DATAIN, data ? data : cell->getPort(id_Q));
        ctx->assignArchInfo();
        ctx->bindBel(bel(x, y, z), cell, STRENGTH_WEAK);
        return cell;
    }
    CellInfo *timing_cone(NetInfo *clk)
    {
        auto *launch = ff(clk, 30, 20, 8);
        auto *logic = lut(24, 3, 0);
        logic->disconnectPort(id_A);
        logic->connectPort(id_A, launch->getPort(id_Q));
        ff(clk, 30, 20, 14, logic->getPort(id_Q));
        ctx->assignArchInfo();
        return logic;
    }
    json11::Json guidance(CellInfo *logic)
    {
        using J = json11::Json;
        const auto &driver = logic->getPort(id_A)->driver;
        const auto &sink = *logic->getPort(id_Q)->users.begin();
        TimingAnalyser timing(ctx.get());
        timing.setup(false, false, true);
        double constraint = timing.get_timing_result().clock_fmax.at(ctx->id("clk")).constraint;
        auto pin = [&](CellInfo *cell, IdString port) {
            return J::object{{"cell", ctx->nameOf(cell)}, {"port", port.str(ctx.get())}};
        };
        return J::object{{"timing_summary", J::object{{"final_analogue_model", true}}},
                         {"fmax", J::object{{"clk", J::object{{"achieved", 1}, {"constraint", constraint}}}}},
                         {"critical_paths",
                          J::array{J::object{{"from", "posedge clk"},
                                             {"to", "posedge clk"},
                                             {"path", J::array{J::object{{"type", "routing"},
                                                                         {"delay", 3},
                                                                         {"from", pin(driver.cell, driver.port)},
                                                                         {"to", pin(logic, id_A)}},
                                                               J::object{{"type", "setup"},
                                                                         {"to", pin(sink.cell, sink.port)}}}}}}}};
    }
    json11::Json route_through_guidance(CellInfo *logic)
    {
        using J = json11::Json;
        auto document = guidance(logic).object_items();
        auto path = document.at("critical_paths").array_items().front().object_items();
        auto *capture = (*logic->getPort(id_Q)->users.begin()).cell;
        auto pin = [&](const std::string &name, const char *port) { return J::object{{"cell", name}, {"port", port}}; };
        const auto rt = capture->name.str(ctx.get()) + "$ROUTETHRU";
        auto endpoint = pin(capture->name.str(ctx.get()), "DATAIN");
        path["path"] =
                J::array{J::object{{"type", "routing"},
                                   {"delay", 3},
                                   {"from", pin(logic->name.str(ctx.get()), "Q")},
                                   {"to", pin(rt, "A")}},
                         J::object{{"type", "logic"}, {"delay", 0.512}, {"from", pin(rt, "A")}, {"to", pin(rt, "Q")}},
                         J::object{{"type", "routing"}, {"delay", 0}, {"from", pin(rt, "Q")}, {"to", endpoint}},
                         J::object{{"type", "setup"}, {"to", endpoint}}};
        document["critical_paths"] = J::array{path};
        return document;
    }
    void move_logic(CellInfo *logic)
    {
        ctx->unbindBel(logic->bel);
        ctx->bindBel(bel(30, 20, 0), logic, STRENGTH_WEAK);
        for (const auto &entry : ctx->cells)
            ASSERT_TRUE(ctx->isBelLocationValid(entry.second->bel));
    }
    struct Snapshot
    {
        std::vector<std::tuple<CellInfo *, BelId, PlaceStrength>> cells;
        std::vector<LABInfo> labs;
        explicit Snapshot(Context *ctx) : labs(ctx->labs)
        {
            for (const auto &entry : ctx->cells)
                cells.emplace_back(entry.second.get(), entry.second->bel, entry.second->belStrength);
        }
        void check(Context *ctx) const
        {
            EXPECT_EQ(ctx->cells.size(), cells.size());
            for (const auto &saved : cells) {
                auto *cell = std::get<0>(saved);
                EXPECT_EQ(cell->bel, std::get<1>(saved));
                EXPECT_EQ(cell->belStrength, std::get<2>(saved));
                EXPECT_EQ(ctx->getBoundBelCell(cell->bel), cell);
                EXPECT_EQ(get_constraints_distance(ctx, cell), 0);
            }
            ASSERT_EQ(ctx->labs.size(), labs.size());
            for (size_t i = 0; i < labs.size(); ++i)
                for (int j = 0; j < 10; ++j)
                    EXPECT_EQ(ctx->labs[i].alms[j].unique_input_count, labs[i].alms[j].unique_input_count);
            ctx->check();
        }
    };
};

TEST_F(CriticalCohort, EmptyDestinationMovesWholeCohort)
{
    auto *a = lut(30, 20, 0), *b = lut(30, 20, 6);
    CriticalCohortStats stats;
    ASSERT_TRUE(critical_cohort_trial(
            ctx.get(), {a, b}, -6, 0, 100,
            [&]() {
                EXPECT_EQ(a->bel, bel(24, 20, 0));
                EXPECT_EQ(b->bel, bel(24, 20, 6));
                return true;
            },
            stats));
    EXPECT_EQ(stats.cells, 2);
    EXPECT_EQ(stats.displaced, 0);
    EXPECT_TRUE(stats.timed);
}

TEST_F(CriticalCohort, SingleRegisterCanPackWithItsDriverAtAnotherAlmSlot)
{
    auto *logic = lut(24, 20, 6);
    auto *capture = ff(clock("clk", 20000), 30, 20, 14, logic->getPort(id_Q));
    CriticalCohortStats stats;
    ASSERT_TRUE(critical_cohort_trial(
            ctx.get(), {capture}, -6, 0, 1000,
            [&]() {
                EXPECT_EQ(logic->bel, bel(24, 20, 6));
                EXPECT_EQ(capture->bel, bel(24, 20, 8));
                EXPECT_EQ(ctx->predictArcDelay(logic->getPort(id_Q), {capture, id_DATAIN}), 20);
                const auto &loc = ctx->bel_data(capture->bel).lab_data;
                critical_cohort_pin_preview(
                        ctx.get(), [&]() { EXPECT_EQ(ctx->get_alm_route_through_ff(loc.lab, loc.alm, 0), nullptr); });
                return true;
            },
            stats, -6));
    EXPECT_TRUE(stats.timed);
    ctx->check();
}

TEST_F(CriticalCohort, RejectedRegisterSlotChangeRestoresEveryParticipant)
{
    auto *logic = lut(24, 20, 6);
    auto *capture = ff(clock("clk", 20000), 30, 20, 14, logic->getPort(id_Q));
    Snapshot saved(ctx.get());
    CriticalCohortStats stats;
    EXPECT_FALSE(critical_cohort_trial(ctx.get(), {capture}, -6, 0, 1000, []() { return false; }, stats, -6));
    EXPECT_TRUE(stats.timed);
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, MultiCellCohortsCannotChangeTheirAlmSlots)
{
    auto *clk = clock("clk", 20000);
    auto *a = ff(clk, 30, 20, 8), *b = ff(clk, 30, 20, 14);
    Snapshot saved(ctx.get());
    CriticalCohortStats stats;
    EXPECT_FALSE(critical_cohort_trial(ctx.get(), {a, b}, -6, 0, 1000, []() { return true; }, stats, -6));
    EXPECT_FALSE(stats.timed);
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, PinPreviewMatchesRoutingWithoutInsertingRouteThroughs)
{
    auto *logic = lut(30, 20, 0);
    auto *reg = ff(clock("clk", 20000), 30, 20, 4);
    const auto pins = logic->pin_data;
    const size_t cells = ctx->cells.size(), nets = ctx->nets.size();
    std::vector<IdString> mapped_a, mapped_b;
    const auto &location = ctx->bel_data(logic->bel).lab_data;
    critical_cohort_pin_preview(ctx.get(), [&]() {
        mapped_a = logic->pin_data.at(id_A).bel_pins;
        mapped_b = logic->pin_data.at(id_B).bel_pins;
        EXPECT_EQ(mapped_a, std::vector<IdString>{id_C});
        EXPECT_EQ(mapped_b, std::vector<IdString>{id_E0});
        EXPECT_EQ(ctx->cells.size(), cells);
        EXPECT_EQ(ctx->nets.size(), nets);
        EXPECT_EQ(reg->getPort(id_DATAIN), reg->getPort(id_Q));
    });
    for (const auto &pin : pins) {
        EXPECT_EQ(logic->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
        EXPECT_EQ(logic->pin_data.at(pin.first).state, pin.second.state);
    }
    ctx->reassign_alm_inputs(location.lab, location.alm);
    EXPECT_EQ(logic->pin_data.at(id_A).bel_pins, mapped_a);
    EXPECT_EQ(logic->pin_data.at(id_B).bel_pins, mapped_b);
    EXPECT_EQ(ctx->cells.size(), cells + 1);
    EXPECT_NE(reg->getPort(id_DATAIN), reg->getPort(id_Q));
    ctx->check();
}

TEST_F(CriticalCohort, PinPreviewTimesSharedInputsOnTheirPhysicalPins)
{
    auto *a = lut(30, 20, 0);
    lut(30, 20, 1);
    DelayQuad original;
    ASSERT_TRUE(ctx->getCellDelay(a, id_A, id_Q, original));
    critical_cohort_pin_preview(ctx.get(), [&]() {
        EXPECT_EQ(a->pin_data.at(id_A).bel_pins, std::vector<IdString>{id_A});
        EXPECT_EQ(a->pin_data.at(id_B).bel_pins, std::vector<IdString>{id_B});
        DelayQuad actual;
        ASSERT_TRUE(ctx->getCellDelay(a, id_A, id_Q, actual));
        EXPECT_GT(actual.maxDelay(), original.maxDelay() + 100);
    });
    DelayQuad restored;
    ASSERT_TRUE(ctx->getCellDelay(a, id_A, id_Q, restored));
    EXPECT_EQ(restored.maxDelay(), original.maxDelay());
}

TEST_F(CriticalCohort, PinPreviewExceptionRestoresPinsAndLabModes)
{
    auto chain = carry(30, 20, 2);
    auto *logic = lut(24, 20, 0);
    const auto pins = logic->pin_data;
    const auto lab = ctx->bel_data(chain.front()->bel).lab_data.lab;
    ASSERT_FALSE(ctx->labs.at(lab).alms[0].carry_mode);
    Snapshot saved(ctx.get());
    EXPECT_THROW(critical_cohort_pin_preview(ctx.get(),
                                             [&]() {
                                                 EXPECT_TRUE(ctx->labs.at(lab).alms[0].carry_mode);
                                                 throw std::runtime_error("inspection failed");
                                             }),
                 std::runtime_error);
    EXPECT_FALSE(ctx->labs.at(lab).alms[0].carry_mode);
    for (const auto &pin : pins)
        EXPECT_EQ(logic->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, RouteThroughPreviewMatchesInsertedGraphEarlyAndLateTiming)
{
    auto *clk = clock("clk", 3000);
    auto *logic = timing_cone(clk);
    auto *capture = (*logic->getPort(id_Q)->users.begin()).cell;
    const auto cells = ctx->cells.size(), nets = ctx->nets.size();
    const auto input = capture->getPort(id_DATAIN);
    std::vector<EndpointClockPairTiming> predicted;
    float slack = 0;
    critical_cohort_pin_preview(ctx.get(), [&]() {
        TimingAnalyser timing(ctx.get());
        critical_cohort_setup_timing(ctx.get(), timing);
        slack = timing.get_setup_slack({capture->name, id_DATAIN});
        ASSERT_TRUE(timing.get_endpoint_clock_pair_timings({capture->name, id_DATAIN}, predicted));
        EXPECT_EQ(ctx->cells.size(), cells);
        EXPECT_EQ(ctx->nets.size(), nets);
        EXPECT_EQ(capture->getPort(id_DATAIN), input);
    });
    // Actually insert the buffers and run native STA on that larger graph.
    for (uint32_t lab = 0; lab < ctx->labs.size(); ++lab)
        for (uint8_t alm = 0; alm < 10; ++alm)
            ctx->reassign_alm_inputs(lab, alm);
    ASSERT_GT(ctx->cells.size(), cells);
    ASSERT_NE(capture->getPort(id_DATAIN), input);
    TimingAnalyser actual(ctx.get());
    actual.setup(false, false, true);
    EXPECT_EQ(actual.get_setup_slack({capture->name, id_DATAIN}), slack);
    std::vector<EndpointClockPairTiming> measured;
    ASSERT_TRUE(actual.get_endpoint_clock_pair_timings({capture->name, id_DATAIN}, measured));
    ASSERT_EQ(measured.size(), predicted.size());
    ASSERT_FALSE(measured.empty());
    for (size_t i = 0; i < measured.size(); ++i) {
        EXPECT_EQ(measured[i].min_path_delay, predicted[i].min_path_delay);
        EXPECT_EQ(measured[i].max_path_delay, predicted[i].max_path_delay);
        EXPECT_EQ(measured[i].setup_margin, predicted[i].setup_margin);
        EXPECT_EQ(measured[i].hold_margin, predicted[i].hold_margin);
    }
    ctx->check();
}

TEST_F(CriticalCohort, RouteThroughSelectionMatchesFourRegisterRoutingPriority)
{
    ctx->lab_ff4 = true;
    auto *clk = clock("clk", 20000);
    std::vector<CellInfo *> regs;
    for (int i = 0; i < 4; ++i)
        regs.push_back(ff(clk, 30, 20, 2 + i));
    const auto loc = ctx->bel_data(regs.front()->bel).lab_data;
    const auto cells = ctx->cells.size();
    critical_cohort_pin_preview(ctx.get(), [&]() {
        EXPECT_EQ(ctx->get_alm_route_through_ff(loc.lab, loc.alm, 0), regs[0]);
        EXPECT_EQ(ctx->get_alm_route_through_ff(loc.lab, loc.alm, 1), regs[3]);
        TimingAnalyser timing(ctx.get());
        critical_cohort_setup_timing(ctx.get(), timing);
        EXPECT_EQ(ctx->cells.size(), cells);
    });
    std::vector<NetInfo *> inputs;
    for (auto *reg : regs)
        inputs.push_back(reg->getPort(id_DATAIN));
    ctx->reassign_alm_inputs(loc.lab, loc.alm);
    EXPECT_NE(regs[0]->getPort(id_DATAIN), inputs[0]);
    EXPECT_EQ(regs[1]->getPort(id_DATAIN), inputs[1]);
    EXPECT_EQ(regs[2]->getPort(id_DATAIN), inputs[2]);
    EXPECT_NE(regs[3]->getPort(id_DATAIN), inputs[3]);
    ctx->check();
}

TEST_F(CriticalCohort, RouteThroughPreviewLeavesFrozenLabTimingUntouched)
{
    auto *clk = clock("clk", 3000);
    auto *launch = ff(clk, 30, 20, 8);
    auto *capture = ff(clk, 30, 20, 14, launch->getPort(id_Q));
    launch->belStrength = STRENGTH_LOCKED;
    Snapshot saved(ctx.get());
    TimingAnalyser ordinary(ctx.get());
    ordinary.setup(false, false, true);
    critical_cohort_pin_preview(ctx.get(), [&]() {
        TimingAnalyser preview(ctx.get());
        critical_cohort_setup_timing(ctx.get(), preview);
        EXPECT_EQ(preview.get_setup_slack({capture->name, id_DATAIN}),
                  ordinary.get_setup_slack({capture->name, id_DATAIN}));
    });
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, RouteThroughSelectionExcludesOccupiedLutCarryAndLut6Modes)
{
    auto chain = carry(30, 20, 1);
    auto *clk = clock("clk", 20000);
    auto *carry_ff = ff(clk, 30, 20, 4);
    auto *logic = lut(24, 20, 0);
    ff(clk, 24, 20, 2);
    const auto carry_loc = ctx->bel_data(carry_ff->bel).lab_data;
    const auto logic_loc = ctx->bel_data(logic->bel).lab_data;
    auto *l6 = lut(18, 20, 0);
    l6->type = id_MISTRAL_ALUT6;
    for (IdString pin : {id_C, id_D, id_E, id_F})
        l6->addInput(pin);
    ctx->assign_comb_info(l6);
    ctx->assign_default_pinmap(l6);
    auto *l6_ff = ff(clk, 18, 20, 4);
    const auto l6_loc = ctx->bel_data(l6_ff->bel).lab_data;
    critical_cohort_pin_preview(ctx.get(), [&]() {
        EXPECT_EQ(ctx->get_alm_route_through_ff(logic_loc.lab, logic_loc.alm, 0), nullptr);
        EXPECT_EQ(ctx->get_alm_route_through_ff(l6_loc.lab, l6_loc.alm, 1), nullptr);
        // Empty carry half must also reject a route-through.
        EXPECT_EQ(ctx->get_alm_route_through_ff(carry_loc.lab, carry_loc.alm, 1), nullptr);
    });
}

TEST_F(CriticalCohort, NeighborhoodIncludesDirectDataNeighborsOnly)
{
    auto *clk = clock("clk", 20000);
    auto *source = ff(clk, 30, 20, 8);
    auto *logic = lut(30, 20, 0);
    logic->disconnectPort(id_A);
    logic->connectPort(id_A, source->getPort(id_Q));
    auto *sink = ff(clk, 30, 20, 2, logic->getPort(id_Q));
    auto *unrelated = ff(clk, 30, 20, 14);
    auto *distant = ff(clk, 24, 20, 2, logic->getPort(id_Q));
    auto cells = critical_cohort_neighborhood(ctx.get(), logic);
    EXPECT_EQ(cells.size(), 3);
    for (auto *cell : {logic, source, sink})
        EXPECT_NE(std::find(cells.begin(), cells.end(), cell), cells.end());
    for (auto *cell : {unrelated, distant})
        EXPECT_EQ(std::find(cells.begin(), cells.end(), cell), cells.end());
}

TEST_F(CriticalCohort, NeighborhoodClosesNeighborCarryCluster)
{
    auto chain = carry(30, 20, 22);
    auto *logic = ff(clock("clk", 20000), 30, 20, 2, chain.front()->getPort(id_SO));
    auto cells = critical_cohort_neighborhood(ctx.get(), logic);
    EXPECT_EQ(cells.size(), 23);
    EXPECT_NE(std::find(cells.begin(), cells.end(), chain.back()), cells.end());
}

TEST_F(CriticalCohort, UnrelatedDestinationGuestsStayAtHome)
{
    auto *source = lut(30, 20, 0);
    auto *collision = lut(24, 20, 0);
    auto *unrelated = lut(24, 20, 6);
    CriticalCohortStats stats;
    ASSERT_TRUE(critical_cohort_trial(ctx.get(), {source}, -6, 0, 1000, []() { return true; }, stats));
    EXPECT_EQ(source->bel, bel(24, 20, 0));
    EXPECT_EQ(ctx->getBelLocation(collision->bel).x, 24);
    EXPECT_EQ(ctx->getBelLocation(collision->bel).y, 20);
    EXPECT_EQ(unrelated->bel, bel(24, 20, 6));
}

TEST_F(CriticalCohort, ClearsAnOccupiedLabAndPreservesGuests)
{
    std::vector<CellInfo *> source, guests;
    // Neither destination has seventeen empty COMB sites. A complete exchange
    // succeeds without requiring a vacant LAB anywhere on the device.
    for (int i = 0; i < 17; ++i) {
        int z = (i / 2) * 6 + i % 2;
        source.push_back(lut(30, 20, z));
        guests.push_back(lut(24, 20, z));
    }
    // Reproduce the issue's topology: one LUT and sixteen direct same-LAB
    // data neighbors, rather than a manually selected collection of cells.
    for (auto *cell : source)
        ctx->unbindBel(cell->bel);
    for (size_t i = 1; i < source.size(); ++i) {
        source[i]->disconnectPort(id_A);
        source[i]->connectPort(id_A, source.front()->getPort(id_Q));
    }
    ctx->assignArchInfo();
    for (int i = 0; i < 17; ++i)
        ctx->bindBel(bel(30, 20, (i / 2) * 6 + i % 2), source[i], STRENGTH_WEAK);
    auto cohort = critical_cohort_neighborhood(ctx.get(), source.front());
    ASSERT_EQ(cohort.size(), 17);
    CriticalCohortStats stats;
    ASSERT_TRUE(critical_cohort_trial(ctx.get(), cohort, -6, 0, 1000, []() { return true; }, stats));
    EXPECT_EQ(stats.cells, 17);
    EXPECT_EQ(stats.displaced, 17);
    for (int i = 0; i < 17; ++i) {
        int z = (i / 2) * 6 + i % 2;
        EXPECT_EQ(source[i]->bel, bel(24, 20, z));
        EXPECT_NE(guests[i]->bel, BelId());
        EXPECT_TRUE(ctx->isBelLocationValid(guests[i]->bel));
    }
    ctx->check();
}

TEST_F(CriticalCohort, IncludesCarryMembersOutsideTheSelectedLab)
{
    auto chain = carry(30, 20, 22);
    CriticalCohortStats stats;
    ASSERT_TRUE(critical_cohort_trial(ctx.get(), {chain.front()}, -6, 0, 1000, []() { return true; }, stats));
    EXPECT_EQ(stats.cells, 22);
    for (auto *cell : chain) {
        EXPECT_EQ(ctx->getBelLocation(cell->bel).x, 24);
        EXPECT_EQ(cell->belStrength, STRENGTH_STRONG);
        EXPECT_EQ(get_constraints_distance(ctx.get(), cell), 0);
    }
}

TEST_F(CriticalCohort, DisplacesWholeDestinationCarryCluster)
{
    auto *source = lut(30, 20, 0);
    auto guests = carry(24, 20, 22);
    CriticalCohortStats stats;
    ASSERT_TRUE(critical_cohort_trial(ctx.get(), {source}, -6, 0, 1000, []() { return true; }, stats));
    EXPECT_EQ(stats.displaced, 22);
    for (auto *cell : guests) {
        EXPECT_EQ(ctx->getBelLocation(cell->bel).x, ctx->getBelLocation(guests.front()->bel).x);
        EXPECT_EQ(get_constraints_distance(ctx.get(), cell), 0);
        EXPECT_EQ(cell->belStrength, STRENGTH_STRONG);
    }
}

TEST_F(CriticalCohort, RejectedTimingRestoresBindingsStrengthsAndInputCounts)
{
    auto *source = lut(30, 20, 0);
    carry(24, 20, 22);
    Snapshot saved(ctx.get());
    CriticalCohortStats stats;
    EXPECT_FALSE(critical_cohort_trial(ctx.get(), {source}, -6, 0, 1000, []() { return false; }, stats));
    EXPECT_TRUE(stats.timed);
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, ExceptionDuringAcceptanceRestoresThePlacement)
{
    auto *source = lut(30, 20, 0);
    lut(24, 20, 0);
    Snapshot saved(ctx.get());
    CriticalCohortStats stats;
    EXPECT_THROW(critical_cohort_trial(
                         ctx.get(), {source}, -6, 0, 1000, []() -> bool { throw std::runtime_error("timing failed"); },
                         stats),
                 std::runtime_error);
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, SearchExhaustionRestoresPartiallyMovedParticipants)
{
    auto *source = lut(30, 20, 0);
    carry(24, 20, 22);
    lut(30, 19, 0, STRENGTH_USER);
    Snapshot saved(ctx.get());
    CriticalCohortStats stats;
    EXPECT_FALSE(critical_cohort_trial(ctx.get(), {source}, -6, 0, 1, []() { return true; }, stats));
    EXPECT_EQ(stats.nodes, 1);
    EXPECT_FALSE(stats.timed);
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, UnrelatedProtectedDestinationIsUntouched)
{
    auto *source = lut(30, 20, 0);
    auto *fixed = lut(24, 20, 6, STRENGTH_USER);
    CriticalCohortStats stats;
    EXPECT_TRUE(critical_cohort_trial(ctx.get(), {source}, -6, 0, 1000, []() { return true; }, stats));
    EXPECT_EQ(fixed->bel, bel(24, 20, 6));
    EXPECT_EQ(fixed->belStrength, STRENGTH_USER);
}

TEST_F(CriticalCohort, CollidingProtectedDestinationIsUntouched)
{
    auto *source = lut(30, 20, 0);
    lut(24, 20, 0, STRENGTH_USER);
    Snapshot saved(ctx.get());
    CriticalCohortStats stats;
    EXPECT_FALSE(critical_cohort_trial(ctx.get(), {source}, -6, 0, 1000, []() { return true; }, stats));
    EXPECT_FALSE(stats.timed);
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, ProtectedMemberOutsideTheLabBlocksClusterMovement)
{
    auto chain = carry(30, 20, 22);
    chain.back()->belStrength = STRENGTH_USER;
    Snapshot saved(ctx.get());
    CriticalCohortStats stats;
    EXPECT_FALSE(critical_cohort_trial(ctx.get(), {chain.front()}, -6, 0, 1000, []() { return true; }, stats));
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, MalformedClusterIsRejectedBeforeAnyMove)
{
    auto chain = carry(30, 20, 2);
    chain.back()->constr_z = 6;
    auto home = chain.front()->bel;
    CriticalCohortStats stats;
    EXPECT_FALSE(critical_cohort_trial(ctx.get(), {chain.front()}, -6, 0, 1000, []() { return true; }, stats));
    EXPECT_EQ(chain.front()->bel, home);
    EXPECT_EQ(stats.nodes, 0);
}

TEST_F(CriticalCohort, NativeTimingAcceptsCompleteSetupImprovement)
{
    auto *logic = timing_cone(clock("clk", 3000));
    TimingAnalyser before(ctx.get());
    before.with_clock_skew = true;
    before.setup(false, false, true);
    move_logic(logic);
    TimingAnalyser after(ctx.get());
    after.with_clock_skew = true;
    after.setup(false, false, true);
    EXPECT_TRUE(critical_cohort_timing_safe(ctx.get(), before, after));
}

TEST_F(CriticalCohort, NativeTimingRejectsRegressingAPassingClock)
{
    auto *logic = timing_cone(clock("clk", 3000));
    auto *other = clock("other", 20000);
    auto *launch = ff(other, 24, 3, 8);
    logic->disconnectPort(id_B);
    logic->connectPort(id_B, launch->getPort(id_Q));
    ff(other, 24, 3, 14, logic->getPort(id_Q));
    ctx->assignArchInfo();
    TimingAnalyser before(ctx.get());
    before.with_clock_skew = true;
    before.setup(false, false, true);
    ASSERT_GE(before.get_timing_result().clock_setup_slack.at(other->name), 0);
    move_logic(logic);
    TimingAnalyser after(ctx.get());
    after.with_clock_skew = true;
    after.setup(false, false, true);
    EXPECT_LT(after.get_timing_result().clock_fmax.at(other->name).achieved,
              before.get_timing_result().clock_fmax.at(other->name).achieved);
    EXPECT_FALSE(critical_cohort_timing_safe(ctx.get(), before, after));
}

TEST_F(CriticalCohort, ImprovingWorstSetupCannotWorsenAnotherFailingEndpoint)
{
    auto *clk = clock("clk", 3000);
    auto *primary = timing_cone(clk);
    auto *launch = ff(clk, 30, 21, 8);
    auto *secondary = lut(24, 10, 0);
    secondary->disconnectPort(id_A);
    secondary->connectPort(id_A, launch->getPort(id_Q));
    auto *capture = ff(clk, 30, 21, 14, secondary->getPort(id_Q));
    TimingAnalyser before(ctx.get());
    before.setup(false, false, true);
    ASSERT_LT(before.get_setup_slack({capture->name, id_DATAIN}), 0);
    move_logic(primary);
    ctx->unbindBel(secondary->bel);
    ctx->bindBel(bel(24, 8, 0), secondary, STRENGTH_WEAK);
    TimingAnalyser after(ctx.get());
    after.setup(false, false, true);
    EXPECT_LT(after.get_setup_slack({capture->name, id_DATAIN}), before.get_setup_slack({capture->name, id_DATAIN}));
    ASSERT_GT(after.get_timing_result().clock_setup_slack.at(clk->name),
              before.get_timing_result().clock_setup_slack.at(clk->name));
    EXPECT_FALSE(critical_cohort_timing_safe(ctx.get(), before, after));
}

TEST_F(CriticalCohort, ImprovedClockPairCannotMakeAPassingEndpointFail)
{
    auto *clk = clock("clk", 3000);
    auto *primary = timing_cone(clk);
    auto *launch = ff(clk, 30, 21, 8);
    auto *secondary = lut(30, 21, 0);
    secondary->disconnectPort(id_A);
    secondary->connectPort(id_A, launch->getPort(id_Q));
    auto *capture = ff(clk, 30, 21, 14, secondary->getPort(id_Q));
    TimingAnalyser before(ctx.get());
    before.setup(false, false, true);
    ASSERT_GE(before.get_setup_slack({capture->name, id_DATAIN}), 0);
    move_logic(primary);
    ctx->unbindBel(secondary->bel);
    ctx->bindBel(bel(24, 8, 0), secondary, STRENGTH_WEAK);
    TimingAnalyser after(ctx.get());
    after.setup(false, false, true);
    ASSERT_GT(after.get_timing_result().clock_setup_slack.at(clk->name),
              before.get_timing_result().clock_setup_slack.at(clk->name));
    EXPECT_LT(after.get_setup_slack({capture->name, id_DATAIN}), 0);
    EXPECT_FALSE(critical_cohort_timing_safe(ctx.get(), before, after));
}

TEST_F(CriticalCohort, RoutedFocusCanImproveWithoutChangingThePredictedWorstEndpoint)
{
    auto *logic = timing_cone(clock("clk", 3000));
    auto *other = clock("other", 1000);
    auto *launch = ff(other, 30, 21, 8);
    auto *other_logic = lut(24, 4, 0);
    other_logic->disconnectPort(id_A);
    other_logic->connectPort(id_A, launch->getPort(id_Q));
    auto *other_capture = ff(other, 30, 21, 14, other_logic->getPort(id_Q));
    std::vector<CellPortKey> focus;
    for (const auto &user : logic->getPort(id_Q)->users)
        focus.emplace_back(user);
    TimingAnalyser before(ctx.get());
    before.with_clock_skew = false;
    before.setup(false, false, true);
    move_logic(logic);
    TimingAnalyser after(ctx.get());
    after.with_clock_skew = false;
    after.setup(false, false, true);
    EXPECT_FALSE(critical_cohort_timing_safe(ctx.get(), before, after));
    EXPECT_TRUE(critical_cohort_timing_safe(ctx.get(), before, after, focus));
    // Guidance cannot accept improvement at an unrelated, unchanged endpoint.
    EXPECT_FALSE(critical_cohort_timing_safe(ctx.get(), before, after, {{other_capture->name, id_DATAIN}}));
}

TEST_F(CriticalCohort, NativeTimingRejectsRelatedClockHoldFailure)
{
    auto *clk = clock("clk", 3000);
    auto *logic = timing_cone(clk);
    auto *related = clock("related", 3000);
    clk->clkconstr->phase_group = related->clkconstr->phase_group = ctx->id("phase_group");
    related->clkconstr->phase_shift = 2600;
    auto *capture = ff(related, 30, 21, 8, logic->getPort(id_Q));
    TimingAnalyser before(ctx.get());
    before.with_clock_skew = true;
    before.setup(false, false, true);
    std::vector<EndpointClockPairTiming> old_rows;
    ASSERT_TRUE(before.get_endpoint_clock_pair_timings({capture->name, id_DATAIN}, old_rows));
    ASSERT_FALSE(old_rows.empty());
    ASSERT_TRUE(old_rows.front().hold_margin);
    ASSERT_GE(*old_rows.front().hold_margin, 0);
    move_logic(logic);
    TimingAnalyser after(ctx.get());
    after.with_clock_skew = true;
    after.setup(false, false, true);
    // A late capture-clock route makes the shortened data path fail hold.
    // Exercise native early/late STA rather than editing its result rows.
    after.set_route_delay({capture->name, id_CLK}, DelayPair(4000));
    after.run(false, false, false, true);
    std::vector<EndpointClockPairTiming> new_rows;
    ASSERT_TRUE(after.get_endpoint_clock_pair_timings({capture->name, id_DATAIN}, new_rows));
    ASSERT_TRUE(new_rows.front().hold_margin);
    EXPECT_LT(*new_rows.front().hold_margin, 0);
    EXPECT_FALSE(critical_cohort_timing_safe(ctx.get(), before, after));
}

TEST_F(CriticalCohort, PassingDesignRemainsUnmoved)
{
    timing_cone(clock("clk", 20000));
    ctx->settings[ctx->id("timing_driven")] = true;
    Snapshot saved(ctx.get());
    repair_critical_cohorts(ctx.get(), 16);
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, RoutedGuidanceRepairsAMatchingNativeTimingCone)
{
    auto *logic = timing_cone(clock("clk", 3000));
    ctx->settings[ctx->id("timing_driven")] = true;
    ctx->critical_cohort_report = guidance(logic).dump();
    TimingAnalyser before(ctx.get());
    before.setup(false, false, true);
    repair_critical_cohorts(ctx.get(), 8);
    TimingAnalyser after(ctx.get());
    after.setup(false, false, true);
    EXPECT_GT(after.get_timing_result().clock_fmax.at(ctx->id("clk")).achieved,
              before.get_timing_result().clock_fmax.at(ctx->id("clk")).achieved);
    ctx->check();
}

TEST_F(CriticalCohort, RoutedGuidanceResolvesInsertedRegisterRouteThrough)
{
    auto *logic = timing_cone(clock("clk", 3000));
    auto *capture = (*logic->getPort(id_Q)->users.begin()).cell;
    const auto home = capture->bel;
    ctx->settings[ctx->id("timing_driven")] = true;
    ctx->critical_cohort_report = route_through_guidance(logic).dump();
    repair_critical_cohorts(ctx.get(), 8);
    EXPECT_NE(capture->bel, home);
    ctx->check();
}

TEST_F(CriticalCohort, RouteThroughGuidanceRejectsADifferentOriginalDriver)
{
    auto *logic = timing_cone(clock("clk", 3000));
    ctx->settings[ctx->id("timing_driven")] = true;
    auto document = route_through_guidance(logic).object_items();
    auto path = document.at("critical_paths").array_items().front().object_items();
    auto segments = path.at("path").array_items();
    auto incoming = segments.front().object_items();
    auto original = incoming.at("from").object_items();
    original["cell"] = logic->getPort(id_A)->driver.cell->name.str(ctx.get());
    incoming["from"] = original;
    segments.front() = incoming;
    path["path"] = segments;
    document["critical_paths"] = json11::Json::array{path};
    ctx->critical_cohort_report = json11::Json(document).dump();
    Snapshot saved(ctx.get());
    EXPECT_THROW(repair_critical_cohorts(ctx.get(), 8), log_execution_error_exception);
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, MismatchedGuidanceFailsBeforeMovingAnyCell)
{
    auto *logic = timing_cone(clock("clk", 3000));
    ctx->settings[ctx->id("timing_driven")] = true;
    Snapshot saved(ctx.get());
    auto document = guidance(logic).object_items();
    auto clocks = document.at("fmax").object_items();
    clocks["clk"] = json11::Json::object{{"achieved", 1}, {"constraint", 500}};
    document["fmax"] = clocks;
    ctx->critical_cohort_report = json11::Json(document).dump();
    EXPECT_THROW(repair_critical_cohorts(ctx.get(), 8), log_execution_error_exception);
    saved.check(ctx.get());
}

TEST_F(CriticalCohort, NativeTimingRejectsChangedClockConstraints)
{
    auto *clk = clock("clk", 3000);
    auto *logic = timing_cone(clk);
    TimingAnalyser before(ctx.get());
    before.with_clock_skew = true;
    before.setup(false, false, true);
    move_logic(logic);
    clk->clkconstr->period = DelayPair(6000);
    TimingAnalyser after(ctx.get());
    after.with_clock_skew = true;
    after.setup(false, false, true);
    EXPECT_FALSE(critical_cohort_timing_safe(ctx.get(), before, after));
}
} // namespace

TEST_F(CriticalCohort, MeasuredModelMatchesInsertedGraphAndRejectsMismatches)
{
    auto *clk = clock("clk", 3000);
    auto *logic = timing_cone(clk);
    auto capture_name = (*logic->getPort(id_Q)->users.begin()).cell->name;
    for (uint32_t lab = 0; lab < ctx->labs.size(); ++lab)
        for (uint8_t alm = 0; alm < 10; ++alm)
            ctx->reassign_alm_inputs(lab, alm);
    TimingAnalyser actual(ctx.get());
    actual.with_clock_skew = false;
    actual.setup(false, false, true);
    std::vector<EndpointClockPairTiming> expected;
    ASSERT_TRUE(actual.get_endpoint_clock_pair_timings({capture_name, id_DATAIN}, expected));
    ctx->timing_result = actual.get_timing_result();
    std::ostringstream out;
    EXPECT_THROW(write_critical_cohort_route_model(ctx.get(), out), log_execution_error_exception);
    ctx->timing_result_is_final_analogue = true;
    write_critical_cohort_route_model(ctx.get(), out);
    // Supply distinct measured early/late wire delays, independently evaluate
    // them on the expanded graph, then verify their calibrated folded graph.
    std::string parse_error;
    auto exported = json11::Json::parse(out.str(), parse_error).object_items();
    auto route_model = exported.at("cohort_route_model").object_items();
    auto measured_arcs = route_model.at("arcs").array_items();
    for (auto &entry : measured_arcs) {
        auto row = entry.object_items();
        row["wire"] = json11::Json::array{100, 250};
        row["local"] = json11::Json::array{0, 0};
        auto name = ctx->id(row.at("sink").string_value());
        auto port = ctx->id(row.at("port").string_value());
        if (row.at("route_through").bool_value()) {
            actual.set_route_delay({name, id_DATAIN}, DelayPair());
            name = ctx->id(name.str(ctx.get()) + "$ROUTETHRU");
            port = id_A;
        }
        actual.set_route_delay({name, port}, DelayPair(100, 250));
        entry = row;
    }
    actual.run(false, false, false, true);
    expected.clear();
    ASSERT_TRUE(actual.get_endpoint_clock_pair_timings({capture_name, id_DATAIN}, expected));
    route_model["arcs"] = measured_arcs;
    exported["cohort_route_model"] = route_model;
    const auto report = json11::Json(exported).dump();
    // Recreate the original graph without inserted routing cells.
    serial = 0;
    SetUp();
    clk = clock("clk", 3000);
    logic = timing_cone(clk);
    critical_cohort_pin_preview(ctx.get(), [&]() {
        CriticalCohortRouteModel model(ctx.get(), report);
        ASSERT_TRUE(model.active());
        TimingAnalyser calibrated(ctx.get());
        calibrated.with_clock_skew = false;
        critical_cohort_setup_timing(ctx.get(), calibrated);
        model.apply(ctx.get(), calibrated);
        std::vector<EndpointClockPairTiming> got;
        ASSERT_TRUE(calibrated.get_endpoint_clock_pair_timings({capture_name, id_DATAIN}, got));
        ASSERT_EQ(got.size(), expected.size());
        for (size_t i = 0; i < got.size(); ++i) {
            EXPECT_EQ(got[i].min_path_delay, expected[i].min_path_delay);
            EXPECT_EQ(got[i].max_path_delay, expected[i].max_path_delay);
            EXPECT_EQ(got[i].setup_margin, expected[i].setup_margin);
            EXPECT_EQ(got[i].hold_margin, expected[i].hold_margin);
        }
        auto *capture = ctx->cells.at(capture_name).get();
        EXPECT_EQ(model.route_delay(ctx.get(), {capture, id_DATAIN}), 762);
        const auto original_input = capture->getPort(id_DATAIN);
        const auto original_count = ctx->cells.size();
        CriticalCohortStats stats;
        // The matching driver half removes the virtual buffer. The calibrated
        // wire clamps at zero rather than becoming a negative arrival delay.
        EXPECT_FALSE(critical_cohort_trial(
                ctx.get(), {capture}, -6, -17, 10000,
                [&]() {
                    critical_cohort_pin_preview(ctx.get(), [&]() {
                        TimingAnalyser after(ctx.get());
                        after.with_clock_skew = false;
                        critical_cohort_setup_timing(ctx.get(), after);
                        model.apply(ctx.get(), after);
                        EXPECT_EQ(model.route_delay(ctx.get(), {capture, id_DATAIN}), 0);
                        EXPECT_GT(after.get_setup_slack({capture_name, id_DATAIN}),
                                  calibrated.get_setup_slack({capture_name, id_DATAIN}) + 500);
                        EXPECT_EQ(ctx->cells.size(), original_count);
                        EXPECT_EQ(capture->getPort(id_DATAIN), original_input);
                    });
                    return false;
                },
                stats, -12));
        EXPECT_TRUE(stats.timed);
        EXPECT_EQ(capture->bel, bel(30, 20, 14));
        clk->clkconstr->phase_shift = 10;
        EXPECT_THROW(CriticalCohortRouteModel(ctx.get(), report), log_execution_error_exception);
        clk->clkconstr->phase_shift = 0;
        logic->params[ctx->id("LUT")] = 3;
        EXPECT_THROW(CriticalCohortRouteModel(ctx.get(), report), log_execution_error_exception);
        logic->params.erase(ctx->id("LUT"));
        std::string error;
        auto document = json11::Json::parse(report, error).object_items();
        auto metadata = document.at("cohort_route_model").object_items();
        auto arcs = metadata.at("arcs").array_items();
        auto row = arcs.front().object_items();
        row["wire"] = json11::Json::array{20, 10};
        auto saved = arcs.front();
        arcs.front() = row;
        metadata["arcs"] = arcs;
        document["cohort_route_model"] = metadata;
        EXPECT_THROW(CriticalCohortRouteModel(ctx.get(), json11::Json(document).dump()), log_execution_error_exception);
        arcs.front() = saved;
        arcs.push_back(saved);
        metadata["arcs"] = arcs;
        document["cohort_route_model"] = metadata;
        EXPECT_THROW(CriticalCohortRouteModel(ctx.get(), json11::Json(document).dump()), log_execution_error_exception);
        arcs.pop_back();
        arcs.pop_back();
        metadata["arcs"] = arcs;
        document["cohort_route_model"] = metadata;
        EXPECT_THROW(CriticalCohortRouteModel(ctx.get(), json11::Json(document).dump()), log_execution_error_exception);
    });
    move_logic(logic);
    critical_cohort_pin_preview(ctx.get(), [&]() {
        EXPECT_THROW(CriticalCohortRouteModel(ctx.get(), report), log_execution_error_exception);
    });
    ctx->check();
}

TEST_F(CriticalCohort, CalibratedRepairPacksACriticalRegisterWithItsDriver)
{
    auto *clk = clock("clk", 1000);
    auto *logic = timing_cone(clk);
    const auto capture_name = (*logic->getPort(id_Q)->users.begin()).cell->name;
    for (uint32_t lab = 0; lab < ctx->labs.size(); ++lab)
        for (uint8_t alm = 0; alm < 10; ++alm)
            ctx->reassign_alm_inputs(lab, alm);
    TimingAnalyser actual(ctx.get());
    actual.with_clock_skew = false;
    actual.setup(false, false, true);
    ctx->timing_result = actual.get_timing_result();
    ctx->timing_result_is_final_analogue = true;
    std::ostringstream out;
    write_critical_cohort_route_model(ctx.get(), out);
    std::string error;
    auto report = json11::Json::parse(out.str(), error).object_items();
    auto model = report.at("cohort_route_model").object_items();
    auto arcs = model.at("arcs").array_items();
    for (auto &entry : arcs) {
        auto row = entry.object_items();
        row["wire"] = json11::Json::array{100, 250};
        row["local"] = json11::Json::array{0, 0};
        entry = row;
    }
    model["arcs"] = arcs;
    report["cohort_route_model"] = model;
    const auto text = json11::Json(report).dump();
    serial = 0;
    SetUp();
    clk = clock("clk", 1000);
    logic = timing_cone(clk);
    const auto cells = ctx->cells.size(), nets = ctx->nets.size();
    ctx->settings[ctx->id("timing_driven")] = 1;
    ctx->critical_cohort_report = text;
    repair_critical_cohorts(ctx.get(), 8);
    EXPECT_EQ(ctx->cells.at(capture_name)->bel, bel(24, 3, 2));
    EXPECT_EQ(logic->bel, bel(24, 3, 0));
    EXPECT_EQ(ctx->cells.size(), cells);
    EXPECT_EQ(ctx->nets.size(), nets);
    ctx->check();
}

TEST_F(CriticalCohort, CalibratedRepairImprovesOneOfTwoTiedEnableEndpoints)
{
    auto create = [&]() {
        auto *clk = clock("clk", 1000);
        auto *logic = timing_cone(clk);
        auto *capture = (*logic->getPort(id_Q)->users.begin()).cell;
        auto *enable = lut(27, 5, 0);
        enable->disconnectPort(id_A);
        enable->connectPort(id_A, logic->getPort(id_A));
        auto *other = ff(clk, 30, 20, 20, logic->getPort(id_Q));
        for (auto *ff : {capture, other}) {
            ff->pin_data[id_ENA].state = PIN_SIG;
            ff->connectPort(id_ENA, enable->getPort(id_Q));
        }
        ctx->assignArchInfo();
        return std::pair<CellInfo *, CellInfo *>{capture, other};
    };
    create();
    for (uint32_t lab = 0; lab < ctx->labs.size(); ++lab)
        for (uint8_t alm = 0; alm < 10; ++alm)
            ctx->reassign_alm_inputs(lab, alm);
    TimingAnalyser actual(ctx.get());
    actual.with_clock_skew = false;
    actual.setup(false, false, true);
    ctx->timing_result = actual.get_timing_result();
    ctx->timing_result_is_final_analogue = true;
    std::ostringstream out;
    write_critical_cohort_route_model(ctx.get(), out);
    std::string error;
    auto report = json11::Json::parse(out.str(), error).object_items();
    auto metadata = report.at("cohort_route_model").object_items();
    auto arcs = metadata.at("arcs").array_items();
    for (auto &entry : arcs) {
        auto row = entry.object_items();
        row["wire"] = row.at("port").string_value() == "ENA" ? json11::Json::array{1200, 1500}
                                                             : json11::Json::array{100, 250};
        row["local"] = json11::Json::array{0, 0};
        entry = row;
    }
    metadata["arcs"] = arcs;
    report["cohort_route_model"] = metadata;
    auto text = json11::Json(report).dump();
    serial = 0;
    SetUp();
    auto captures = create();
    std::unique_ptr<CriticalCohortRouteModel> model;
    TimingAnalyser before(ctx.get());
    before.with_clock_skew = false;
    critical_cohort_pin_preview(ctx.get(), [&]() {
        model = std::make_unique<CriticalCohortRouteModel>(ctx.get(), text);
        critical_cohort_setup_timing(ctx.get(), before);
        model->apply(ctx.get(), before);
    });
    ASSERT_EQ(before.get_setup_slack({captures.first->name, id_ENA}),
              before.get_setup_slack({captures.second->name, id_ENA}));
    ASSERT_LT(before.get_setup_slack({captures.first->name, id_ENA}), 0);
    ctx->settings[ctx->id("timing_driven")] = 1;
    ctx->critical_cohort_report = text;
    repair_critical_cohorts(ctx.get(), 1);
    TimingAnalyser after(ctx.get());
    after.with_clock_skew = false;
    critical_cohort_pin_preview(ctx.get(), [&]() {
        critical_cohort_setup_timing(ctx.get(), after);
        model->apply(ctx.get(), after);
        EXPECT_EQ(before.get_timing_result().clock_setup_slack, after.get_timing_result().clock_setup_slack);
        const auto first_gain = after.get_setup_slack({captures.first->name, id_ENA}) -
                                before.get_setup_slack({captures.first->name, id_ENA});
        const auto second_gain = after.get_setup_slack({captures.second->name, id_ENA}) -
                                 before.get_setup_slack({captures.second->name, id_ENA});
        EXPECT_GE(std::max(first_gain, second_gain), 20);
        EXPECT_EQ(std::min(first_gain, second_gain), 0);
    });
    ctx->check();
}
