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
        int32_t udata;
        PortRef driver;
        indexed_store<PortRef> users;
        decltype(NetInfo::attrs) attrs;
        decltype(NetInfo::wires) wires;
    };
    std::map<IdString, Cell> cells;
    std::map<IdString, Net> nets;
    std::map<IdString, IdString> aliases;
    std::map<IdString, NetInfo *> alias_owners;
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
            nets.emplace(entry.first, Net{net, &entry.second, net->udata, net->driver, net->users, net->attrs, net->wires});
            net_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->net_aliases) {
            aliases.emplace(entry.first, entry.second); alias_order.push_back(entry.first);
            alias_owners.emplace(entry.first, ctx->getNetByAlias(entry.first));
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

    void expect(Context *ctx, const std::set<IdString> &moved = {}, bool owner_addresses = true) const
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
            EXPECT_EQ(cell, saved.identity);
            if (owner_addresses) EXPECT_EQ(&ctx->cells.at(row.first), saved.owner);
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
            EXPECT_EQ(net, saved.identity);
            if (owner_addresses) EXPECT_EQ(&ctx->nets.at(row.first), saved.owner);
            EXPECT_EQ(net->udata, saved.udata);
            EXPECT_EQ(net->driver.cell, saved.driver.cell); EXPECT_EQ(net->driver.port, saved.driver.port);
            EXPECT_EQ(net->attrs, saved.attrs); users(net->users, saved.users);
            ASSERT_EQ(net->wires.size(), saved.wires.size());
            for (const auto &wire : saved.wires) {
                ASSERT_TRUE(net->wires.count(wire.first));
                EXPECT_EQ(net->wires.at(wire.first).pip, wire.second.pip);
                EXPECT_EQ(net->wires.at(wire.first).strength, wire.second.strength);
            }
        }
        for (const auto &row : aliases) {
            EXPECT_EQ(ctx->net_aliases.at(row.first), row.second);
            EXPECT_EQ(ctx->getNetByAlias(row.first), alias_owners.at(row.first));
        }
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
    int trials = 0, setup_rejections = 0, hold_rejections = 0, shorter_setup_hold_rejections = 0,
        negative_trial_holds = 0, shorter_setup_negative_holds = 0;
    std::vector<delay_t> terminal_hold_margins;
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
                for (const auto &row : rows) if (row.hold_margin) {
                    terminal_hold_margins.push_back(*row.hold_margin);
                    negative_trial_holds += *row.hold_margin < 0;
                    shorter_setup_negative_holds += *row.hold_margin < 0 && line.find("improve=1") != std::string::npos &&
                        line.find("hold=0") != std::string::npos;
                }
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

    CellInfo *clock_source(NetInfo *net, const std::string &name, bool require_left = false)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_CLKBUF);
        cell->addOutput(id_Q); cell->connectPort(id_Q, net); ctx->assign_default_pinmap(cell);
        for (auto bel : ctx->getBels()) {
            auto loc = ctx->getBelLocation(bel);
            if (require_left && (loc.x != 0 || loc.y != 35)) continue;
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
    // Every FF remains fixed by the pass. A weak capture avoids protecting
    // the source LAB while retaining its original dedicated DATAIN link.
    capture->belStrength = STRENGTH_WEAK;
    ASSERT_EQ(capture->belStrength, STRENGTH_WEAK);
    ASSERT_EQ(ctx->getBelLocation(capture->bel).x, 24); ASSERT_EQ(ctx->getBelLocation(capture->bel).y, 20);
    ASSERT_EQ(ctx->getBelLocation(capture->bel).z / 6, ctx->getBelLocation(inner->bel).z / 6);
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    EXPECT_TRUE(std::isfinite(before.get_setup_slack(CellPortKey(capture->name, id_DATAIN))));
    std::vector<EndpointClockPairTiming> rows;
    ASSERT_TRUE(before.get_endpoint_clock_pair_timings(CellPortKey(capture->name, id_DATAIN), rows)); ASSERT_FALSE(rows.empty());
    for (const auto &row : rows) { ASSERT_TRUE(row.setup_timed); ASSERT_TRUE(row.setup_margin); ASSERT_TRUE(row.hold_margin); }
    PairSnapshot saved(ctx.get()); PairTrialObserver probe(ctx.get(), sink);
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), -1));
    EXPECT_GT(probe.trials, 0); EXPECT_GT(probe.setup_rejections, 0);
    EXPECT_NE(probe.text.find("LUT pair placement: 0 qualified candidates; no candidate applied."), std::string::npos)
        << probe.text;
    saved.expect(ctx.get());
}

TEST_F(LutPairPlacementTest, UnsupportedOppositeEdgeHoldCoverageRejectsBeforeAnyMove)
{
    ctx->unbindBel(sink->bel);
    clock->clkconstr->period = DelayPair(8000); clock->clkconstr->high = clock->clkconstr->low = DelayPair(4000);
    ctx->settings[ctx->id("target_freq")] = 125e6;
    sink->pin_data[id_CLK].state = PIN_INV; ctx->assign_ff_info(sink); ctx->assign_default_pinmap(sink);
    place(sink, 30, 20, STRENGTH_LOCKED, 8);
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    std::vector<EndpointClockPairTiming> rows;
    ASSERT_TRUE(before.get_endpoint_clock_pair_timings(CellPortKey(sink->name, id_ENA), rows)); ASSERT_FALSE(rows.empty());
    for (const auto &row : rows) { ASSERT_TRUE(row.setup_timed); ASSERT_FALSE(row.hold_related); ASSERT_FALSE(row.hold_margin); }
    PairSnapshot saved(ctx.get()); PairLog log;
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), -1));
    EXPECT_NE(log.stream.str().find("reason=endpoint-related-hold-coverage-unavailable"), std::string::npos) << log.stream.str();
    EXPECT_EQ(log.stream.str().find("LUT pair placement trial"), std::string::npos);
    EXPECT_NE(log.stream.str().find("LUT pair placement: 0 qualified candidates; no candidate applied."), std::string::npos);
    saved.expect(ctx.get());
}

TEST_F(LutPairPlacementTest, NativeRelatedClockHoldRejectsARealShorterSetupPath)
{
    clock->clkconstr->period = DelayPair(8000); clock->clkconstr->high = clock->clkconstr->low = DelayPair(4000);
    clock->clkconstr->phase_group = ctx->id("hold_shared_phase");
    ctx->settings[ctx->id("target_freq")] = 125e6;
    ASSERT_NE(clock_source(clock, "hold_primary_clock", true), nullptr);
    auto *capture_clock = clock;
    // Six real identity LUT arcs on physical B create a late common-root
    // capture clock. The default F input is too fast to cross the hold
    // boundary; choose the physical mux level explicitly, without injection.
    for (int index = 0; index < 6; ++index) {
        auto name = "hold_clock_stage_" + std::to_string(index);
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_ALUT2);
        cell->params[id_LUT] = Property(0xa, 4); cell->addInput(id_A); cell->addInput(id_B); cell->addOutput(id_Q);
        cell->connectPort(id_A, capture_clock); cell->pin_data[id_B].state = PIN_0;
        capture_clock = ctx->createNet(ctx->id(name + "$q")); capture_clock->is_global = true;
        cell->connectPort(id_Q, capture_clock); ctx->assign_comb_info(cell); ctx->assign_default_pinmap(cell);
        place(cell, 2, 35, STRENGTH_LOCKED, 6 * index);
        cell->pin_data[id_A].bel_pins = {id_B};
    }
    capture_clock->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
    capture_clock->clkconstr->phase_shift = 0;
    ctx->unbindBel(sink->bel); sink->disconnectPort(id_CLK); sink->connectPort(id_CLK, capture_clock);
    ctx->assign_ff_info(sink); ctx->assign_default_pinmap(sink); place(sink, 30, 20, STRENGTH_LOCKED, 8);
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    std::vector<EndpointClockPairTiming> rows;
    ASSERT_TRUE(before.get_endpoint_clock_pair_timings(CellPortKey(sink->name, id_ENA), rows)); ASSERT_FALSE(rows.empty());
    for (const auto &row : rows) {
        ASSERT_EQ(row.launch.clock, clock->name); ASSERT_EQ(row.capture.clock, capture_clock->name);
        ASSERT_EQ(row.launch.edge, RISING_EDGE); ASSERT_EQ(row.capture.edge, RISING_EDGE);
        ASSERT_TRUE(row.setup_timed); ASSERT_TRUE(row.hold_related); ASSERT_TRUE(row.hold_margin);
        ASSERT_TRUE(row.setup_window); ASSERT_EQ(*row.setup_window, 8000);
        ASSERT_GE(*row.hold_margin, 0);
    }
    PairSnapshot saved(ctx.get()); PairTrialObserver probe(ctx.get(), sink);
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), -1));
    EXPECT_GT(probe.trials, 0); EXPECT_GT(probe.hold_rejections, 0); EXPECT_GT(probe.negative_trial_holds, 0);
    EXPECT_GT(probe.shorter_setup_hold_rejections, 0);
    EXPECT_GT(probe.shorter_setup_negative_holds, 0) << ::testing::PrintToString(probe.terminal_hold_margins) << probe.text;
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
    ctx->assign_ff_info(capture); ctx->assign_default_pinmap(capture); place(capture, 24, 20, STRENGTH_WEAK);
    ASSERT_EQ(capture->belStrength, STRENGTH_WEAK);
    ASSERT_EQ(ctx->getBelLocation(capture->bel).x, 24); ASSERT_EQ(ctx->getBelLocation(capture->bel).y, 20);
    ASSERT_EQ(ctx->getBelLocation(capture->bel).z / 6, ctx->getBelLocation(inner->bel).z / 6);
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

namespace {
struct CopyTrialObserver : std::streambuf {
    Context *ctx;
    CellInfo *endpoint;
    std::ostream stream;
    std::string text, line;
    int trials = 0, improving_negative_holds = 0;
    bool throw_on_trial;
    CopyTrialObserver(Context *ctx, CellInfo *endpoint, bool fail = false)
        : ctx(ctx), endpoint(endpoint), stream(this), throw_on_trial(fail)
    {
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        log_streams.emplace_back(&stream, LogLevel::INFO_MSG);
    }
    ~CopyTrialObserver() { log_streams.pop_back(); }
    void append(char value)
    {
        text.push_back(value);
        if (value != '\n') { line.push_back(value); return; }
        if (line.find("LUT pair copy trial ") != std::string::npos) {
            ++trials;
            TimingAnalyser timing(ctx); timing.with_clock_skew = true; timing.setup(false, false, true);
            std::vector<EndpointClockPairTiming> rows;
            if (timing.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_ENA), rows))
                for (const auto &row : rows) if (row.hold_margin && *row.hold_margin < 0 &&
                        line.find("improve=1") != std::string::npos) {
                    ++improving_negative_holds;
                    EXPECT_TRUE(line.find("endpoints=0") != std::string::npos || line.find("hold=0") != std::string::npos)
                        << "An actual negative related hold must reject this improving trial: " << line;
                }
            if (throw_on_trial) throw std::runtime_error("actual composed-copy trial exception");
        }
        line.clear();
    }
    std::streamsize xsputn(const char *data, std::streamsize count) override
    { for (std::streamsize i = 0; i < count; ++i) append(data[i]); return count; }
    int_type overflow(int_type value) override
    { if (!traits_type::eq_int_type(value, traits_type::eof())) append(traits_type::to_char_type(value)); return traits_type::not_eof(value); }
};
} // namespace

class LutPairCopyTest : public LutPairPlacementTest {
  protected:
    const std::array<IdString, 6> pins = {id_A, id_B, id_C, id_D, id_E, id_F};
    std::vector<NetInfo *> literal_nets;

    void SetUp() override
    {
        LutPairPlacementTest::SetUp();
        ctx->lut_pair_compose_copy = true;
        for (auto *cell : inputs) literal_nets.push_back(cell->getPort(id_Q));
        // The FF itself is eligible; its real carry neighbour protects the
        // entire terminal LAB against source/destination LUT placement.
        sink->belStrength = STRENGTH_WEAK;
    }

    bool value(NetInfo *net, unsigned assignment) const
    {
        for (unsigned i = 0; i < literal_nets.size(); ++i)
            if (net == literal_nets[i]) return (assignment >> i) & 1;
        if (!net || !net->driver.cell || !net->driver.cell->params.count(id_LUT)) {
            ADD_FAILURE() << "Unexpected composed-copy boundary"; return false;
        }
        auto *cell = net->driver.cell;
        unsigned lut_row = 0;
        for (unsigned i = 0; i < pins.size(); ++i) {
            if (!cell->ports.count(pins[i])) continue;
            auto state = cell->get_pin_state(pins[i]);
            bool bit = state == PIN_1;
            if (state == PIN_SIG || state == PIN_INV) {
                bit = value(cell->getPort(pins[i]), assignment);
                if (state == PIN_INV) bit = !bit;
            } else EXPECT_TRUE(state == PIN_0 || state == PIN_1);
            lut_row |= unsigned(bit) << i;
        }
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> lut_row) & 1;
    }

    CellInfo *new_clone(const PairSnapshot &saved) const
    {
        auto *net = sink->getPort(id_ENA);
        if (!net || !net->driver.cell || saved.cells.count(net->driver.cell->name)) return nullptr;
        return net->driver.cell;
    }

    void restored(const PairSnapshot &saved) const
    {
        // Dictionary insertion may reallocate owner entries; original owned
        // CellInfo/NetInfo identities and their order must still be exact.
        saved.expect(ctx.get(), {}, false);
    }

    void rejected(const PairSnapshot &saved, int selection = 0)
    {
        try { EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), selection)); }
        catch (const log_execution_error_exception &) { }
        restored(saved);
    }

    void assign_router_net_indices() const
    {
        // Match gpurouter.cc::setup_net_indices without constructing a GPU
        // router: every live net receives its actual full iteration ordinal.
        int32_t index = 0;
        for (const auto &entry : ctx->nets) entry.second->udata = index++;
    }

    void originals(const PairSnapshot &saved, CellInfo *clone, const std::set<IdString> &cohort,
                   bool require_clone_outside_sink_lab = true) const
    {
        ASSERT_NE(clone, nullptr); auto *output = clone->getPort(id_Q);
        ASSERT_NE(output, nullptr); ASSERT_EQ(ctx->cells.size(), saved.cells.size() + 1);
        ASSERT_EQ(ctx->nets.size(), saved.nets.size() + 1);
        ASSERT_EQ(ctx->net_aliases.size(), saved.aliases.size() + 1);
        ASSERT_TRUE(ctx->net_aliases.count(output->name));
        EXPECT_EQ(ctx->net_aliases.at(output->name), output->name);
        std::vector<IdString> cells, nets, aliases;
        for (const auto &row : ctx->cells) cells.push_back(row.first);
        for (const auto &row : ctx->nets) nets.push_back(row.first);
        for (const auto &row : ctx->net_aliases) aliases.push_back(row.first);
        auto expected_cells = saved.cell_order, expected_nets = saved.net_order, expected_aliases = saved.alias_order;
        expected_cells.push_back(clone->name); expected_nets.push_back(output->name); expected_aliases.push_back(output->name);
        // Keep the new owners in the comparison: filtering them out would
        // hide a head insertion that shifts every original GPU net ID.
        EXPECT_EQ(cells, expected_cells); EXPECT_EQ(nets, expected_nets); EXPECT_EQ(aliases, expected_aliases);
        EXPECT_EQ(ctx->ports.size(), saved.ports.size());
        for (const auto &row : saved.ports) PairSnapshot::port(ctx->ports.at(row.first), row.second);
        for (const auto &row : saved.aliases) {
            EXPECT_EQ(ctx->net_aliases.at(row.first), row.second);
            EXPECT_EQ(ctx->getNetByAlias(row.first), saved.alias_owners.at(row.first));
        }
        EXPECT_EQ(ctx->getNetByAlias(output->name), output);
        for (const auto &row : saved.cells) {
            SCOPED_TRACE(row.first.str(ctx.get())); auto *cell = ctx->cells.at(row.first).get(); const auto &old = row.second;
            EXPECT_EQ(cell, old.identity); EXPECT_EQ(cell->type, old.type); EXPECT_EQ(cell->bel, old.bel);
            EXPECT_EQ(cell->belStrength, old.strength); EXPECT_EQ(cell->params, old.params); EXPECT_EQ(cell->attrs, old.attrs);
            EXPECT_EQ(cell->cluster, old.cluster); EXPECT_EQ(cell->region, old.region);
            EXPECT_EQ(cell->constr_children, old.cache.constr_children);
            EXPECT_EQ(cell->constr_x, old.cache.constr_x); EXPECT_EQ(cell->constr_y, old.cache.constr_y);
            EXPECT_EQ(cell->constr_z, old.cache.constr_z); EXPECT_EQ(cell->constr_abs_z, old.cache.constr_abs_z);
            std::vector<IdString> port_order;
            for (const auto &pin : cell->ports) port_order.push_back(pin.first);
            EXPECT_EQ(port_order, old.port_order); ASSERT_EQ(cell->pin_data.size(), old.pins.size());
            for (const auto &pin : old.pins) {
                ASSERT_TRUE(cell->pin_data.count(pin.first));
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
            for (const auto &pin : old.ports) {
                const auto &actual = cell->ports.at(pin.first);
                if (cohort.count(row.first) && pin.first == id_ENA) {
                    EXPECT_EQ(actual.name, pin.second.name); EXPECT_EQ(actual.type, pin.second.type); EXPECT_EQ(actual.net, output);
                    ASSERT_TRUE(output->users.count(actual.user_idx));
                    EXPECT_EQ(output->users.at(actual.user_idx).cell, cell); EXPECT_EQ(output->users.at(actual.user_idx).port, id_ENA);
                } else PairSnapshot::port(actual, pin.second);
            }
            if (cell->type == id_MISTRAL_FF) {
                const auto &a = cell->ffInfo.ctrlset, &b = old.cache.ffInfo.ctrlset;
                EXPECT_EQ(a.clk, b.clk); EXPECT_EQ(a.aclr, b.aclr); EXPECT_EQ(a.sclr, b.sclr); EXPECT_EQ(a.sload, b.sload);
                EXPECT_EQ(a.ena.inverted, b.ena.inverted);
                EXPECT_EQ(a.ena.net, cohort.count(row.first) ? output : b.ena.net);
                EXPECT_EQ(cell->ffInfo.datain, old.cache.ffInfo.datain); EXPECT_EQ(cell->ffInfo.sdata, old.cache.ffInfo.sdata);
            } else if (ctx->is_comb_cell(cell->type)) {
                const auto &a = cell->combInfo, &b = old.cache.combInfo;
                EXPECT_EQ(a.comb_out, b.comb_out); EXPECT_EQ(a.lut_input_count, b.lut_input_count);
                EXPECT_EQ(a.used_lut_input_count, b.used_lut_input_count); EXPECT_EQ(a.lut_bits_count, b.lut_bits_count);
                EXPECT_EQ(a.chain_shared_input_count, b.chain_shared_input_count); EXPECT_EQ(a.mlab_group, b.mlab_group);
                EXPECT_EQ(a.is_carry, b.is_carry); EXPECT_EQ(a.is_shared, b.is_shared); EXPECT_EQ(a.is_extended, b.is_extended);
                EXPECT_EQ(a.carry_start, b.carry_start); EXPECT_EQ(a.carry_end, b.carry_end);
                for (int i = 0; i < b.lut_input_count; ++i) EXPECT_EQ(a.lut_in[i], b.lut_in[i]);
            }
        }
        ASSERT_EQ(output->users.entries(), cohort.size());
        for (const auto &row : saved.nets) {
            auto *net = ctx->nets.at(row.first).get(); const auto &old = row.second;
            EXPECT_EQ(net, old.identity); EXPECT_EQ(net->driver.cell, old.driver.cell); EXPECT_EQ(net->driver.port, old.driver.port);
            EXPECT_EQ(net->udata, old.udata);
            EXPECT_EQ(net->attrs, old.attrs); EXPECT_EQ(net->wires.size(), old.wires.size());
            for (const auto &wire : old.wires) {
                ASSERT_TRUE(net->wires.count(wire.first));
                EXPECT_EQ(net->wires.at(wire.first).pip, wire.second.pip);
                EXPECT_EQ(net->wires.at(wire.first).strength, wire.second.strength);
            }
            auto expected = old.users;
            for (auto name : cohort) {
                const auto &port = saved.cells.at(name).ports.at(id_ENA);
                if (port.net == net) expected.remove(port.user_idx);
            }
            size_t added = 0;
            for (auto user : net->users.enumerate()) {
                if (user.value.cell == clone) {
                    ++added; EXPECT_NE(user.value.port, id_Q);
                    EXPECT_EQ(clone->ports.at(user.value.port).user_idx, user.index);
                    EXPECT_EQ(clone->getPort(user.value.port), net);
                } else {
                    ASSERT_TRUE(expected.count(user.index)); EXPECT_EQ(expected.at(user.index).cell, user.value.cell);
                    EXPECT_EQ(expected.at(user.index).port, user.value.port);
                }
            }
            EXPECT_EQ(net->users.entries(), expected.entries() + added);
        }
        assign_router_net_indices();
        for (size_t index = 0; index < saved.net_order.size(); ++index) {
            auto *net = ctx->nets.at(saved.net_order[index]).get();
            EXPECT_EQ(net, saved.nets.at(saved.net_order[index]).identity);
            EXPECT_EQ(net->udata, int32_t(index));
        }
        EXPECT_EQ(output->udata, int32_t(saved.net_order.size()));
        EXPECT_TRUE(ctx->isBelLocationValid(clone->bel));
        auto site = ctx->getBelLocation(clone->bel), target = ctx->getBelLocation(sink->bel);
        if (require_clone_outside_sink_lab) EXPECT_FALSE(site.x == target.x && site.y == target.y);
        auto alm = ctx->bel_data(clone->bel).lab_data;
        for (auto bel : ctx->getBelsByTile(site.x, site.y))
            if ((ctx->isValidBelForCellType(id_MISTRAL_ALUT2, bel) || ctx->isValidBelForCellType(id_MISTRAL_FF, bel)) &&
                ctx->bel_data(bel).lab_data.alm == alm.alm && ctx->getBoundBelCell(bel)) EXPECT_EQ(ctx->getBoundBelCell(bel), clone);
        ctx->check();
    }
};

TEST_F(LutPairCopyTest, ComposedCopyRetainsSharedOriginalPathsAndRedirectsOnlySelectedLab)
{
    auto *inner_side = side("original_inner_capture", inner->getPort(id_Q), 24, 20); inner_side->belStrength = STRENGTH_WEAK;
    auto *outer_side = side("original_outer_capture", outer->getPort(id_Q), 24, 21); outer_side->belStrength = STRENGTH_WEAK;
    auto *peer = ff("same_lab_enable", outer->getPort(id_Q)); ctx->assign_ff_info(peer); ctx->assign_default_pinmap(peer);
    place(peer, 30, 20, STRENGTH_WEAK, 14);
    auto *remote = ff("different_lab_enable", outer->getPort(id_Q)); ctx->assign_ff_info(remote); ctx->assign_default_pinmap(remote);
    place(remote, 24, 22, STRENGTH_WEAK);
    std::array<std::vector<EndpointClockPairTiming>, 2> old_rows;
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    for (int i = 0; i < 2; ++i) {
        auto *cell = i ? outer_side : inner_side;
        ASSERT_TRUE(before.get_endpoint_clock_pair_timings(CellPortKey(cell->name, id_DATAIN), old_rows[i]));
        ASSERT_FALSE(old_rows[i].empty()); ASSERT_TRUE(old_rows[i][0].setup_margin); ASSERT_LT(*old_rows[i][0].setup_margin, 0);
    }
    float old_slack = before.get_setup_slack(CellPortKey(sink->name, id_ENA)); ASSERT_TRUE(std::isfinite(old_slack));
    PairSnapshot saved(ctx.get()); PairLog log;
    ASSERT_TRUE(ctx->remap_lut_pair_critical(report(), 0)) << log.stream.str();
    auto *clone = new_clone(saved); ASSERT_NE(clone, nullptr); EXPECT_EQ(clone->type, id_MISTRAL_ALUT4);
    originals(saved, clone, {sink->name, peer->name}); EXPECT_EQ(remote->getPort(id_ENA), outer->getPort(id_Q));
    for (unsigned row = 0; row < 16; ++row) EXPECT_EQ(value(outer->getPort(id_Q), row), value(clone->getPort(id_Q), row)) << row;
    TimingAnalyser after(ctx.get()); after.with_clock_skew = true; after.setup(false, false, true);
    EXPECT_GE(after.get_setup_slack(CellPortKey(sink->name, id_ENA)), old_slack + 250);
    for (int i = 0; i < 2; ++i) {
        std::vector<EndpointClockPairTiming> now; auto *cell = i ? outer_side : inner_side;
        ASSERT_TRUE(after.get_endpoint_clock_pair_timings(CellPortKey(cell->name, id_DATAIN), now)); ASSERT_EQ(now.size(), old_rows[i].size());
        for (size_t j = 0; j < now.size(); ++j) {
            EXPECT_EQ(now[j].launch, old_rows[i][j].launch); EXPECT_EQ(now[j].capture, old_rows[i][j].capture);
            EXPECT_EQ(now[j].max_path_delay, old_rows[i][j].max_path_delay); EXPECT_EQ(now[j].min_path_delay, old_rows[i][j].min_path_delay);
            EXPECT_EQ(now[j].setup_margin, old_rows[i][j].setup_margin); EXPECT_EQ(now[j].hold_margin, old_rows[i][j].hold_margin);
        }
    }
    EXPECT_NE(log.stream.str().find("LUT pair copy applied candidate 0"), std::string::npos);
    EXPECT_EQ(log.stream.str().find("LUT pair placement trial "), std::string::npos);
}

TEST_F(LutPairCopyTest, ConstantInvertedAndSharedLeavesHaveExhaustiveEffectivePinTruth)
{
    auto old_bel = outer->bel; ctx->unbindBel(old_bel);
    outer->disconnectPort(id_A); outer->connectPort(id_A, inputs[1]->getPort(id_Q));
    outer->disconnectPort(id_C); outer->pin_data[id_C].state = PIN_1; outer->pin_data[id_B].state = PIN_INV;
    ctx->assign_comb_info(outer); ctx->assign_default_pinmap(outer); ctx->bindBel(old_bel, outer, STRENGTH_WEAK);
    ASSERT_TRUE(ctx->isBelLocationValid(old_bel));
    std::array<bool, 16> truth;
    for (unsigned row = 0; row < 16; ++row) truth[row] = value(outer->getPort(id_Q), row);
    PairSnapshot saved(ctx.get()); PairLog log;
    ASSERT_TRUE(ctx->remap_lut_pair_critical(report(), 0)) << log.stream.str();
    auto *clone = new_clone(saved); ASSERT_NE(clone, nullptr); EXPECT_EQ(clone->type, id_MISTRAL_ALUT2);
    originals(saved, clone, {sink->name});
    for (unsigned row = 0; row < 16; ++row) {
        EXPECT_EQ(value(outer->getPort(id_Q), row), truth[row]); EXPECT_EQ(value(clone->getPort(id_Q), row), truth[row]);
    }
}

TEST_F(LutPairCopyTest, SixDistinctLeavesRetainAllSixtyFourAssignments)
{
    auto bel = outer->bel; ctx->unbindBel(bel); outer->type = id_MISTRAL_ALUT5; outer->params[id_LUT] = Property(0x7fff0000, 32);
    for (int i = 0; i < 2; ++i) {
        auto *launch = ff("sixth_cut_literal_" + std::to_string(i)); ctx->assign_ff_info(launch); ctx->assign_default_pinmap(launch);
        place(launch, 30, 19, STRENGTH_LOCKED); literal_nets.push_back(launch->getPort(id_Q));
        outer->addInput(pins[i + 3]); outer->connectPort(pins[i + 3], launch->getPort(id_Q));
    }
    ctx->assign_comb_info(outer); ctx->assign_default_pinmap(outer); ctx->bindBel(bel, outer, STRENGTH_WEAK);
    ASSERT_TRUE(ctx->isBelLocationValid(bel));
    std::array<bool, 64> truth;
    for (unsigned row = 0; row < 64; ++row) truth[row] = value(outer->getPort(id_Q), row);
    PairSnapshot saved(ctx.get()); PairLog log;
    ASSERT_TRUE(ctx->remap_lut_pair_critical(report(), 0)) << log.stream.str();
    auto *clone = new_clone(saved); ASSERT_NE(clone, nullptr); EXPECT_EQ(clone->type, id_MISTRAL_ALUT6);
    originals(saved, clone, {sink->name});
    for (unsigned row = 0; row < 64; ++row) {
        EXPECT_EQ(value(outer->getPort(id_Q), row), truth[row]); EXPECT_EQ(value(clone->getPort(id_Q), row), truth[row]);
    }
}

TEST_F(LutPairCopyTest, QualifiedListingUnavailableSelectionAndTrialExceptionRestoreExactStores)
{
    PairSnapshot saved(ctx.get());
    EXPECT_THROW(ctx->remap_lut_pair_critical("{", -1), log_execution_error_exception); restored(saved);
    auto stale = report(); auto offset = stale.find("inner$q"); ASSERT_NE(offset, std::string::npos);
    stale.replace(offset, 7, "wrong$q");
    EXPECT_THROW(ctx->remap_lut_pair_critical(stale, -1), log_execution_error_exception); restored(saved);
    { PairLog log; EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), -1));
      ASSERT_NE(log.stream.str().find("LUT pair copy candidate 0:"), std::string::npos) << log.stream.str(); }
    restored(saved);
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), 9999)); restored(saved);
    { CopyTrialObserver probe(ctx.get(), sink, true);
      EXPECT_THROW(ctx->remap_lut_pair_critical(report(), -1), std::exception); EXPECT_GT(probe.trials, 0); }
    restored(saved);
}

TEST_F(LutPairCopyTest, PrivateNamePairsRespectBothAliasNamespaces)
{
    const auto stem = outer->name.str(ctx.get()) + "$lut_pair_copy";
    ctx->net_aliases[ctx->id(stem)] = outer->getPort(id_Q)->name;
    ctx->net_aliases[ctx->id(stem + "$1$Q")] = outer->getPort(id_Q)->name;
    PairSnapshot saved(ctx.get()); PairLog log;
    ASSERT_TRUE(ctx->remap_lut_pair_critical(report(), 0)) << log.stream.str();
    auto *clone = new_clone(saved); ASSERT_NE(clone, nullptr);
    EXPECT_EQ(clone->name.str(ctx.get()), stem + "$2");
    EXPECT_EQ(clone->getPort(id_Q)->name.str(ctx.get()), stem + "$2$Q");
    originals(saved, clone, {sink->name});
}

TEST_F(LutPairCopyTest, LaterFailedProbeRetainsAnEarlierAcceptedCopyAndItsUsers)
{
    PairLog log; ASSERT_TRUE(ctx->remap_lut_pair_critical(report(), 0)) << log.stream.str();
    auto *first = sink->getPort(id_ENA)->driver.cell; auto first_bel = first->bel; auto first_q = first->getPort(id_Q);
    auto *next = ff("later_target", outer->getPort(id_Q)); ctx->assign_ff_info(next); ctx->assign_default_pinmap(next);
    place(next, 31, 20, STRENGTH_WEAK);
    sink = next; PairSnapshot retained(ctx.get());
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), -1)); restored(retained);
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), 9999)); restored(retained);
    EXPECT_EQ(ctx->cells.at(first->name).get(), first); EXPECT_EQ(first->bel, first_bel); EXPECT_EQ(first->getPort(id_Q), first_q);
}

TEST_F(LutPairCopyTest, RepeatedAcceptedCopiesAppendAfterEveryPriorOwnerAndGpuNetId)
{
    // Reuse the real later-target geometry, but create it before either
    // acceptance. No new fixture owners are inserted between the two copies.
    auto *next = ff("later_target", outer->getPort(id_Q));
    ctx->assign_ff_info(next); ctx->assign_default_pinmap(next);
    place(next, 31, 20, STRENGTH_WEAK);
    assign_router_net_indices();
    PairSnapshot original(ctx.get()); PairLog log;
    ASSERT_TRUE(ctx->remap_lut_pair_critical(report(), 0)) << log.stream.str();
    auto *first = new_clone(original); ASSERT_NE(first, nullptr);
    originals(original, first, {sink->name});
    auto *first_output = first->getPort(id_Q);
    ASSERT_NE(first_output, nullptr);
    EXPECT_EQ(first_output->udata, int32_t(original.net_order.size()));

    // The first clone/output/identity alias are now originals themselves.
    // They must remain in the exact next prefix, at the same live net IDs.
    sink = next;
    PairSnapshot retained(ctx.get());
    ASSERT_EQ(retained.cell_order.back(), first->name);
    ASSERT_EQ(retained.net_order.back(), first_output->name);
    ASSERT_EQ(retained.alias_order.back(), first_output->name);
    ASSERT_TRUE(ctx->remap_lut_pair_critical(report(), 0)) << log.stream.str();
    auto *second = new_clone(retained); ASSERT_NE(second, nullptr); ASSERT_NE(second, first);
    // This target's LAB is unprotected, so a separate empty ALM in the same
    // LAB is legal. Keep all owner, pin, ALM isolation and net-ID checks.
    originals(retained, second, {next->name}, false);
    EXPECT_EQ(ctx->cells.at(first->name).get(), first);
    EXPECT_EQ(ctx->nets.at(first_output->name).get(), first_output);
    EXPECT_EQ(ctx->getNetByAlias(first_output->name), first_output);
    EXPECT_EQ(first->getPort(id_Q), first_output);
    EXPECT_EQ(first_output->udata, int32_t(original.net_order.size()));
    EXPECT_EQ(second->getPort(id_Q)->udata, int32_t(retained.net_order.size()));

    // The old outer-to-ENA report is now stale. Both selection modes must
    // reject it while retaining both clones and the assigned routing indices.
    PairSnapshot twice(ctx.get());
    EXPECT_THROW(ctx->remap_lut_pair_critical(report(), -1), log_execution_error_exception); restored(twice);
    EXPECT_THROW(ctx->remap_lut_pair_critical(report(), 9999), log_execution_error_exception); restored(twice);
}

TEST_F(LutPairCopyTest, ProtectedSourceOrTargetAndMixedClockCohortCannotBeCopied)
{
    inner->belStrength = STRENGTH_LOCKED; { PairSnapshot saved(ctx.get()); rejected(saved); }
    inner->belStrength = STRENGTH_WEAK;
    for (auto *cell : {inner, outer, sink}) {
        for (auto key : {ctx->id("keep"), ctx->id("dont_touch")}) {
            cell->attrs[key] = Property(1); { PairSnapshot saved(ctx.get()); rejected(saved); } cell->attrs.erase(key);
        }
        cell->cluster = cell->name; { PairSnapshot saved(ctx.get()); rejected(saved); } cell->cluster = ClusterId();
        Region region; cell->region = &region; { PairSnapshot saved(ctx.get()); rejected(saved); } cell->region = nullptr;
    }
    sink->belStrength = STRENGTH_LOCKED; { PairSnapshot saved(ctx.get()); rejected(saved); } sink->belStrength = STRENGTH_WEAK;
    auto *other_clock = ctx->createNet(ctx->id("cohort_other_clock")); other_clock->is_global = true;
    other_clock->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
    auto *peer = ff("mixed_clock_enable", outer->getPort(id_Q));
    ctx->assign_ff_info(peer); ctx->assign_default_pinmap(peer); place(peer, 30, 20, STRENGTH_WEAK, 14);
    ASSERT_TRUE(ctx->isBelLocationValid(peer->bel));
    // LAB validity currently supports one clock. Inject the mismatch only
    // after legal placement to exercise preflight rejection of an inconsistent
    // cohort; no candidate should probe or repair this deliberately bad input.
    peer->disconnectPort(id_CLK); peer->connectPort(id_CLK, other_clock); ctx->assign_ff_info(peer);
    ASSERT_FALSE(ctx->isBelLocationValid(peer->bel));
    PairSnapshot saved(ctx.get()); PairLog log; rejected(saved);
    EXPECT_NE(log.stream.str().find("reason=whole-enable-cohort-unavailable"), std::string::npos) << log.stream.str();
    EXPECT_EQ(log.stream.str().find("LUT pair copy trial "), std::string::npos);
}

TEST_F(LutPairCopyTest, SevenDistinctLeavesRejectBeforeTrial)
{
    auto bel = outer->bel; ctx->unbindBel(bel); outer->type = id_MISTRAL_ALUT6; outer->params[id_LUT] = Property(uint64_t(0xf0f0f0f0f0f0f0f0), 64);
    for (int i = 0; i < 3; ++i) {
        auto *launch = ff("extra_literal_" + std::to_string(i)); ctx->assign_ff_info(launch); ctx->assign_default_pinmap(launch);
        place(launch, 30, 19, STRENGTH_LOCKED); outer->addInput(pins[i + 3]); outer->connectPort(pins[i + 3], launch->getPort(id_Q));
    }
    ctx->assign_comb_info(outer); ctx->assign_default_pinmap(outer); ctx->bindBel(bel, outer, STRENGTH_WEAK);
    ASSERT_TRUE(ctx->isBelLocationValid(bel));
    { PairSnapshot saved(ctx.get()); PairLog log; rejected(saved);
      EXPECT_EQ(log.stream.str().find("LUT pair copy trial "), std::string::npos); }
}

TEST_F(LutPairCopyTest, UnsupportedSideBoundaryRejectsWithoutChangingOriginalGraph)
{
    // This case retains a supported four-leaf cut; the side boundary is
    // independently responsible for rejection rather than cut-width failure.
    // The root is also an uncharacterized generated clock, not a register input.
    auto *capture = ff("unsupported_clock_side");
    capture->disconnectPort(id_CLK); capture->connectPort(id_CLK, outer->getPort(id_Q));
    ctx->assign_ff_info(capture); ctx->assign_default_pinmap(capture); place(capture, 24, 22);
    PairSnapshot saved(ctx.get()); PairLog log; rejected(saved);
    EXPECT_NE(log.stream.str().find("reason=boundary-clock-or-fanout-unavailable"), std::string::npos) << log.stream.str();
    EXPECT_EQ(log.stream.str().find("LUT pair copy trial "), std::string::npos);
}

TEST_F(LutPairCopyTest, KnownUnrelatedOriginalSideUserKeepsItsFiniteMaxAndMinBounds)
{
    auto *other = ctx->createNet(ctx->id("copy_unrelated_clock")); other->is_global = true;
    other->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
    ASSERT_NE(clock_source(clock, "copy_launch_clock"), nullptr);
    ASSERT_NE(clock_source(other, "copy_independent_capture_clock"), nullptr);
    auto *capture = ff("copy_unrelated_original_side", nullptr, inner->getPort(id_Q));
    capture->disconnectPort(id_CLK); capture->connectPort(id_CLK, other);
    ctx->assign_ff_info(capture); ctx->assign_default_pinmap(capture); place(capture, 24, 20, STRENGTH_WEAK);
    TimingAnalyser before(ctx.get()); before.with_clock_skew = false; before.setup(false, false, true);
    std::vector<EndpointClockPairTiming> old_rows;
    ASSERT_TRUE(before.get_endpoint_clock_pair_timings(CellPortKey(capture->name, id_DATAIN), old_rows)); ASSERT_FALSE(old_rows.empty());
    for (const auto &row : old_rows) {
        ASSERT_FALSE(row.setup_timed); ASSERT_FALSE(row.setup_window); ASSERT_FALSE(row.setup_margin);
        ASSERT_GT(row.min_path_delay, 0); ASSERT_GE(row.max_path_delay, row.min_path_delay);
    }
    PairSnapshot saved(ctx.get()); PairLog log;
    ASSERT_TRUE(ctx->remap_lut_pair_critical(report(), 0)) << log.stream.str();
    auto *clone = new_clone(saved); ASSERT_NE(clone, nullptr); originals(saved, clone, {sink->name});
    TimingAnalyser after(ctx.get()); after.with_clock_skew = false; after.setup(false, false, true);
    std::vector<EndpointClockPairTiming> now;
    ASSERT_TRUE(after.get_endpoint_clock_pair_timings(CellPortKey(capture->name, id_DATAIN), now)); ASSERT_EQ(now.size(), old_rows.size());
    for (size_t i = 0; i < now.size(); ++i) {
        EXPECT_EQ(now[i].launch, old_rows[i].launch); EXPECT_EQ(now[i].capture, old_rows[i].capture);
        EXPECT_FALSE(now[i].setup_timed); EXPECT_FALSE(now[i].setup_margin); EXPECT_FALSE(now[i].setup_window);
        EXPECT_EQ(now[i].max_path_delay, old_rows[i].max_path_delay); EXPECT_EQ(now[i].min_path_delay, old_rows[i].min_path_delay);
    }
    EXPECT_NE(log.stream.str().find("reference_free=1"), std::string::npos) << log.stream.str();
}

TEST_F(LutPairCopyTest, UnsupportedHoldCoverageAndRealNegativeHoldRejectSafely)
{
    auto old_state = sink->pin_data[id_CLK].state;
    sink->pin_data[id_CLK].state = PIN_INV; ctx->assign_ff_info(sink);
    { PairSnapshot saved(ctx.get()); PairLog log; rejected(saved);
      EXPECT_EQ(log.stream.str().find("LUT pair copy trial "), std::string::npos); }
    sink->pin_data[id_CLK].state = old_state; ctx->assign_ff_info(sink);
    clock->clkconstr->period = DelayPair(8000); clock->clkconstr->high = clock->clkconstr->low = DelayPair(4000);
    clock->clkconstr->phase_group = ctx->id("copy_hold_phase"); ctx->settings[ctx->id("target_freq")] = 125e6;
    ASSERT_NE(clock_source(clock, "copy_primary_clock", true), nullptr); auto *capture = clock;
    for (int i = 0; i < 8; ++i) {
        auto *cell = ctx->createCell(ctx->idf("copy_clock_stage_%d", i), id_MISTRAL_ALUT2);
        cell->params[id_LUT] = Property(0xa, 4); cell->addInput(id_A); cell->addInput(id_B); cell->addOutput(id_Q);
        cell->connectPort(id_A, capture); cell->pin_data[id_B].state = PIN_0;
        capture = ctx->createNet(ctx->idf("copy_clock_q_%d", i)); capture->is_global = true; cell->connectPort(id_Q, capture);
        ctx->assign_comb_info(cell); ctx->assign_default_pinmap(cell); place(cell, 2, 35, STRENGTH_LOCKED, 6 * i);
    }
    capture->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr); capture->clkconstr->phase_shift = 0;
    sink->disconnectPort(id_CLK); sink->connectPort(id_CLK, capture); ctx->assign_ff_info(sink);
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    std::vector<EndpointClockPairTiming> rows;
    ASSERT_TRUE(before.get_endpoint_clock_pair_timings(CellPortKey(sink->name, id_ENA), rows)); ASSERT_FALSE(rows.empty());
    for (const auto &row : rows) { ASSERT_TRUE(row.hold_related); ASSERT_TRUE(row.hold_margin); ASSERT_GE(*row.hold_margin, 0); }
    PairSnapshot saved(ctx.get()); CopyTrialObserver probe(ctx.get(), sink);
    EXPECT_FALSE(ctx->remap_lut_pair_critical(report(), -1)); EXPECT_GT(probe.trials, 0); EXPECT_GT(probe.improving_negative_holds, 0) << probe.text;
    restored(saved);
}
