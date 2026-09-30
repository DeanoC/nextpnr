#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "gtest/gtest.h"
#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

namespace {
const IdString pins[] = {id_A, id_B, id_C, id_D, id_E, id_F};

struct DecompositionSnapshot {
    struct Cell {
        CellInfo *identity;
        const std::unique_ptr<CellInfo> *owner_slot;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        decltype(CellInfo::attrs) attrs;
        decltype(CellInfo::params) params;
        std::map<IdString, PortInfo> ports;
        std::map<IdString, ArchPinInfo> pin_data;
    };
    struct Net {
        NetInfo *identity;
        const std::unique_ptr<NetInfo> *owner_slot;
        PortRef driver;
        indexed_store<PortRef> users;
        decltype(NetInfo::attrs) attrs;
        decltype(NetInfo::wires) wires;
    };
    std::map<IdString, Cell> cells;
    std::map<IdString, Net> nets;
    std::map<IdString, IdString> aliases;
    std::map<IdString, PortInfo> ports;
    std::vector<IdString> cell_order, net_order, alias_order;

    explicit DecompositionSnapshot(Context *ctx)
    {
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            Cell saved{cell, &entry.second, cell->type, cell->bel, cell->belStrength,
                       cell->attrs, cell->params, {}, {}};
            for (const auto &port : cell->ports) saved.ports.emplace(port.first, port.second);
            for (const auto &pin : cell->pin_data) saved.pin_data.emplace(pin.first, pin.second);
            cells.emplace(entry.first, std::move(saved));
            cell_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            nets.emplace(entry.first, Net{net, &entry.second, net->driver, net->users, net->attrs, net->wires});
            net_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->net_aliases) {
            aliases.emplace(entry.first, entry.second);
            alias_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->ports) ports.emplace(entry.first, entry.second);
    }

    static void expect_port(const PortInfo &actual, const PortInfo &saved)
    {
        EXPECT_EQ(actual.name, saved.name);
        EXPECT_EQ(actual.net, saved.net);
        EXPECT_EQ(actual.type, saved.type);
        EXPECT_EQ(actual.user_idx, saved.user_idx);
    }

    void expect_exact(Context *ctx) const
    {
        ASSERT_EQ(ctx->cells.size(), cells.size());
        ASSERT_EQ(ctx->nets.size(), nets.size());
        ASSERT_EQ(ctx->net_aliases.size(), aliases.size());
        ASSERT_EQ(ctx->ports.size(), ports.size());
        std::vector<IdString> actual_cells, actual_nets, actual_aliases;
        for (const auto &entry : ctx->cells) actual_cells.push_back(entry.first);
        for (const auto &entry : ctx->nets) actual_nets.push_back(entry.first);
        for (const auto &entry : ctx->net_aliases) actual_aliases.push_back(entry.first);
        EXPECT_EQ(actual_cells, cell_order);
        EXPECT_EQ(actual_nets, net_order);
        EXPECT_EQ(actual_aliases, alias_order);
        for (const auto &entry : cells) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->cells.count(entry.first));
            auto *cell = ctx->cells.at(entry.first).get();
            const auto &saved = entry.second;
            EXPECT_EQ(&ctx->cells.at(entry.first), saved.owner_slot);
            EXPECT_EQ(cell, saved.identity);
            EXPECT_EQ(cell->type, saved.type);
            EXPECT_EQ(cell->bel, saved.bel);
            EXPECT_EQ(cell->belStrength, saved.strength);
            EXPECT_EQ(cell->attrs, saved.attrs);
            EXPECT_EQ(cell->params, saved.params);
            ASSERT_EQ(cell->ports.size(), saved.ports.size());
            for (const auto &port : saved.ports) {
                ASSERT_TRUE(cell->ports.count(port.first));
                expect_port(cell->ports.at(port.first), port.second);
            }
            ASSERT_EQ(cell->pin_data.size(), saved.pin_data.size());
            for (const auto &pin : saved.pin_data) {
                ASSERT_TRUE(cell->pin_data.count(pin.first));
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
        }
        for (const auto &entry : nets) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->nets.count(entry.first));
            auto *net = ctx->nets.at(entry.first).get();
            const auto &saved = entry.second;
            EXPECT_EQ(&ctx->nets.at(entry.first), saved.owner_slot);
            EXPECT_EQ(net, saved.identity);
            EXPECT_EQ(net->driver.cell, saved.driver.cell);
            EXPECT_EQ(net->driver.port, saved.driver.port);
            EXPECT_EQ(net->attrs, saved.attrs);
            ASSERT_EQ(net->wires.size(), saved.wires.size());
            for (const auto &wire : saved.wires) {
                ASSERT_TRUE(net->wires.count(wire.first));
                EXPECT_EQ(net->wires.at(wire.first).pip, wire.second.pip);
                EXPECT_EQ(net->wires.at(wire.first).strength, wire.second.strength);
            }
            auto actual = net->users, expected = saved.users;
            ASSERT_EQ(actual.entries(), expected.entries());
            ASSERT_EQ(actual.capacity(), expected.capacity());
            for (auto user : expected.enumerate()) {
                ASSERT_TRUE(actual.count(user.index));
                EXPECT_EQ(actual.at(user.index).cell, user.value.cell);
                EXPECT_EQ(actual.at(user.index).port, user.value.port);
            }
            // Use copies: cover every original hole and allocations beyond
            // the old capacity without changing the live stores.
            const size_t probes = size_t(saved.users.capacity()) + 8;
            for (size_t probe = 0; probe < probes; ++probe)
                EXPECT_EQ(actual.add(PortRef{}), expected.add(PortRef{}));
        }
        for (const auto &entry : aliases) EXPECT_EQ(ctx->net_aliases.at(entry.first), entry.second);
        for (const auto &entry : ports) expect_port(ctx->ports.at(entry.first), entry.second);
        ctx->check();
    }
};

struct DecompositionLog {
    std::ostringstream stream;
    DecompositionLog() { log_streams.emplace_back(&stream, LogLevel::INFO_MSG); }
    ~DecompositionLog() { log_streams.pop_back(); }
};

std::map<std::string, int> decomposition_hold_slacks(TimingAnalyser &timing)
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

class DecompositionBackendTest : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clock;
    std::array<NetInfo *, 7> boundary;
    std::vector<CellInfo *> source_ffs;
    CellInfo *r, *n0, *n, *n1, *n2, *terminal, *endpoint, *side;

    CellInfo *ff(const std::string &name, NetInfo *ena = nullptr)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clock);
        if (ena) cell->connectPort(id_ENA, ena);
        else cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        auto *q = ctx->createNet(ctx->idf("%s$q", name.c_str()));
        cell->connectPort(id_Q, q);
        cell->connectPort(id_DATAIN, q);
        return cell;
    }

    CellInfo *lut(const char *name, IdString type, int width, uint64_t mask)
    {
        auto *cell = ctx->createCell(ctx->id(name), type);
        cell->params[id_LUT] = Property(int64_t(mask), 1u << width);
        for (int pin = 0; pin < width; ++pin) cell->addInput(pins[pin]);
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", name)));
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
        FAIL() << "No legal fixture BEL for " << cell->name.str(ctx.get()) << " at " << x << ',' << y;
    }

    void hole(NetInfo *net, const std::string &name)
    {
        std::array<CellInfo *, 2> cells;
        for (int i = 0; i < 2; ++i) {
            cells[i] = ctx->createCell(ctx->id(name + std::to_string(i)), id_MISTRAL_ALUT2);
            cells[i]->addInput(id_A);
            cells[i]->connectPort(id_A, net);
        }
        for (auto *cell : cells) {
            cell->disconnectPort(id_A);
            auto id = cell->name;
            ctx->cells.erase(id);
        }
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
        const char *names[] = {"a", "s", "t", "b", "h", "q"};
        const int indices[] = {0, 1, 2, 3, 5, 6};
        for (int i = 0; i < 6; ++i) {
            auto *cell = ff(names[i]);
            source_ffs.push_back(cell);
            boundary[indices[i]] = cell->getPort(id_Q);
        }
        r = lut("outside_R", id_MISTRAL_ALUT6, 6, uint64_t(1) << 63);
        for (int i = 0; i < 6; ++i) {
            auto *cell = ff("r_input_" + std::to_string(i));
            source_ffs.push_back(cell);
            r->connectPort(pins[i], cell->getPort(id_Q));
        }
        boundary[4] = r->getPort(id_Q);
        ctx->net_aliases[ctx->id("R_alias")] = boundary[4]->name;
        n0 = lut("N0", id_MISTRAL_ALUT2, 2, 0x8);
        n0->connectPort(id_A, boundary[1]); n0->connectPort(id_B, boundary[4]);
        n = lut("n", id_MISTRAL_ALUT3, 3, 0x4c);
        n->connectPort(id_A, boundary[0]); n->connectPort(id_B, boundary[6]);
        n->connectPort(id_C, ctx->getNetByAlias(ctx->id("R_alias")));
        n1 = lut("N1", id_MISTRAL_ALUT2, 2, 0xb);
        n1->connectPort(id_A, n0->getPort(id_Q)); n1->connectPort(id_B, n->getPort(id_Q));
        n2 = lut("N2", id_MISTRAL_ALUT5, 5, 0x7500);
        n2->connectPort(id_A, boundary[1]); n2->connectPort(id_B, boundary[2]);
        n2->connectPort(id_C, boundary[3]); n2->connectPort(id_D, boundary[5]);
        n2->connectPort(id_E, n1->getPort(id_Q));
        terminal = lut("terminal", id_MISTRAL_ALUT3, 3, 0xd0);
        terminal->connectPort(id_A, boundary[0]); terminal->connectPort(id_B, boundary[3]);
        terminal->connectPort(id_C, n2->getPort(id_Q));
        endpoint = ff("endpoint", terminal->getPort(id_Q));
        side = ff("shared_N2_endpoint", n2->getPort(id_Q));
        ctx->assignArchInfo();
        for (int i = 0; i < 12; ++i)
            place(source_ffs[i], i < 6 ? 30 : 31, 22, STRENGTH_LOCKED);
        place(r, 25, 20);
        for (auto *cell : {n0, n, n1, n2}) place(cell, 24, 20);
        place(terminal, 30, 20);
        place(endpoint, 30, 19, STRENGTH_LOCKED);
        place(side, 24, 21, STRENGTH_LOCKED);
        for (int i = 0; i < 7; ++i) hole(boundary[i], "hole_" + std::to_string(i) + "_");
        hole(n2->getPort(id_Q), "hole_N2");
        ctx->check();
    }

    json11::Json endpoint_json(CellInfo *cell, IdString port) const
    {
        auto loc = ctx->getBelLocation(cell->bel);
        return json11::Json::object{{"cell", cell->name.str(ctx.get())}, {"port", port.str(ctx.get())},
                                   {"loc", json11::Json::array{loc.x, loc.y}}};
    }

    std::string report(bool stale = false) const
    {
        json11::Json::array path;
        auto segment = [&](const char *type, CellInfo *from, IdString fp, CellInfo *to, IdString tp) {
            json11::Json::object value{{"type", type}, {"delay", std::string(type) == "routing" ? 2.0 : 0.4},
                                      {"from", endpoint_json(from, fp)}, {"to", endpoint_json(to, tp)}};
            if (std::string(type) == "routing") value["net"] = from->getPort(fp)->name.str(ctx.get());
            path.emplace_back(value);
        };
        segment("routing", r, id_Q, n0, id_B);
        segment("logic", n0, id_B, n0, id_Q);
        segment("routing", n0, id_Q, n1, id_A);
        segment("logic", n1, id_A, n1, id_Q);
        segment("routing", n1, id_Q, n2, id_E);
        segment("logic", n2, id_E, n2, id_Q);
        segment("routing", n2, id_Q, terminal, id_C);
        segment("logic", terminal, id_C, terminal, id_Q);
        segment("routing", terminal, id_Q, endpoint, id_ENA);
        segment("setup", endpoint, id_ENA, endpoint, id_ENA);
        if (stale) {
            auto edge = path[6].object_items();
            auto target = edge["to"].object_items();
            auto loc = target["loc"].array_items(); loc[0] = loc[0].int_value() + 1;
            target["loc"] = loc; edge["to"] = target; path[6] = edge;
        }
        return json11::Json(json11::Json::object{{"critical_paths", json11::Json::array{
            json11::Json::object{{"from", "posedge clock"}, {"to", "posedge clock"},
                                 {"max_delay", 1}, {"path", path}}}}}).dump();
    }

    bool evaluate(NetInfo *net, unsigned row, std::set<NetInfo *> *active = nullptr) const
    {
        for (unsigned i = 0; i < boundary.size(); ++i) if (net == boundary[i]) return (row >> i) & 1;
        std::set<NetInfo *> owned;
        if (!active) active = &owned;
        if (!net || !net->driver.cell || !active->insert(net).second) {
            ADD_FAILURE() << "Invalid evaluation graph";
            return false;
        }
        auto *cell = net->driver.cell;
        unsigned physical_row = 0;
        for (int i = 0; i < 6; ++i) if (cell->ports.count(pins[i])) {
            auto state = cell->get_pin_state(pins[i]);
            bool value = state == PIN_1;
            if (state == PIN_SIG || state == PIN_INV) {
                value = evaluate(cell->getPort(pins[i]), row, active);
                if (state == PIN_INV) value = !value;
            }
            physical_row |= unsigned(value) << i;
        }
        active->erase(net);
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> physical_row) & 1;
    }

    void expect_rejected(const std::string &text, const DecompositionSnapshot &saved, int selection = 0)
    {
        try { EXPECT_FALSE(ctx->remap_decomposed_critical(text, selection)); }
        catch (const log_execution_error_exception &) { }
        saved.expect_exact(ctx.get());
    }
};

TEST_F(DecompositionBackendTest, FourNodeParallelFaninListsAndRollsBackExactly)
{
    DecompositionSnapshot saved(ctx.get());
    DecompositionLog log;
    EXPECT_FALSE(ctx->remap_decomposed_critical(report(), -1));
    // This is a real four-node, seven-essential-input cone: a two/three-node
    // path-only clone cannot implement it. A production rejection remains safe.
    EXPECT_NE(log.stream.str().find("Decomposition discovery: 1 bounded seven-input cuts."), std::string::npos);
    EXPECT_NE(log.stream.str().find("Decomposition trial root=N2 sink=terminal.C "), std::string::npos);
    saved.expect_exact(ctx.get());
    EXPECT_FALSE(ctx->remap_decomposed_critical(report(), -1));
    saved.expect_exact(ctx.get());
    EXPECT_FALSE(ctx->remap_decomposed_critical(report(), 9999));
    saved.expect_exact(ctx.get());
}

TEST_F(DecompositionBackendTest, QualifiedSelectionPreservesOriginalDagAndEveryTruthRow)
{
    std::array<bool, 128> expected;
    for (unsigned row = 0; row < expected.size(); ++row) expected[row] = evaluate(n2->getPort(id_Q), row);
    TimingAnalyser before(ctx.get()); before.setup(false, false, true);
    auto old_target_slack = before.get_setup_slack(CellPortKey(terminal->name, id_C));
    auto old_holds = decomposition_hold_slacks(before);
    std::map<CellInfo *, float> endpoint_slacks;
    for (auto *cell : {endpoint, side})
        endpoint_slacks[cell] = before.get_setup_slack(CellPortKey(cell->name, id_ENA));
    DecompositionSnapshot saved(ctx.get());
    DecompositionLog log;
    EXPECT_FALSE(ctx->remap_decomposed_critical(report(), -1));
    saved.expect_exact(ctx.get());
    ASSERT_TRUE(ctx->remap_decomposed_critical(report(), 0)) << log.stream.str();
    EXPECT_EQ(ctx->cells.size(), saved.cells.size() + 3);
    EXPECT_EQ(ctx->nets.size(), saved.nets.size() + 3);
    auto *replacement = terminal->getPort(id_C);
    ASSERT_NE(replacement, n2->getPort(id_Q));
    EXPECT_EQ(side->getPort(id_ENA), n2->getPort(id_Q));
    for (unsigned row = 0; row < expected.size(); ++row)
        EXPECT_EQ(evaluate(replacement, row), expected[row]) << row;
    for (const auto &entry : saved.cells) {
        auto *cell = ctx->cells.at(entry.first).get();
        EXPECT_EQ(cell, entry.second.identity);
        EXPECT_EQ(cell->type, entry.second.type);
        EXPECT_EQ(cell->params, entry.second.params);
        EXPECT_EQ(cell->attrs, entry.second.attrs);
        EXPECT_EQ(cell->bel, entry.second.bel);
        EXPECT_EQ(cell->belStrength, entry.second.strength);
        for (const auto &port : entry.second.ports) {
            if (cell != terminal || port.first != id_C)
                DecompositionSnapshot::expect_port(cell->ports.at(port.first), port.second);
        }
        for (const auto &pin : entry.second.pin_data) {
            EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
            EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
        }
    }
    for (const auto &entry : saved.nets) {
        auto *net = ctx->nets.at(entry.first).get();
        EXPECT_EQ(net, entry.second.identity);
        EXPECT_EQ(net->driver.cell, entry.second.driver.cell);
        EXPECT_EQ(net->driver.port, entry.second.driver.port);
        auto users = entry.second.users;
        for (auto user : users.enumerate()) {
            if (user.value.cell == terminal && user.value.port == id_C) continue;
            ASSERT_TRUE(net->users.count(user.index));
            EXPECT_EQ(net->users.at(user.index).cell, user.value.cell);
            EXPECT_EQ(net->users.at(user.index).port, user.value.port);
        }
    }
    TimingAnalyser after(ctx.get()); after.setup(false, false, true);
    EXPECT_GE(after.get_setup_slack(CellPortKey(terminal->name, id_C)), old_target_slack + 250);
    for (const auto &entry : endpoint_slacks)
        EXPECT_GE(after.get_setup_slack(CellPortKey(entry.first->name, id_ENA)), entry.second);
    for (const auto &entry : before.get_timing_result().clock_fmax) {
        ASSERT_TRUE(after.get_timing_result().clock_fmax.count(entry.first));
        EXPECT_GE(after.get_timing_result().clock_fmax.at(entry.first).achieved + 1e-4, entry.second.achieved);
    }
    for (const auto &entry : decomposition_hold_slacks(after)) {
        ASSERT_TRUE(old_holds.count(entry.first));
        EXPECT_GE(entry.second, old_holds.at(entry.first));
    }
    for (const auto &entry : ctx->cells) EXPECT_TRUE(ctx->isBelLocationValid(entry.second->bel));
    ctx->check();
}

TEST_F(DecompositionBackendTest, MixedInversionsAndAliasedBoundaryRemainExactOnRollback)
{
    n0->pin_data[id_A].state = PIN_INV; n0->params[id_LUT] = Property(int64_t(0x4), 4);
    n->pin_data[id_A].state = PIN_INV; n->params[id_LUT] = Property(int64_t(0x8c), 8);
    n1->pin_data[id_B].state = PIN_INV; n1->params[id_LUT] = Property(int64_t(0xe), 4);
    n2->pin_data[id_B].state = PIN_INV; n2->params[id_LUT] = Property(int64_t(0xd500), 32);
    ctx->assignArchInfo();
    // A transaction must retain existing physical mappings rather than
    // globally replace them with the architecture's default COMB pinmap.
    ASSERT_NE(n0->pin_data.at(id_A).bel_pins, n0->pin_data.at(id_B).bel_pins);
    ASSERT_NE(terminal->pin_data.at(id_A).bel_pins, terminal->pin_data.at(id_C).bel_pins);
    std::swap(n0->pin_data[id_A].bel_pins, n0->pin_data[id_B].bel_pins);
    std::swap(terminal->pin_data[id_A].bel_pins, terminal->pin_data[id_C].bel_pins);
    for (unsigned row = 0; row < 128; ++row) {
        bool a = row & 1, s = row & 2, t = row & 4, b = row & 8, r_value = row & 16;
        bool h = row & 32, q = row & 64;
        EXPECT_EQ(evaluate(n2->getPort(id_Q), row), h && q && ((!a && !s) || (!r_value && (!s || (!t && b)))));
    }
    DecompositionSnapshot saved(ctx.get());
    EXPECT_FALSE(ctx->remap_decomposed_critical(report(), -1));
    saved.expect_exact(ctx.get());
    EXPECT_FALSE(ctx->remap_decomposed_critical(report(), 9999));
    saved.expect_exact(ctx.get());
}

TEST_F(DecompositionBackendTest, StaleAndDiscontinuousReportsRejectBeforeMutation)
{
    DecompositionSnapshot saved(ctx.get());
    EXPECT_THROW(ctx->remap_decomposed_critical(report(true), 0), log_execution_error_exception);
    saved.expect_exact(ctx.get());
    std::string error;
    auto document = json11::Json::parse(report(), error).object_items();
    ASSERT_TRUE(error.empty());
    auto paths = document["critical_paths"].array_items();
    auto path = paths[0].object_items();
    auto segments = path["path"].array_items();
    segments.erase(segments.begin() + 3); // N1.A -> N1.Q is required for continuity.
    path["path"] = segments; paths[0] = path; document["critical_paths"] = paths;
    EXPECT_THROW(ctx->remap_decomposed_critical(json11::Json(document).dump(), 0), log_execution_error_exception);
    saved.expect_exact(ctx.get());
    document = json11::Json::parse(report(), error).object_items();
    paths = document["critical_paths"].array_items();
    path = paths[0].object_items();
    segments = path["path"].array_items();
    auto wrong_net = segments[6].object_items();
    wrong_net["net"] = boundary[0]->name.str(ctx.get());
    segments[6] = wrong_net; path["path"] = segments; paths[0] = path;
    document["critical_paths"] = paths;
    EXPECT_THROW(ctx->remap_decomposed_critical(json11::Json(document).dump(), 0), log_execution_error_exception);
    saved.expect_exact(ctx.get());
    EXPECT_THROW(ctx->remap_decomposed_critical(report(), -2), log_execution_error_exception);
    saved.expect_exact(ctx.get());
}

TEST_F(DecompositionBackendTest, ProtectedCutAndSinkLabExcludeProbes)
{
    n2->attrs[ctx->id("dont_touch")] = 1;
    DecompositionSnapshot protected_cut(ctx.get());
    expect_rejected(report(), protected_cut);
    n2->attrs.erase(ctx->id("dont_touch"));
    auto bel = terminal->bel;
    ctx->unbindBel(bel); ctx->bindBel(bel, terminal, STRENGTH_LOCKED);
    DecompositionSnapshot protected_sink(ctx.get());
    expect_rejected(report(), protected_sink);
}

TEST_F(DecompositionBackendTest, CyclicAndMalformedConesNeverReachUnsafeTiming)
{
    const auto original = n2->params;
    for (const auto &table : {Property(std::string("0111010100000000")), Property::from_string("0000000000000000000000000000000x"),
                              Property(int64_t(0x7500), 31), Property(int64_t(0x7500), 33)}) {
        n2->params[id_LUT] = table;
        DecompositionSnapshot saved(ctx.get());
        expect_rejected(report(), saved);
    }
    n2->params = original;
    n1->disconnectPort(id_B); n1->connectPort(id_B, n2->getPort(id_Q));
    ctx->assignArchInfo();
    DecompositionSnapshot cyclic(ctx.get());
    expect_rejected(report(), cyclic);
}

TEST_F(DecompositionBackendTest, ClockBoundaryAndObservableOutputExcludeCloning)
{
    auto *clock_user = ff("implicit_clock_user");
    clock_user->disconnectPort(id_CLK); clock_user->connectPort(id_CLK, boundary[1]);
    ctx->assignArchInfo(); place(clock_user, 15, 20, STRENGTH_LOCKED);
    DecompositionSnapshot clock_boundary(ctx.get());
    expect_rejected(report(), clock_boundary);
    clock_user->disconnectPort(id_CLK); clock_user->connectPort(id_CLK, clock);
    auto name = ctx->id("observable_terminal");
    ctx->ports[name] = PortInfo{name, terminal->getPort(id_Q), PORT_OUT, {}};
    ctx->assignArchInfo();
    DecompositionSnapshot observable(ctx.get());
    expect_rejected(report(), observable);
}

TEST_F(DecompositionBackendTest, SlotModeAndExistingRoutingRejectWithoutMutation)
{
    ctx->fes_any_slot_region_active = true;
    DecompositionSnapshot slot(ctx.get());
    expect_rejected(report(), slot);
    ctx->fes_any_slot_region_active = false;
    auto *net = n2->getPort(id_Q);
    auto wire = ctx->getBelPinWire(n2->bel, id_COMBOUT);
    ctx->bindWire(wire, net, STRENGTH_WEAK);
    DecompositionSnapshot routed(ctx.get());
    expect_rejected(report(), routed);
    ctx->unbindWire(wire);
}

TEST_F(DecompositionBackendTest, UnknownEndpointClockCannotQualify)
{
    endpoint->disconnectPort(id_CLK);
    ctx->assignArchInfo();
    DecompositionSnapshot saved(ctx.get());
    expect_rejected(report(), saved);
}

TEST_F(DecompositionBackendTest, ArrivalGetterIncludesClockToQOnceAndHandlesZeroAndMissing)
{
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    delay_t value = -1;
    ASSERT_TRUE(timing.get_max_arrival(CellPortKey(source_ffs[0]->name, id_Q), value));
    EXPECT_EQ(value, 731);
    delay_t deep = -1;
    ASSERT_TRUE(timing.get_max_arrival(CellPortKey(r->name, id_Q), deep));
    EXPECT_GT(deep, value);
    EXPECT_FALSE(timing.get_max_arrival(CellPortKey(ctx->id("absent_cell"), id_Q), value));
    EXPECT_FALSE(timing.get_max_arrival(CellPortKey(source_ffs[0]->name, ctx->id("absent_port")), value));
    auto *input = ctx->createCell(ctx->id("zero_arrival_input"), id_MISTRAL_CLKENA);
    input->addOutput(id_ENAOUT);
    input->connectPort(id_ENAOUT, ctx->createNet(ctx->id("zero_arrival_net")));
    ctx->assignArchInfo();
    TimingAnalyser input_timing(ctx.get()); input_timing.setup(false, false, true);
    value = -1;
    ASSERT_TRUE(input_timing.get_max_arrival(CellPortKey(input->name, id_ENAOUT), value));
    EXPECT_EQ(value, 0);
}
