#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
const IdString copy_pins[] = {id_A, id_B, id_C, id_D, id_E, id_F};

struct DriverCopySnapshot {
    struct Cell {
        CellInfo *identity;
        const std::unique_ptr<CellInfo> *owner_slot;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        decltype(CellInfo::attrs) attrs;
        decltype(CellInfo::params) params;
        std::map<IdString, PortInfo> ports;
        std::map<IdString, ArchPinInfo> pins;
        ClusterId cluster;
        std::vector<CellInfo *> children;
        int x, y, z;
        bool abs_z;
        bool comb;
        std::vector<const NetInfo *> lut_inputs;
        const NetInfo *output;
        int input_count, used_inputs, bits, shared_inputs, mlab_group;
        bool carry, shared, extended, carry_start, carry_end;
        FFControlSet controls;
        const NetInfo *datain, *sdata;
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
    std::vector<LABInfo> labs;

    explicit DriverCopySnapshot(Context *ctx) : labs(ctx->labs)
    {
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            Cell saved{};
            saved.identity = cell; saved.owner_slot = &entry.second;
            saved.type = cell->type; saved.bel = cell->bel; saved.strength = cell->belStrength;
            saved.attrs = cell->attrs; saved.params = cell->params;
            for (const auto &port : cell->ports) saved.ports.emplace(port.first, port.second);
            for (const auto &pin : cell->pin_data) saved.pins.emplace(pin.first, pin.second);
            saved.cluster = cell->cluster; saved.children = cell->constr_children;
            saved.x = cell->constr_x; saved.y = cell->constr_y; saved.z = cell->constr_z;
            saved.abs_z = cell->constr_abs_z;
            saved.comb = ctx->is_comb_cell(cell->type) || cell->type.in(id_MISTRAL_BUF, id_MISTRAL_MLAB);
            if (saved.comb) {
                const auto &info = cell->combInfo;
                saved.input_count = info.lut_input_count; saved.used_inputs = info.used_lut_input_count;
                saved.bits = info.lut_bits_count; saved.shared_inputs = info.chain_shared_input_count;
                saved.mlab_group = info.mlab_group; saved.output = info.comb_out;
                saved.carry = info.is_carry; saved.shared = info.is_shared; saved.extended = info.is_extended;
                saved.carry_start = info.carry_start; saved.carry_end = info.carry_end;
                for (int i = 0; i < info.lut_input_count; ++i) saved.lut_inputs.push_back(info.lut_in[i]);
            } else if (cell->type == id_MISTRAL_FF) {
                saved.controls = cell->ffInfo.ctrlset;
                saved.datain = cell->ffInfo.datain; saved.sdata = cell->ffInfo.sdata;
            }
            cells.emplace(entry.first, std::move(saved));
            cell_order.push_back(entry.first);
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

    static void expect_port(const PortInfo &actual, const PortInfo &saved)
    {
        EXPECT_EQ(actual.name, saved.name); EXPECT_EQ(actual.net, saved.net);
        EXPECT_EQ(actual.type, saved.type); EXPECT_EQ(actual.user_idx, saved.user_idx);
    }

    static void expect_users(indexed_store<PortRef> actual, indexed_store<PortRef> expected)
    {
        ASSERT_EQ(actual.entries(), expected.entries());
        ASSERT_EQ(actual.capacity(), expected.capacity());
        for (auto user : expected.enumerate()) {
            ASSERT_TRUE(actual.count(user.index));
            EXPECT_EQ(actual.at(user.index).cell, user.value.cell);
            EXPECT_EQ(actual.at(user.index).port, user.value.port);
        }
        // Copies expose both original free holes and the next allocations.
        const size_t probes = size_t(expected.capacity()) + 8;
        for (size_t i = 0; i < probes; ++i) EXPECT_EQ(actual.add(PortRef{}), expected.add(PortRef{}));
    }

    void expect_cell(Context *ctx, IdString name, CellInfo *changed = nullptr, IdString pin = IdString(),
                     bool successor_cache_change = false) const
    {
        ASSERT_TRUE(ctx->cells.count(name));
        const auto &saved = cells.at(name);
        auto *cell = ctx->cells.at(name).get();
        EXPECT_EQ(cell, saved.identity); EXPECT_EQ(cell->type, saved.type);
        EXPECT_EQ(cell->bel, saved.bel); EXPECT_EQ(cell->belStrength, saved.strength);
        EXPECT_EQ(cell->attrs, saved.attrs); EXPECT_EQ(cell->params, saved.params);
        EXPECT_EQ(cell->cluster, saved.cluster); EXPECT_EQ(cell->constr_children, saved.children);
        EXPECT_EQ(cell->constr_x, saved.x); EXPECT_EQ(cell->constr_y, saved.y); EXPECT_EQ(cell->constr_z, saved.z);
        EXPECT_EQ(cell->constr_abs_z, saved.abs_z);
        ASSERT_EQ(cell->ports.size(), saved.ports.size());
        for (const auto &port : saved.ports) {
            ASSERT_TRUE(cell->ports.count(port.first));
            if (cell != changed || port.first != pin) expect_port(cell->ports.at(port.first), port.second);
        }
        ASSERT_EQ(cell->pin_data.size(), saved.pins.size());
        for (const auto &entry : saved.pins) {
            ASSERT_TRUE(cell->pin_data.count(entry.first));
            EXPECT_EQ(cell->pin_data.at(entry.first).state, entry.second.state);
            EXPECT_EQ(cell->pin_data.at(entry.first).bel_pins, entry.second.bel_pins);
        }
        if (saved.comb) {
            const auto &info = cell->combInfo;
            EXPECT_EQ(info.lut_input_count, saved.input_count); EXPECT_EQ(info.used_lut_input_count, saved.used_inputs);
            EXPECT_EQ(info.lut_bits_count, saved.bits);
            if (!successor_cache_change) {
                EXPECT_EQ(info.chain_shared_input_count, saved.shared_inputs);
            }
            EXPECT_EQ(info.mlab_group, saved.mlab_group); EXPECT_EQ(info.comb_out, saved.output);
            EXPECT_EQ(info.is_carry, saved.carry); EXPECT_EQ(info.is_shared, saved.shared);
            EXPECT_EQ(info.is_extended, saved.extended); EXPECT_EQ(info.carry_start, saved.carry_start);
            EXPECT_EQ(info.carry_end, saved.carry_end);
            for (int i = 0; i < saved.input_count; ++i) {
                if (cell != changed || pin != id_A || i != 0) {
                    EXPECT_EQ(info.lut_in[i], saved.lut_inputs[i]);
                }
            }
        } else if (cell->type == id_MISTRAL_FF) {
            EXPECT_EQ(cell->ffInfo.ctrlset, saved.controls);
            EXPECT_EQ(cell->ffInfo.datain, saved.datain); EXPECT_EQ(cell->ffInfo.sdata, saved.sdata);
        }
    }

    void expect_exact(Context *ctx) const
    {
        ASSERT_EQ(ctx->cells.size(), cells.size()); ASSERT_EQ(ctx->nets.size(), nets.size());
        ASSERT_EQ(ctx->net_aliases.size(), aliases.size()); ASSERT_EQ(ctx->ports.size(), ports.size());
        std::vector<IdString> actual_cells, actual_nets, actual_aliases;
        for (const auto &entry : ctx->cells) actual_cells.push_back(entry.first);
        for (const auto &entry : ctx->nets) actual_nets.push_back(entry.first);
        for (const auto &entry : ctx->net_aliases) actual_aliases.push_back(entry.first);
        EXPECT_EQ(actual_cells, cell_order); EXPECT_EQ(actual_nets, net_order); EXPECT_EQ(actual_aliases, alias_order);
        for (const auto &entry : cells) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->cells.count(entry.first));
            EXPECT_EQ(&ctx->cells.at(entry.first), entry.second.owner_slot);
            expect_cell(ctx, entry.first);
        }
        for (const auto &entry : nets) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->nets.count(entry.first));
            auto *net = ctx->nets.at(entry.first).get();
            EXPECT_EQ(net, entry.second.identity); EXPECT_EQ(&ctx->nets.at(entry.first), entry.second.owner_slot);
            EXPECT_EQ(net->driver.cell, entry.second.driver.cell); EXPECT_EQ(net->driver.port, entry.second.driver.port);
            EXPECT_EQ(net->attrs, entry.second.attrs);
            ASSERT_EQ(net->wires.size(), entry.second.wires.size());
            for (const auto &wire : entry.second.wires) {
                ASSERT_TRUE(net->wires.count(wire.first));
                EXPECT_EQ(net->wires.at(wire.first).pip, wire.second.pip);
                EXPECT_EQ(net->wires.at(wire.first).strength, wire.second.strength);
            }
            expect_users(net->users, entry.second.users);
        }
        for (const auto &entry : aliases) EXPECT_EQ(ctx->net_aliases.at(entry.first), entry.second);
        for (const auto &entry : ports) expect_port(ctx->ports.at(entry.first), entry.second);
        ASSERT_EQ(ctx->labs.size(), labs.size());
        for (size_t i = 0; i < labs.size(); ++i) {
            const auto &actual = ctx->labs[i], &saved = labs[i];
            EXPECT_EQ(actual.is_mlab, saved.is_mlab); EXPECT_EQ(actual.aclr_used, saved.aclr_used);
            for (size_t j = 0; j < saved.alms.size(); ++j) {
                const auto &a = actual.alms[j], &b = saved.alms[j];
                EXPECT_EQ(a.l6_mode, b.l6_mode); EXPECT_EQ(a.carry_mode, b.carry_mode);
                EXPECT_EQ(a.clk_ena_idx, b.clk_ena_idx); EXPECT_EQ(a.aclr_idx, b.aclr_idx);
                EXPECT_EQ(a.unique_input_count, b.unique_input_count);
            }
        }
        ctx->check();
    }
};

struct DriverCopyLog {
    std::ostringstream stream;
    DriverCopyLog() { log_streams.emplace_back(&stream, LogLevel::INFO_MSG); }
    ~DriverCopyLog() { log_streams.pop_back(); }
};

// Observe the real post-redirect graph at the existing trial-log boundary.
// This does not repair any cache or alter production candidate decisions.
struct DriverCopyProbeLog : std::streambuf {
    Context *ctx;
    CellInfo *sink, *successor;
    std::ostream stream;
    std::string text, line;
    int timed = 0, same_lab = 0;

    DriverCopyProbeLog(Context *ctx, CellInfo *sink, CellInfo *successor)
        : ctx(ctx), sink(sink), successor(successor), stream(this)
    {
        log_streams.emplace_back(&stream, LogLevel::INFO_MSG);
    }
    ~DriverCopyProbeLog() { log_streams.pop_back(); }

    void append(char value)
    {
        text.push_back(value);
        if (value != '\n') { line.push_back(value); return; }
        if (line.find("LUT driver copy trial source=decoder sink=carry_middle.A ") != std::string::npos) {
            ++timed;
            auto *net = sink->getPort(id_A);
            if (!net || !net->driver.cell || net->driver.cell->bel == BelId()) {
                ADD_FAILURE() << "Trial lacks a bound private source";
            } else {
                auto site = ctx->getBelLocation(net->driver.cell->bel);
                auto target = ctx->getBelLocation(sink->bel);
                if (site.x == target.x && site.y == target.y) ++same_lab;
                int inputs = 0, shared = 0;
                for (auto pin : {id_A, id_B, id_C, id_D0, id_D1}) {
                    if (!successor->getPort(pin)) continue;
                    ++inputs;
                    if (successor->getPort(pin) == sink->getPort(pin)) ++shared;
                }
                EXPECT_EQ(sink->combInfo.lut_in[0], net);
                EXPECT_EQ(successor->combInfo.chain_shared_input_count, shared);
                const auto &bel = ctx->bel_data(successor->bel);
                // The successor occupies its own ALM; its only FF is driven
                // by local SO. Thus only its unshared signal inputs consume
                // fabric inputs: five original inputs minus four shared.
                EXPECT_EQ(ctx->labs.at(bel.lab_data.lab).alms.at(bel.lab_data.alm).unique_input_count,
                          inputs - shared);
                EXPECT_EQ(inputs - shared, 1);
            }
        }
        line.clear();
    }
    std::streamsize xsputn(const char *data, std::streamsize size) override
    {
        for (std::streamsize i = 0; i < size; ++i) append(data[i]);
        return size;
    }
    int_type overflow(int_type value) override
    {
        if (!traits_type::eq_int_type(value, traits_type::eof())) append(traits_type::to_char_type(value));
        return traits_type::not_eof(value);
    }
};

// Compare the real trial graph using both native STA modes. There are no
// injected route delays and the observer never changes placement or caches.
struct DriverCopySkewProbeLog : std::streambuf {
    Context *ctx;
    CellInfo *sink, *endpoint;
    float old_target, old_skew_target, old_skew_endpoint;
    std::ostream stream;
    std::string text, line;
    int trials = 0, unskewed_gains = 0, skewed_regressions = 0;

    DriverCopySkewProbeLog(Context *ctx, CellInfo *sink, CellInfo *endpoint,
                          float old_target, float old_skew_target, float old_skew_endpoint)
        : ctx(ctx), sink(sink), endpoint(endpoint), old_target(old_target), old_skew_target(old_skew_target),
          old_skew_endpoint(old_skew_endpoint), stream(this)
    {
        log_streams.emplace_back(&stream, LogLevel::INFO_MSG);
    }
    ~DriverCopySkewProbeLog() { log_streams.pop_back(); }

    void append(char value)
    {
        text.push_back(value);
        if (value != '\n') { line.push_back(value); return; }
        if (line.find("LUT driver copy trial source=decoder sink=carry_middle.A ") != std::string::npos) {
            ++trials;
            TimingAnalyser unskewed(ctx); unskewed.setup(false, false, true);
            TimingAnalyser skewed(ctx); skewed.with_clock_skew = true; skewed.setup(false, false, true);
            const auto gain = unskewed.get_setup_slack(CellPortKey(sink->name, id_A)) - old_target;
            if (gain >= 250) {
                ++unskewed_gains;
                EXPECT_LT(skewed.get_setup_slack(CellPortKey(sink->name, id_A)) - old_skew_target, 0);
                if (skewed.get_setup_slack(CellPortKey(endpoint->name, id_DATAIN)) < old_skew_endpoint)
                    ++skewed_regressions;
                EXPECT_NE(line.find("improve=0"), std::string::npos);
            }
        }
        line.clear();
    }
    std::streamsize xsputn(const char *data, std::streamsize size) override
    {
        for (std::streamsize i = 0; i < size; ++i) append(data[i]);
        return size;
    }
    int_type overflow(int_type value) override
    {
        if (!traits_type::eq_int_type(value, traits_type::eof())) append(traits_type::to_char_type(value));
        return traits_type::not_eof(value);
    }
};

std::map<std::string, int> copy_hold_slacks(TimingAnalyser &timing)
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

class LutDriverCopyTest : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clock;
    std::array<CellInfo *, 6> inputs;
    CellInfo *source, *head, *sink, *successor, *endpoint;
    std::vector<CellInfo *> side_ffs;

    CellInfo *ff(const std::string &name, NetInfo *ena = nullptr, NetInfo *data = nullptr)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->addOutput(id_Q); cell->connectPort(id_CLK, clock);
        if (ena) cell->connectPort(id_ENA, ena);
        else cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1; cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        auto *q = ctx->createNet(ctx->id(name + "$q"));
        cell->connectPort(id_Q, q); cell->connectPort(id_DATAIN, data ? data : q);
        return cell;
    }

    CellInfo *arith(const char *name)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_ALUT_ARITH);
        for (auto pin : {id_A, id_B, id_C, id_D0, id_D1, id_CI}) cell->addInput(pin);
        for (auto pin : {id_SO, id_CO}) {
            cell->addOutput(pin); cell->connectPort(pin, ctx->createNet(ctx->idf("%s$%s", name, pin.str(ctx.get()).c_str())));
        }
        cell->params[id_LUT0] = Property(0x6666, 16); cell->params[id_LUT1] = Property(0x6666, 16);
        cell->connectPort(id_A, source->getPort(id_Q));
        cell->connectPort(id_B, inputs[0]->getPort(id_Q));
        cell->connectPort(id_C, inputs[1]->getPort(id_Q));
        cell->connectPort(id_D0, inputs[2]->getPort(id_Q));
        cell->connectPort(id_D1, inputs[0]->getPort(id_Q));
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
        FAIL() << "No legal driver-copy fixture BEL for " << cell->name.str(ctx.get()) << " at " << x << ',' << y << ',' << z;
    }

    void hole(NetInfo *net, const std::string &name)
    {
        std::array<CellInfo *, 2> temporary;
        for (int i = 0; i < 2; ++i) {
            temporary[i] = ctx->createCell(ctx->id(name + std::to_string(i)), id_MISTRAL_ALUT2);
            temporary[i]->addInput(id_A); temporary[i]->connectPort(id_A, net);
        }
        for (auto *cell : temporary) {
            cell->disconnectPort(id_A); ctx->cells.erase(cell->name);
        }
    }

    CellInfo *clock_source(NetInfo *net, const std::string &name, bool require_left = false)
    {
        auto *driver = ctx->createCell(ctx->id(name), id_MISTRAL_CLKBUF);
        driver->addOutput(id_Q); driver->connectPort(id_Q, net);
        ctx->assign_default_pinmap(driver);
        for (auto bel : ctx->getBels()) {
            const auto loc = ctx->getBelLocation(bel);
            if (require_left && (loc.x != 0 || loc.y != 35)) continue;
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(driver->type, bel)) continue;
            ctx->bindBel(bel, driver, STRENGTH_LOCKED);
            if (ctx->isBelLocationValid(bel)) return driver;
            ctx->unbindBel(bel);
        }
        ADD_FAILURE() << "No legal native driver-copy clock source";
        return nullptr;
    }

    CellInfo *cdc_capture(NetInfo *&independent_clock)
    {
        independent_clock = ctx->createNet(ctx->id("independent_capture_clock"));
        independent_clock->is_global = true;
        independent_clock->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
        EXPECT_NE(clock_source(independent_clock, "independent_capture_clock_source"), nullptr);
        auto *cell = ff("independent_capture", nullptr, source->getPort(id_Q));
        cell->disconnectPort(id_CLK); cell->connectPort(id_CLK, independent_clock);
        ctx->assign_ff_info(cell); ctx->assign_default_pinmap(cell);
        place(cell, 25, 22, STRENGTH_LOCKED);
        return cell;
    }

    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e9;
        clock = ctx->createNet(ctx->id("clock")); clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>();
        clock->clkconstr->period = DelayPair(1000);
        clock->clkconstr->high = clock->clkconstr->low = DelayPair(500);
        ctx->createNet(ctx->id("$PACKER_GND_NET")); ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        for (int i = 0; i < 6; ++i) inputs[i] = ff("literal_" + std::to_string(i));
        source = ctx->createCell(ctx->id("decoder"), id_MISTRAL_ALUT3);
        source->params[id_LUT] = Property(0x20, 8);
        for (int i = 0; i < 3; ++i) {
            source->addInput(copy_pins[i]); source->connectPort(copy_pins[i], inputs[i]->getPort(id_Q));
        }
        source->pin_data[id_A].state = PIN_INV;
        source->addOutput(id_Q); source->connectPort(id_Q, ctx->createNet(ctx->id("decoder$q")));
        ctx->net_aliases[ctx->id("decoder_alias")] = source->getPort(id_Q)->name;
        head = arith("carry_head"); sink = arith("carry_middle"); successor = arith("carry_successor");
        head->pin_data[id_CI].state = PIN_0;
        sink->connectPort(id_CI, head->getPort(id_CO)); successor->connectPort(id_CI, sink->getPort(id_CO));
        for (auto *cell : {head, sink, successor}) {
            cell->cluster = head->name; cell->constr_abs_z = true;
        }
        head->constr_z = 0; sink->constr_z = 1; successor->constr_z = 6;
        head->constr_children = {sink, successor};
        endpoint = ff("captured_carry", nullptr, successor->getPort(id_SO));
        // Forty-six ordinary users plus the three arithmetic A users. The
        // selected middle.A is the only edge a successful copy may remove.
        for (int i = 0; i < 46; ++i)
            side_ffs.push_back(ff("side_" + std::to_string(i), i & 1 ? source->getPort(id_Q) : nullptr,
                                   i & 1 ? nullptr : source->getPort(id_Q)));
        ctx->assignArchInfo();
        for (auto *cell : inputs) place(cell, 30, 19, STRENGTH_LOCKED);
        place(source, 24, 20);
        place(head, 30, 20, STRENGTH_LOCKED, 0);
        place(sink, 30, 20, STRENGTH_LOCKED, 1);
        place(successor, 30, 20, STRENGTH_LOCKED, 6);
        place(endpoint, 30, 20, STRENGTH_LOCKED, 8);
        for (size_t i = 0; i < side_ffs.size(); ++i)
            place(side_ffs[i], i < 16 ? 24 : i < 32 ? 23 : 25, i < 32 ? 21 : 20, STRENGTH_LOCKED);
        for (int i = 0; i < 6; ++i) hole(inputs[i]->getPort(id_Q), "input_hole_" + std::to_string(i) + "_");
        hole(source->getPort(id_Q), "output_hole_");
        // Existing custom physical mappings must survive both probing and
        // acceptance; a global assignArchInfo would silently reset these.
        std::swap(source->pin_data[id_A].bel_pins, source->pin_data[id_B].bel_pins);
        ctx->check();
    }

    json11::Json endpoint_json(CellInfo *cell, IdString pin) const
    {
        auto loc = ctx->getBelLocation(cell->bel);
        return json11::Json::object{{"cell", cell->name.str(ctx.get())}, {"port", pin.str(ctx.get())},
                                   {"loc", json11::Json::array{loc.x, loc.y}}};
    }

    std::string report(IdString target = id_A) const
    {
        json11::Json::array path;
        auto segment = [&](const char *type, CellInfo *from, IdString fp, CellInfo *to, IdString tp) {
            json11::Json::object row{{"type", type}, {"delay", std::string(type) == "routing" ? 2.0 : 0.4},
                                    {"from", endpoint_json(from, fp)}, {"to", endpoint_json(to, tp)}};
            if (std::string(type) == "routing") row["net"] = from->getPort(fp)->name.str(ctx.get());
            path.emplace_back(row);
        };
        segment("routing", source, id_Q, sink, target);
        segment("logic", sink, target, sink, id_CO);
        segment("routing", sink, id_CO, successor, id_CI);
        segment("logic", successor, id_CI, successor, id_SO);
        segment("routing", successor, id_SO, endpoint, id_DATAIN);
        segment("setup", endpoint, id_DATAIN, endpoint, id_DATAIN);
        return json11::Json(json11::Json::object{{"critical_paths", json11::Json::array{
            json11::Json::object{{"from", "posedge clock"}, {"to", "posedge clock"},
                                 {"max_delay", 1}, {"path", path}}}}}).dump();
    }

    bool value(CellInfo *cell, unsigned row) const
    {
        unsigned lut_row = 0;
        for (unsigned pin = 0; pin < 6; ++pin) {
            if (!cell->ports.count(copy_pins[pin])) continue;
            auto state = cell->get_pin_state(copy_pins[pin]);
            bool bit = state == PIN_1;
            if (state == PIN_SIG || state == PIN_INV) {
                auto *net = cell->getPort(copy_pins[pin]);
                bool found = false;
                for (unsigned literal = 0; literal < inputs.size(); ++literal)
                    if (net == inputs[literal]->getPort(id_Q)) { bit = (row >> literal) & 1; found = true; break; }
                EXPECT_TRUE(found) << "Unexpected clone boundary";
                if (state == PIN_INV) bit = !bit;
            }
            lut_row |= unsigned(bit) << pin;
        }
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> lut_row) & 1;
    }

    void reject(const std::string &text, const DriverCopySnapshot &saved, int selection = 0)
    {
        try { EXPECT_FALSE(ctx->remap_lut_driver_critical(text, selection)); }
        catch (const log_execution_error_exception &) { }
        saved.expect_exact(ctx.get());
    }

    CellInfo *expect_accepted(const DriverCopySnapshot &saved, const std::vector<bool> &expected,
                             bool protected_sink_lab = true)
    {
        DriverCopyLog log;
        EXPECT_TRUE(ctx->remap_lut_driver_critical(report(), 0)) << log.stream.str();
        if (ctx->cells.size() != saved.cells.size() + 1 || ctx->nets.size() != saved.nets.size() + 1) {
            ADD_FAILURE() << "Expected exactly one private clone and net"; return nullptr;
        }
        auto *replacement = sink->getPort(id_A);
        if (!replacement || replacement == source->getPort(id_Q) || !replacement->driver.cell) {
            ADD_FAILURE() << "Selected edge was not redirected"; return nullptr;
        }
        auto *clone = replacement->driver.cell;
        EXPECT_EQ(replacement->driver.port, id_Q); EXPECT_EQ(clone->type, source->type);
        EXPECT_EQ(clone->params.at(id_LUT), source->params.at(id_LUT));
        for (auto pin : copy_pins) if (source->ports.count(pin)) {
            EXPECT_EQ(clone->getPort(pin), source->getPort(pin));
            EXPECT_EQ(clone->get_pin_state(pin), source->get_pin_state(pin));
        }
        EXPECT_EQ(replacement->users.entries(), 1);
        auto users = replacement->users;
        for (auto user : users.enumerate()) {
            EXPECT_EQ(user.value.cell, sink); EXPECT_EQ(user.value.port, id_A);
            EXPECT_EQ(user.index, sink->ports.at(id_A).user_idx);
        }
        for (unsigned row = 0; row < expected.size(); ++row) EXPECT_EQ(value(clone, row), expected[row]) << row;
        std::vector<IdString> cell_order, net_order, alias_order;
        for (const auto &entry : ctx->cells) cell_order.push_back(entry.first);
        for (const auto &entry : ctx->nets) net_order.push_back(entry.first);
        for (const auto &entry : ctx->net_aliases) if (saved.aliases.count(entry.first)) {
            alias_order.push_back(entry.first); EXPECT_EQ(entry.second, saved.aliases.at(entry.first));
        }
        EXPECT_EQ(std::vector<IdString>(cell_order.begin(), cell_order.begin() + saved.cells.size()), saved.cell_order);
        EXPECT_EQ(std::vector<IdString>(net_order.begin(), net_order.begin() + saved.nets.size()), saved.net_order);
        EXPECT_EQ(alias_order, saved.alias_order);
        for (const auto &entry : saved.cells) {
            SCOPED_TRACE(entry.first.str(ctx.get()));
            // The successor cache legitimately changes with the declared data
            // edge, but its ports, pins, identity and placement remain exact.
            saved.expect_cell(ctx.get(), entry.first, sink, id_A, entry.second.identity == successor);
        }
        for (const auto &entry : saved.nets) {
            auto *net = ctx->nets.at(entry.first).get();
            EXPECT_EQ(net, entry.second.identity); EXPECT_EQ(net->driver.cell, entry.second.driver.cell);
            EXPECT_EQ(net->driver.port, entry.second.driver.port); EXPECT_EQ(net->attrs, entry.second.attrs);
            auto old_users = entry.second.users;
            size_t removed = 0, added = 0;
            for (auto user : old_users.enumerate()) {
                if (user.value.cell == sink && user.value.port == id_A) {
                    ++removed; EXPECT_FALSE(net->users.count(user.index)); continue;
                }
                if (!net->users.count(user.index)) {
                    ADD_FAILURE() << "Original user slot disappeared";
                    return nullptr;
                }
                EXPECT_EQ(net->users.at(user.index).cell, user.value.cell);
                EXPECT_EQ(net->users.at(user.index).port, user.value.port);
            }
            for (const auto &port : clone->ports)
                if (port.second.type == PORT_IN && port.second.net == net) ++added;
            EXPECT_EQ(size_t(net->users.entries()), size_t(old_users.entries()) - removed + added);
            EXPECT_TRUE(net->wires.empty());
        }
        auto location = ctx->getBelLocation(clone->bel);
        EXPECT_LE(std::abs(location.x - 30) + std::abs(location.y - 20), 3);
        if (protected_sink_lab) {
            EXPECT_FALSE(location.x == 30 && location.y == 20);
        }
        for (const auto &entry : saved.cells) {
            auto old = ctx->getBelLocation(entry.second.bel);
            EXPECT_FALSE(location.x == old.x && location.y == old.y && location.z / 6 == old.z / 6);
        }
        EXPECT_EQ(head->getPort(id_CO), sink->getPort(id_CI));
        EXPECT_EQ(sink->getPort(id_CO), successor->getPort(id_CI));
        EXPECT_EQ(sink->combInfo.lut_in[0], replacement);
        int shared = 0;
        for (auto pin : {id_A, id_B, id_C, id_D0, id_D1})
            if (successor->getPort(pin) && successor->getPort(pin) == sink->getPort(pin)) ++shared;
        EXPECT_EQ(successor->combInfo.chain_shared_input_count, shared);
        EXPECT_EQ(shared, saved.cells.at(successor->name).shared_inputs - 1);
        for (const auto &entry : ctx->cells) EXPECT_TRUE(ctx->isBelLocationValid(entry.second->bel));
        ctx->check();
        return clone;
    }
};

TEST_F(LutDriverCopyTest, QualifiedCopyPreservesEveryOriginalAndEightTruthRows)
{
    ASSERT_EQ(source->getPort(id_Q)->users.entries(), 49);
    ASSERT_EQ(successor->combInfo.chain_shared_input_count, 5);
    std::vector<bool> expected;
    for (unsigned row = 0; row < 8; ++row) {
        bool a = row & 1, b = row & 2, c = row & 4;
        expected.push_back(!a && !b && c); // independent mask0x20/PIN_INV-A oracle
        ASSERT_EQ(value(source, row), expected.back());
    }
    TimingAnalyser before(ctx.get()); before.setup(false, false, true);
    auto old_target = before.get_setup_slack(CellPortKey(sink->name, id_A));
    auto old_holds = copy_hold_slacks(before);
    std::map<CellPortKey, float> endpoints;
    endpoints.emplace(CellPortKey(endpoint->name, id_DATAIN), before.get_setup_slack(CellPortKey(endpoint->name, id_DATAIN)));
    for (auto *cell : side_ffs) {
        auto pin = cell->getPort(id_ENA) ? id_ENA : id_DATAIN;
        endpoints.emplace(CellPortKey(cell->name, pin), before.get_setup_slack(CellPortKey(cell->name, pin)));
    }
    DriverCopySnapshot saved(ctx.get());
    ASSERT_NE(expect_accepted(saved, expected), nullptr);
    TimingAnalyser after(ctx.get()); after.setup(false, false, true);
    EXPECT_GE(after.get_setup_slack(CellPortKey(sink->name, id_A)), old_target + 250);
    for (const auto &entry : endpoints) EXPECT_GE(after.get_setup_slack(entry.first), entry.second);
    for (const auto &entry : before.get_timing_result().clock_fmax) {
        ASSERT_TRUE(after.get_timing_result().clock_fmax.count(entry.first));
        EXPECT_GE(after.get_timing_result().clock_fmax.at(entry.first).achieved + 1e-4, entry.second.achieved);
    }
    for (const auto &entry : copy_hold_slacks(after)) {
        ASSERT_TRUE(old_holds.count(entry.first)); EXPECT_GE(entry.second, old_holds.at(entry.first));
    }
}

TEST_F(LutDriverCopyTest, ListingAndUnavailableSelectionRestoreOwnerStorageHolesPinsAndCaches)
{
    DriverCopySnapshot saved(ctx.get());
    DriverCopyLog log;
    EXPECT_FALSE(ctx->remap_lut_driver_critical(report(), -1));
    EXPECT_NE(log.stream.str().find("LUT driver copy trial source=decoder sink=carry_middle.A "), std::string::npos);
    EXPECT_NE(log.stream.str().find("LUT driver copy candidate 0:"), std::string::npos);
    saved.expect_exact(ctx.get());
    EXPECT_FALSE(ctx->remap_lut_driver_critical(report(), 9999));
    saved.expect_exact(ctx.get());
}

TEST_F(LutDriverCopyTest, SameLabProbesRetainPostRedirectCarryCachesAndRollbackExactly)
{
    // Keep the actual contiguous carry BELs and relative z values, but allow
    // an empty ALM in this LAB to be considered as a clone site.
    for (auto *cell : {head, sink, successor, endpoint}) {
        auto bel = cell->bel;
        ctx->unbindBel(bel);
        cell->cluster = ClusterId(); cell->constr_children.clear();
        ctx->bindBel(bel, cell, STRENGTH_WEAK);
        ASSERT_TRUE(ctx->isBelLocationValid(bel));
    }
    ASSERT_EQ(successor->constr_z, 6);
    ASSERT_EQ(successor->combInfo.chain_shared_input_count, 5);
    ASSERT_EQ(endpoint->getPort(id_DATAIN), successor->getPort(id_SO));
    const auto &successor_bel = ctx->bel_data(successor->bel);
    ASSERT_EQ(ctx->labs.at(successor_bel.lab_data.lab).alms.at(successor_bel.lab_data.alm).unique_input_count, 0);
    DriverCopySnapshot saved(ctx.get());
    std::string first_site;
    {
        DriverCopyProbeLog probes(ctx.get(), sink, successor);
        EXPECT_FALSE(ctx->remap_lut_driver_critical(report(), -1));
        saved.expect_exact(ctx.get());
        EXPECT_GT(probes.same_lab, 0) << probes.text;
        EXPECT_GT(probes.timed, 1) << probes.text;
        const std::string marker = "LUT driver copy candidate 0: source=decoder sink=carry_middle.A bel=";
        auto position = probes.text.find(marker);
        ASSERT_NE(position, std::string::npos) << probes.text;
        position += marker.size();
        auto end = probes.text.find(" gain=", position);
        ASSERT_NE(end, std::string::npos);
        first_site = probes.text.substr(position, end - position);
        EXPECT_FALSE(ctx->remap_lut_driver_critical(report(), 9999));
        saved.expect_exact(ctx.get());
    }
    std::vector<bool> expected;
    for (unsigned row = 0; row < 8; ++row)
        expected.push_back(!(row & 1) && !(row & 2) && bool(row & 4));
    auto *clone = expect_accepted(saved, expected, false);
    ASSERT_NE(clone, nullptr);
    EXPECT_EQ(ctx->getBelName(clone->bel).str(ctx.get()), first_site);
    EXPECT_EQ(ctx->labs.at(successor_bel.lab_data.lab).alms.at(successor_bel.lab_data.alm).unique_input_count, 1);
}

TEST_F(LutDriverCopyTest, SixInputConstantsInversionsAndAliasedSignalsRetainAllSixtyFourRows)
{
    ctx->unbindBel(source->bel);
    source->type = id_MISTRAL_ALUT6;
    for (int i = 3; i < 6; ++i) source->addInput(copy_pins[i]);
    source->disconnectPort(id_B); source->pin_data[id_B].state = PIN_1;
    source->pin_data[id_C].state = PIN_INV;
    source->connectPort(id_D, inputs[3]->getPort(id_Q));
    source->pin_data[id_E].state = PIN_0;
    ctx->net_aliases[ctx->id("literal_alias")] = inputs[3]->getPort(id_Q)->name;
    source->connectPort(id_F, ctx->getNetByAlias(ctx->id("literal_alias")));
    const uint64_t table = UINT64_C(0x96abcdf047e12d38);
    source->params[id_LUT] = Property(int64_t(table), 64);
    ctx->assign_comb_info(source); ctx->assign_default_pinmap(source);
    place(source, 24, 20);
    std::vector<bool> expected;
    for (unsigned row = 0; row < 64; ++row) {
        unsigned index = unsigned(!(row & 1)) | 2u | (unsigned(!(row & 4)) << 2) |
                         (((row >> 3) & 1) << 3) | (((row >> 3) & 1) << 5);
        expected.push_back((table >> index) & 1);
        ASSERT_EQ(value(source, row), expected.back());
    }
    DriverCopySnapshot saved(ctx.get());
    ASSERT_NE(expect_accepted(saved, expected), nullptr);
}

TEST_F(LutDriverCopyTest, StaleDisconnectedAndMalformedReportsRejectBeforeMutation)
{
    DriverCopySnapshot saved(ctx.get());
    std::string error;
    auto original = json11::Json::parse(report(), error).object_items(); ASSERT_TRUE(error.empty());
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto document = original;
        auto paths = document["critical_paths"].array_items();
        auto path = paths[0].object_items(); auto segments = path["path"].array_items();
        if (mutation == 0) {
            auto edge = segments[0].object_items(); auto to = edge["to"].object_items();
            auto loc = to["loc"].array_items(); loc[0] = loc[0].int_value() + 1;
            to["loc"] = loc; edge["to"] = to; segments[0] = edge;
        } else if (mutation == 1) {
            auto edge = segments[0].object_items(); edge["net"] = inputs[0]->getPort(id_Q)->name.str(ctx.get()); segments[0] = edge;
        } else if (mutation == 2) segments.erase(segments.begin() + 1);
        else { auto segment = segments[0].object_items(); segment["delay"] = "unknown"; segments[0] = segment; }
        path["path"] = segments; paths[0] = path; document["critical_paths"] = paths;
        EXPECT_THROW(ctx->remap_lut_driver_critical(json11::Json(document).dump(), 0), log_execution_error_exception);
        saved.expect_exact(ctx.get());
    }
    EXPECT_THROW(ctx->remap_lut_driver_critical(report(), -2), log_execution_error_exception);
    saved.expect_exact(ctx.get());
}

TEST_F(LutDriverCopyTest, ProtectedMalformedSourceAndCarryPortCannotBecomeCandidates)
{
    source->attrs[ctx->id("dont_touch")] = 1;
    { DriverCopySnapshot saved(ctx.get()); reject(report(), saved); }
    source->attrs.erase(ctx->id("dont_touch"));
    auto original_table = source->params.at(id_LUT);
    source->params[id_LUT] = Property(0x20, 16); // wrong defined width
    { DriverCopySnapshot saved(ctx.get()); reject(report(), saved); }
    source->params[id_LUT] = original_table;
    source->pin_data[id_Q].state = PIN_INV;
    { DriverCopySnapshot saved(ctx.get()); reject(report(), saved); }
    source->pin_data[id_Q].state = PIN_SIG;
    sink->disconnectPort(id_CI); sink->connectPort(id_CI, source->getPort(id_Q));
    ctx->assign_comb_info(sink); ctx->update_bel(sink->bel);
    { DriverCopySnapshot saved(ctx.get()); reject(report(id_CI), saved); }
}

TEST_F(LutDriverCopyTest, HardBoundaryUnknownClockAndFeedbackCannotBeIgnored)
{
    auto *hard = ctx->createCell(ctx->id("unsupported_hard_boundary"), id_MISTRAL_M10K);
    hard->addInput(ctx->id("UNKNOWN")); hard->connectPort(ctx->id("UNKNOWN"), source->getPort(id_Q));
    { DriverCopySnapshot saved(ctx.get()); reject(report(), saved); }
    // A new load also touches all existing descendants of the copied LUT's
    // signal inputs, even when the hard branch is absent from the guide.
    hard->disconnectPort(ctx->id("UNKNOWN"));
    hard->connectPort(ctx->id("UNKNOWN"), inputs[0]->getPort(id_Q));
    { DriverCopySnapshot saved(ctx.get()); reject(report(), saved); }
    hard->disconnectPort(ctx->id("UNKNOWN")); ctx->cells.erase(hard->name);
    auto constraint = std::move(clock->clkconstr);
    { DriverCopySnapshot saved(ctx.get()); reject(report(), saved); }
    clock->clkconstr = std::move(constraint);
    source->disconnectPort(id_C); source->connectPort(id_C, source->getPort(id_Q));
    ctx->assign_comb_info(source); ctx->update_bel(source->bel);
    { DriverCopySnapshot saved(ctx.get()); reject(report(), saved); }
}

TEST_F(LutDriverCopyTest, KnownUnrelatedCaptureIsGuardedWithoutBlockingAnOtherwiseQualifiedCopy)
{
    ASSERT_NE(clock_source(clock, "primary_clock_source", true), nullptr);
    NetInfo *independent_clock = nullptr;
    auto *capture = cdc_capture(independent_clock);
    ASSERT_NE(capture->bel, BelId());
    TimingAnalyser before(ctx.get()); before.setup(false, false, true);
    EXPECT_EQ(before.get_setup_slack(CellPortKey(capture->name, id_DATAIN)),
              float(std::numeric_limits<delay_t>::max()));
    std::vector<EndpointClockPairTiming> old_pairs;
    ASSERT_TRUE(before.get_endpoint_clock_pair_timings(CellPortKey(capture->name, id_DATAIN), old_pairs));
    ASSERT_EQ(old_pairs.size(), 1u); EXPECT_FALSE(old_pairs.front().setup_timed);
    EXPECT_FALSE(old_pairs.front().setup_window.has_value());
    DriverCopySnapshot saved(ctx.get());
    {
        DriverCopyLog listing;
        EXPECT_FALSE(ctx->remap_lut_driver_critical(report(), -1));
        EXPECT_NE(listing.stream.str().find("LUT driver copy candidate 0:"), std::string::npos)
            << listing.stream.str();
        EXPECT_NE(listing.stream.str().find("unrelated_pairs="), std::string::npos);
        saved.expect_exact(ctx.get());
    }
    std::vector<bool> expected;
    for (unsigned row = 0; row < 8; ++row) expected.push_back(!(row & 1) && !(row & 2) && bool(row & 4));
    ASSERT_NE(expect_accepted(saved, expected), nullptr);
    TimingAnalyser after(ctx.get()); after.setup(false, false, true);
    std::vector<EndpointClockPairTiming> new_pairs;
    ASSERT_TRUE(after.get_endpoint_clock_pair_timings(CellPortKey(capture->name, id_DATAIN), new_pairs));
    ASSERT_EQ(new_pairs.size(), old_pairs.size());
    EXPECT_EQ(new_pairs.front().launch, old_pairs.front().launch);
    EXPECT_EQ(new_pairs.front().capture, old_pairs.front().capture);
    EXPECT_EQ(new_pairs.front().max_path_delay, old_pairs.front().max_path_delay);
    EXPECT_EQ(new_pairs.front().min_path_delay, old_pairs.front().min_path_delay);
    EXPECT_FALSE(new_pairs.front().setup_window.has_value());
    EXPECT_EQ(capture->getPort(id_DATAIN), source->getPort(id_Q));
}

TEST_F(LutDriverCopyTest, UnknownUnrelatedCaptureClockStillRejectsWithoutMutation)
{
    ASSERT_NE(clock_source(clock, "primary_clock_source", true), nullptr);
    NetInfo *independent_clock = nullptr;
    cdc_capture(independent_clock);
    independent_clock->clkconstr.reset();
    DriverCopySnapshot saved(ctx.get());
    DriverCopyLog listing;
    reject(report(), saved, -1);
    EXPECT_EQ(listing.stream.str().find("LUT driver copy candidate "), std::string::npos);
    EXPECT_NE(listing.stream.str().find("clock-constraint-missing"), std::string::npos);
}

TEST_F(LutDriverCopyTest, NativeClockSkewRejectsAnUnskewedGainThatWorsensRegisteredSetup)
{
    ASSERT_NE(clock_source(clock, "primary_clock_source", true), nullptr);
    for (auto *cell : {inputs[0], inputs[1]}) ctx->unbindBel(cell->bel);
    place(inputs[0], 30, 26, STRENGTH_LOCKED);
    place(inputs[1], 24, 17, STRENGTH_LOCKED);
    // Retain the carry links, but remove the head decoder branch which would
    // otherwise hide middle.A behind an unchanged, slightly longer prefix.
    head->disconnectPort(id_A); head->pin_data[id_A].state = PIN_0;
    ctx->assign_comb_info(head); ctx->update_bel(head->bel);
    // Leave only the x31,y20 LAB available. Column x32 is a DSP column on
    // this device. Locked ordinary FFs make every
    // other candidate LAB protected, without injecting timing or bypassing
    // any production placement/protection guard.
    for (int x = 27; x <= 33; ++x) for (int y = 17; y <= 23; ++y) {
        if (std::abs(x - 30) + std::abs(y - 20) > 3 || (x == 31 && y == 20)) continue;
        bool protected_lab = false, has_ff = false;
        for (auto bel : ctx->getBelsByTile(x, y)) {
            has_ff |= ctx->getBelType(bel) == id_MISTRAL_FF;
            auto *cell = ctx->getBoundBelCell(bel);
            protected_lab |= cell && (cell->belStrength > STRENGTH_WEAK || cell->cluster != ClusterId());
        }
        if (protected_lab || !has_ff) continue;
        auto *blocker = ff("skew_block_" + std::to_string(x) + "_" + std::to_string(y));
        ctx->assign_ff_info(blocker); ctx->assign_default_pinmap(blocker);
        place(blocker, x, y, STRENGTH_LOCKED);
    }
    bool candidate_bel_available = false;
    for (auto bel : ctx->getBelsByTile(31, 20))
        candidate_bel_available |= ctx->checkBelAvail(bel) && ctx->isValidBelForCellType(source->type, bel);
    ASSERT_TRUE(candidate_bel_available) << "Skew fixture requires an actual available LUT BEL";
    for (const auto &entry : ctx->cells)
        if (entry.second->bel != BelId())
            ASSERT_TRUE(ctx->isBelLocationValid(entry.second->bel)) << entry.first.str(ctx.get());
    ctx->check();
    TimingAnalyser before(ctx.get()); before.setup(false, false, true);
    TimingAnalyser skewed(ctx.get()); skewed.with_clock_skew = true; skewed.setup(false, false, true);
    DriverCopySnapshot saved(ctx.get());
    DriverCopySkewProbeLog probes(ctx.get(), sink, endpoint,
        before.get_setup_slack(CellPortKey(sink->name, id_A)),
        skewed.get_setup_slack(CellPortKey(sink->name, id_A)),
        skewed.get_setup_slack(CellPortKey(endpoint->name, id_DATAIN)));
    EXPECT_FALSE(ctx->remap_lut_driver_critical(report(), -1));
    EXPECT_GT(probes.trials, 0) << probes.text;
    EXPECT_GT(probes.unskewed_gains, 0) << probes.text;
    EXPECT_EQ(probes.skewed_regressions, probes.unskewed_gains);
    EXPECT_EQ(probes.text.find("LUT driver copy candidate "), std::string::npos) << probes.text;
    saved.expect_exact(ctx.get());
}
