/* Backend proofs for explicit local-remap plans. SPDX-License-Identifier: ISC */
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>
#include "gtest/gtest.h"
#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"
#include "remap_report.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

namespace {
const IdString plan_pins[] = {id_A, id_B, id_C, id_D, id_E, id_F};

struct PlanSnapshot {
    struct Cell {
        CellInfo *identity;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        decltype(CellInfo::params) params;
        decltype(CellInfo::attrs) attrs;
        std::map<IdString, PortInfo> ports;
        std::map<IdString, ArchPinInfo> pins;
    };
    struct Net {
        NetInfo *identity;
        PortRef driver;
        indexed_store<PortRef> users;
    };
    std::map<IdString, Cell> cells;
    std::map<IdString, Net> nets;
    std::map<IdString, IdString> aliases;
    std::map<IdString, PortInfo> ports;

    explicit PlanSnapshot(Context *ctx)
    {
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            Cell saved{cell, cell->type, cell->bel, cell->belStrength, cell->params, cell->attrs, {}, {}};
            for (const auto &port : cell->ports) saved.ports.emplace(port.first, port.second);
            for (const auto &pin : cell->pin_data) saved.pins.emplace(pin.first, pin.second);
            cells.emplace(entry.first, std::move(saved));
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            nets.emplace(entry.first, Net{net, net->driver, net->users});
        }
        for (const auto &entry : ctx->net_aliases) aliases.emplace(entry.first, entry.second);
        for (const auto &entry : ctx->ports) ports.emplace(entry.first, entry.second);
    }

    void expect_cell(Context *ctx, IdString name, IdString changed_port = IdString()) const
    {
        ASSERT_TRUE(ctx->cells.count(name));
        const auto &saved = cells.at(name);
        auto *cell = ctx->cells.at(name).get();
        EXPECT_EQ(cell, saved.identity); EXPECT_EQ(cell->type, saved.type);
        EXPECT_EQ(cell->bel, saved.bel); EXPECT_EQ(cell->belStrength, saved.strength);
        EXPECT_EQ(cell->params, saved.params); EXPECT_EQ(cell->attrs, saved.attrs);
        ASSERT_EQ(cell->ports.size(), saved.ports.size());
        for (const auto &port : saved.ports) {
            ASSERT_TRUE(cell->ports.count(port.first));
            const auto &actual = cell->ports.at(port.first);
            EXPECT_EQ(actual.name, port.second.name); EXPECT_EQ(actual.type, port.second.type);
            if (port.first != changed_port) {
                EXPECT_EQ(actual.net, port.second.net); EXPECT_EQ(actual.user_idx, port.second.user_idx);
            }
        }
        ASSERT_EQ(cell->pin_data.size(), saved.pins.size());
        for (const auto &pin : saved.pins) {
            ASSERT_TRUE(cell->pin_data.count(pin.first));
            EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
            EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
        }
    }

    void expect_exact(Context *ctx) const
    {
        ASSERT_EQ(ctx->cells.size(), cells.size());
        ASSERT_EQ(ctx->nets.size(), nets.size());
        ASSERT_EQ(ctx->net_aliases.size(), aliases.size());
        ASSERT_EQ(ctx->ports.size(), ports.size());
        for (const auto &entry : cells) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->cells.count(entry.first));
            auto *cell = ctx->cells.at(entry.first).get();
            const auto &saved = entry.second;
            EXPECT_EQ(cell, saved.identity);
            EXPECT_EQ(cell->type, saved.type);
            EXPECT_EQ(cell->bel, saved.bel);
            EXPECT_EQ(cell->belStrength, saved.strength);
            EXPECT_EQ(cell->params, saved.params);
            EXPECT_EQ(cell->attrs, saved.attrs);
            ASSERT_EQ(cell->ports.size(), saved.ports.size());
            for (const auto &port : saved.ports) {
                ASSERT_TRUE(cell->ports.count(port.first));
                const auto &actual = cell->ports.at(port.first);
                EXPECT_EQ(actual.name, port.second.name);
                EXPECT_EQ(actual.type, port.second.type);
                EXPECT_EQ(actual.net, port.second.net);
                EXPECT_EQ(actual.user_idx, port.second.user_idx);
            }
            ASSERT_EQ(cell->pin_data.size(), saved.pins.size());
            for (const auto &pin : saved.pins) {
                ASSERT_TRUE(cell->pin_data.count(pin.first));
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
        }
        for (const auto &entry : nets) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->nets.count(entry.first));
            auto *net = ctx->nets.at(entry.first).get();
            EXPECT_EQ(net, entry.second.identity);
            EXPECT_EQ(net->driver.cell, entry.second.driver.cell);
            EXPECT_EQ(net->driver.port, entry.second.driver.port);
            auto actual = net->users, expected = entry.second.users;
            ASSERT_EQ(actual.entries(), expected.entries());
            ASSERT_EQ(actual.capacity(), expected.capacity());
            for (auto user : expected.enumerate()) {
                ASSERT_TRUE(actual.count(user.index));
                EXPECT_EQ(actual.at(user.index).cell, user.value.cell);
                EXPECT_EQ(actual.at(user.index).port, user.value.port);
            }
            // Copies expose every existing free slot, then allocation beyond
            // capacity. No probe modifies the live graph or its snapshot.
            for (size_t probe = 0; probe < size_t(entry.second.users.capacity()) + 8; ++probe)
                EXPECT_EQ(actual.add(PortRef{}), expected.add(PortRef{}));
        }
        for (const auto &entry : aliases) EXPECT_EQ(ctx->net_aliases.at(entry.first), entry.second);
        for (const auto &entry : ports) {
            const auto &actual = ctx->ports.at(entry.first);
            EXPECT_EQ(actual.name, entry.second.name);
            EXPECT_EQ(actual.net, entry.second.net);
            EXPECT_EQ(actual.type, entry.second.type);
            EXPECT_EQ(actual.user_idx, entry.second.user_idx);
        }
        ctx->check();
    }
};

// Capture the actual accepted first graph during one two-step execution. The
// normal acceptance log comes after remap_critical's graph consistency check.
struct PlanLogCapture {
    struct Buffer : std::streambuf {
        Context *ctx;
        std::string text;
        std::unique_ptr<PlanSnapshot> first;
        explicit Buffer(Context *ctx) : ctx(ctx) {}
        void inspect()
        {
            if (!first && text.find("Local remap applied candidate 0;") != std::string::npos)
                first = std::make_unique<PlanSnapshot>(ctx);
        }
        std::streamsize xsputn(const char *data, std::streamsize count) override
        {
            text.append(data, size_t(count)); inspect(); return count;
        }
        int_type overflow(int_type value) override
        {
            if (!traits_type::eq_int_type(value, traits_type::eof())) {
                text.push_back(traits_type::to_char_type(value)); inspect();
            }
            return traits_type::not_eof(value);
        }
    } buffer;
    std::ostream stream;
    explicit PlanLogCapture(Context *ctx) : buffer(ctx), stream(&buffer)
    {
        log_streams.emplace_back(&stream, LogLevel::INFO_MSG);
    }
    ~PlanLogCapture() { log_streams.pop_back(); }
};
} // namespace

class LocalRemapPlanTest : public ::testing::Test {
  protected:
    struct Cone {
        CellInfo *a, *b, *inner, *outer, *near_a, *near_b, *remote;
        NetInfo *enable;
        std::string report;
    } first, second;
    std::unique_ptr<Context> ctx;
    NetInfo *clock;

    CellInfo *ff(const std::string &name, NetInfo *enable)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (IdString pin : {id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN}) cell->addInput(pin);
        cell->connectPort(id_CLK, clock);
        if (enable) cell->connectPort(id_ENA, enable);
        else cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        cell->addOutput(id_Q);
        auto *output = ctx->createNet(ctx->id(name + "$q"));
        cell->connectPort(id_Q, output);
        cell->connectPort(id_DATAIN, output);
        return cell;
    }

    void place(CellInfo *cell, int x, int y)
    {
        for (auto bel : ctx->getBelsByTile(x, y)) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel)) continue;
            ctx->bindBel(bel, cell, STRENGTH_WEAK);
            if (ctx->isBelLocationValid(bel)) return;
            ctx->unbindBel(bel);
        }
        FAIL() << "No legal plan-fixture BEL for " << cell->name.str(ctx.get()) << " at " << x << "," << y;
    }

    json11::Json endpoint(CellInfo *cell, IdString pin) const
    {
        auto loc = ctx->getBelLocation(cell->bel);
        return json11::Json::object{{"cell", cell->name.str(ctx.get())}, {"port", pin.str(ctx.get())},
                                  {"loc", json11::Json::array{loc.x, loc.y}}};
    }

    std::string report(const Cone &cone) const
    {
        using json11::Json;
        auto segment = [&](const char *type, double delay, CellInfo *from, IdString source,
                           CellInfo *to, IdString target) {
            Json::object result{{"type", type}, {"delay", delay}, {"from", endpoint(from, source)},
                                {"to", endpoint(to, target)}};
            if (std::string(type) == "routing") result["net"] = from->getPort(source)->name.str(ctx.get());
            return Json(result);
        };
        Json::array path{
            segment("clk-to-q", .731, cone.a, id_Q, cone.a, id_Q),
            segment("routing", 2, cone.a, id_Q, cone.inner, id_A),
            segment("logic", .4, cone.inner, id_A, cone.inner, id_Q),
            segment("routing", 2, cone.inner, id_Q, cone.outer, id_A),
            segment("logic", .4, cone.outer, id_A, cone.outer, id_Q),
            segment("routing", 2, cone.outer, id_Q, cone.near_a, id_ENA),
            segment("setup", -.196, cone.near_a, id_ENA, cone.near_a, id_ENA)};
        return Json(Json::object{{"critical_paths", Json::array{Json::object{{"max_delay", 1}, {"path", path}}}}}).dump();
    }

    Cone make_cone(const std::string &name, int y, CellInfo *shared_source)
    {
        Cone cone;
        cone.a = shared_source ? shared_source : ff(name + "$source_a", nullptr);
        cone.b = ff(name + "$source_b", nullptr);
        cone.inner = ctx->createCell(ctx->id(name + "$inner"), id_MISTRAL_ALUT2);
        cone.inner->params[id_LUT] = Property(0x8, 4);
        cone.inner->addInput(id_A); cone.inner->addInput(id_B); cone.inner->addOutput(id_Q);
        cone.inner->connectPort(id_A, cone.a->getPort(id_Q));
        cone.inner->pin_data[id_B].state = PIN_1;
        cone.inner->connectPort(id_Q, ctx->createNet(ctx->id(name + "$intermediate")));
        cone.outer = ctx->createCell(ctx->id(name + "$outer"), id_MISTRAL_ALUT3);
        cone.outer->params[id_LUT] = Property(0x80, 8);
        for (IdString pin : {id_A,id_B,id_C}) cone.outer->addInput(pin);
        cone.outer->addOutput(id_Q);
        cone.outer->connectPort(id_A, cone.inner->getPort(id_Q));
        cone.outer->pin_data[id_A].state = PIN_INV;
        cone.outer->connectPort(id_B, cone.b->getPort(id_Q));
        cone.outer->pin_data[id_C].state = PIN_1;
        cone.enable = ctx->createNet(ctx->id(name + "$enable"));
        cone.outer->connectPort(id_Q, cone.enable);
        cone.near_a = ff(name + "$near_a", cone.enable);
        cone.near_b = ff(name + "$near_b", cone.enable);
        cone.near_b->pin_data[id_ENA].state = PIN_INV;
        cone.remote = ff(name + "$remote", cone.enable);
        ctx->assignArchInfo();
        if (!shared_source) place(cone.a, 34, y);
        place(cone.b, 34, y);
        place(cone.inner, 24, y); place(cone.outer, 24, y);
        place(cone.near_a, 30, y); place(cone.near_b, 30, y); place(cone.remote, 24, y);
        cone.report = report(cone);
        return cone;
    }

    void make_holes(NetInfo *net, const std::string &name)
    {
        std::vector<CellInfo *> removed;
        for (int i = 0; i < 4; ++i) {
            auto *cell = ctx->createCell(ctx->id(name + std::to_string(i)), id_MISTRAL_ALUT2);
            cell->addInput(id_A); cell->connectPort(id_A, net);
            removed.push_back(cell);
        }
        for (int index : {1, 3, 0, 2}) {
            auto *cell = removed[index];
            cell->disconnectPort(id_A); ctx->cells.erase(cell->name);
        }
    }

    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e9;
        ctx->settings[id_placer] = Property(std::string("heap"));
        clock = ctx->createNet(ctx->id("plan_clock")); clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>();
        clock->clkconstr->period = DelayPair(1000);
        clock->clkconstr->high = clock->clkconstr->low = DelayPair(500);
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        first = make_cone("first", 20, nullptr);
        second = make_cone("second", 21, first.a);
        // Several recycled slots on a shared source catch damage to the
        // already-accepted first clone during second-stage probe rollback.
        make_holes(first.a->getPort(id_Q), "shared_hole_");
        make_holes(second.b->getPort(id_Q), "second_input_hole_");
        make_holes(second.enable, "second_enable_hole_");
        ctx->local_remap_optimize_pins = true;
        ctx->local_remap_preserve_ff_placement = false;
        ctx->check();
    }

    Arch::LocalRemapStep step(const Cone &cone, int candidate = 0) const
    {
        Arch::LocalRemapStep result;
        result.report = cone.report; result.candidate = candidate; result.groups = 1;
        result.optimize_pins = false; result.preserve_ff_placement = true;
        return result;
    }

    CellInfo *clone(const Cone &cone, int suffix = 0) const
    {
        auto name = cone.outer->name.str(ctx.get()) + "$local_remap";
        if (suffix) name += "$" + std::to_string(suffix);
        auto found = ctx->cells.find(ctx->id(name));
        return found == ctx->cells.end() ? nullptr : found->second.get();
    }

    Cone remaining_cohort()
    {
        // Two original consumers stay in the first LAB. This third original
        // consumer needs a separate nearby copy of the same two-cell cone.
        ctx->unbindBel(first.remote->bel);
        place(first.remote, 30, 23);
        make_holes(first.enable, "repeated_enable_hole_");
        make_holes(first.b->getPort(id_Q), "repeated_input_hole_");
        Cone remaining = first;
        remaining.near_a = first.remote;
        remaining.report = report(remaining);
        ctx->check();
        return remaining;
    }

    bool evaluate(NetInfo *net, const Cone &cone, unsigned assignment) const
    {
        if (net == cone.a->getPort(id_Q)) return assignment & 1;
        if (net == cone.b->getPort(id_Q)) return (assignment >> 1) & 1;
        auto *cell = net->driver.cell;
        unsigned row = 0;
        int width = cell->type == id_MISTRAL_ALUT2 ? 2 : cell->type == id_MISTRAL_ALUT3 ? 3 : 0;
        EXPECT_NE(width, 0);
        for (int i = 0; i < width; ++i) {
            auto state = cell->get_pin_state(plan_pins[i]);
            bool value = state == PIN_1;
            if (state == PIN_SIG || state == PIN_INV) {
                value = evaluate(cell->getPort(plan_pins[i]), cone, assignment);
                if (state == PIN_INV) value = !value;
            }
            row |= unsigned(value) << i;
        }
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> row) & 1;
    }

    void expect_options_restored(bool pins = true, bool placement = false) const
    {
        EXPECT_EQ(ctx->local_remap_optimize_pins, pins);
        EXPECT_EQ(ctx->local_remap_preserve_ff_placement, placement);
    }
};

TEST_F(LocalRemapPlanTest, TwoSequentialConesPreserveRegistersPolarityAndSideUsers)
{
    PlanSnapshot before(ctx.get());
    ctx->local_remap_plan = {step(first), step(second)};
    PlanLogCapture capture(ctx.get());
    ASSERT_TRUE(ctx->execute_local_remap_plan()) << capture.buffer.text;
    ASSERT_NE(clone(first), nullptr);
    ASSERT_NE(clone(second), nullptr);
    EXPECT_EQ(ctx->cells.size(), before.cells.size() + 2);
    EXPECT_EQ(ctx->nets.size(), before.nets.size() + 2);
    for (const auto &entry : before.cells) {
        auto *cell = ctx->cells.at(entry.first).get();
        EXPECT_EQ(cell, entry.second.identity);
        EXPECT_EQ(cell->type, entry.second.type);
        EXPECT_EQ(cell->bel, entry.second.bel);
        EXPECT_EQ(cell->belStrength, entry.second.strength);
        EXPECT_EQ(cell->params, entry.second.params);
        for (const auto &port : entry.second.ports) {
            auto pin = entry.second.pins.find(port.first);
            EXPECT_EQ(cell->get_pin_state(port.first), pin == entry.second.pins.end() ? PIN_SIG : pin->second.state);
            if ((cell == first.near_a || cell == first.near_b || cell == second.near_a || cell == second.near_b) &&
                port.first == id_ENA) continue;
            EXPECT_EQ(cell->getPort(port.first), port.second.net);
            EXPECT_EQ(cell->ports.at(port.first).user_idx, port.second.user_idx);
        }
    }
    for (const auto *cone : {&first, &second}) {
        auto *copy = clone(*cone);
        EXPECT_EQ(cone->near_a->getPort(id_ENA), copy->getPort(id_Q));
        EXPECT_EQ(cone->near_b->getPort(id_ENA), copy->getPort(id_Q));
        EXPECT_EQ(cone->near_b->get_pin_state(id_ENA), PIN_INV);
        EXPECT_EQ(cone->remote->getPort(id_ENA), cone->enable);
        EXPECT_EQ(cone->outer->getPort(id_A), cone->inner->getPort(id_Q));
        for (unsigned row = 0; row < 4; ++row) {
            bool expected = !(row & 1) && ((row >> 1) & 1);
            EXPECT_EQ(evaluate(cone->enable, *cone, row), expected);
            EXPECT_EQ(evaluate(copy->getPort(id_Q), *cone, row), expected);
        }
    }
    std::istringstream lines(capture.buffer.text); std::string line;
    int trials = 0;
    while (std::getline(lines, line)) if (line.find("Local remap trial ") != std::string::npos) {
        EXPECT_NE(line.find("shift=0,0 "), std::string::npos) << line;
        ++trials;
    }
    EXPECT_GT(trials, 0);
    expect_options_restored();
    ctx->check();
}

TEST_F(LocalRemapPlanTest, RepeatedOuterRetainsFirstTwoUsersAndAddsASeparateOneUserCopy)
{
    auto remaining = remaining_cohort();
    ctx->local_remap_plan = {step(first)};
    ASSERT_TRUE(ctx->execute_local_remap_plan());
    auto *old_copy = clone(first);
    ASSERT_NE(old_copy, nullptr);
    ASSERT_EQ(old_copy->getPort(id_Q)->users.entries(), 2);
    PlanSnapshot retained(ctx.get());
    TimingAnalyser before(ctx.get()); before.setup(false, false, true);
    const float old_a = before.get_setup_slack(CellPortKey(first.near_a->name, id_ENA));
    const float old_b = before.get_setup_slack(CellPortKey(first.near_b->name, id_ENA));
    const float old_remote = before.get_setup_slack(CellPortKey(first.remote->name, id_ENA));
    for (float slack : {old_a, old_b, old_remote})
        ASSERT_TRUE(std::isfinite(slack) && slack < float(std::numeric_limits<delay_t>::max()));
    ctx->local_remap_plan = {step(remaining)};
    PlanLogCapture capture(ctx.get());
    ASSERT_TRUE(ctx->execute_local_remap_plan()) << capture.buffer.text;
    auto *new_copy = clone(first, 1);
    ASSERT_NE(new_copy, nullptr);
    EXPECT_EQ(clone(first), old_copy);
    EXPECT_NE(new_copy, old_copy);
    EXPECT_EQ(ctx->cells.size(), retained.cells.size() + 1);
    EXPECT_EQ(ctx->nets.size(), retained.nets.size() + 1);
    EXPECT_EQ(ctx->net_aliases.size(), retained.aliases.size() + 1);
    EXPECT_EQ(first.near_a->getPort(id_ENA), old_copy->getPort(id_Q));
    EXPECT_EQ(first.near_b->getPort(id_ENA), old_copy->getPort(id_Q));
    EXPECT_EQ(first.remote->getPort(id_ENA), new_copy->getPort(id_Q));
    EXPECT_EQ(first.near_b->get_pin_state(id_ENA), PIN_INV);
    EXPECT_EQ(old_copy->getPort(id_Q)->users.entries(), 2);
    ASSERT_EQ(new_copy->getPort(id_Q)->users.entries(), 1);
    auto only_user = *new_copy->getPort(id_Q)->users.begin();
    EXPECT_EQ(only_user.cell, first.remote); EXPECT_EQ(only_user.port, id_ENA);
    for (const auto &entry : retained.cells)
        retained.expect_cell(ctx.get(), entry.first, entry.second.identity == first.remote ? id_ENA : IdString());
    for (const auto &entry : retained.nets) {
        auto *net = ctx->nets.at(entry.first).get();
        EXPECT_EQ(net, entry.second.identity);
        EXPECT_EQ(net->driver.cell, entry.second.driver.cell); EXPECT_EQ(net->driver.port, entry.second.driver.port);
        auto saved_users = entry.second.users;
        for (auto user : saved_users.enumerate()) {
            if (user.value.cell == first.remote && user.value.port == id_ENA) continue;
            ASSERT_TRUE(net->users.count(user.index));
            EXPECT_EQ(net->users.at(user.index).cell, user.value.cell);
            EXPECT_EQ(net->users.at(user.index).port, user.value.port);
        }
    }
    for (const auto &entry : retained.aliases) EXPECT_EQ(ctx->net_aliases.at(entry.first), entry.second);
    for (unsigned row = 0; row < 4; ++row) {
        const bool expected = !(row & 1) && ((row >> 1) & 1);
        EXPECT_EQ(evaluate(first.enable, first, row), expected);
        EXPECT_EQ(evaluate(old_copy->getPort(id_Q), first, row), expected);
        EXPECT_EQ(evaluate(new_copy->getPort(id_Q), first, row), expected);
    }
    TimingAnalyser after(ctx.get()); after.setup(false, false, true);
    EXPECT_GE(after.get_setup_slack(CellPortKey(first.near_a->name, id_ENA)), old_a);
    EXPECT_GE(after.get_setup_slack(CellPortKey(first.near_b->name, id_ENA)), old_b);
    EXPECT_GE(after.get_setup_slack(CellPortKey(first.remote->name, id_ENA)), old_remote + 250);
    EXPECT_NE(capture.buffer.text.find("Local remap candidate 0:"), std::string::npos);
    expect_options_restored();
    ctx->check();
}

TEST_F(LocalRemapPlanTest, RepeatedOuterFinalListingRestoresExactAcceptedFirstCopy)
{
    auto remaining = remaining_cohort();
    ctx->local_remap_plan = {step(first), step(remaining, -1)};
    ctx->local_remap_plan_list_only = true;
    PlanLogCapture capture(ctx.get());
    EXPECT_FALSE(ctx->execute_local_remap_plan());
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    const auto second = capture.buffer.text.find("Local-remap plan step 1:");
    ASSERT_NE(second, std::string::npos);
    EXPECT_NE(capture.buffer.text.find("Local remap candidate 0:", second), std::string::npos) << capture.buffer.text;
    EXPECT_EQ(clone(first, 1), nullptr);
    capture.buffer.first->expect_exact(ctx.get());
    expect_options_restored();
}

TEST_F(LocalRemapPlanTest, RepeatedOuterFailedSelectionRestoresExactAcceptedFirstCopy)
{
    auto remaining = remaining_cohort();
    auto failed = step(remaining, 9999);
    failed.optimize_pins = true;
    failed.preserve_ff_placement = false;
    ctx->local_remap_plan = {step(first), failed};
    PlanLogCapture capture(ctx.get());
    EXPECT_THROW(ctx->execute_local_remap_plan(), log_execution_error_exception);
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    const auto second = capture.buffer.text.find("Local-remap plan step 1:");
    ASSERT_NE(second, std::string::npos);
    EXPECT_NE(capture.buffer.text.find("Local remap candidate 0:", second), std::string::npos) << capture.buffer.text;
    EXPECT_EQ(clone(first, 1), nullptr);
    capture.buffer.first->expect_exact(ctx.get());
    expect_options_restored();
}

TEST_F(LocalRemapPlanTest, RepeatedOuterPreservesCellNetAndAliasCollisionsForBothNames)
{
    auto remaining = remaining_cohort();
    const auto base = first.outer->name.str(ctx.get()) + "$local_remap";
    auto name = [&](int suffix, bool output) { return base + "$" + std::to_string(suffix) + (output ? "$Q" : ""); };
    auto marker_cell = [&](const std::string &text) {
        auto *cell = ctx->createCell(ctx->id(text), id_MISTRAL_ALUT2);
        cell->params[id_LUT] = Property(0, 4);
        for (IdString pin : {id_A, id_B}) { cell->addInput(pin); cell->pin_data[pin].state = PIN_0; }
        cell->addOutput(id_Q);
    };
    marker_cell(name(1, false)); marker_cell(name(2, true));
    ctx->createNet(ctx->id(name(3, false))); ctx->createNet(ctx->id(name(4, true)));
    ctx->net_aliases.emplace(ctx->id(name(5, false)), first.enable->name);
    ctx->net_aliases.emplace(ctx->id(name(6, true)), first.enable->name);
    ctx->assignArchInfo(); ctx->check();
    ctx->local_remap_plan = {step(first)};
    ASSERT_TRUE(ctx->execute_local_remap_plan());
    auto *old_copy = clone(first);
    ASSERT_NE(old_copy, nullptr);
    PlanSnapshot retained(ctx.get());
    ctx->local_remap_plan = {step(remaining)};
    PlanLogCapture capture(ctx.get());
    ASSERT_TRUE(ctx->execute_local_remap_plan()) << capture.buffer.text;
    auto *new_copy = clone(first, 7);
    ASSERT_NE(new_copy, nullptr);
    EXPECT_EQ(clone(first), old_copy);
    EXPECT_EQ(first.near_a->getPort(id_ENA), old_copy->getPort(id_Q));
    EXPECT_EQ(first.near_b->getPort(id_ENA), old_copy->getPort(id_Q));
    EXPECT_EQ(first.remote->getPort(id_ENA), new_copy->getPort(id_Q));
    for (int suffix : {1, 2}) retained.expect_cell(ctx.get(), ctx->id(name(suffix, suffix == 2)));
    for (int suffix : {3, 4}) {
        const auto id = ctx->id(name(suffix, suffix == 4));
        auto *net = ctx->nets.at(id).get();
        EXPECT_EQ(net, retained.nets.at(id).identity);
        EXPECT_EQ(net->driver.cell, retained.nets.at(id).driver.cell);
        EXPECT_EQ(net->driver.port, retained.nets.at(id).driver.port);
        EXPECT_EQ(net->users.entries(), retained.nets.at(id).users.entries());
    }
    for (const auto &entry : retained.aliases) EXPECT_EQ(ctx->net_aliases.at(entry.first), entry.second);
    // A Q-only collision must not intern the corresponding absent cell name.
    for (int suffix : {2, 4, 6})
        EXPECT_EQ(ctx->idstring_str_to_idx->count(name(suffix, false)), 0u);
    retained.expect_cell(ctx.get(), old_copy->name);
    EXPECT_EQ(ctx->cells.size(), retained.cells.size() + 1);
    EXPECT_EQ(ctx->nets.size(), retained.nets.size() + 1);
    expect_options_restored();
    ctx->check();
}

TEST_F(LocalRemapPlanTest, FailedSecondSelectionRestoresItsProbeAndRetainsExactAcceptedFirstGraph)
{
    ctx->local_remap_optimize_pins = false;
    ctx->local_remap_preserve_ff_placement = true;
    auto failed = step(second, 9999); failed.groups = 8; failed.optimize_pins = true;
    failed.preserve_ff_placement = false;
    ctx->local_remap_plan = {step(first), failed};
    PlanLogCapture capture(ctx.get());
    EXPECT_THROW(ctx->execute_local_remap_plan(), log_execution_error_exception);
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    ASSERT_NE(clone(first), nullptr);
    EXPECT_EQ(clone(second), nullptr);
    EXPECT_NE(capture.buffer.text.find("Local-remap plan step 1:"), std::string::npos);
    EXPECT_NE(capture.buffer.text.find("Local remap candidate 0: second$"), std::string::npos) << capture.buffer.text;
    capture.buffer.first->expect_exact(ctx.get());
    expect_options_restored(false, true);
}

TEST_F(LocalRemapPlanTest, PreservePlacementSuppressesOtherwiseQualifiedMovement)
{
    PlanSnapshot before(ctx.get());
    std::string unrestricted;
    {
        PlanLogCapture capture(ctx.get());
        EXPECT_FALSE(ctx->remap_critical(first.report, -1));
        unrestricted = capture.buffer.text;
    }
    before.expect_exact(ctx.get());
    std::istringstream unrestricted_lines(unrestricted);
    std::string line;
    bool last_moved = false, moving_candidate = false;
    while (std::getline(unrestricted_lines, line)) {
        if (line.find("Local remap trial ") != std::string::npos)
            last_moved = line.find("shift=0,0 ") == std::string::npos;
        if (line.find("Local remap candidate ") != std::string::npos)
            moving_candidate |= last_moved;
    }
    ASSERT_TRUE(moving_candidate) << unrestricted;
    ctx->local_remap_plan = {step(first, -1)};
    ctx->local_remap_plan_list_only = true;
    PlanLogCapture capture(ctx.get());
    EXPECT_FALSE(ctx->execute_local_remap_plan());
    EXPECT_NE(capture.buffer.text.find("Local remap candidate 0:"), std::string::npos) << capture.buffer.text;
    std::istringstream preserved_lines(capture.buffer.text);
    while (std::getline(preserved_lines, line))
        if (line.find("Local remap trial ") != std::string::npos)
            EXPECT_NE(line.find("shift=0,0 "), std::string::npos) << line;
    before.expect_exact(ctx.get());
    expect_options_restored();
}

TEST_F(LocalRemapPlanTest, FinalListRollsBackEveryProbeAndRetainsExactAcceptedFirstGraph)
{
    auto listing = step(second, -1); listing.groups = 8; listing.optimize_pins = true;
    ctx->local_remap_plan = {step(first), listing};
    ctx->local_remap_plan_list_only = true;
    PlanLogCapture capture(ctx.get());
    EXPECT_FALSE(ctx->execute_local_remap_plan());
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    ASSERT_NE(clone(first), nullptr);
    EXPECT_EQ(clone(second), nullptr);
    EXPECT_NE(capture.buffer.text.find("Local remap candidate 0: second$"), std::string::npos) << capture.buffer.text;
    capture.buffer.first->expect_exact(ctx.get());
    expect_options_restored();
}

TEST_F(LocalRemapPlanTest, StaleSecondPathThrowsWithoutChangingFirstGraphOrOptions)
{
    auto stale = step(second);
    auto at = stale.report.find("second$inner"); ASSERT_NE(at, std::string::npos);
    stale.report.replace(at, std::string("second$inner").size(), "missing$inner");
    ctx->local_remap_plan = {step(first), stale};
    PlanLogCapture capture(ctx.get());
    EXPECT_THROW(ctx->execute_local_remap_plan(), log_execution_error_exception);
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    capture.buffer.first->expect_exact(ctx.get());
    expect_options_restored();
}

TEST_F(LocalRemapPlanTest, CompletePlanBoundsAndReportSyntaxAreRejectedBeforeFirstMutation)
{
    PlanSnapshot before(ctx.get());
    struct Invalid { std::vector<Arch::LocalRemapStep> steps; bool listing; };
    std::vector<Invalid> invalid{{{}, false}, {std::vector<Arch::LocalRemapStep>(9, step(first)), false}};
    for (int candidate : {-2, -1}) {
        auto bad = step(second, candidate);
        invalid.push_back({{step(first), bad}, false});
    }
    invalid.push_back({{step(first, -1), step(second, -1)}, true});
    invalid.push_back({{step(first), step(second)}, true});
    for (int groups : {0, 9}) {
        auto bad = step(second); bad.groups = groups;
        invalid.push_back({{step(first), bad}, false});
    }
    for (const std::string report : {std::string(), std::string("{"), std::string("{}"),
                                    std::string("{\"critical_paths\":{}}")}) {
        auto bad = step(second); bad.report = report;
        invalid.push_back({{step(first), bad}, false});
    }
    for (size_t i = 0; i < invalid.size(); ++i) {
        SCOPED_TRACE(i);
        ctx->local_remap_plan = invalid[i].steps;
        ctx->local_remap_plan_list_only = invalid[i].listing;
        EXPECT_THROW(ctx->execute_local_remap_plan(), log_execution_error_exception);
        before.expect_exact(ctx.get());
        expect_options_restored();
    }
}

namespace {
json11::Json with_plan_path_segments(const json11::Json &report, const json11::Json::array &segments)
{
    auto paths = report["critical_paths"].array_items();
    auto path = paths.at(0).object_items();
    path["path"] = segments;
    paths.at(0) = path;
    auto result = report.object_items();
    result["critical_paths"] = paths;
    return result;
}

json11::Json with_plan_clock_skew(const json11::Json &report, const json11::Json &launch,
                                 const json11::Json &capture, bool duplicate = false)
{
    json11::Json skew = json11::Json::object{{"type", "clk-skew"}, {"delay", .125},
                                           {"from", launch}, {"to", capture}};
    auto segments = report["critical_paths"][0]["path"].array_items();
    segments.insert(segments.begin(), skew);
    if (duplicate) segments.insert(segments.begin(), skew);
    return with_plan_path_segments(report, segments);
}
} // namespace

TEST_F(LocalRemapPlanTest, ReportClockSkewMatchesRegisteredLaunchAndCapture)
{
    std::string error;
    auto original = json11::Json::parse(first.report, error);
    ASSERT_TRUE(error.empty()) << error;
    auto report = with_plan_clock_skew(original, endpoint(first.a, id_CLK), endpoint(first.near_a, id_CLK));
    PlanSnapshot before(ctx.get());
    auto paths = mistral_remap_report::validate(ctx.get(), report, false);
    before.expect_exact(ctx.get());
    ASSERT_EQ(paths.size(), 1u);
    ASSERT_EQ(paths.front().edges.size(), 3u);
    EXPECT_EQ(paths.front().edges.front().first.cell, first.a);
    EXPECT_EQ(paths.front().edges.back().second.cell, first.near_a);
}

TEST_F(LocalRemapPlanTest, ReportClockSkewRejectsDifferentLaunchRegisterOnSameClock)
{
    std::string error;
    auto original = json11::Json::parse(first.report, error);
    ASSERT_TRUE(error.empty()) << error;
    // This is a valid placed clock input on the same net, but not the register
    // that launches the following clock-to-Q segment.
    auto report = with_plan_clock_skew(original, endpoint(second.b, id_CLK), endpoint(first.near_a, id_CLK));
    PlanSnapshot before(ctx.get());
    EXPECT_THROW(mistral_remap_report::validate(ctx.get(), report, false), log_execution_error_exception);
    before.expect_exact(ctx.get());
}

TEST_F(LocalRemapPlanTest, ReportClockSkewRejectsDifferentCaptureRegisterOnSameClock)
{
    std::string error;
    auto original = json11::Json::parse(first.report, error);
    ASSERT_TRUE(error.empty()) << error;
    auto report = with_plan_clock_skew(original, endpoint(first.a, id_CLK), endpoint(second.near_a, id_CLK));
    PlanSnapshot before(ctx.get());
    EXPECT_THROW(mistral_remap_report::validate(ctx.get(), report, false), log_execution_error_exception);
    before.expect_exact(ctx.get());
}

TEST_F(LocalRemapPlanTest, ReportRejectsDuplicateClockSkewBeforeGraphMutation)
{
    std::string error;
    auto original = json11::Json::parse(first.report, error);
    ASSERT_TRUE(error.empty()) << error;
    auto report = with_plan_clock_skew(original, endpoint(first.a, id_CLK), endpoint(first.near_a, id_CLK), true);
    PlanSnapshot before(ctx.get());
    EXPECT_THROW(mistral_remap_report::validate(ctx.get(), report, false), log_execution_error_exception);
    before.expect_exact(ctx.get());
}

TEST_F(LocalRemapPlanTest, ReportRejectsNonfiniteSumOfFiniteDelaysBeforeGraphMutation)
{
    std::string error;
    auto original = json11::Json::parse(first.report, error);
    ASSERT_TRUE(error.empty()) << error;
    auto segments = original["critical_paths"][0]["path"].array_items();
    // Both segment delays are finite; their sum overflows a double.
    for (size_t index = 0; index < 2; ++index) {
        auto segment = segments.at(index).object_items();
        segment["delay"] = 1e308;
        segments.at(index) = segment;
    }
    auto report = with_plan_path_segments(original, segments);
    PlanSnapshot before(ctx.get());
    EXPECT_THROW(mistral_remap_report::validate(ctx.get(), report, false), log_execution_error_exception);
    before.expect_exact(ctx.get());
}
