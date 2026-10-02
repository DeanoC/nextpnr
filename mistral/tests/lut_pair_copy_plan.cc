/* Native sequencing and retention proofs for LUT pair copy plans. SPDX-License-Identifier: ISC */
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>
#include "gtest/gtest.h"
#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

namespace {
const std::array<IdString, 6> sequence_pins = {id_A, id_B, id_C, id_D, id_E, id_F};

// A separate fixture keeps this test translation unit independent of the
// existing placement tests and their GoogleTest registrations.
struct SequenceSnapshot {
    struct Cell {
        CellInfo *identity;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        ClusterId cluster;
        Region *region;
        decltype(CellInfo::params) params;
        decltype(CellInfo::attrs) attrs;
        std::map<IdString, PortInfo> ports;
        std::map<IdString, ArchPinInfo> pins;
        std::vector<IdString> port_order;
        ArchCellInfo cache;
    };
    struct Net {
        NetInfo *identity;
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
    std::vector<LABInfo> labs;

    explicit SequenceSnapshot(Context *ctx) : labs(ctx->labs)
    {
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            Cell saved{cell, cell->type, cell->bel, cell->belStrength, cell->cluster, cell->region,
                       cell->params, cell->attrs, {}, {}, {}, static_cast<const ArchCellInfo &>(*cell)};
            for (const auto &port : cell->ports) {
                saved.ports.emplace(port.first, port.second);
                saved.port_order.push_back(port.first);
            }
            for (const auto &pin : cell->pin_data) saved.pins.emplace(pin.first, pin.second);
            cells.emplace(entry.first, std::move(saved));
            cell_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            nets.emplace(entry.first, Net{net, net->driver, net->users, net->attrs, net->wires});
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
        EXPECT_EQ(actual.type, saved.type);
        EXPECT_EQ(actual.net, saved.net);
        EXPECT_EQ(actual.user_idx, saved.user_idx);
    }

    static void expect_users(indexed_store<PortRef> actual, indexed_store<PortRef> saved)
    {
        ASSERT_EQ(actual.entries(), saved.entries());
        ASSERT_EQ(actual.capacity(), saved.capacity());
        for (auto user : saved.enumerate()) {
            ASSERT_TRUE(actual.count(user.index));
            EXPECT_EQ(actual.at(user.index).cell, user.value.cell);
            EXPECT_EQ(actual.at(user.index).port, user.value.port);
        }
        // Copies, not the live net, expose the complete recycled slot order.
        const size_t probes = size_t(saved.capacity()) + 8;
        for (size_t i = 0; i < probes; ++i) EXPECT_EQ(actual.add(PortRef{}), saved.add(PortRef{}));
    }

    void expect_cell(Context *ctx, IdString name, bool changed_enable = false) const
    {
        SCOPED_TRACE(name.str(ctx));
        ASSERT_TRUE(ctx->cells.count(name));
        const auto &saved = cells.at(name);
        auto *cell = ctx->cells.at(name).get();
        EXPECT_EQ(cell, saved.identity);
        EXPECT_EQ(cell->type, saved.type);
        EXPECT_EQ(cell->bel, saved.bel);
        EXPECT_EQ(cell->belStrength, saved.strength);
        EXPECT_EQ(cell->cluster, saved.cluster);
        EXPECT_EQ(cell->region, saved.region);
        EXPECT_EQ(cell->params, saved.params);
        EXPECT_EQ(cell->attrs, saved.attrs);
        EXPECT_EQ(cell->constr_children, saved.cache.constr_children);
        EXPECT_EQ(cell->constr_x, saved.cache.constr_x);
        EXPECT_EQ(cell->constr_y, saved.cache.constr_y);
        EXPECT_EQ(cell->constr_z, saved.cache.constr_z);
        EXPECT_EQ(cell->constr_abs_z, saved.cache.constr_abs_z);
        std::vector<IdString> order;
        for (const auto &port : cell->ports) order.push_back(port.first);
        EXPECT_EQ(order, saved.port_order);
        ASSERT_EQ(cell->ports.size(), saved.ports.size());
        for (const auto &port : saved.ports) {
            ASSERT_TRUE(cell->ports.count(port.first));
            if (changed_enable && port.first == id_ENA) {
                EXPECT_EQ(cell->ports.at(port.first).name, port.second.name);
                EXPECT_EQ(cell->ports.at(port.first).type, port.second.type);
            } else expect_port(cell->ports.at(port.first), port.second);
        }
        ASSERT_EQ(cell->pin_data.size(), saved.pins.size());
        for (const auto &pin : saved.pins) {
            ASSERT_TRUE(cell->pin_data.count(pin.first));
            EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
            EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
        }
        if (cell->type == id_MISTRAL_FF) {
            const auto &a = cell->ffInfo.ctrlset, &b = saved.cache.ffInfo.ctrlset;
            EXPECT_EQ(a.clk, b.clk);
            EXPECT_EQ(a.aclr, b.aclr);
            EXPECT_EQ(a.sclr, b.sclr);
            EXPECT_EQ(a.sload, b.sload);
            EXPECT_EQ(a.ena.inverted, b.ena.inverted);
            if (!changed_enable) EXPECT_EQ(a.ena.net, b.ena.net);
            EXPECT_EQ(cell->ffInfo.datain, saved.cache.ffInfo.datain);
            EXPECT_EQ(cell->ffInfo.sdata, saved.cache.ffInfo.sdata);
        } else if (ctx->is_comb_cell(cell->type)) {
            const auto &a = cell->combInfo, &b = saved.cache.combInfo;
            EXPECT_EQ(a.comb_out, b.comb_out);
            EXPECT_EQ(a.lut_input_count, b.lut_input_count);
            EXPECT_EQ(a.used_lut_input_count, b.used_lut_input_count);
            EXPECT_EQ(a.lut_bits_count, b.lut_bits_count);
            EXPECT_EQ(a.chain_shared_input_count, b.chain_shared_input_count);
            EXPECT_EQ(a.is_carry, b.is_carry);
            EXPECT_EQ(a.is_shared, b.is_shared);
            EXPECT_EQ(a.is_extended, b.is_extended);
            EXPECT_EQ(a.carry_start, b.carry_start);
            EXPECT_EQ(a.carry_end, b.carry_end);
            EXPECT_EQ(a.mlab_group, b.mlab_group);
            for (int i = 0; i < b.lut_input_count; ++i) EXPECT_EQ(a.lut_in[i], b.lut_in[i]);
        }
    }

    void expect_net(Context *ctx, IdString name) const
    {
        SCOPED_TRACE(name.str(ctx));
        ASSERT_TRUE(ctx->nets.count(name));
        auto *net = ctx->nets.at(name).get();
        const auto &saved = nets.at(name);
        EXPECT_EQ(net, saved.identity);
        EXPECT_EQ(net->driver.cell, saved.driver.cell);
        EXPECT_EQ(net->driver.port, saved.driver.port);
        EXPECT_EQ(net->attrs, saved.attrs);
        expect_users(net->users, saved.users);
        ASSERT_EQ(net->wires.size(), saved.wires.size());
        for (const auto &wire : saved.wires) {
            ASSERT_TRUE(net->wires.count(wire.first));
            EXPECT_EQ(net->wires.at(wire.first).pip, wire.second.pip);
            EXPECT_EQ(net->wires.at(wire.first).strength, wire.second.strength);
        }
    }

    void expect_exact(Context *ctx) const
    {
        ASSERT_EQ(ctx->cells.size(), cells.size());
        ASSERT_EQ(ctx->nets.size(), nets.size());
        ASSERT_EQ(ctx->net_aliases.size(), aliases.size());
        ASSERT_EQ(ctx->ports.size(), ports.size());
        std::vector<IdString> actual_cells, actual_nets, actual_aliases;
        for (const auto &row : ctx->cells) actual_cells.push_back(row.first);
        for (const auto &row : ctx->nets) actual_nets.push_back(row.first);
        for (const auto &row : ctx->net_aliases) actual_aliases.push_back(row.first);
        EXPECT_EQ(actual_cells, cell_order);
        EXPECT_EQ(actual_nets, net_order);
        EXPECT_EQ(actual_aliases, alias_order);
        for (const auto &entry : cells) expect_cell(ctx, entry.first);
        for (const auto &entry : nets) expect_net(ctx, entry.first);
        for (const auto &entry : aliases) EXPECT_EQ(ctx->net_aliases.at(entry.first), entry.second);
        for (const auto &entry : ports) expect_port(ctx->ports.at(entry.first), entry.second);
        ASSERT_EQ(ctx->labs.size(), labs.size());
        for (size_t i = 0; i < labs.size(); ++i) {
            const auto &a = ctx->labs[i], &b = labs[i];
            EXPECT_EQ(a.is_mlab, b.is_mlab);
            EXPECT_EQ(a.aclr_used, b.aclr_used);
            EXPECT_EQ(a.clk_wires, b.clk_wires);
            EXPECT_EQ(a.ena_wires, b.ena_wires);
            EXPECT_EQ(a.aclr_wires, b.aclr_wires);
            EXPECT_EQ(a.sclr_wire, b.sclr_wire);
            EXPECT_EQ(a.sload_wire, b.sload_wire);
            ASSERT_EQ(a.alms.size(), b.alms.size());
            for (size_t j = 0; j < b.alms.size(); ++j) {
                const auto &x = a.alms[j], &y = b.alms[j];
                EXPECT_EQ(x.comb_out, y.comb_out);
                EXPECT_EQ(x.sel_clk, y.sel_clk);
                EXPECT_EQ(x.sel_ena, y.sel_ena);
                EXPECT_EQ(x.sel_aclr, y.sel_aclr);
                EXPECT_EQ(x.sel_ef, y.sel_ef);
                EXPECT_EQ(x.ff_in, y.ff_in);
                EXPECT_EQ(x.ff_out, y.ff_out);
                EXPECT_EQ(x.lut_bels, y.lut_bels);
                EXPECT_EQ(x.ff_bels, y.ff_bels);
                EXPECT_EQ(x.l6_mode, y.l6_mode);
                EXPECT_EQ(x.carry_mode, y.carry_mode);
                EXPECT_EQ(x.clk_ena_idx, y.clk_ena_idx);
                EXPECT_EQ(x.aclr_idx, y.aclr_idx);
                EXPECT_EQ(x.unique_input_count, y.unique_input_count);
            }
        }
        ctx->check();
    }
};

// Observe the real first acceptance during one call to the sequencer. No
// preparatory copy is used to approximate that intermediate graph.
struct SequenceLog {
    struct Buffer : std::streambuf {
        Context *ctx;
        std::string text;
        std::unique_ptr<SequenceSnapshot> first;
        explicit Buffer(Context *ctx) : ctx(ctx) {}
        void inspect()
        {
            if (!first && text.find("LUT pair copy applied candidate 0;") != std::string::npos)
                first = std::make_unique<SequenceSnapshot>(ctx);
        }
        std::streamsize xsputn(const char *data, std::streamsize count) override
        {
            text.append(data, size_t(count));
            inspect();
            return count;
        }
        int_type overflow(int_type value) override
        {
            if (!traits_type::eq_int_type(value, traits_type::eof())) {
                text.push_back(traits_type::to_char_type(value));
                inspect();
            }
            return traits_type::not_eof(value);
        }
    } buffer;
    std::ostream stream;
    explicit SequenceLog(Context *ctx) : buffer(ctx), stream(&buffer)
    {
        log_streams.emplace_back(&stream, LogLevel::INFO_MSG);
    }
    ~SequenceLog() { log_streams.pop_back(); }
};
} // namespace

class LutPairCopyPlanTest : public ::testing::Test {
  protected:
    struct Cone {
        std::array<CellInfo *, 4> inputs;
        CellInfo *inner, *outer, *sink, *peer, *remote, *carry;
        std::string report;
    } first, second;
    std::unique_ptr<Context> ctx;
    NetInfo *clock;

    CellInfo *ff(const std::string &name, NetInfo *enable = nullptr)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->connectPort(id_CLK, clock);
        if (enable) cell->connectPort(id_ENA, enable);
        else cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = cell->pin_data[id_SLOAD].state = PIN_0;
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx->createNet(ctx->id(name + "$q")));
        cell->connectPort(id_DATAIN, cell->getPort(id_Q));
        return cell;
    }

    void place(CellInfo *cell, int x, int y, PlaceStrength strength = STRENGTH_WEAK, int z = -1)
    {
        for (auto bel : ctx->getBelsByTile(x, y)) {
            if (z >= 0 && ctx->getBelLocation(bel).z != z) continue;
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel)) continue;
            ctx->bindBel(bel, cell, strength);
            if (ctx->isBelLocationValid(bel)) return;
            ctx->unbindBel(bel);
        }
        FAIL() << "No legal copy-plan BEL for " << cell->name.str(ctx.get()) << " at " << x << ',' << y << ',' << z;
    }

    void make_holes(NetInfo *net, const std::string &prefix)
    {
        std::array<CellInfo *, 4> temporary;
        for (int i = 0; i < 4; ++i) {
            temporary[i] = ctx->createCell(ctx->id(prefix + std::to_string(i)), id_MISTRAL_ALUT2);
            temporary[i]->addInput(id_A);
            temporary[i]->connectPort(id_A, net);
        }
        for (int index : {1, 3, 0, 2}) {
            auto *cell = temporary[index];
            cell->disconnectPort(id_A);
            ctx->cells.erase(cell->name);
        }
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
            Json::object row{{"type", type}, {"delay", delay}, {"from", endpoint(from, source)},
                             {"to", endpoint(to, target)}};
            if (std::string(type) == "routing") row["net"] = from->getPort(source)->name.str(ctx.get());
            return Json(row);
        };
        Json::array path{
            segment("clk-to-q", .731, cone.inputs[0], id_Q, cone.inputs[0], id_Q),
            segment("routing", 2, cone.inputs[0], id_Q, cone.inner, id_A),
            segment("logic", .4, cone.inner, id_A, cone.inner, id_Q),
            segment("routing", 2, cone.inner, id_Q, cone.outer, id_B),
            segment("logic", .4, cone.outer, id_B, cone.outer, id_Q),
            segment("routing", 2, cone.outer, id_Q, cone.sink, id_ENA),
            segment("setup", -.196, cone.sink, id_ENA, cone.sink, id_ENA)};
        return Json(Json::object{{"critical_paths", Json::array{Json::object{{"max_delay", 1}, {"path", path}}}}}).dump();
    }

    Cone make_cone(const std::string &name, int y, CellInfo *shared_source = nullptr)
    {
        Cone cone;
        for (int i = 0; i < 4; ++i)
            cone.inputs[i] = i == 0 && shared_source ? shared_source : ff(name + "$literal_" + std::to_string(i));
        cone.inner = ctx->createCell(ctx->id(name + "$inner"), id_MISTRAL_ALUT2);
        cone.inner->params[id_LUT] = Property(0xb, 4);
        cone.inner->addInput(id_A); cone.inner->addInput(id_B); cone.inner->addOutput(id_Q);
        cone.inner->connectPort(id_A, cone.inputs[0]->getPort(id_Q));
        cone.inner->connectPort(id_B, cone.inputs[1]->getPort(id_Q));
        cone.inner->pin_data[id_B].state = PIN_INV;
        cone.inner->connectPort(id_Q, ctx->createNet(ctx->id(name + "$inner$q")));
        cone.outer = ctx->createCell(ctx->id(name + "$outer"), id_MISTRAL_ALUT3);
        cone.outer->params[id_LUT] = Property(0x32, 8);
        for (auto pin : {id_A, id_B, id_C}) cone.outer->addInput(pin);
        cone.outer->addOutput(id_Q);
        cone.outer->connectPort(id_A, cone.inputs[2]->getPort(id_Q));
        cone.outer->connectPort(id_B, cone.inner->getPort(id_Q));
        cone.outer->connectPort(id_C, cone.inputs[3]->getPort(id_Q));
        cone.outer->connectPort(id_Q, ctx->createNet(ctx->id(name + "$outer$q")));
        ctx->net_aliases[ctx->id(name + "$original_alias")] = cone.outer->getPort(id_Q)->name;
        cone.sink = ff(name + "$terminal", cone.outer->getPort(id_Q));
        cone.peer = ff(name + "$peer", cone.outer->getPort(id_Q));
        cone.peer->pin_data[id_ENA].state = PIN_INV;
        cone.remote = ff(name + "$remote", cone.outer->getPort(id_Q));
        cone.carry = ctx->createCell(ctx->id(name + "$protected_carry"), id_MISTRAL_ALUT_ARITH);
        for (auto pin : {id_A, id_B, id_C, id_D0, id_D1, id_CI}) cone.carry->addInput(pin);
        for (auto pin : {id_SO, id_CO}) {
            cone.carry->addOutput(pin);
            cone.carry->connectPort(pin, ctx->createNet(ctx->id(name + "$carry$" + pin.str(ctx.get()))));
        }
        cone.carry->params[id_LUT0] = Property(0x6666, 16);
        cone.carry->params[id_LUT1] = Property(0x6666, 16);
        for (auto pin : {id_A, id_B, id_C, id_D0, id_D1})
            cone.carry->connectPort(pin, cone.inputs[0]->getPort(id_Q));
        cone.carry->pin_data[id_CI].state = PIN_0;
        cone.carry->cluster = cone.carry->name;
        ctx->assignArchInfo();
        for (int i = 0; i < 4; ++i)
            if (!(i == 0 && shared_source)) place(cone.inputs[i], 30, y - 1, STRENGTH_LOCKED);
        place(cone.inner, 24, y); place(cone.outer, 24, y + 1);
        place(cone.carry, 30, y, STRENGTH_LOCKED, 0);
        place(cone.sink, 30, y, STRENGTH_WEAK, 8);
        place(cone.peer, 30, y, STRENGTH_WEAK, 14);
        place(cone.remote, 24, y + 2);
        std::swap(cone.inner->pin_data[id_A].bel_pins, cone.inner->pin_data[id_B].bel_pins);
        cone.report = report(cone);
        return cone;
    }

    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e9;
        ctx->settings[id_placer] = Property(std::string("heap"));
        clock = ctx->createNet(ctx->id("copy_plan_clock"));
        clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>();
        clock->clkconstr->period = DelayPair(1000);
        clock->clkconstr->high = clock->clkconstr->low = DelayPair(500);
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        first = make_cone("first", 20);
        second = make_cone("second", 23, first.inputs[0]);
        make_holes(first.inputs[0]->getPort(id_Q), "shared_source_hole_");
        make_holes(second.inputs[1]->getPort(id_Q), "second_source_hole_");
        make_holes(first.outer->getPort(id_Q), "first_enable_hole_");
        make_holes(second.outer->getPort(id_Q), "second_enable_hole_");
        ctx->check();
    }

    Arch::LutPairCopyStep step(const Cone &cone, int candidate = 0) const
    {
        return {cone.report, candidate};
    }

    bool value(NetInfo *net, const Cone &cone, unsigned assignment) const
    {
        for (unsigned i = 0; i < cone.inputs.size(); ++i)
            if (net == cone.inputs[i]->getPort(id_Q)) return (assignment >> i) & 1;
        if (!net || !net->driver.cell || !net->driver.cell->params.count(id_LUT)) {
            ADD_FAILURE() << "Unexpected copy-plan truth boundary";
            return false;
        }
        auto *cell = net->driver.cell;
        unsigned row = 0;
        for (unsigned i = 0; i < sequence_pins.size(); ++i) {
            auto pin = sequence_pins[i];
            if (!cell->ports.count(pin)) continue;
            auto state = cell->get_pin_state(pin);
            bool bit = state == PIN_1;
            if (state == PIN_SIG || state == PIN_INV) {
                bit = value(cell->getPort(pin), cone, assignment);
                if (state == PIN_INV) bit = !bit;
            } else EXPECT_TRUE(state == PIN_0 || state == PIN_1);
            row |= unsigned(bit) << i;
        }
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> row) & 1;
    }
};

TEST_F(LutPairCopyPlanTest, TwoDisjointCohortsKeepBothTruthTablesAndFirstAcceptedCopy)
{
    SequenceSnapshot before(ctx.get());
    TimingAnalyser old_timing(ctx.get());
    old_timing.with_clock_skew = true;
    old_timing.setup(false, false, true);
    std::array<float, 2> old_slacks{
        old_timing.get_setup_slack(CellPortKey(first.sink->name, id_ENA)),
        old_timing.get_setup_slack(CellPortKey(second.sink->name, id_ENA))};
    for (float slack : old_slacks) ASSERT_TRUE(std::isfinite(slack));
    ctx->lut_pair_copy_plan = {step(first), step(second)};
    SequenceLog capture(ctx.get());
    ASSERT_TRUE(ctx->execute_lut_pair_copy_plan()) << capture.buffer.text;
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    auto *first_q = first.sink->getPort(id_ENA);
    auto *second_q = second.sink->getPort(id_ENA);
    ASSERT_NE(first_q, first.outer->getPort(id_Q));
    ASSERT_NE(second_q, second.outer->getPort(id_Q));
    ASSERT_NE(first_q, second_q);
    auto *first_copy = first_q->driver.cell;
    auto *second_copy = second_q->driver.cell;
    ASSERT_NE(first_copy, nullptr); ASSERT_NE(second_copy, nullptr);
    EXPECT_EQ(first_copy->type, id_MISTRAL_ALUT4);
    EXPECT_EQ(second_copy->type, id_MISTRAL_ALUT4);
    EXPECT_EQ(ctx->cells.size(), before.cells.size() + 2);
    EXPECT_EQ(ctx->nets.size(), before.nets.size() + 2);
    EXPECT_EQ(ctx->net_aliases.size(), before.aliases.size() + 2);
    // The second copy may add shared-input users, but cannot alter the first
    // accepted clone, its private output or either retained ENA user.
    capture.buffer.first->expect_cell(ctx.get(), first_copy->name);
    capture.buffer.first->expect_net(ctx.get(), first_q->name);
    capture.buffer.first->expect_cell(ctx.get(), first.sink->name);
    capture.buffer.first->expect_cell(ctx.get(), first.peer->name);
    const std::set<IdString> changed = {first.sink->name, first.peer->name, second.sink->name, second.peer->name};
    for (const auto &entry : before.cells) before.expect_cell(ctx.get(), entry.first, changed.count(entry.first));
    for (const auto *cone : {&first, &second}) {
        auto *output = cone->sink->getPort(id_ENA);
        EXPECT_EQ(cone->peer->getPort(id_ENA), output);
        EXPECT_EQ(cone->peer->get_pin_state(id_ENA), PIN_INV);
        EXPECT_EQ(cone->remote->getPort(id_ENA), cone->outer->getPort(id_Q));
        ASSERT_EQ(output->users.entries(), 2);
        for (unsigned row = 0; row < 16; ++row)
            EXPECT_EQ(value(output, *cone, row), value(cone->outer->getPort(id_Q), *cone, row)) << row;
    }
    for (const auto &entry : before.aliases) EXPECT_EQ(ctx->net_aliases.at(entry.first), entry.second);
    TimingAnalyser new_timing(ctx.get());
    new_timing.with_clock_skew = true;
    new_timing.setup(false, false, true);
    EXPECT_GE(new_timing.get_setup_slack(CellPortKey(first.sink->name, id_ENA)), old_slacks[0] + 250);
    EXPECT_GE(new_timing.get_setup_slack(CellPortKey(second.sink->name, id_ENA)), old_slacks[1] + 250);
    EXPECT_NE(capture.buffer.text.find("LUT pair copy plan step 1: candidate=0."), std::string::npos);
    ctx->check();
}

TEST_F(LutPairCopyPlanTest, FailedSecondSelectionRestoresExactAcceptedFirstGraph)
{
    ctx->lut_pair_copy_plan = {step(first), step(second, 9999)};
    SequenceLog capture(ctx.get());
    EXPECT_THROW(ctx->execute_lut_pair_copy_plan(), log_execution_error_exception);
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    const auto stage = capture.buffer.text.find("LUT pair copy plan step 1:");
    ASSERT_NE(stage, std::string::npos);
    EXPECT_NE(capture.buffer.text.find("LUT pair copy candidate 0:", stage), std::string::npos) << capture.buffer.text;
    EXPECT_NE(capture.buffer.text.find("did not qualify; routing was not started", stage), std::string::npos);
    capture.buffer.first->expect_exact(ctx.get());
}

TEST_F(LutPairCopyPlanTest, FinalListingRestoresExactAcceptedFirstGraph)
{
    ctx->lut_pair_copy_plan = {step(first), step(second, -1)};
    ctx->lut_pair_copy_plan_list_only = true;
    SequenceLog capture(ctx.get());
    EXPECT_FALSE(ctx->execute_lut_pair_copy_plan());
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    const auto stage = capture.buffer.text.find("LUT pair copy plan step 1:");
    ASSERT_NE(stage, std::string::npos);
    EXPECT_NE(capture.buffer.text.find("LUT pair copy candidate 0:", stage), std::string::npos) << capture.buffer.text;
    capture.buffer.first->expect_exact(ctx.get());
}

TEST_F(LutPairCopyPlanTest, RetainedNonterminalMemberRejectsWholeCohortBeforeTrial)
{
    SequenceSnapshot before(ctx.get());
    pool<IdString> retained;
    retained.insert(second.peer->name); // The requested terminal is not in the set.
    SequenceLog capture(ctx.get());
    EXPECT_FALSE(ctx->remap_lut_pair_copy_critical(second.report, 0, &retained));
    EXPECT_NE(capture.buffer.text.find("reason=cohort-retargets-retained-copy"), std::string::npos) << capture.buffer.text;
    EXPECT_EQ(capture.buffer.text.find("LUT pair copy composition "), std::string::npos);
    EXPECT_EQ(capture.buffer.text.find("LUT pair copy trial "), std::string::npos);
    before.expect_exact(ctx.get());
}

TEST_F(LutPairCopyPlanTest, MalformedLaterReportIsRejectedBeforeAnyAcceptedCopy)
{
    SequenceSnapshot before(ctx.get());
    for (const std::string text : {
             std::string(), std::string("{"), std::string("{}"), std::string("{\"critical_paths\":{}}"),
             std::string("{\"critical_paths\":[{\"max_delay\":1,\"path\":\"bad\"}]}"),
             std::string("{\"critical_paths\":[{\"max_delay\":\"bad\",\"path\":[]}]}"),
             std::string("{\"critical_paths\":[{\"max_delay\":1,\"path\":[{\"type\":\"logic\",\"delay\":\"bad\"}]}]}")}) {
        SCOPED_TRACE(text);
        ctx->lut_pair_copy_plan = {step(first), {text, 0}};
        SequenceLog capture(ctx.get());
        EXPECT_THROW(ctx->execute_lut_pair_copy_plan(), log_execution_error_exception);
        EXPECT_EQ(capture.buffer.first, nullptr);
        EXPECT_EQ(capture.buffer.text.find("LUT pair copy discovery:"), std::string::npos);
        before.expect_exact(ctx.get());
    }
}

TEST_F(LutPairCopyPlanTest, StaleLaterLiveEdgeKeepsExactFirstCopy)
{
    auto stale = step(second);
    const auto offset = stale.report.find("second$inner$q");
    ASSERT_NE(offset, std::string::npos);
    stale.report.replace(offset, std::string("second$inner$q").size(), "wrong$inner$q");
    ctx->lut_pair_copy_plan = {step(first), stale};
    SequenceLog capture(ctx.get());
    EXPECT_THROW(ctx->execute_lut_pair_copy_plan(), log_execution_error_exception);
    ASSERT_NE(capture.buffer.first, nullptr) << capture.buffer.text;
    capture.buffer.first->expect_exact(ctx.get());
}

TEST_F(LutPairCopyPlanTest, BoundsAndListingModesRejectBeforeFirstMutation)
{
    SequenceSnapshot before(ctx.get());
    struct Invalid {
        std::vector<Arch::LutPairCopyStep> steps;
        bool listing;
    };
    const std::vector<Invalid> invalid{
        {{}, false}, {{}, true}, {std::vector<Arch::LutPairCopyStep>(3, step(first)), false},
        {{step(first), step(second, -2)}, false}, {{step(first), step(second, -1)}, false},
        {{step(first, -1), step(second, -1)}, true}, {{step(first), step(second)}, true}};
    for (size_t i = 0; i < invalid.size(); ++i) {
        SCOPED_TRACE(i);
        ctx->lut_pair_copy_plan = invalid[i].steps;
        ctx->lut_pair_copy_plan_list_only = invalid[i].listing;
        SequenceLog capture(ctx.get());
        EXPECT_THROW(ctx->execute_lut_pair_copy_plan(), log_execution_error_exception);
        EXPECT_EQ(capture.buffer.first, nullptr);
        EXPECT_EQ(capture.buffer.text.find("LUT pair copy discovery:"), std::string::npos);
        before.expect_exact(ctx.get());
    }
}

TEST_F(LutPairCopyPlanTest, EarlierListingLegacyPairAndFollowingDriverRejectBeforeMutation)
{
    SequenceSnapshot before(ctx.get());
    auto reject = [&]() {
        SequenceLog capture(ctx.get());
        EXPECT_THROW(ctx->execute_lut_pair_copy_plan(), log_execution_error_exception);
        EXPECT_EQ(capture.buffer.text.find("LUT pair copy discovery:"), std::string::npos);
        EXPECT_EQ(capture.buffer.first, nullptr);
        before.expect_exact(ctx.get());
    };
    ctx->lut_pair_copy_plan = {step(first)};
    for (auto *flag : {&ctx->local_remap_plan_list_only, &ctx->local_remap_post_plan_list_only,
                       &ctx->comb_remap_plan_list_only}) {
        *flag = true; reject(); *flag = false;
    }
    ctx->decomposition_remap_report = "{}";
    reject(); ctx->decomposition_remap_report.clear();
    ctx->lut_pair_report = first.report;
    reject(); ctx->lut_pair_report.clear();
    ctx->lut_pair_selection = 0;
    reject(); ctx->lut_pair_selection = -1;
    ctx->lut_pair_compose_copy = true;
    reject(); ctx->lut_pair_compose_copy = false;
    ctx->lut_pair_copy_plan = {step(first, -1)};
    ctx->lut_pair_copy_plan_list_only = true;
    ctx->lut_driver_copy_report = "{\"critical_paths\":[]}";
    reject();
}

TEST_F(LutPairCopyPlanTest, FullDesignGuardAndDefaultOffLeaveGraphUnchanged)
{
    SequenceSnapshot before(ctx.get());
    EXPECT_TRUE(ctx->lut_pair_copy_plan.empty());
    EXPECT_FALSE(ctx->lut_pair_copy_plan_list_only);
    EXPECT_NO_THROW(ctx->prevalidate_lut_pair_copy_plan());
    before.expect_exact(ctx.get());
    ctx->lut_pair_copy_plan = {step(first)};
    ctx->fes_any_slot_region_active = true;
    SequenceLog capture(ctx.get());
    EXPECT_THROW(ctx->execute_lut_pair_copy_plan(), log_execution_error_exception);
    EXPECT_EQ(capture.buffer.text.find("LUT pair copy discovery:"), std::string::npos);
    before.expect_exact(ctx.get());
}
