/* Native graph and clock-window checks for capture pipeline placement. SPDX-License-Identifier: ISC */
#include "gtest/gtest.h"
#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"
#include "remap_clock_guard.h"
#include "timing.h"
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

USING_NEXTPNR_NAMESPACE

NEXTPNR_NAMESPACE_BEGIN
int capture_pipeline_locality(Context *, const std::string &, int, int);
void preload_capture_pipeline_locality(Context *, const char *);
NEXTPNR_NAMESPACE_END

namespace {
namespace guard = mistral_remap_clock_guard;

struct PipelineLog {
    std::ostringstream stream;
    PipelineLog() { log_streams.emplace_back(&stream, LogLevel::INFO_MSG); }
    ~PipelineLog() { log_streams.pop_back(); }
};

// Save identities and the raw indexed user allocation order, rather than
// merely comparing the visible connections after a placement transaction.
struct PipelineSnapshot {
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
        ArchCellInfo cache;
        std::vector<IdString> port_order;
    };
    struct Net {
        NetInfo *identity;
        PortRef driver;
        indexed_store<PortRef> users;
        decltype(NetInfo::attrs) attrs;
        decltype(NetInfo::wires) wires;
        Region *region;
        IdString constant;
        bool global;
        std::vector<IdString> aliases;
    };
    std::map<IdString, Cell> cells;
    std::map<IdString, Net> nets;
    std::map<IdString, IdString> aliases;
    std::map<IdString, PortInfo> ports;
    std::vector<IdString> cell_order, net_order;
    std::vector<LABInfo> labs;

    explicit PipelineSnapshot(Context *ctx) : labs(ctx->labs)
    {
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            Cell saved{cell, cell->type, cell->bel, cell->belStrength, cell->cluster, cell->region,
                       cell->params, cell->attrs, {}, static_cast<const ArchCellInfo &>(*cell), {}};
            for (const auto &port : cell->ports) {
                saved.ports.emplace(port.first, port.second);
                saved.port_order.push_back(port.first);
            }
            cells.emplace(entry.first, std::move(saved));
            cell_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            nets.emplace(entry.first, Net{net, net->driver, net->users, net->attrs, net->wires,
                                         net->region, net->constant_value, net->is_global, net->aliases});
            net_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->net_aliases) aliases.emplace(entry.first, entry.second);
        for (const auto &entry : ctx->ports) ports.emplace(entry.first, entry.second);
    }

    static void expect_port(const PortInfo &now, const PortInfo &old)
    {
        EXPECT_EQ(now.name, old.name); EXPECT_EQ(now.net, old.net);
        EXPECT_EQ(now.type, old.type); EXPECT_EQ(now.user_idx, old.user_idx);
    }

    static void expect_users(indexed_store<PortRef> now, indexed_store<PortRef> old)
    {
        ASSERT_EQ(now.entries(), old.entries()); ASSERT_EQ(now.capacity(), old.capacity());
        for (auto user : old.enumerate()) {
            ASSERT_TRUE(now.count(user.index));
            EXPECT_EQ(now.at(user.index).cell, user.value.cell);
            EXPECT_EQ(now.at(user.index).port, user.value.port);
        }
        for (int i = 0, probes = old.capacity() + 4; i < probes; ++i)
            EXPECT_EQ(now.add(PortRef{}), old.add(PortRef{}));
    }

    static void expect_lab(const LABInfo &now, const LABInfo &old)
    {
        EXPECT_EQ(now.is_mlab, old.is_mlab); EXPECT_EQ(now.aclr_used, old.aclr_used);
        EXPECT_EQ(now.clk_wires, old.clk_wires); EXPECT_EQ(now.ena_wires, old.ena_wires);
        EXPECT_EQ(now.aclr_wires, old.aclr_wires); EXPECT_EQ(now.sclr_wire, old.sclr_wire);
        EXPECT_EQ(now.sload_wire, old.sload_wire);
        for (size_t i = 0; i < old.alms.size(); ++i) {
            const auto &a = now.alms[i], &b = old.alms[i];
            EXPECT_EQ(a.comb_out, b.comb_out); EXPECT_EQ(a.sel_clk, b.sel_clk);
            EXPECT_EQ(a.sel_ena, b.sel_ena); EXPECT_EQ(a.sel_aclr, b.sel_aclr);
            EXPECT_EQ(a.sel_ef, b.sel_ef); EXPECT_EQ(a.ff_in, b.ff_in); EXPECT_EQ(a.ff_out, b.ff_out);
            EXPECT_EQ(a.lut_bels, b.lut_bels); EXPECT_EQ(a.ff_bels, b.ff_bels);
            EXPECT_EQ(a.l6_mode, b.l6_mode); EXPECT_EQ(a.carry_mode, b.carry_mode);
            EXPECT_EQ(a.clk_ena_idx, b.clk_ena_idx); EXPECT_EQ(a.aclr_idx, b.aclr_idx);
            EXPECT_EQ(a.unique_input_count, b.unique_input_count);
        }
    }

    void expect(Context *ctx, const std::set<IdString> &moved = {}) const
    {
        ASSERT_EQ(ctx->cells.size(), cells.size()); ASSERT_EQ(ctx->nets.size(), nets.size());
        ASSERT_EQ(ctx->net_aliases.size(), aliases.size()); ASSERT_EQ(ctx->ports.size(), ports.size());
        std::vector<IdString> actual_cells, actual_nets;
        for (const auto &entry : ctx->cells) actual_cells.push_back(entry.first);
        for (const auto &entry : ctx->nets) actual_nets.push_back(entry.first);
        EXPECT_EQ(actual_cells, cell_order); EXPECT_EQ(actual_nets, net_order);
        std::set<size_t> changed_labs;
        for (const auto &entry : cells) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->cells.count(entry.first));
            auto *cell = ctx->cells.at(entry.first).get(); const auto &old = entry.second;
            EXPECT_EQ(cell, old.identity); EXPECT_EQ(cell->type, old.type);
            EXPECT_EQ(cell->belStrength, old.strength); EXPECT_EQ(cell->cluster, old.cluster);
            EXPECT_EQ(cell->region, old.region); EXPECT_EQ(cell->params, old.params); EXPECT_EQ(cell->attrs, old.attrs);
            EXPECT_EQ(cell->constr_children, old.cache.constr_children);
            EXPECT_EQ(cell->constr_x, old.cache.constr_x); EXPECT_EQ(cell->constr_y, old.cache.constr_y);
            EXPECT_EQ(cell->constr_z, old.cache.constr_z); EXPECT_EQ(cell->constr_abs_z, old.cache.constr_abs_z);
            if (moved.count(entry.first)) {
                ASSERT_NE(cell->bel, old.bel);
                EXPECT_EQ(ctx->getBoundBelCell(old.bel), nullptr);
                changed_labs.insert(ctx->bel_data(old.bel).lab_data.lab);
                changed_labs.insert(ctx->bel_data(cell->bel).lab_data.lab);
            } else EXPECT_EQ(cell->bel, old.bel);
            if (cell->bel != BelId()) EXPECT_EQ(ctx->getBoundBelCell(cell->bel), cell);
            std::vector<IdString> order;
            for (const auto &port : cell->ports) order.push_back(port.first);
            EXPECT_EQ(order, old.port_order); ASSERT_EQ(cell->ports.size(), old.ports.size());
            for (const auto &port : old.ports) {
                ASSERT_TRUE(cell->ports.count(port.first)); expect_port(cell->ports.at(port.first), port.second);
            }
            ASSERT_EQ(cell->pin_data.size(), old.cache.pin_data.size());
            for (const auto &pin : old.cache.pin_data) {
                ASSERT_TRUE(cell->pin_data.count(pin.first));
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
            if (cell->type == id_MISTRAL_FF) {
                EXPECT_EQ(cell->ffInfo.ctrlset, old.cache.ffInfo.ctrlset);
                EXPECT_EQ(cell->ffInfo.datain, old.cache.ffInfo.datain); EXPECT_EQ(cell->ffInfo.sdata, old.cache.ffInfo.sdata);
            }
        }
        for (const auto &entry : nets) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->nets.count(entry.first));
            auto *net = ctx->nets.at(entry.first).get(); const auto &old = entry.second;
            EXPECT_EQ(net, old.identity); EXPECT_EQ(net->driver.cell, old.driver.cell);
            EXPECT_EQ(net->driver.port, old.driver.port); EXPECT_EQ(net->attrs, old.attrs);
            EXPECT_EQ(net->region, old.region); EXPECT_EQ(net->constant_value, old.constant);
            EXPECT_EQ(net->is_global, old.global); EXPECT_EQ(net->aliases, old.aliases);
            expect_users(net->users, old.users); ASSERT_EQ(net->wires.size(), old.wires.size());
            for (const auto &wire : old.wires) {
                ASSERT_TRUE(net->wires.count(wire.first));
                EXPECT_EQ(net->wires.at(wire.first).pip, wire.second.pip);
                EXPECT_EQ(net->wires.at(wire.first).strength, wire.second.strength);
            }
        }
        for (const auto &entry : aliases) { ASSERT_TRUE(ctx->net_aliases.count(entry.first)); EXPECT_EQ(ctx->net_aliases.at(entry.first), entry.second); }
        for (const auto &entry : ports) { ASSERT_TRUE(ctx->ports.count(entry.first)); expect_port(ctx->ports.at(entry.first), entry.second); }
        ASSERT_EQ(ctx->labs.size(), labs.size());
        for (size_t i = 0; i < labs.size(); ++i) if (!changed_labs.count(i)) expect_lab(ctx->labs[i], labs[i]);
        ctx->check();
    }
};

class CapturePipelineLocalityTest : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clock;
    CellInfo *hard, *first, *second, *terminal;
    IdString data_pin;

    CellInfo *ff(const std::string &name, NetInfo *data, bool initial)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->pin_data[id_ENA].state = cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = cell->pin_data[id_SLOAD].state = PIN_0;
        cell->params[id_INIT] = Property(initial ? 1 : 0, 1);
        cell->connectPort(id_CLK, clock); cell->connectPort(id_DATAIN, data);
        cell->addOutput(id_Q); cell->connectPort(id_Q, ctx->createNet(ctx->id(name + "$q")));
        return cell;
    }

    void holes(NetInfo *net)
    {
        std::array<CellInfo *, 3> temporary;
        for (int i = 0; i < 3; ++i) {
            temporary[i] = ctx->createCell(ctx->idf("hole_%s_%d", net->name.c_str(ctx.get()), i), id_MISTRAL_ALUT2);
            temporary[i]->addInput(id_A); temporary[i]->connectPort(id_A, net);
        }
        for (int i : {1, 0, 2}) {
            temporary[i]->disconnectPort(id_A); ctx->cells.erase(temporary[i]->name);
        }
    }

    void place(CellInfo *cell, int x, int y, PlaceStrength strength = STRENGTH_WEAK)
    {
        auto bel = ctx->getBelByLocation(Loc(x, y, 2));
        ASSERT_NE(bel, BelId()); ASSERT_TRUE(ctx->checkBelAvail(bel));
        ctx->bindBel(bel, cell, strength); ASSERT_TRUE(ctx->isBelLocationValid(bel));
    }

    json11::Json endpoint(CellInfo *cell, IdString pin) const
    {
        auto at = ctx->getBelLocation(cell->bel);
        return json11::Json::object{{"cell", cell->name.str(ctx.get())}, {"port", pin.str(ctx.get())},
                                  {"loc", json11::Json::array{at.x, at.y}}};
    }

    json11::Json report_json() const
    {
        using json11::Json;
        auto source = endpoint(hard, data_pin), sink = endpoint(first, id_DATAIN);
        auto launch = ctx->getPortClockingInfo(hard, data_pin, 0), capture = ctx->getPortClockingInfo(first, id_DATAIN, 0);
        Json::array segments{
            Json::object{{"type", "clk-to-q"}, {"delay", launch.clockToQ.maxDelay() / 1000.0}, {"from", source}, {"to", source}},
            Json::object{{"type", "routing"}, {"delay", 10.0}, {"net", first->getPort(id_DATAIN)->name.str(ctx.get())}, {"from", source}, {"to", sink}},
            Json::object{{"type", "setup"}, {"delay", capture.setup.maxDelay() / 1000.0}, {"from", sink}, {"to", sink}}};
        return Json::object{{"critical_paths", Json::array{Json::object{{"max_delay", 7.692}, {"path", segments}}}}};
    }

    std::string report() const { return report_json().dump(); }

    guard::Rows rows(TimingAnalyser &timing, CellInfo *cell)
    {
        guard::Rows result;
        EXPECT_TRUE(timing.get_endpoint_clock_pair_timings(CellPortKey(cell->name, id_DATAIN), result));
        EXPECT_FALSE(result.empty()); return result;
    }

    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7"; ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 130e6;
        clock = ctx->createNet(ctx->id("unrelated_name_clock")); clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>(); clock->clkconstr->period = DelayPair(7692);
        clock->clkconstr->high = clock->clkconstr->low = DelayPair(3846);
        auto *driver = ctx->createCell(ctx->id("unrelated_name_clock_buffer"), id_MISTRAL_CLKBUF);
        driver->addOutput(id_Q); driver->connectPort(id_Q, clock);
        ctx->createNet(ctx->id("$PACKER_GND_NET")); ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        hard = ctx->createCell(ctx->id("renamed_registered_hard_source"), id_cyclonev_hps_interface_fpga2sdram);
        auto clock_pin = ctx->id("rd_clk_0"); data_pin = ctx->id("rd_data_0[0]");
        hard->addInput(clock_pin); hard->connectPort(clock_pin, clock); hard->addOutput(data_pin);
        hard->connectPort(data_pin, ctx->createNet(ctx->id("renamed_registered_hard_data")));
        first = ff("arbitrary_capture_stage", hard->getPort(data_pin), true);
        second = ff("arbitrary_pipeline_stage", first->getPort(id_Q), false);
        terminal = ff("arbitrary_terminal_stage", second->getPort(id_Q), true);
        ctx->net_aliases[ctx->id("arbitrary_data_alias")] = first->getPort(id_Q)->name;
        ctx->ports[ctx->id("observed_output")] = PortInfo{ctx->id("observed_output"), terminal->getPort(id_Q), PORT_OUT, {}};
        ctx->assignArchInfo();
        for (auto bel : ctx->getBels()) if (ctx->getBelType(bel) == hard->type) {
            ctx->bindBel(bel, hard, STRENGTH_LOCKED); break;
        }
        ASSERT_NE(hard->bel, BelId());
        place(first, 25, 23); place(second, 25, 17); place(terminal, 30, 25, STRENGTH_LOCKED);
        for (auto *net : {first->getPort(id_DATAIN), first->getPort(id_Q), second->getPort(id_Q)}) holes(net);
        ctx->check();
    }
};

TEST_F(CapturePipelineLocalityTest, RenamedPairMovesWithoutChangingLatencyGraphOrInitialStates)
{
    PipelineSnapshot saved(ctx.get());
    TimingAnalyser before(ctx.get()), before_reference(ctx.get());
    before.with_clock_skew = true; before.setup(false, false, true); before_reference.setup(false, false, true);
    auto old_first = rows(before, first), old_second = rows(before, second), old_terminal = rows(before, terminal);
    auto old_terminal_reference = rows(before_reference, terminal); auto old_holds = guard::holds(before);
    ASSERT_EQ(capture_pipeline_locality(ctx.get(), report(), 1, 24), 1);
    saved.expect(ctx.get(), {first->name, second->name});
    for (auto *cell : {first, second, terminal}) {
        EXPECT_TRUE(ctx->isBelLocationValid(cell->bel));
        EXPECT_NE(ctx->getNetinfoSinkWire(cell->getPort(id_DATAIN), PortRef{cell, id_DATAIN}, 0), WireId());
        EXPECT_NE(ctx->getNetinfoSourceWire(cell->getPort(id_Q)), WireId());
    }
    TimingAnalyser after(ctx.get()), after_reference(ctx.get());
    after.with_clock_skew = true; after.setup(false, false, true); after_reference.setup(false, false, true);
    auto new_first = rows(after, first), new_second = rows(after, second);
    ASSERT_TRUE(guard::rows_match(old_first, new_first)); ASSERT_TRUE(guard::rows_match(old_second, new_second));
    EXPECT_TRUE(guard::rows_nonregressing(old_first, new_first, false));
    for (size_t i = 0; i < old_first.size(); ++i) {
        ASSERT_TRUE(old_first[i].setup_margin); ASSERT_TRUE(new_first[i].setup_margin);
        EXPECT_GE(*new_first[i].setup_margin, *old_first[i].setup_margin + 250);
    }
    // The internal stage may spend its positive full-cycle setup margin;
    // the registered terminal may not pay for the capture improvement.
    for (size_t i = 0; i < old_second.size(); ++i) {
        ASSERT_TRUE(old_second[i].setup_margin); ASSERT_TRUE(new_second[i].setup_margin);
        EXPECT_GE(*old_second[i].setup_margin, 0); EXPECT_GE(*new_second[i].setup_margin, 0);
        if (old_second[i].hold_related) {
            ASSERT_TRUE(old_second[i].hold_margin); ASSERT_TRUE(new_second[i].hold_margin);
            EXPECT_GE(*new_second[i].hold_margin, std::min(delay_t(0), *old_second[i].hold_margin));
        }
    }
    EXPECT_TRUE(guard::rows_nonregressing(old_terminal, rows(after, terminal), false));
    EXPECT_TRUE(guard::rows_nonregressing(old_terminal_reference, rows(after_reference, terminal), false));
    EXPECT_TRUE(guard::holds_nonregressing(old_holds, guard::holds(after)));
    EXPECT_TRUE(guard::clocks_nonregressing(before, after));
    PipelineSnapshot accepted(ctx.get());
    EXPECT_EQ(capture_pipeline_locality(ctx.get(), report(), 1, 1), 0);
    accepted.expect(ctx.get());
}

TEST_F(CapturePipelineLocalityTest, DistinctHardClockPortsOnTheSameNetQualifyBothIndependentPairs)
{
    auto other_clock_pin = ctx->id("rd_clk_2"), other_data_pin = ctx->id("rd_data_2[0]");
    hard->addInput(other_clock_pin); hard->connectPort(other_clock_pin, clock);
    hard->addOutput(other_data_pin);
    hard->connectPort(other_data_pin, ctx->createNet(ctx->id("unrelated_other_hard_data")));
    auto *other_first = ff("cedar_register", hard->getPort(other_data_pin), false);
    auto *other_second = ff("harbor_register", other_first->getPort(id_Q), true);
    auto *other_terminal = ff("violet_register", other_second->getPort(id_Q), false);
    ctx->net_aliases[ctx->id("unrelated_other_data_alias")] = other_first->getPort(id_Q)->name;
    ctx->ports[ctx->id("observed_other_output")] =
            PortInfo{ctx->id("observed_other_output"), other_terminal->getPort(id_Q), PORT_OUT, {}};
    ctx->assignArchInfo();
    place(other_first, 27, 23); place(other_second, 27, 17); place(other_terminal, 30, 24, STRENGTH_LOCKED);
    for (auto *net : {other_first->getPort(id_DATAIN), other_first->getPort(id_Q), other_second->getPort(id_Q)}) holes(net);
    ctx->check();

    auto launch = ctx->getPortClockingInfo(hard, data_pin, 0);
    auto other_launch = ctx->getPortClockingInfo(hard, other_data_pin, 0);
    ASSERT_EQ(launch.clock_port, ctx->id("rd_clk_0")); ASSERT_EQ(other_launch.clock_port, other_clock_pin);
    ASSERT_NE(launch.clock_port, other_launch.clock_port);
    ASSERT_EQ(hard->getPort(launch.clock_port), clock); ASSERT_EQ(hard->getPort(other_launch.clock_port), clock);
    ASSERT_EQ(launch.edge, other_launch.edge);
    const std::array<std::array<CellInfo *, 3>, 2> chains{{{first, second, terminal},
                                                        {other_first, other_second, other_terminal}}};
    PipelineSnapshot saved(ctx.get());
    TimingAnalyser before(ctx.get()), before_reference(ctx.get());
    before.with_clock_skew = true; before.setup(false, false, true); before_reference.setup(false, false, true);
    ASSERT_FALSE(before.have_loops); ASSERT_FALSE(before_reference.have_loops);
    std::array<guard::Rows, 2> old_first, old_second, old_terminal, old_terminal_reference;
    std::array<delay_t, 2> old_incoming;
    for (size_t i = 0; i < chains.size(); ++i) {
        const auto &chain = chains[i]; SCOPED_TRACE(chain[0]->name.str(ctx.get()));
        for (auto *cell : chain) {
            ASSERT_EQ(cell->getPort(id_CLK), clock);
            ASSERT_EQ(ctx->getPortClockingInfo(cell, id_DATAIN, 0).edge, launch.edge);
        }
        auto *data = chain[0]->getPort(id_DATAIN);
        auto source_wire = ctx->getNetinfoSourceWire(data);
        auto sink_wire = ctx->getNetinfoSinkWire(data, PortRef{chain[0], id_DATAIN}, 0);
        ASSERT_NE(source_wire, WireId()); ASSERT_NE(sink_wire, WireId());
        old_incoming[i] = ctx->estimateDelay(source_wire, sink_wire);
        old_first[i] = rows(before, chain[0]); old_second[i] = rows(before, chain[1]);
        old_terminal[i] = rows(before, chain[2]); old_terminal_reference[i] = rows(before_reference, chain[2]);
    }
    auto old_holds = guard::holds(before);
    // The report names only the original chain; the budget must reach the
    // other registered output through the shared actual clock net and edge.
    ASSERT_EQ(capture_pipeline_locality(ctx.get(), report(), 2, 24), 2);
    saved.expect(ctx.get(), {first->name, second->name, other_first->name, other_second->name});
    TimingAnalyser after(ctx.get()), after_reference(ctx.get());
    after.with_clock_skew = true; after.setup(false, false, true); after_reference.setup(false, false, true);
    ASSERT_FALSE(after.have_loops); ASSERT_FALSE(after_reference.have_loops);
    for (size_t i = 0; i < chains.size(); ++i) {
        const auto &chain = chains[i]; SCOPED_TRACE(chain[0]->name.str(ctx.get()));
        for (auto *cell : chain) {
            EXPECT_TRUE(ctx->isBelLocationValid(cell->bel));
            EXPECT_NE(ctx->getNetinfoSinkWire(cell->getPort(id_DATAIN), PortRef{cell, id_DATAIN}, 0), WireId());
            EXPECT_NE(ctx->getNetinfoSourceWire(cell->getPort(id_Q)), WireId());
        }
        auto *data = chain[0]->getPort(id_DATAIN);
        auto source_wire = ctx->getNetinfoSourceWire(data);
        auto sink_wire = ctx->getNetinfoSinkWire(data, PortRef{chain[0], id_DATAIN}, 0);
        ASSERT_NE(source_wire, WireId()); ASSERT_NE(sink_wire, WireId());
        EXPECT_LE(ctx->estimateDelay(source_wire, sink_wire), old_incoming[i] - 250);
        auto new_first = rows(after, chain[0]), new_second = rows(after, chain[1]);
        ASSERT_TRUE(guard::rows_match(old_first[i], new_first)); ASSERT_TRUE(guard::rows_match(old_second[i], new_second));
        EXPECT_TRUE(guard::rows_nonregressing(old_first[i], new_first, false));
        for (size_t j = 0; j < old_first[i].size(); ++j) {
            ASSERT_TRUE(old_first[i][j].setup_margin); ASSERT_TRUE(new_first[j].setup_margin);
            EXPECT_GE(*new_first[j].setup_margin, *old_first[i][j].setup_margin + 250);
        }
        for (size_t j = 0; j < old_second[i].size(); ++j) {
            ASSERT_TRUE(old_second[i][j].setup_margin); ASSERT_TRUE(new_second[j].setup_margin);
            EXPECT_GE(*old_second[i][j].setup_margin, 0); EXPECT_GE(*new_second[j].setup_margin, 0);
            if (old_second[i][j].hold_related) {
                ASSERT_TRUE(old_second[i][j].hold_margin); ASSERT_TRUE(new_second[j].hold_margin);
                EXPECT_GE(*new_second[j].hold_margin, std::min(delay_t(0), *old_second[i][j].hold_margin));
            }
        }
        EXPECT_TRUE(guard::rows_nonregressing(old_terminal[i], rows(after, chain[2]), false));
        EXPECT_TRUE(guard::rows_nonregressing(old_terminal_reference[i], rows(after_reference, chain[2]), false));
    }
    EXPECT_TRUE(guard::holds_nonregressing(old_holds, guard::holds(after)));
    EXPECT_TRUE(guard::clocks_nonregressing(before, after));
}

TEST_F(CapturePipelineLocalityTest, IllegalClockGeometriesDoNotSpendTheTimingTrialBudget)
{
    auto *foreign_clock = ctx->createNet(ctx->id("independent_occupant_clock"));
    foreign_clock->is_global = true;
    foreign_clock->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
    auto *driver = ctx->createCell(ctx->id("independent_occupant_clock_buffer"), id_MISTRAL_CLKBUF);
    driver->addOutput(id_Q); driver->connectPort(id_Q, foreign_clock);
    ctx->assignArchInfo();
    const BelId old_first = first->bel, old_second = second->bel;
    const auto first_strength = first->belStrength, second_strength = second->belStrength;
    auto clock_values = [](const ClockConstraint &c) {
        return std::make_tuple(c.period.minDelay(), c.period.maxDelay(), c.high.minDelay(), c.high.maxDelay(),
                               c.low.minDelay(), c.low.maxDelay(), c.phase_group, c.phase_shift);
    };
    const auto old_clock = clock_values(*clock->clkconstr), old_foreign_clock = clock_values(*foreign_clock->clkconstr);

    // Build a congested fixture using the real pass's accepted first-stage
    // LABs, without copying its candidate scoring or adding production hooks.
    // A weak FF on a different clock leaves the feed-through geometry free
    // but makes that LAB illegal for either pipeline register. Setup is
    // bounded to 32 discovery passes; the final pass must actually reach a
    // legal, guarded candidate after more than 16 native-illegal geometries.
    for (int discovery = 0; discovery < 32; ++discovery) {
        SCOPED_TRACE(discovery);
        PipelineSnapshot saved(ctx.get());
        TimingAnalyser before(ctx.get()), before_reference(ctx.get());
        before.with_clock_skew = true; before.setup(false, false, true);
        before_reference.setup(false, false, true);
        ASSERT_FALSE(before.have_loops); ASSERT_FALSE(before_reference.have_loops);
        auto old_first_rows = rows(before, first), old_second_rows = rows(before, second);
        auto old_terminal = rows(before, terminal), old_terminal_reference = rows(before_reference, terminal);
        auto old_holds = guard::holds(before);
        PipelineLog log;
        ASSERT_EQ(capture_pipeline_locality(ctx.get(), report(), 1, 24), 1) << log.stream.str();
        saved.expect(ctx.get(), {first->name, second->name});
        const BelId accepted_first = first->bel;

        std::string summary, line;
        std::istringstream lines(log.stream.str());
        int rejected_rows = 0;
        while (std::getline(lines, line)) {
            if (line.find("Capture pipeline locality chain summary ") != std::string::npos) {
                ASSERT_TRUE(summary.empty()); summary = line;
            }
            if (line.find("Capture pipeline locality rejected geometry ") != std::string::npos) ++rejected_rows;
        }
        ASSERT_FALSE(summary.empty()) << log.stream.str();
        auto count = [&](const std::string &name) {
            auto at = summary.find(" " + name + "=");
            EXPECT_NE(at, std::string::npos);
            if (at == std::string::npos) return -1;
            std::istringstream value(summary.substr(at + name.size() + 2));
            int result = -1; value >> result; EXPECT_FALSE(value.fail()); return result;
        };
        const int attempts = count("attempted_geometry"), illegal = count("legality_rejects");
        const int preservation = count("preservation_rejects"), timed = count("timed_trials");
        const int first_timed = count("first_timed_geometry");
        EXPECT_LE(attempts, 64); EXPECT_LE(timed, 16); EXPECT_GE(timed, 1);
        EXPECT_EQ(attempts, illegal + preservation + timed);
        EXPECT_EQ(preservation, 0); EXPECT_EQ(count("retained"), 1);
        EXPECT_EQ(rejected_rows, std::min(4, illegal + preservation));
        EXPECT_EQ(clock_values(*clock->clkconstr), old_clock);
        EXPECT_EQ(clock_values(*foreign_clock->clkconstr), old_foreign_clock);
        if (illegal > 16 && first_timed > 17) {
            // More than the old entire geometry budget was rejected before
            // the first timing trial, yet this later native placement passes
            // the complete timing, graph, INIT and preservation checks.
            for (const auto &entry : ctx->cells)
                if (entry.second->bel != BelId()) EXPECT_TRUE(ctx->isBelLocationValid(entry.second->bel));
            TimingAnalyser after(ctx.get()), after_reference(ctx.get());
            after.with_clock_skew = true; after.setup(false, false, true);
            after_reference.setup(false, false, true);
            ASSERT_FALSE(after.have_loops); ASSERT_FALSE(after_reference.have_loops);
            auto new_first = rows(after, first), new_second = rows(after, second);
            ASSERT_TRUE(guard::rows_match(old_first_rows, new_first));
            ASSERT_TRUE(guard::rows_match(old_second_rows, new_second));
            EXPECT_TRUE(guard::rows_nonregressing(old_first_rows, new_first, false));
            for (size_t i = 0; i < old_first_rows.size(); ++i) {
                ASSERT_TRUE(old_first_rows[i].setup_margin); ASSERT_TRUE(new_first[i].setup_margin);
                EXPECT_GE(*new_first[i].setup_margin, *old_first_rows[i].setup_margin + 250);
            }
            for (size_t i = 0; i < old_second_rows.size(); ++i) {
                ASSERT_TRUE(old_second_rows[i].setup_margin); ASSERT_TRUE(new_second[i].setup_margin);
                EXPECT_GE(*old_second_rows[i].setup_margin, 0); EXPECT_GE(*new_second[i].setup_margin, 0);
                if (old_second_rows[i].hold_related) {
                    ASSERT_TRUE(old_second_rows[i].hold_margin); ASSERT_TRUE(new_second[i].hold_margin);
                    EXPECT_GE(*new_second[i].hold_margin, std::min(delay_t(0), *old_second_rows[i].hold_margin));
                }
            }
            EXPECT_TRUE(guard::rows_nonregressing(old_terminal, rows(after, terminal), false));
            EXPECT_TRUE(guard::rows_nonregressing(old_terminal_reference, rows(after_reference, terminal), true));
            EXPECT_TRUE(guard::holds_nonregressing(old_holds, guard::holds(after)));
            EXPECT_TRUE(guard::clocks_nonregressing(before, after));
            return;
        }

        // Undo this setup discovery using ordinary native placement updates;
        // the complete saved fixture must be exact before adding congestion.
        ctx->unbindBel(first->bel); ctx->unbindBel(second->bel);
        ctx->bindBel(old_first, first, first_strength); ctx->bindBel(old_second, second, second_strength);
        saved.expect(ctx.get());
        const auto target = ctx->bel_data(accepted_first).lab_data;
        ASSERT_NE(target.lab, ctx->bel_data(old_first).lab_data.lab);
        ASSERT_NE(target.lab, ctx->bel_data(old_second).lab_data.lab);
        BelId blocker_bel;
        const auto &alms = ctx->labs.at(target.lab).alms;
        for (auto alm = alms.rbegin(); alm != alms.rend() && blocker_bel == BelId(); ++alm)
            for (int half : {2, 0}) {
                auto candidate = alm->ff_bels[half];
                if (ctx->bel_data(candidate).lab_data.alm == target.alm || ctx->getBoundBelCell(candidate) ||
                    ctx->getBoundBelCell(alm->ff_bels[half + 1]) || ctx->getBoundBelCell(alm->lut_bels[half / 2])) continue;
                blocker_bel = candidate; break;
            }
        ASSERT_NE(blocker_bel, BelId());
        auto *blocker = ff("weak_clock_occupant_" + std::to_string(discovery),
                           ctx->nets.at(ctx->id("$PACKER_GND_NET")).get(), discovery % 2);
        blocker->disconnectPort(id_CLK); blocker->connectPort(id_CLK, foreign_clock);
        ctx->assignArchInfo(); ctx->bindBel(blocker_bel, blocker, STRENGTH_WEAK);
        ASSERT_TRUE(ctx->isBelLocationValid(blocker_bel));
        ASSERT_TRUE(blocker->attrs.empty()); ASSERT_EQ(blocker->belStrength, STRENGTH_WEAK);
        ASSERT_EQ(blocker->getPort(id_Q)->users.entries(), 0);
        PipelineSnapshot congested(ctx.get());
        ctx->unbindBel(old_first); ctx->bindBel(accepted_first, first, first_strength);
        EXPECT_FALSE(ctx->isBelLocationValid(accepted_first)); EXPECT_FALSE(ctx->isBelLocationValid(blocker_bel));
        ctx->unbindBel(accepted_first); ctx->bindBel(old_first, first, first_strength);
        congested.expect(ctx.get());
    }
    FAIL() << "Bounded native fixture did not expose a retained candidate beyond 16 illegal geometries";
}

TEST_F(CapturePipelineLocalityTest, RequestIsDefaultOffAndNullPreloadPreservesTheDesign)
{
    PipelineSnapshot saved(ctx.get());
    EXPECT_TRUE(ctx->capture_pipeline_report.empty()); EXPECT_EQ(ctx->capture_pipeline_budget, 0);
    EXPECT_EQ(ctx->capture_pipeline_radius, 24);
    preload_capture_pipeline_locality(ctx.get(), nullptr);
    EXPECT_TRUE(ctx->capture_pipeline_report.empty()); EXPECT_EQ(ctx->capture_pipeline_budget, 0);
    EXPECT_EQ(ctx->capture_pipeline_radius, 24); saved.expect(ctx.get());
}

TEST_F(CapturePipelineLocalityTest, ProtectedPipelineMembersLeaveExactPlacementAndCaches)
{
    for (auto *cell : {first, second}) {
        SCOPED_TRACE(cell->name.str(ctx.get())); cell->attrs[ctx->id("keep")] = 1;
        PipelineSnapshot saved(ctx.get()); EXPECT_EQ(capture_pipeline_locality(ctx.get(), report(), 1, 24), 0);
        saved.expect(ctx.get()); cell->attrs.erase(ctx->id("keep"));
    }
}

TEST_F(CapturePipelineLocalityTest, SharedFirstOutputOrDifferentStageClockCannotQualify)
{
    auto *observer = ff("shared_capture_observer", first->getPort(id_Q), false);
    ctx->assignArchInfo(); place(observer, 30, 24, STRENGTH_LOCKED);
    ASSERT_NE(observer->bel, BelId());
    { PipelineSnapshot saved(ctx.get()); EXPECT_EQ(capture_pipeline_locality(ctx.get(), report(), 1, 24), 0); saved.expect(ctx.get()); }
    ctx->unbindBel(observer->bel); observer->disconnectPort(id_CLK); observer->disconnectPort(id_DATAIN); observer->disconnectPort(id_Q);
    ctx->cells.erase(observer->name);
    auto *other_clock = ctx->createNet(ctx->id("different_pipeline_clock")); other_clock->is_global = true;
    other_clock->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
    second->disconnectPort(id_CLK); second->connectPort(id_CLK, other_clock); ctx->assignArchInfo();
    PipelineSnapshot saved(ctx.get()); EXPECT_EQ(capture_pipeline_locality(ctx.get(), report(), 1, 24), 0); saved.expect(ctx.get());
}

TEST_F(CapturePipelineLocalityTest, InvalidBoundsAndMalformedOrStaleReportsRejectBeforeMutation)
{
    PipelineSnapshot saved(ctx.get());
    for (auto bounds : {std::pair<int, int>{0, 24}, {65, 24}, {1, 0}, {1, 25}}) {
        EXPECT_THROW(capture_pipeline_locality(ctx.get(), report(), bounds.first, bounds.second), log_execution_error_exception);
        saved.expect(ctx.get());
    }
    std::vector<std::string> invalid{"", "{", "{}", "{\"critical_paths\":{}}"};
    auto document = report_json().object_items(); auto paths = document.at("critical_paths").array_items();
    auto path = paths.front().object_items(); path["max_delay"] = "bad";
    paths[0] = path; document["critical_paths"] = paths; invalid.push_back(json11::Json(document).dump());
    auto stale = report(); auto offset = stale.find("renamed_registered_hard_data"); ASSERT_NE(offset, std::string::npos);
    stale.replace(offset, std::string("renamed_registered_hard_data").size(), "wrong_net"); invalid.push_back(stale);
    std::string parse_error;
    auto stale_document = json11::Json::parse(stale, parse_error); ASSERT_TRUE(parse_error.empty());
    document = report_json().object_items(); paths = document.at("critical_paths").array_items();
    paths.push_back(stale_document["critical_paths"].array_items().front()); document["critical_paths"] = paths;
    invalid.push_back(json11::Json(document).dump()); // A later bad row must not follow an accepted first move.
    document = report_json().object_items(); paths = document.at("critical_paths").array_items(); path = paths.front().object_items();
    auto segments = path.at("path").array_items(); auto setup = segments.back().object_items();
    auto sink = setup.at("to").object_items(); sink["loc"] = json11::Json::array{99, 99}; setup["to"] = sink;
    segments.back() = setup; path["path"] = segments; paths[0] = path; document["critical_paths"] = paths;
    invalid.push_back(json11::Json(document).dump());
    for (const auto &text : invalid) {
        SCOPED_TRACE(text); EXPECT_THROW(capture_pipeline_locality(ctx.get(), text, 1, 24), log_execution_error_exception);
        saved.expect(ctx.get());
    }
}

TEST_F(CapturePipelineLocalityTest, RoutedAndSlotDesignsRejectWithoutMutation)
{
    ctx->fes_any_slot_region_active = true;
    { PipelineSnapshot saved(ctx.get()); EXPECT_THROW(capture_pipeline_locality(ctx.get(), report(), 1, 24), log_execution_error_exception); saved.expect(ctx.get()); }
    ctx->fes_any_slot_region_active = false;
    auto *data = first->getPort(id_DATAIN); auto wire = ctx->getNetinfoSourceWire(data); ASSERT_NE(wire, WireId());
    ctx->bindWire(wire, data, STRENGTH_WEAK);
    PipelineSnapshot saved(ctx.get()); EXPECT_THROW(capture_pipeline_locality(ctx.get(), report(), 1, 24), log_execution_error_exception);
    saved.expect(ctx.get());
}

TEST_F(CapturePipelineLocalityTest, UnqualifiedSearchRestoresBothOwnersUsersAndLABCaches)
{
    PipelineSnapshot saved(ctx.get());
    EXPECT_EQ(capture_pipeline_locality(ctx.get(), report(), 1, 1), 0);
    saved.expect(ctx.get());
}
} // namespace
