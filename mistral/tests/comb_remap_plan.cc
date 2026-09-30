/* Backend proofs for staged internal LUT-cut remaps. SPDX-License-Identifier: ISC */
#include <array>
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

USING_NEXTPNR_NAMESPACE

namespace {
const IdString comb_plan_pins[] = {id_A, id_B, id_C, id_D, id_E, id_F};

struct CombPlanSnapshot {
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

    explicit CombPlanSnapshot(Context *ctx)
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

    void expect_cell(CellInfo *cell, const Cell &saved, bool rewired_e = false) const
    {
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
            if (rewired_e && port.first == id_E) continue;
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

    void expect_exact(Context *ctx) const
    {
        ASSERT_EQ(ctx->cells.size(), cells.size());
        ASSERT_EQ(ctx->nets.size(), nets.size());
        ASSERT_EQ(ctx->net_aliases.size(), aliases.size());
        ASSERT_EQ(ctx->ports.size(), ports.size());
        for (const auto &entry : cells) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->cells.count(entry.first));
            expect_cell(ctx->cells.at(entry.first).get(), entry.second);
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
            // Probe copies through every free slot and beyond capacity. Live
            // entries alone cannot expose a changed indexed-store free list.
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

// Observe the actual acceptance in this execution, after the pass calls
// ctx->check(). A separately repeated first step can choose a different BEL.
struct CombPlanLogCapture {
    struct Buffer : std::streambuf {
        Context *ctx;
        std::string text;
        std::unique_ptr<CombPlanSnapshot> first;
        explicit Buffer(Context *ctx) : ctx(ctx) {}
        void inspect()
        {
            if (!first && text.find("Comb remap applied candidate ") != std::string::npos)
                first = std::make_unique<CombPlanSnapshot>(ctx);
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
    explicit CombPlanLogCapture(Context *ctx) : buffer(ctx), stream(&buffer)
    {
        log_streams.emplace_back(&stream, LogLevel::INFO_MSG);
    }
    ~CombPlanLogCapture() { log_streams.pop_back(); }
};
} // namespace

class CombRemapPlanTest : public ::testing::Test {
  protected:
    struct Cone {
        CellInfo *inner, *middle, *root, *terminal, *endpoint, *side_a, *side_b;
        std::string report;
    } first, second;
    std::unique_ptr<Context> ctx;
    NetInfo *clock;
    std::array<CellInfo *, 5> sources;

    CellInfo *ff(const std::string &name, NetInfo *enable)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (IdString pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clock);
        if (enable) cell->connectPort(id_ENA, enable);
        else cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        auto *output = ctx->createNet(ctx->id(name + "$q"));
        cell->connectPort(id_Q, output);
        cell->connectPort(id_DATAIN, output);
        return cell;
    }
    CellInfo *lut(const std::string &name, IdString type, unsigned width, uint64_t mask)
    {
        auto *cell = ctx->createCell(ctx->id(name), type);
        cell->params[id_LUT] = Property(int64_t(mask), 1u << width);
        for (unsigned i = 0; i < width; ++i) cell->addInput(comb_plan_pins[i]);
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx->createNet(ctx->id(name + "$q")));
        return cell;
    }
    void place(CellInfo *cell, int x, int y, PlaceStrength strength = STRENGTH_WEAK)
    {
        for (auto bel : ctx->getBelsByTile(x, y)) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel)) continue;
            ctx->bindBel(bel, cell, strength);
            if (ctx->isBelLocationValid(bel)) return;
            ctx->unbindBel(bel);
        }
        FAIL() << "No legal comb-plan fixture BEL for " << cell->name.str(ctx.get()) << " at " << x << ',' << y;
    }
    void make_hole(NetInfo *net, const std::string &name)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_ALUT2);
        cell->addInput(id_A); cell->connectPort(id_A, net);
        cell->disconnectPort(id_A); ctx->cells.erase(cell->name);
    }
    void make_shared_holes(NetInfo *net, unsigned index)
    {
        std::array<CellInfo *, 2> users;
        for (unsigned i = 0; i < users.size(); ++i) {
            users[i] = ctx->createCell(ctx->id("removed_shared_input_" + std::to_string(index) + "_" + std::to_string(i)),
                                       id_MISTRAL_ALUT2);
            users[i]->addInput(id_A); users[i]->connectPort(id_A, net);
        }
        // Keep both temporary users live until both have distinct slots.
        for (auto *cell : users) { cell->disconnectPort(id_A); ctx->cells.erase(cell->name); }
    }
    Cone make_cone(const std::string &name, int y)
    {
        Cone cone;
        cone.inner = lut(name + "$inner", id_MISTRAL_ALUT2, 2, 0x8);
        cone.inner->connectPort(id_A, sources[0]->getPort(id_Q));
        cone.inner->pin_data[id_A].state = PIN_INV;
        cone.inner->connectPort(id_B, sources[4]->getPort(id_Q));
        cone.middle = lut(name + "$middle", id_MISTRAL_ALUT2, 2, 0x2);
        cone.middle->connectPort(id_A, sources[2]->getPort(id_Q));
        cone.middle->pin_data[id_A].state = PIN_INV;
        cone.middle->connectPort(id_B, cone.inner->getPort(id_Q));
        cone.root = lut(name + "$shared_root", id_MISTRAL_ALUT4, 4, 0xe400);
        cone.root->connectPort(id_A, ctx->getNetByAlias(ctx->id("shared_state_alias")));
        cone.root->pin_data[id_A].state = PIN_INV;
        cone.root->connectPort(id_B, sources[3]->getPort(id_Q));
        cone.root->connectPort(id_C, sources[1]->getPort(id_Q));
        cone.root->connectPort(id_D, cone.middle->getPort(id_Q));
        cone.terminal = lut(name + "$terminal", id_MISTRAL_ALUT5, 5, 0x5f7f0000);
        for (unsigned i = 0; i < 4; ++i) cone.terminal->connectPort(comb_plan_pins[i], sources[i]->getPort(id_Q));
        cone.terminal->pin_data[id_B].state = PIN_INV;
        cone.terminal->connectPort(id_E, cone.root->getPort(id_Q));
        cone.terminal->pin_data[id_E].state = PIN_INV;
        cone.endpoint = ff(name + "$endpoint", cone.terminal->getPort(id_Q));
        cone.side_a = ff(name + "$side_a", cone.root->getPort(id_Q));
        cone.side_b = ff(name + "$side_b", cone.root->getPort(id_Q));
        cone.side_b->pin_data[id_ENA].state = PIN_INV;
        ctx->assignArchInfo();
        place(cone.inner, 24, y); place(cone.middle, 24, y); place(cone.root, 24, y);
        place(cone.terminal, 30, y); place(cone.endpoint, 30, y);
        place(cone.side_a, 24, y + 1); place(cone.side_b, 30, y + 1);
        make_hole(cone.root->getPort(id_Q), name + "$removed_root_user");
        make_hole(cone.middle->getPort(id_Q), name + "$removed_middle_user");
        return cone;
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
        auto segment = [&](const char *type, double delay, CellInfo *from, IdString fp, CellInfo *to, IdString tp) {
            Json::object value{{"type", type}, {"delay", delay}, {"from", endpoint(from, fp)}, {"to", endpoint(to, tp)}};
            if (std::string(type) == "routing") value["net"] = from->getPort(fp)->name.str(ctx.get());
            return Json(value);
        };
        Json::array path{
            segment("clk-to-q", .731, sources[4], id_Q, sources[4], id_Q),
            segment("routing", 2, sources[4], id_Q, cone.inner, id_B),
            segment("logic", .4, cone.inner, id_B, cone.inner, id_Q),
            segment("routing", 2, cone.inner, id_Q, cone.middle, id_B),
            segment("logic", .4, cone.middle, id_B, cone.middle, id_Q),
            segment("routing", 2, cone.middle, id_Q, cone.root, id_D),
            segment("logic", .4, cone.root, id_D, cone.root, id_Q),
            segment("routing", 2, cone.root, id_Q, cone.terminal, id_E),
            segment("logic", .4, cone.terminal, id_E, cone.terminal, id_Q),
            segment("routing", 2, cone.terminal, id_Q, cone.endpoint, id_ENA),
            segment("setup", -.196, cone.endpoint, id_ENA, cone.endpoint, id_ENA)};
        Json::object critical{{"from", "posedge comb_plan_clock"}, {"to", "posedge comb_plan_clock"},
                              {"max_delay", 1}, {"path", path}};
        return Json(Json::object{{"critical_paths", Json::array{critical}}}).dump();
    }
    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e9;
        clock = ctx->createNet(ctx->id("comb_plan_clock"));
        clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>();
        clock->clkconstr->period = DelayPair(1000);
        clock->clkconstr->high = clock->clkconstr->low = DelayPair(500);
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        for (unsigned i = 0; i < sources.size(); ++i) sources[i] = ff("shared_state_" + std::to_string(i), nullptr);
        ctx->net_aliases[ctx->id("shared_state_alias")] = sources[0]->getPort(id_Q)->name;
        ctx->assignArchInfo();
        for (auto *cell : sources) place(cell, 30, 22, STRENGTH_LOCKED);
        first = make_cone("first", 20); second = make_cone("second", 24);
        for (unsigned i = 0; i < sources.size(); ++i) make_shared_holes(sources[i]->getPort(id_Q), i);
        first.report = report(first); second.report = report(second);
        ctx->check();
    }
    Arch::CombRemapStep step(const Cone &cone, int candidate) const
    {
        Arch::CombRemapStep result; result.report = cone.report; result.candidate = candidate; return result;
    }
    CellInfo *clone(const Cone &cone) const
    {
        auto *net = cone.terminal->getPort(id_E);
        return net == cone.root->getPort(id_Q) ? nullptr : net->driver.cell;
    }
    int listed_candidate(const std::string &text, const Cone &cone) const
    {
        const std::string marker = "Comb remap candidate ";
        const std::string sink = "sink=" + cone.terminal->name.str(ctx.get()) + ".E cut=3 ";
        std::istringstream lines(text); std::string line;
        while (std::getline(lines, line)) {
            auto at = line.find(marker);
            if (at != std::string::npos && line.find(sink) != std::string::npos)
                return std::stoi(line.substr(at + marker.size()));
        }
        return -1;
    }
    int candidate(const Cone &cone)
    {
        CombPlanLogCapture capture(ctx.get());
        EXPECT_FALSE(ctx->remap_comb_critical(cone.report, -1));
        int selected = listed_candidate(capture.buffer.text, cone);
        EXPECT_GE(selected, 0) << capture.buffer.text;
        return selected;
    }
    bool evaluate(NetInfo *net, unsigned assignment) const
    {
        for (unsigned i = 0; i < sources.size(); ++i)
            if (net == sources[i]->getPort(id_Q)) return (assignment >> i) & 1;
        auto *cell = net->driver.cell;
        unsigned row = 0;
        for (unsigned i = 0; i < 6; ++i) {
            if (!cell->ports.count(comb_plan_pins[i])) continue;
            auto state = cell->get_pin_state(comb_plan_pins[i]);
            bool value = state == PIN_1;
            if (state == PIN_SIG || state == PIN_INV) {
                value = evaluate(cell->getPort(comb_plan_pins[i]), assignment);
                if (state == PIN_INV) value = !value;
            }
            row |= unsigned(value) << i;
        }
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> row) & 1;
    }
};

TEST_F(CombRemapPlanTest, TwoSharedInputCutsComposeAndPreserveAllOriginalCellsAndPolarity)
{
    CombPlanSnapshot before(ctx.get());
    std::array<bool, 32> expected_first, expected_second;
    for (unsigned row = 0; row < 32; ++row) {
        expected_first[row] = evaluate(first.terminal->getPort(id_Q), row);
        expected_second[row] = evaluate(second.terminal->getPort(id_Q), row);
    }
    int first_selection = candidate(first), second_selection = candidate(second);
    ASSERT_GE(first_selection, 0); ASSERT_GE(second_selection, 0);
    before.expect_exact(ctx.get());
    ctx->comb_remap_plan = {step(first, first_selection), step(second, second_selection)};
    CombPlanLogCapture capture(ctx.get());
    ASSERT_TRUE(ctx->execute_comb_remap_plan()) << capture.buffer.text;
    ASSERT_NE(clone(first), nullptr); ASSERT_NE(clone(second), nullptr);
    ASSERT_NE(capture.buffer.first, nullptr);
    EXPECT_EQ(ctx->cells.size(), before.cells.size() + 2);
    EXPECT_EQ(ctx->nets.size(), before.nets.size() + 2);
    // The first clone itself is preserved when the second stage is accepted.
    auto *first_clone = clone(first);
    capture.buffer.first->expect_cell(first_clone, capture.buffer.first->cells.at(first_clone->name));
    for (const auto &entry : before.cells) {
        SCOPED_TRACE(entry.first.str(ctx.get()));
        auto *cell = ctx->cells.at(entry.first).get();
        before.expect_cell(cell, entry.second, cell == first.terminal || cell == second.terminal);
    }
    for (const auto &entry : before.nets) {
        auto *net = entry.second.identity;
        EXPECT_EQ(net->driver.cell, entry.second.driver.cell);
        EXPECT_EQ(net->driver.port, entry.second.driver.port);
        auto saved_users = entry.second.users;
        for (auto user : saved_users.enumerate()) {
            bool removed = (user.value.cell == first.terminal || user.value.cell == second.terminal) &&
                           user.value.port == id_E;
            if (removed) { EXPECT_FALSE(net->users.count(user.index)); continue; }
            ASSERT_TRUE(net->users.count(user.index));
            EXPECT_EQ(net->users.at(user.index).cell, user.value.cell);
            EXPECT_EQ(net->users.at(user.index).port, user.value.port);
        }
    }
    for (const auto *cone : {&first, &second}) {
        auto *copy = clone(*cone);
        EXPECT_EQ(copy->type, id_MISTRAL_ALUT5);
        EXPECT_EQ(copy->getPort(id_Q)->users.entries(), 1);
        EXPECT_EQ(cone->terminal->get_pin_state(id_E), PIN_INV);
        EXPECT_EQ(cone->side_a->getPort(id_ENA), cone->root->getPort(id_Q));
        EXPECT_EQ(cone->side_b->getPort(id_ENA), cone->root->getPort(id_Q));
        EXPECT_EQ(cone->side_b->get_pin_state(id_ENA), PIN_INV);
        for (unsigned row = 0; row < 32; ++row) {
            EXPECT_EQ(evaluate(copy->getPort(id_Q), row), evaluate(cone->root->getPort(id_Q), row)) << row;
            EXPECT_EQ(evaluate(cone->terminal->getPort(id_Q), row),
                      cone == &first ? expected_first[row] : expected_second[row]) << row;
        }
    }
    for (auto *source : sources) {
        auto *net = source->getPort(id_Q);
        EXPECT_EQ(net->users.entries(), before.nets.at(net->name).users.entries() + 2);
    }
    for (const auto &alias : before.aliases) EXPECT_EQ(ctx->net_aliases.at(alias.first), alias.second);
    for (const auto &entry : ctx->cells) EXPECT_TRUE(ctx->isBelLocationValid(entry.second->bel));
    ctx->check();
}

TEST_F(CombRemapPlanTest, FailedSecondSelectionRestoresEveryProbeAndExactAcceptedFirstGraph)
{
    int selected = candidate(first); ASSERT_GE(selected, 0);
    ctx->comb_remap_plan = {step(first, selected), step(second, std::numeric_limits<int>::max())};
    CombPlanLogCapture capture(ctx.get());
    EXPECT_THROW(ctx->execute_comb_remap_plan(), log_execution_error_exception);
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    ASSERT_NE(clone(first), nullptr); EXPECT_EQ(clone(second), nullptr);
    EXPECT_GE(listed_candidate(capture.buffer.text, second), 0) << capture.buffer.text;
    capture.buffer.first->expect_exact(ctx.get());
}

TEST_F(CombRemapPlanTest, FinalListingRollsBackEveryProbeAndExactAcceptedFirstGraph)
{
    int selected = candidate(first); ASSERT_GE(selected, 0);
    ctx->comb_remap_plan = {step(first, selected), step(second, -1)};
    ctx->comb_remap_plan_list_only = true;
    CombPlanLogCapture capture(ctx.get());
    EXPECT_FALSE(ctx->execute_comb_remap_plan());
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    ASSERT_NE(clone(first), nullptr); EXPECT_EQ(clone(second), nullptr);
    EXPECT_GE(listed_candidate(capture.buffer.text, second), 0) << capture.buffer.text;
    capture.buffer.first->expect_exact(ctx.get());
}

TEST_F(CombRemapPlanTest, StaleSecondReportIsValidatedAgainstTheCurrentFirstAcceptedGraph)
{
    int selected = candidate(first); ASSERT_GE(selected, 0);
    // The report is initially valid. Stage one rewires its selected internal
    // edge, so repeating that original guide must fail current-path validation.
    ctx->comb_remap_plan = {step(first, selected), step(first, selected)};
    CombPlanLogCapture capture(ctx.get());
    EXPECT_THROW(ctx->execute_comb_remap_plan(), log_execution_error_exception);
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    EXPECT_NE(capture.buffer.text.find("Comb-remap plan step 1:"), std::string::npos);
    capture.buffer.first->expect_exact(ctx.get());
}

TEST_F(CombRemapPlanTest, AllPlanBoundsAndReportSyntaxFailBeforeTheFirstMutation)
{
    CombPlanSnapshot before(ctx.get());
    struct Invalid { std::vector<Arch::CombRemapStep> steps; bool listing; };
    std::vector<Invalid> invalid{{{}, false}, {std::vector<Arch::CombRemapStep>(9, step(first, 0)), false},
                                {{step(first, 0)}, true}, {{step(first, -1)}, false},
                                {{step(first, -2)}, false},
                                {{step(first, -1), step(second, -1)}, true},
                                {{step(first, 0), step(second, -2)}, false}};
    for (const std::string &report : {std::string(), std::string("{"), std::string("{}"),
                                    std::string("{\"critical_paths\":{}}")}) {
        auto bad = step(second, 0); bad.report = report;
        invalid.push_back({{step(first, 0), bad}, false});
    }
    for (const auto &request : invalid) {
        ctx->comb_remap_plan = request.steps; ctx->comb_remap_plan_list_only = request.listing;
        CombPlanLogCapture capture(ctx.get());
        EXPECT_THROW(ctx->execute_comb_remap_plan(), log_execution_error_exception);
        EXPECT_EQ(capture.buffer.first, nullptr);
        EXPECT_EQ(capture.buffer.text.find("Comb-remap plan step "), std::string::npos);
        before.expect_exact(ctx.get());
    }
}
