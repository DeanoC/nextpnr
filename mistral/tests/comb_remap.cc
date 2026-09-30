#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include "gtest/gtest.h"
#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

namespace {
const IdString comb_pins[] = {id_A, id_B, id_C, id_D, id_E, id_F};

struct CombSnapshot {
    struct Cell {
        CellInfo *identity;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        decltype(CellInfo::params) params;
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

    explicit CombSnapshot(Context *ctx)
    {
        for (const auto &entry : ctx->cells) {
            auto *c = entry.second.get();
            Cell saved{c, c->type, c->bel, c->belStrength, c->params, {}, {}};
            for (const auto &p : c->ports) saved.ports.emplace(p.first, p.second);
            for (const auto &p : c->pin_data) saved.pins.emplace(p.first, p.second);
            cells.emplace(entry.first, std::move(saved));
        }
        for (const auto &entry : ctx->nets) {
            auto *n = entry.second.get();
            nets.emplace(entry.first, Net{n, n->driver, n->users});
        }
        for (const auto &entry : ctx->net_aliases) aliases.emplace(entry.first, entry.second);
        for (const auto &entry : ctx->ports) ports.emplace(entry.first, entry.second);
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
            auto *c = ctx->cells.at(entry.first).get();
            const auto &saved = entry.second;
            EXPECT_EQ(c, saved.identity);
            EXPECT_EQ(c->type, saved.type);
            EXPECT_EQ(c->bel, saved.bel);
            EXPECT_EQ(c->belStrength, saved.strength);
            EXPECT_EQ(c->params, saved.params);
            ASSERT_EQ(c->ports.size(), saved.ports.size());
            for (const auto &p : saved.ports) {
                ASSERT_TRUE(c->ports.count(p.first));
                const auto &actual = c->ports.at(p.first);
                EXPECT_EQ(actual.name, p.second.name);
                EXPECT_EQ(actual.net, p.second.net);
                EXPECT_EQ(actual.type, p.second.type);
                EXPECT_EQ(actual.user_idx, p.second.user_idx);
            }
            ASSERT_EQ(c->pin_data.size(), saved.pins.size());
            for (const auto &p : saved.pins) {
                ASSERT_TRUE(c->pin_data.count(p.first));
                EXPECT_EQ(c->pin_data.at(p.first).state, p.second.state);
                EXPECT_EQ(c->pin_data.at(p.first).bel_pins, p.second.bel_pins);
            }
        }
        for (const auto &entry : nets) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->nets.count(entry.first));
            auto *n = ctx->nets.at(entry.first).get();
            EXPECT_EQ(n, entry.second.identity);
            EXPECT_EQ(n->driver.cell, entry.second.driver.cell);
            EXPECT_EQ(n->driver.port, entry.second.driver.port);
            auto actual = n->users, expected = entry.second.users;
            ASSERT_EQ(actual.entries(), expected.entries());
            EXPECT_EQ(actual.capacity(), expected.capacity());
            for (auto user : expected.enumerate()) {
                ASSERT_TRUE(actual.count(user.index));
                EXPECT_EQ(actual.at(user.index).cell, user.value.cell);
                EXPECT_EQ(actual.at(user.index).port, user.value.port);
            }
            // Copy-only future allocations expose free-list order damage even
            // when every live user and port has returned to its original slot.
            for (int probe = 0; probe < 8; ++probe)
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

struct CombLogCapture {
    std::ostringstream stream;
    CombLogCapture() { log_streams.emplace_back(&stream, LogLevel::INFO_MSG); }
    ~CombLogCapture() { log_streams.pop_back(); }
};

std::map<std::string, int> comb_hold_slacks(TimingAnalyser &timing)
{
    std::map<std::string, int> result;
    for (const auto &path : timing.get_timing_result().min_delay_violations) {
        if (path.segments.empty()) continue;
        auto endpoint = path.segments.back().to;
        auto key = std::to_string(path.clock_pair.start.clock.index) + ":" +
            std::to_string(int(path.clock_pair.start.edge)) + ":" +
            std::to_string(path.clock_pair.end.clock.index) + ":" +
            std::to_string(int(path.clock_pair.end.edge)) + ":" +
            std::to_string(endpoint.first.index) + ":" + std::to_string(endpoint.second.index);
        int slack = 0;
        for (const auto &segment : path.segments) slack += segment.delay;
        if (!result.count(key)) result.emplace(key, slack);
        else result.at(key) = std::min(result.at(key), slack);
    }
    return result;
}
} // namespace

class CombRemapTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clock;
    std::array<CellInfo *, 5> sources;
    CellInfo *first, *second, *root, *terminal, *endpoint, *side_a, *side_b;
    std::map<NetInfo *, store_index<PortRef>> holes;

    CellInfo *ff(const char *name, NetInfo *ena)
    {
        auto *c = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (IdString p : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) c->addInput(p);
        c->addOutput(id_Q);
        c->connectPort(id_CLK, clock);
        if (ena) c->connectPort(id_ENA, ena);
        else c->pin_data[id_ENA].state = PIN_1;
        c->pin_data[id_ACLR].state = PIN_1;
        c->pin_data[id_SCLR].state = PIN_0;
        c->pin_data[id_SLOAD].state = PIN_0;
        auto *q = ctx->createNet(ctx->idf("%s$q", name));
        c->connectPort(id_Q, q);
        c->connectPort(id_DATAIN, q);
        return c;
    }
    CellInfo *lut(const char *name, IdString type, unsigned width, uint64_t mask)
    {
        auto *c = ctx->createCell(ctx->id(name), type);
        c->params[id_LUT] = Property(int64_t(mask), 1u << width);
        for (unsigned i = 0; i < width; ++i) c->addInput(comb_pins[i]);
        c->addOutput(id_Q);
        c->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", name)));
        return c;
    }
    void place(CellInfo *c, int x, int y, PlaceStrength strength = STRENGTH_WEAK)
    {
        for (auto bel : ctx->getBelsByTile(x, y)) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(c->type, bel)) continue;
            ctx->bindBel(bel, c, strength);
            if (ctx->isBelLocationValid(bel)) return;
            ctx->unbindBel(bel);
        }
        FAIL() << "No legal fixture BEL for " << c->name.str(ctx.get()) << " at " << x << "," << y;
    }
    store_index<PortRef> make_hole(NetInfo *n, const char *name)
    {
        auto *c = ctx->createCell(ctx->id(name), id_MISTRAL_ALUT2);
        c->addInput(id_A); c->connectPort(id_A, n);
        auto slot = c->ports.at(id_A).user_idx;
        c->disconnectPort(id_A); ctx->cells.erase(c->name);
        return slot;
    }
    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e9;
        clock = ctx->createNet(ctx->id("clock"));
        clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>();
        clock->clkconstr->period = DelayPair(1000);
        clock->clkconstr->high = clock->clkconstr->low = DelayPair(500);
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        const char *names[] = {"state_a", "state_b", "state_c", "state_d", "timeout"};
        for (unsigned i = 0; i < sources.size(); ++i) sources[i] = ff(names[i], nullptr);
        ctx->net_aliases[ctx->id("state_a_alias")] = sources[0]->getPort(id_Q)->name;
        first = lut("first", id_MISTRAL_ALUT2, 2, 0x8);
        first->connectPort(id_A, sources[0]->getPort(id_Q));
        first->pin_data[id_A].state = PIN_INV;
        first->connectPort(id_B, sources[4]->getPort(id_Q));
        second = lut("second", id_MISTRAL_ALUT2, 2, 0x2);
        second->connectPort(id_A, sources[2]->getPort(id_Q));
        second->pin_data[id_A].state = PIN_INV;
        second->connectPort(id_B, first->getPort(id_Q));
        root = lut("shared_root", id_MISTRAL_ALUT4, 4, 0xe400);
        root->connectPort(id_A, ctx->getNetByAlias(ctx->id("state_a_alias")));
        root->pin_data[id_A].state = PIN_INV;
        root->connectPort(id_B, sources[3]->getPort(id_Q));
        root->connectPort(id_C, sources[1]->getPort(id_Q));
        root->connectPort(id_D, second->getPort(id_Q));
        terminal = lut("terminal", id_MISTRAL_ALUT5, 5, 0x5f7f0000);
        for (unsigned i = 0; i < 4; ++i) terminal->connectPort(comb_pins[i], sources[i]->getPort(id_Q));
        terminal->pin_data[id_B].state = PIN_INV;
        terminal->connectPort(id_E, root->getPort(id_Q));
        terminal->pin_data[id_E].state = PIN_INV;
        endpoint = ff("endpoint", terminal->getPort(id_Q));
        side_a = ff("side_a", root->getPort(id_Q));
        side_b = ff("side_b", root->getPort(id_Q));
        side_b->pin_data[id_ENA].state = PIN_INV;
        ctx->assignArchInfo();
        // State FFs remain fixed. Their tile is separate from every clone site
        // in the sink tile, while their sources are close to that sink.
        for (auto *c : sources) place(c, 30, 22, STRENGTH_LOCKED);
        place(first, 24, 20); place(second, 24, 20); place(root, 24, 20);
        place(terminal, 30, 20); place(endpoint, 30, 20);
        place(side_a, 24, 21); place(side_b, 30, 21);
        for (unsigned i = 0; i < sources.size(); ++i) {
            auto *n = sources[i]->getPort(id_Q);
            holes[n] = make_hole(n, (std::string("removed_input_") + std::to_string(i)).c_str());
        }
        make_hole(root->getPort(id_Q), "removed_root_user");
        make_hole(second->getPort(id_Q), "removed_second_user");
        ctx->check();
    }

    std::string report(IdString endpoint_pin = id_ENA, bool stale = false) const
    {
        std::ostringstream out;
        auto ep = [&](CellInfo *c, IdString p) {
            auto loc = ctx->getBelLocation(c->bel);
            out << "{\"cell\":\"" << c->name.str(ctx.get()) << "\",\"port\":\"" << p.str(ctx.get())
                << "\",\"loc\":[" << (loc.x + (stale && c == terminal ? 1 : 0)) << ',' << loc.y << "]}";
        };
        bool comma = false;
        auto segment = [&](const char *type, double delay, CellInfo *from, IdString fp, CellInfo *to, IdString tp) {
            if (comma) out << ',';
            comma = true;
            out << "{\"type\":\"" << type << "\",\"delay\":" << delay;
            if (std::string(type) == "routing") out << ",\"net\":\"" << from->getPort(fp)->name.str(ctx.get()) << '\"';
            out << ",\"from\":"; ep(from, fp); out << ",\"to\":"; ep(to, tp); out << '}';
        };
        out << R"({"critical_paths":[{"from":"posedge clock","to":"posedge clock","max_delay":1,"path":[)";
        segment("routing", 2, sources[4], id_Q, first, id_B);
        segment("logic", 0.4, first, id_B, first, id_Q);
        segment("routing", 2, first, id_Q, second, id_B);
        segment("logic", 0.4, second, id_B, second, id_Q);
        segment("routing", 2, second, id_Q, root, id_D);
        segment("logic", 0.4, root, id_D, root, id_Q);
        segment("routing", 2, root, id_Q, terminal, id_E);
        segment("logic", 0.4, terminal, id_E, terminal, id_Q);
        segment("routing", 2, terminal, id_Q, endpoint, endpoint_pin);
        segment("setup", 0, endpoint, endpoint_pin, endpoint, endpoint_pin);
        out << "]}]}";
        return out.str();
    }
    json11::Json report_endpoint(CellInfo *cell, IdString pin) const
    {
        auto loc = ctx->getBelLocation(cell->bel);
        return json11::Json::object{{"cell", cell->name.str(ctx.get())}, {"port", pin.str(ctx.get())},
                                   {"loc", json11::Json::array{loc.x, loc.y}}};
    }
    template <typename Edit> std::string edited_report(Edit edit) const
    {
        std::string error;
        auto document = json11::Json::parse(report(), error).object_items();
        EXPECT_TRUE(error.empty());
        auto paths = document.at("critical_paths").array_items();
        auto path = paths.at(0).object_items();
        auto segments = path.at("path").array_items();
        edit(segments);
        path["path"] = segments;
        paths[0] = path;
        document["critical_paths"] = paths;
        return json11::Json(document).dump();
    }
    int listed_three_lut_candidate(const std::string &text) const
    {
        const std::string marker = "Comb remap candidate ";
        std::istringstream lines(text); std::string line;
        while (std::getline(lines, line)) {
            auto at = line.find(marker);
            if (at != std::string::npos && line.find("sink=terminal.E cut=3 ") != std::string::npos)
                return std::stoi(line.substr(at + marker.size()));
        }
        return -1;
    }
    int list_three_lut_candidate(const std::string &text)
    {
        CombLogCapture log;
        EXPECT_FALSE(ctx->remap_comb_critical(text, -1));
        return listed_three_lut_candidate(log.stream.str());
    }
    bool evaluate(NetInfo *n, unsigned assignment) const
    {
        for (unsigned i = 0; i < sources.size(); ++i)
            if (n == sources[i]->getPort(id_Q)) return (assignment >> i) & 1;
        auto *c = n->driver.cell;
        unsigned row = 0;
        for (unsigned i = 0; i < 6; ++i) {
            if (!c->ports.count(comb_pins[i])) continue;
            auto state = c->get_pin_state(comb_pins[i]);
            bool value = state == PIN_1;
            if (state == PIN_SIG || state == PIN_INV) {
                value = evaluate(c->getPort(comb_pins[i]), assignment);
                if (state == PIN_INV) value = !value;
            }
            row |= unsigned(value) << i;
        }
        return (uint64_t(c->params.at(id_LUT).as_int64()) >> row) & 1;
    }
};

TEST_F(CombRemapTest, ThreeLutInternalClonePreservesSharedRootAndAllOriginalPlacements)
{
    auto text = report();
    CombSnapshot saved(ctx.get());
    std::array<bool, 32> expected;
    for (unsigned row = 0; row < expected.size(); ++row) expected[row] = evaluate(terminal->getPort(id_Q), row);
    auto *old_target = root->getPort(id_Q);
    auto old_slot = terminal->ports.at(id_E).user_idx;
    TimingAnalyser before(ctx.get()); before.setup(false, false, true);
    float target_slack = before.get_setup_slack(CellPortKey(terminal->name, id_E));
    auto hold_slacks = comb_hold_slacks(before);
    std::map<CellInfo *, float> endpoint_slacks;
    for (auto *c : {endpoint, side_a, side_b}) endpoint_slacks[c] = before.get_setup_slack(CellPortKey(c->name, id_ENA));
    int selected = list_three_lut_candidate(text);
    ASSERT_GE(selected, 0);
    saved.expect_exact(ctx.get());
    ASSERT_TRUE(ctx->remap_comb_critical(text, selected));
    auto *replacement = terminal->getPort(id_E);
    ASSERT_NE(replacement, old_target);
    auto *clone = replacement->driver.cell;
    ASSERT_NE(clone, nullptr);
    EXPECT_EQ(ctx->cells.size(), saved.cells.size() + 1);
    EXPECT_EQ(ctx->nets.size(), saved.nets.size() + 1);
    EXPECT_EQ(clone->type, id_MISTRAL_ALUT5);
    ASSERT_EQ(replacement->users.entries(), 1);
    EXPECT_EQ(replacement->users.at(terminal->ports.at(id_E).user_idx).cell, terminal);
    EXPECT_EQ(replacement->users.at(terminal->ports.at(id_E).user_idx).port, id_E);
    EXPECT_EQ(terminal->get_pin_state(id_E), PIN_INV);
    EXPECT_FALSE(old_target->users.count(old_slot));
    EXPECT_EQ(side_a->getPort(id_ENA), old_target);
    EXPECT_EQ(side_b->getPort(id_ENA), old_target);
    EXPECT_EQ(side_b->get_pin_state(id_ENA), PIN_INV);
    auto sink_loc = ctx->getBelLocation(terminal->bel), clone_loc = ctx->getBelLocation(clone->bel);
    EXPECT_LE(std::abs(clone_loc.x - sink_loc.x) + std::abs(clone_loc.y - sink_loc.y), 3);
    for (unsigned row = 0; row < expected.size(); ++row) {
        EXPECT_EQ(evaluate(terminal->getPort(id_Q), row), expected[row]) << row;
        EXPECT_EQ(evaluate(replacement, row), evaluate(old_target, row)) << row;
    }
    for (const auto &entry : saved.cells) {
        auto *c = ctx->cells.at(entry.first).get();
        EXPECT_EQ(c, entry.second.identity);
        EXPECT_EQ(c->type, entry.second.type);
        EXPECT_EQ(c->params, entry.second.params);
        EXPECT_EQ(c->bel, entry.second.bel);
        EXPECT_EQ(c->belStrength, entry.second.strength);
        for (const auto &p : entry.second.ports) {
            EXPECT_EQ(c->get_pin_state(p.first), entry.second.pins.at(p.first).state);
            if (c == terminal && p.first == id_E) continue;
            EXPECT_EQ(c->ports.at(p.first).net, p.second.net);
            EXPECT_EQ(c->ports.at(p.first).user_idx, p.second.user_idx);
        }
    }
    for (const auto &entry : saved.nets) {
        auto *n = entry.second.identity;
        auto old_users = entry.second.users;
        EXPECT_EQ(n->driver.cell, entry.second.driver.cell);
        EXPECT_EQ(n->driver.port, entry.second.driver.port);
        for (auto user : old_users.enumerate()) {
            if (n == old_target && user.index == old_slot) continue;
            ASSERT_TRUE(n->users.count(user.index));
            EXPECT_EQ(n->users.at(user.index).cell, user.value.cell);
            EXPECT_EQ(n->users.at(user.index).port, user.value.port);
        }
    }
    for (unsigned i = 0; i < 5; ++i) {
        auto p = clone->ports.at(comb_pins[i]);
        ASSERT_TRUE(holes.count(p.net));
        EXPECT_EQ(p.user_idx, holes.at(p.net));
        EXPECT_EQ(p.net->users.at(p.user_idx).cell, clone);
        EXPECT_EQ(p.net->users.at(p.user_idx).port, comb_pins[i]);
        EXPECT_EQ(clone->get_pin_state(comb_pins[i]), PIN_SIG);
    }
    for (const auto &alias : saved.aliases) EXPECT_EQ(ctx->net_aliases.at(alias.first), alias.second);
    TimingAnalyser after(ctx.get()); after.setup(false, false, true);
    EXPECT_GE(after.get_setup_slack(CellPortKey(terminal->name, id_E)), target_slack + 250);
    for (const auto &entry : endpoint_slacks)
        EXPECT_GE(after.get_setup_slack(CellPortKey(entry.first->name, id_ENA)), entry.second);
    for (const auto &entry : before.get_timing_result().clock_fmax) {
        ASSERT_TRUE(after.get_timing_result().clock_fmax.count(entry.first));
        EXPECT_GE(after.get_timing_result().clock_fmax.at(entry.first).achieved + 1e-4, entry.second.achieved);
    }
    for (const auto &entry : comb_hold_slacks(after)) {
        ASSERT_TRUE(hold_slacks.count(entry.first));
        EXPECT_GE(entry.second, hold_slacks.at(entry.first));
    }
    for (const auto &entry : ctx->cells) EXPECT_TRUE(ctx->isBelLocationValid(entry.second->bel));
    ctx->check();
}

TEST_F(CombRemapTest, ListAndInvalidSelectionRestoreAllSlotsAndFreeListsRepeatedly)
{
    auto text = report();
    CombSnapshot saved(ctx.get());
    ASSERT_GE(list_three_lut_candidate(text), 0);
    saved.expect_exact(ctx.get());
    ASSERT_GE(list_three_lut_candidate(text), 0);
    saved.expect_exact(ctx.get());
    EXPECT_FALSE(ctx->remap_comb_critical(text, 9999));
    saved.expect_exact(ctx.get());
    EXPECT_FALSE(ctx->remap_comb_critical(text, 9999));
    saved.expect_exact(ctx.get());
}

TEST_F(CombRemapTest, DataInputEndingPathCanSelectAnInternalLutEdge)
{
    endpoint->disconnectPort(id_ENA); endpoint->pin_data[id_ENA].state = PIN_1;
    endpoint->disconnectPort(id_DATAIN); endpoint->connectPort(id_DATAIN, terminal->getPort(id_Q));
    ctx->assignArchInfo();
    auto text = report(id_DATAIN);
    CombSnapshot saved(ctx.get());
    int selected = list_three_lut_candidate(text);
    ASSERT_GE(selected, 0);
    saved.expect_exact(ctx.get());
    auto *data = endpoint->getPort(id_DATAIN);
    auto data_slot = endpoint->ports.at(id_DATAIN).user_idx;
    ASSERT_TRUE(ctx->remap_comb_critical(text, selected));
    EXPECT_NE(terminal->getPort(id_E), root->getPort(id_Q));
    EXPECT_EQ(endpoint->getPort(id_DATAIN), data);
    EXPECT_EQ(endpoint->ports.at(id_DATAIN).user_idx, data_slot);
    ctx->check();
}

TEST_F(CombRemapTest, StaleReportAndInvalidNegativeSelectionFailBeforeMutation)
{
    CombSnapshot saved(ctx.get());
    EXPECT_THROW(ctx->remap_comb_critical(report(id_ENA, true), 0), log_execution_error_exception);
    saved.expect_exact(ctx.get());
    EXPECT_THROW(ctx->remap_comb_critical(report(), -2), log_execution_error_exception);
    saved.expect_exact(ctx.get());
}

TEST_F(CombRemapTest, ProtectedRootAndFrozenTargetExcludeAllOverlappingCuts)
{
    root->attrs[ctx->id("dont_touch")] = 1;
    CombSnapshot protected_state(ctx.get());
    EXPECT_FALSE(ctx->remap_comb_critical(report(), 0));
    protected_state.expect_exact(ctx.get());
    root->attrs.erase(ctx->id("dont_touch"));
    auto bel = root->bel; ctx->unbindBel(bel); ctx->bindBel(bel, root, STRENGTH_LOCKED);
    CombSnapshot fixed_state(ctx.get());
    EXPECT_FALSE(ctx->remap_comb_critical(report(), 0));
    fixed_state.expect_exact(ctx.get());
}

TEST_F(CombRemapTest, ObservableInputAndIntermediateNetsExcludeCuts)
{
    auto name = ctx->id("observed_state");
    ctx->ports[name] = PortInfo{name, sources[0]->getPort(id_Q), PORT_OUT, {}};
    CombSnapshot input_state(ctx.get());
    EXPECT_FALSE(ctx->remap_comb_critical(report(), 0));
    input_state.expect_exact(ctx.get());
    ctx->ports.erase(name);
    name = ctx->id("observed_intermediate");
    ctx->ports[name] = PortInfo{name, first->getPort(id_Q), PORT_OUT, {}};
    CombSnapshot intermediate_state(ctx.get());
    EXPECT_FALSE(ctx->remap_comb_critical(report(), 0));
    intermediate_state.expect_exact(ctx.get());
}

TEST_F(CombRemapTest, UntimedHardInputConsumerExcludesCuts)
{
    auto *hard = ctx->createCell(ctx->id("hard_boundary"), id_cyclonev_hps_interface_fpga2sdram);
    auto pin = ctx->id("cmd_data_0[0]");
    hard->addInput(pin); hard->connectPort(pin, sources[0]->getPort(id_Q));
    ctx->assignArchInfo();
    bool placed = false;
    for (auto bel : ctx->getBels()) {
        if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(hard->type, bel)) continue;
        ctx->bindBel(bel, hard, STRENGTH_WEAK); placed = true; break;
    }
    ASSERT_TRUE(placed);
    CombSnapshot saved(ctx.get());
    EXPECT_FALSE(ctx->remap_comb_critical(report(), 0));
    saved.expect_exact(ctx.get());
}

TEST_F(CombRemapTest, ImplicitClockInputConsumerExcludesCuts)
{
    auto *clock_user = ff("clock_user", nullptr);
    clock_user->disconnectPort(id_CLK); clock_user->connectPort(id_CLK, sources[0]->getPort(id_Q));
    ctx->assignArchInfo(); place(clock_user, 15, 20);
    CombSnapshot saved(ctx.get());
    EXPECT_FALSE(ctx->remap_comb_critical(report(), 0));
    saved.expect_exact(ctx.get());
}

TEST_F(CombRemapTest, MalformedCutOrSinkTruthTablesRejectBeforeTimingAnalysis)
{
    // Every overlapping window either contains shared_root or uses it as its
    // sink. A literal string must be rejected before as_int64() or STA, while
    // X/Z and wrong-width numeric tables must never be silently interpreted.
    const auto original = root->params;
    const std::vector<std::pair<const char *, Property>> malformed = {
        {"string", Property(std::string("1110010000000000"))},
        {"unknown bit", Property::from_string("111001000000000x")},
        {"high impedance bit", Property::from_string("111001000000000z")},
        {"short table", Property(int64_t(0xe400), 15)},
        {"long table", Property(int64_t(0xe400), 17)},
    };
    auto text = report();
    for (const auto &entry : malformed) {
        SCOPED_TRACE(entry.first);
        root->params = original;
        root->params[id_LUT] = entry.second;
        CombSnapshot saved(ctx.get());
        EXPECT_FALSE(ctx->remap_comb_critical(text, 0));
        saved.expect_exact(ctx.get());
    }
    root->params = original;
    root->params[ctx->id("EXTRA_LUT_PARAM")] = 1;
    {
        SCOPED_TRACE("extra parameter");
        CombSnapshot saved(ctx.get());
        EXPECT_FALSE(ctx->remap_comb_critical(text, 0));
        saved.expect_exact(ctx.get());
    }
    root->params = original;
    root->params.erase(id_LUT);
    {
        SCOPED_TRACE("missing LUT parameter");
        CombSnapshot saved(ctx.get());
        EXPECT_FALSE(ctx->remap_comb_critical(text, 0));
        saved.expect_exact(ctx.get());
    }
    root->params = original;
    root->pin_data[id_Q].state = PIN_INV;
    {
        SCOPED_TRACE("inverted Q");
        CombSnapshot saved(ctx.get());
        EXPECT_FALSE(ctx->remap_comb_critical(text, 0));
        saved.expect_exact(ctx.get());
    }
    root->pin_data[id_Q].state = PIN_SIG;
}

TEST_F(CombRemapTest, ObservableDownstreamOutputExcludesEarlierAndTerminalCuts)
{
    auto name = ctx->id("observed_terminal_output");
    ctx->ports[name] = PortInfo{name, terminal->getPort(id_Q), PORT_OUT, {}};
    ASSERT_EQ(terminal->getPort(id_Q)->users.entries(), 1);
    // The ordinary FF user still exists, so noticing that endpoint is not
    // enough to establish that this net has no untimed external observation.
    CombSnapshot saved(ctx.get());
    EXPECT_FALSE(ctx->remap_comb_critical(report(), 0));
    saved.expect_exact(ctx.get());
}

TEST_F(CombRemapTest, ProtectedDownstreamNetExcludesEarlierAndTerminalCuts)
{
    auto *output = terminal->getPort(id_Q);
    for (const char *name : {"keep", "dont_touch"}) {
        SCOPED_TRACE(name);
        auto attr = ctx->id(name);
        output->attrs[attr] = 1;
        CombSnapshot saved(ctx.get());
        EXPECT_FALSE(ctx->remap_comb_critical(report(), 0));
        saved.expect_exact(ctx.get());
        EXPECT_EQ(output->attrs.at(attr).as_int64(), 1);
        output->attrs.erase(attr);
    }
}

TEST_F(CombRemapTest, ProtectedDownstreamLutExcludesEarlierAndTerminalCuts)
{
    // terminal is the direct sink of the longest cut and lies downstream of
    // the shorter first/second cut into shared_root.D.
    terminal->attrs[ctx->id("dont_touch")] = 1;
    CombSnapshot saved(ctx.get());
    EXPECT_FALSE(ctx->remap_comb_critical(report(), 0));
    saved.expect_exact(ctx.get());
    EXPECT_EQ(terminal->attrs.at(ctx->id("dont_touch")).as_int64(), 1);
}

TEST_F(CombRemapTest, ValidCutPathCannotBeSplicedOntoAnUnrelatedRegisteredSetupEndpoint)
{
    auto text = edited_report([&](json11::Json::array &segments) {
        auto setup = segments.back().object_items();
        // state_a.DATAIN is an existing, placed, registered FF input. Every
        // routing edge remains valid, but the final edge ends at endpoint.ENA.
        auto unrelated = report_endpoint(sources[0], id_DATAIN);
        setup["from"] = unrelated;
        setup["to"] = unrelated;
        segments.back() = setup;
    });
    CombSnapshot saved(ctx.get());
    CombLogCapture log;
    EXPECT_THROW(ctx->remap_comb_critical(text, 0), log_execution_error_exception);
    EXPECT_EQ(log.stream.str().find("Comb remap trial "), std::string::npos);
    saved.expect_exact(ctx.get());
}

TEST_F(CombRemapTest, MissingInterveningLutLogicArcRejectsBeforeAnyTrial)
{
    auto text = edited_report([](json11::Json::array &segments) {
        // Removing second.B -> second.Q leaves two individually valid routing
        // edges separated by an unreported combinational transition.
        segments.erase(segments.begin() + 3);
    });
    CombSnapshot saved(ctx.get());
    CombLogCapture log;
    EXPECT_THROW(ctx->remap_comb_critical(text, 0), log_execution_error_exception);
    EXPECT_EQ(log.stream.str().find("Comb remap trial "), std::string::npos);
    saved.expect_exact(ctx.get());
}

TEST_F(CombRemapTest, ValidLogicArcOnTheWrongInputCannotBridgeRoutingEdges)
{
    auto text = edited_report([&](json11::Json::array &segments) {
        auto logic = segments.at(3).object_items();
        // second.A -> Q is a real cell arc, but the preceding routing edge
        // ends at second.B. Port existence and arc legality alone are too weak.
        logic["from"] = report_endpoint(second, id_A);
        segments[3] = logic;
    });
    CombSnapshot saved(ctx.get());
    CombLogCapture log;
    EXPECT_THROW(ctx->remap_comb_critical(text, 0), log_execution_error_exception);
    EXPECT_EQ(log.stream.str().find("Comb remap trial "), std::string::npos);
    saved.expect_exact(ctx.get());
}
