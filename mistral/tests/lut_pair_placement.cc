/* Native joint-placement guards and exact rollback. SPDX-License-Identifier: ISC */
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
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
struct PairSnapshot {
    struct Cell {
        CellInfo *identity;
        const std::unique_ptr<CellInfo> *owner;
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
        const std::unique_ptr<NetInfo> *owner;
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

    explicit PairSnapshot(Context *ctx) : labs(ctx->labs)
    {
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            Cell saved{cell, &entry.second, cell->type, cell->bel, cell->belStrength, cell->cluster, cell->region, cell->params,
                       cell->attrs, {}, {}, {}, static_cast<const ArchCellInfo &>(*cell)};
            for (const auto &pin : cell->ports) {
                saved.ports.emplace(pin.first, pin.second); saved.port_order.push_back(pin.first);
            }
            for (const auto &pin : cell->pin_data) saved.pins.emplace(pin.first, pin.second);
            cells.emplace(entry.first, std::move(saved)); cell_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            nets.emplace(entry.first, Net{net, &entry.second, net->driver, net->users, net->attrs, net->wires});
            net_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->net_aliases) {
            aliases.emplace(entry.first, entry.second); alias_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->ports) ports.emplace(entry.first, entry.second);
    }

    static void port(const PortInfo &a, const PortInfo &b)
    {
        EXPECT_EQ(a.name, b.name); EXPECT_EQ(a.type, b.type);
        EXPECT_EQ(a.net, b.net); EXPECT_EQ(a.user_idx, b.user_idx);
    }

    static void users(indexed_store<PortRef> a, indexed_store<PortRef> b)
    {
        ASSERT_EQ(a.entries(), b.entries()); ASSERT_EQ(a.capacity(), b.capacity());
        for (auto user : b.enumerate()) {
            ASSERT_TRUE(a.count(user.index));
            EXPECT_EQ(a.at(user.index).cell, user.value.cell); EXPECT_EQ(a.at(user.index).port, user.value.port);
        }
        const size_t probes = size_t(b.capacity()) + 8;
        for (size_t i = 0; i < probes; ++i) EXPECT_EQ(a.add(PortRef{}), b.add(PortRef{}));
    }

    void expect(Context *ctx, const std::set<IdString> &moved = {}) const
    {
        ASSERT_EQ(ctx->cells.size(), cells.size()); ASSERT_EQ(ctx->nets.size(), nets.size());
        ASSERT_EQ(ctx->net_aliases.size(), aliases.size()); ASSERT_EQ(ctx->ports.size(), ports.size());
        std::vector<IdString> actual_cells, actual_nets, actual_aliases;
        for (const auto &row : ctx->cells) actual_cells.push_back(row.first);
        for (const auto &row : ctx->nets) actual_nets.push_back(row.first);
        for (const auto &row : ctx->net_aliases) actual_aliases.push_back(row.first);
        EXPECT_EQ(actual_cells, cell_order); EXPECT_EQ(actual_nets, net_order); EXPECT_EQ(actual_aliases, alias_order);
        std::set<std::pair<int, int>> changed_alms;
        for (const auto &row : cells) {
            SCOPED_TRACE(row.first.str(ctx));
            ASSERT_TRUE(ctx->cells.count(row.first));
            auto *cell = ctx->cells.at(row.first).get(); const auto &saved = row.second;
            EXPECT_EQ(cell, saved.identity); EXPECT_EQ(&ctx->cells.at(row.first), saved.owner);
            EXPECT_EQ(cell->type, saved.type); EXPECT_EQ(cell->belStrength, saved.strength);
            EXPECT_EQ(cell->params, saved.params); EXPECT_EQ(cell->attrs, saved.attrs);
            EXPECT_EQ(cell->cluster, saved.cluster); EXPECT_EQ(cell->region, saved.region);
            EXPECT_EQ(cell->constr_children, saved.cache.constr_children);
            EXPECT_EQ(cell->constr_x, saved.cache.constr_x); EXPECT_EQ(cell->constr_y, saved.cache.constr_y);
            EXPECT_EQ(cell->constr_z, saved.cache.constr_z); EXPECT_EQ(cell->constr_abs_z, saved.cache.constr_abs_z);
            if (!moved.count(row.first)) EXPECT_EQ(cell->bel, saved.bel);
            else for (auto bel : {cell->bel, saved.bel}) {
                const auto &data = ctx->bel_data(bel).lab_data;
                changed_alms.emplace(data.lab, data.alm);
            }
            std::vector<IdString> actual_ports;
            for (const auto &pin : cell->ports) actual_ports.push_back(pin.first);
            EXPECT_EQ(actual_ports, saved.port_order); ASSERT_EQ(cell->ports.size(), saved.ports.size());
            for (const auto &pin : saved.ports) { ASSERT_TRUE(cell->ports.count(pin.first)); port(cell->ports.at(pin.first), pin.second); }
            ASSERT_EQ(cell->pin_data.size(), saved.pins.size());
            for (const auto &pin : saved.pins) {
                ASSERT_TRUE(cell->pin_data.count(pin.first));
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
            if (ctx->is_comb_cell(cell->type)) {
                const auto &a = cell->combInfo, &b = saved.cache.combInfo;
                EXPECT_EQ(a.comb_out, b.comb_out); EXPECT_EQ(a.lut_input_count, b.lut_input_count);
                EXPECT_EQ(a.used_lut_input_count, b.used_lut_input_count); EXPECT_EQ(a.lut_bits_count, b.lut_bits_count);
                EXPECT_EQ(a.chain_shared_input_count, b.chain_shared_input_count); EXPECT_EQ(a.mlab_group, b.mlab_group);
                EXPECT_EQ(a.is_carry, b.is_carry); EXPECT_EQ(a.is_shared, b.is_shared); EXPECT_EQ(a.is_extended, b.is_extended);
                EXPECT_EQ(a.carry_start, b.carry_start); EXPECT_EQ(a.carry_end, b.carry_end);
                for (int i = 0; i < b.lut_input_count; ++i) EXPECT_EQ(a.lut_in[i], b.lut_in[i]);
            } else if (cell->type == id_MISTRAL_FF) {
                EXPECT_EQ(cell->ffInfo.ctrlset, saved.cache.ffInfo.ctrlset);
                EXPECT_EQ(cell->ffInfo.datain, saved.cache.ffInfo.datain); EXPECT_EQ(cell->ffInfo.sdata, saved.cache.ffInfo.sdata);
            }
        }
        for (const auto &row : nets) {
            ASSERT_TRUE(ctx->nets.count(row.first));
            auto *net = ctx->nets.at(row.first).get(); const auto &saved = row.second;
            EXPECT_EQ(net, saved.identity); EXPECT_EQ(&ctx->nets.at(row.first), saved.owner);
            EXPECT_EQ(net->driver.cell, saved.driver.cell); EXPECT_EQ(net->driver.port, saved.driver.port);
            EXPECT_EQ(net->attrs, saved.attrs); users(net->users, saved.users);
            ASSERT_EQ(net->wires.size(), saved.wires.size());
            for (const auto &wire : saved.wires) {
                ASSERT_TRUE(net->wires.count(wire.first));
                EXPECT_EQ(net->wires.at(wire.first).pip, wire.second.pip);
                EXPECT_EQ(net->wires.at(wire.first).strength, wire.second.strength);
            }
        }
        for (const auto &row : aliases) EXPECT_EQ(ctx->net_aliases.at(row.first), row.second);
        for (const auto &row : ports) port(ctx->ports.at(row.first), row.second);
        ASSERT_EQ(ctx->labs.size(), labs.size());
        for (size_t i = 0; i < labs.size(); ++i) {
            const auto &a = ctx->labs[i], &b = labs[i];
            EXPECT_EQ(a.is_mlab, b.is_mlab); EXPECT_EQ(a.aclr_used, b.aclr_used);
            EXPECT_EQ(a.clk_wires, b.clk_wires); EXPECT_EQ(a.ena_wires, b.ena_wires); EXPECT_EQ(a.aclr_wires, b.aclr_wires);
            EXPECT_EQ(a.sclr_wire, b.sclr_wire); EXPECT_EQ(a.sload_wire, b.sload_wire);
            for (size_t j = 0; j < b.alms.size(); ++j) {
                const auto &x = a.alms[j], &y = b.alms[j];
                EXPECT_EQ(x.comb_out, y.comb_out); EXPECT_EQ(x.sel_clk, y.sel_clk); EXPECT_EQ(x.sel_ena, y.sel_ena);
                EXPECT_EQ(x.sel_aclr, y.sel_aclr); EXPECT_EQ(x.sel_ef, y.sel_ef); EXPECT_EQ(x.ff_in, y.ff_in); EXPECT_EQ(x.ff_out, y.ff_out);
                EXPECT_EQ(x.lut_bels, y.lut_bels); EXPECT_EQ(x.ff_bels, y.ff_bels);
                EXPECT_EQ(x.l6_mode, y.l6_mode); EXPECT_EQ(x.carry_mode, y.carry_mode);
                EXPECT_EQ(x.clk_ena_idx, y.clk_ena_idx); EXPECT_EQ(x.aclr_idx, y.aclr_idx);
                if (!changed_alms.count({int(i), int(j)})) EXPECT_EQ(x.unique_input_count, y.unique_input_count);
            }
        }
        ctx->check();
    }
};

struct PairLog {
    std::ostringstream stream;
    PairLog() { log_streams.emplace_back(&stream, LogLevel::INFO_MSG); }
    ~PairLog() { log_streams.pop_back(); }
};

struct PairTrialObserver : std::streambuf {
    Context *ctx;
    CellInfo *endpoint;
    std::ostream stream;
    std::string text, line;
    int trials = 0, setup_rejections = 0, hold_rejections = 0, shorter_setup_hold_rejections = 0, negative_trial_holds = 0;
    bool throw_on_trial;
    PairTrialObserver(Context *ctx, CellInfo *endpoint, bool throw_on_trial = false)
        : ctx(ctx), endpoint(endpoint), stream(this), throw_on_trial(throw_on_trial)
    {
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        log_streams.emplace_back(&stream, LogLevel::INFO_MSG);
    }
    ~PairTrialObserver() { log_streams.pop_back(); }
    void append(char value)
    {
        text.push_back(value);
        if (value != '\n') { line.push_back(value); return; }
        if (line.find("LUT pair placement trial ") != std::string::npos) {
            ++trials;
            setup_rejections += line.find("endpoints=0") != std::string::npos;
            hold_rejections += line.find("hold=0") != std::string::npos;
            shorter_setup_hold_rejections += line.find("improve=1") != std::string::npos &&
                line.find("hold=0") != std::string::npos;
            TimingAnalyser timing(ctx); timing.with_clock_skew = true; timing.setup(false, false, true);
            std::vector<EndpointClockPairTiming> rows;
            if (timing.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_ENA), rows))
                for (const auto &row : rows) negative_trial_holds += row.hold_margin && *row.hold_margin < 0;
            if (throw_on_trial) throw std::runtime_error("actual joint-placement trial log exception");
        }
        line.clear();
    }
    std::streamsize xsputn(const char *data, std::streamsize count) override
    { for (std::streamsize i = 0; i < count; ++i) append(data[i]); return count; }
    int_type overflow(int_type value) override
    { if (!traits_type::eq_int_type(value, traits_type::eof())) append(traits_type::to_char_type(value)); return traits_type::not_eof(value); }
};
} // namespace

class LutPairPlacementTest : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clock;
    std::array<CellInfo *, 4> inputs;
    CellInfo *inner, *outer, *sink, *carry;

    CellInfo *ff(const std::string &name, NetInfo *enable = nullptr, NetInfo *data = nullptr)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->connectPort(id_CLK, clock);
        if (enable) cell->connectPort(id_ENA, enable); else cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1; cell->pin_data[id_SCLR].state = cell->pin_data[id_SLOAD].state = PIN_0;
        cell->addOutput(id_Q); cell->connectPort(id_Q, ctx->createNet(ctx->id(name + "$q")));
        cell->connectPort(id_DATAIN, data ? data : cell->getPort(id_Q));
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
        FAIL() << "No legal pair-fixture BEL for " << cell->name.str(ctx.get()) << " at " << x << ',' << y << ',' << z;
    }

    void hole(NetInfo *net, const std::string &name)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_ALUT2);
        cell->addInput(id_A); cell->connectPort(id_A, net); cell->disconnectPort(id_A); ctx->cells.erase(cell->name);
    }

    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7"; ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e9; ctx->settings[id_placer] = Property(std::string("heap"));
        clock = ctx->createNet(ctx->id("pair_clock")); clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>();
        clock->clkconstr->period = DelayPair(1000); clock->clkconstr->high = clock->clkconstr->low = DelayPair(500);
        ctx->createNet(ctx->id("$PACKER_GND_NET")); ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        for (int i = 0; i < 4; ++i) inputs[i] = ff("literal_" + std::to_string(i));
        inner = ctx->createCell(ctx->id("inner"), id_MISTRAL_ALUT2); inner->params[id_LUT] = Property(0xb, 4);
        inner->addInput(id_A); inner->addInput(id_B); inner->addOutput(id_Q);
        inner->connectPort(id_A, inputs[0]->getPort(id_Q)); inner->connectPort(id_B, inputs[1]->getPort(id_Q));
        inner->connectPort(id_Q, ctx->createNet(ctx->id("inner$q"))); inner->pin_data[id_B].state = PIN_INV;
        outer = ctx->createCell(ctx->id("outer"), id_MISTRAL_ALUT3); outer->params[id_LUT] = Property(0x32, 8);
        for (auto pin : {id_A, id_B, id_C}) outer->addInput(pin);
        outer->addOutput(id_Q); outer->connectPort(id_A, inputs[2]->getPort(id_Q));
        outer->connectPort(id_B, inner->getPort(id_Q)); outer->connectPort(id_C, inputs[3]->getPort(id_Q));
        outer->connectPort(id_Q, ctx->createNet(ctx->id("outer$q")));
        ctx->net_aliases[ctx->id("original_outer_alias")] = outer->getPort(id_Q)->name;
        sink = ff("terminal", outer->getPort(id_Q));
        carry = ctx->createCell(ctx->id("protected_carry"), id_MISTRAL_ALUT_ARITH);
        for (auto pin : {id_A, id_B, id_C, id_D0, id_D1, id_CI}) carry->addInput(pin);
        for (auto pin : {id_SO, id_CO}) { carry->addOutput(pin); carry->connectPort(pin, ctx->createNet(ctx->idf("carry$%s", pin.str(ctx.get()).c_str()))); }
        carry->params[id_LUT0] = Property(0x6666, 16); carry->params[id_LUT1] = Property(0x6666, 16);
        for (auto pin : {id_A, id_B, id_C, id_D0, id_D1}) carry->connectPort(pin, inputs[0]->getPort(id_Q));
        carry->pin_data[id_CI].state = PIN_0; carry->cluster = carry->name;
        ctx->assignArchInfo();
        for (auto *cell : inputs) place(cell, 30, 19, STRENGTH_LOCKED);
        place(inner, 24, 20); place(outer, 24, 21);
        place(carry, 30, 20, STRENGTH_LOCKED, 0); place(sink, 30, 20, STRENGTH_LOCKED, 8);
        std::swap(inner->pin_data[id_A].bel_pins, inner->pin_data[id_B].bel_pins);
        for (auto *cell : inputs) hole(cell->getPort(id_Q), cell->name.str(ctx.get()) + "$hole");
        hole(inner->getPort(id_Q), "inner$hole"); hole(outer->getPort(id_Q), "outer$hole"); ctx->check();
    }

    json11::Json endpoint(CellInfo *cell, IdString pin) const
    {
        auto loc = ctx->getBelLocation(cell->bel);
        return json11::Json::object{{"cell", cell->name.str(ctx.get())}, {"port", pin.str(ctx.get())}, {"loc", json11::Json::array{loc.x, loc.y}}};
    }

    std::string report() const
    {
        using json11::Json;
        auto segment = [&](const char *type, double delay, CellInfo *a, IdString ap, CellInfo *b, IdString bp) {
            Json::object row{{"type", type}, {"delay", delay}, {"from", endpoint(a, ap)}, {"to", endpoint(b, bp)}};
            if (std::string(type) == "routing") row["net"] = a->getPort(ap)->name.str(ctx.get());
            return Json(row);
        };
        Json::array path{segment("clk-to-q", .731, inputs[0], id_Q, inputs[0], id_Q),
            segment("routing", 2, inputs[0], id_Q, inner, id_A), segment("logic", .4, inner, id_A, inner, id_Q),
            segment("routing", 2, inner, id_Q, outer, id_B), segment("logic", .4, outer, id_B, outer, id_Q),
            segment("routing", 2, outer, id_Q, sink, id_ENA), segment("setup", -.196, sink, id_ENA, sink, id_ENA)};
        return Json(Json::object{{"critical_paths", Json::array{Json::object{{"max_delay", 1}, {"path", path}}}}}).dump();
    }

    CellInfo *side(const std::string &name, NetInfo *net, int x, int y)
    {
        auto *cell = ff(name, nullptr, net); ctx->assign_ff_info(cell); ctx->assign_default_pinmap(cell);
        place(cell, x, y, STRENGTH_LOCKED); return cell;
    }

    CellInfo *clock_source(NetInfo *net, const std::string &name)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_CLKBUF);
        cell->addOutput(id_Q); cell->connectPort(id_Q, net); ctx->assign_default_pinmap(cell);
        for (auto bel : ctx->getBels()) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel)) continue;
            ctx->bindBel(bel, cell, STRENGTH_LOCKED);
            if (ctx->isBelLocationValid(bel)) return cell;
            ctx->unbindBel(bel);
        }
        ADD_FAILURE() << "No legal independent clock buffer"; return nullptr;
    }

    void reject(const std::string &text, const PairSnapshot &saved, int selection = 0)
    {
        try { EXPECT_FALSE(ctx->remap_lut_pair_critical(text, selection)); }
        catch (const log_execution_error_exception &) { }
        saved.expect(ctx.get());
    }
};

TEST_F(LutPairPlacementTest, JointMoveAllowsProtectedCarryEndpointAndPreservesEntireGraph)
{
    PairSnapshot saved(ctx.get()); PairLog log;
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    const auto slack = before.get_setup_slack(CellPortKey(sink->name, id_ENA));
    ASSERT_TRUE(std::isfinite(slack));
    ASSERT_TRUE(ctx->remap_lut_pair_critical(report(), 0)) << log.stream.str();
    EXPECT_NE(inner->bel, saved.cells.at(inner->name).bel); EXPECT_NE(outer->bel, saved.cells.at(outer->name).bel);
    saved.expect(ctx.get(), {inner->name, outer->name});
    EXPECT_EQ(ctx->getBelLocation(sink->bel).x, 30); EXPECT_EQ(ctx->getBelLocation(sink->bel).y, 20);
    for (auto *cell : {inner, outer}) {
        const auto site = ctx->getBelLocation(cell->bel);
        EXPECT_FALSE(site.x == 30 && site.y == 20); EXPECT_TRUE(ctx->isBelLocationValid(cell->bel));
    }
    TimingAnalyser after(ctx.get()); after.with_clock_skew = true; after.setup(false, false, true);
    EXPECT_GE(after.get_setup_slack(CellPortKey(sink->name, id_ENA)), slack + 250);
    EXPECT_NE(log.stream.str().find("LUT pair placement applied candidate 0"), std::string::npos);
}

TEST_F(LutPairPlacementTest, QualifiedListingAndOutOfRangeRestorePinsCachesLabsAndStorageHoles)
{
    PairSnapshot saved(ctx.get()); PairLog log;
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), -1)); saved.expect(ctx.get());
    ASSERT_NE(log.stream.str().find("LUT pair placement candidate 0"), std::string::npos) << log.stream.str();
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), 9999)); saved.expect(ctx.get());
}

TEST_F(LutPairPlacementTest, TrialExceptionRestoresBothBindingsAndEveryOriginalCache)
{
    PairSnapshot saved(ctx.get()); PairTrialObserver probe(ctx.get(), sink, true);
    EXPECT_THROW(ctx->remap_lut_pair_critical(report(), -1), std::exception);
    ASSERT_GT(probe.trials, 0); saved.expect(ctx.get());
}

TEST_F(LutPairPlacementTest, TimedInnerSideUserRegressionRejectsEveryOtherwiseTargetedMove)
{
    auto *capture = side("inner_side", inner->getPort(id_Q), 24, 20);
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    EXPECT_TRUE(std::isfinite(before.get_setup_slack(CellPortKey(capture->name, id_DATAIN))));
    PairSnapshot saved(ctx.get()); PairTrialObserver probe(ctx.get(), sink);
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), -1));
    EXPECT_GT(probe.trials, 0); EXPECT_GT(probe.setup_rejections, 0);
    EXPECT_NE(probe.text.find("LUT pair placement: 0 qualified candidates; no candidate applied."), std::string::npos)
        << probe.text;
    saved.expect(ctx.get());
}

TEST_F(LutPairPlacementTest, NativeFallingEdgeHoldRejectsARealShorterSetupPath)
{
    ctx->unbindBel(sink->bel);
    clock->clkconstr->period = DelayPair(8000); clock->clkconstr->high = clock->clkconstr->low = DelayPair(4000);
    ctx->settings[ctx->id("target_freq")] = 125e6;
    sink->pin_data[id_CLK].state = PIN_INV; ctx->assign_ff_info(sink); ctx->assign_default_pinmap(sink);
    place(sink, 30, 20, STRENGTH_LOCKED, 8);
    PairSnapshot saved(ctx.get()); PairTrialObserver probe(ctx.get(), sink);
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), -1));
    EXPECT_GT(probe.trials, 0); EXPECT_GT(probe.hold_rejections, 0); EXPECT_GT(probe.negative_trial_holds, 0);
    EXPECT_GT(probe.shorter_setup_hold_rejections, 0);
    EXPECT_NE(probe.text.find("LUT pair placement: 0 qualified candidates; no candidate applied."), std::string::npos)
        << probe.text;
    saved.expect(ctx.get());
}

TEST_F(LutPairPlacementTest, UnrelatedSideCaptureKeepsFiniteBoundsWithoutInventingAWindow)
{
    auto *capture = side("unrelated_side", inner->getPort(id_Q), 24, 20);
    auto *other = ctx->createNet(ctx->id("independent_clock")); other->is_global = true;
    other->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
    ASSERT_NE(clock_source(clock, "primary_clock_source"), nullptr);
    ASSERT_NE(clock_source(other, "independent_clock_source"), nullptr);
    ctx->unbindBel(capture->bel); capture->disconnectPort(id_CLK); capture->connectPort(id_CLK, other);
    ctx->assign_ff_info(capture); ctx->assign_default_pinmap(capture); place(capture, 24, 20, STRENGTH_LOCKED);
    TimingAnalyser before(ctx.get()); before.with_clock_skew = false; before.setup(false, false, true);
    std::vector<EndpointClockPairTiming> rows;
    ASSERT_TRUE(before.get_endpoint_clock_pair_timings(CellPortKey(capture->name, id_DATAIN), rows)); ASSERT_FALSE(rows.empty());
    for (const auto &row : rows) {
        EXPECT_FALSE(row.setup_timed); EXPECT_FALSE(row.setup_window); EXPECT_FALSE(row.setup_margin);
        EXPECT_GE(row.max_path_delay, row.min_path_delay); EXPECT_GT(row.min_path_delay, 0);
    }
    PairSnapshot saved(ctx.get()); PairTrialObserver probe(ctx.get(), sink);
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), -1));
    EXPECT_NE(probe.text.find("reference_free=1"), std::string::npos) << probe.text;
    EXPECT_GT(probe.trials, 0); EXPECT_GT(probe.setup_rejections, 0);
    EXPECT_NE(probe.text.find("LUT pair placement: 0 qualified candidates; no candidate applied."), std::string::npos)
        << probe.text;
    saved.expect(ctx.get());
}

TEST_F(LutPairPlacementTest, OccupiedMateAlmCannotBecomeEitherDestination)
{
    auto *blocker = ctx->createCell(ctx->id("existing_mate"), id_MISTRAL_ALUT2);
    blocker->params[id_LUT] = Property(0x8, 4); blocker->addInput(id_A); blocker->addInput(id_B); blocker->addOutput(id_Q);
    blocker->connectPort(id_A, inputs[0]->getPort(id_Q)); blocker->connectPort(id_B, inputs[1]->getPort(id_Q));
    blocker->connectPort(id_Q, ctx->createNet(ctx->id("mate$q"))); ctx->assign_comb_info(blocker); ctx->assign_default_pinmap(blocker);
    place(blocker, 29, 20, STRENGTH_WEAK, 1);
    PairSnapshot saved(ctx.get()); PairLog log;
    ASSERT_TRUE(ctx->remap_lut_pair_critical(report(), 0)) << log.stream.str();
    saved.expect(ctx.get(), {inner->name, outer->name});
    for (auto *cell : {inner, outer}) {
        auto site = ctx->getBelLocation(cell->bel);
        EXPECT_FALSE(site.x == 29 && site.y == 20 && site.z / 6 == 0);
    }
}

TEST_F(LutPairPlacementTest, ProtectedOriginalPairOrSourceLabIsNotMoved)
{
    inner->belStrength = STRENGTH_LOCKED;
    { PairSnapshot saved(ctx.get()); reject(report(), saved); }
    inner->belStrength = STRENGTH_WEAK;
    inner->cluster = inner->name;
    { PairSnapshot saved(ctx.get()); reject(report(), saved); }
    inner->cluster = ClusterId();
    auto *fixed = side("fixed_source_lab", inputs[0]->getPort(id_Q), 24, 21);
    ASSERT_EQ(fixed->belStrength, STRENGTH_LOCKED);
    { PairSnapshot saved(ctx.get()); reject(report(), saved); }
}

TEST_F(LutPairPlacementTest, ProtectedDestinationLabsCannotSupplyAnyCandidate)
{
    for (int x = 27; x <= 33; ++x) for (int y = 17; y <= 23; ++y) {
        if (x == 30 && (y == 19 || y == 20)) continue;
        bool has_ff_bel = false;
        for (auto bel : ctx->getBelsByTile(x, y)) has_ff_bel |= ctx->isValidBelForCellType(id_MISTRAL_FF, bel);
        if (!has_ff_bel) continue;
        auto *guard = ff("destination_guard_" + std::to_string(x) + "_" + std::to_string(y));
        ctx->assign_ff_info(guard); ctx->assign_default_pinmap(guard); place(guard, x, y, STRENGTH_LOCKED);
    }
    PairSnapshot saved(ctx.get()); reject(report(), saved);
}

TEST_F(LutPairPlacementTest, StaleDisconnectedAndMalformedReportsThrowBeforeAnyProbe)
{
    PairSnapshot saved(ctx.get());
    for (const auto &text : {std::string("{"), std::string("[]"), std::string("{\"critical_paths\":3}")}) {
        EXPECT_THROW(ctx->remap_lut_pair_critical(text, -1), log_execution_error_exception); saved.expect(ctx.get());
    }
    auto stale = report(); auto where = stale.find("inner$q"); ASSERT_NE(where, std::string::npos);
    stale.replace(where, 7, "wrong$q"); EXPECT_THROW(ctx->remap_lut_pair_critical(stale, -1), log_execution_error_exception); saved.expect(ctx.get());
    EXPECT_THROW(ctx->remap_lut_pair_critical(report(), -2), log_execution_error_exception); saved.expect(ctx.get());
    outer->disconnectPort(id_B); PairSnapshot disconnected(ctx.get());
    EXPECT_THROW(ctx->remap_lut_pair_critical(report(), -1), log_execution_error_exception); disconnected.expect(ctx.get());
}

TEST_F(LutPairPlacementTest, DirectListingRejectsFollowingDriverBeforeDiscovery)
{
    ctx->lut_pair_report.clear();
    ctx->lut_driver_copy_report = "{\"critical_paths\": []}";
    ctx->lut_driver_copy_selection = 0;
    ASSERT_TRUE(ctx->lut_pair_report.empty());
    PairSnapshot saved(ctx.get()); PairLog log;
    EXPECT_THROW(ctx->remap_lut_pair_critical(report(), -1), log_execution_error_exception);
    EXPECT_NE(log.stream.str().find("A LUT pair placement listing must be final; it cannot precede LUT driver copy."),
              std::string::npos) << log.stream.str();
    EXPECT_EQ(log.stream.str().find("LUT pair placement discovery"), std::string::npos);
    EXPECT_EQ(log.stream.str().find("LUT pair placement trial"), std::string::npos);
    saved.expect(ctx.get());
}
