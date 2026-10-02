/* Real native hard-input transactions and complete branch guards. SPDX-License-Identifier: ISC */
#include "gtest/gtest.h"
#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"
#include "remap_clock_guard.h"
#include "timing.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <tuple>
#include <vector>

USING_NEXTPNR_NAMESPACE
NEXTPNR_NAMESPACE_BEGIN
int hard_input_locality(Context *, const std::string &, int, int);
void preload_hard_input_locality(Context *, const char *);
NEXTPNR_NAMESPACE_END

namespace {
namespace guard = mistral_remap_clock_guard;

// Independent state capture: include raw user allocation order and every
// LAB cache, so a rejected/exceptional trial cannot silently leave damage.
struct HardSnapshot {
    struct Cell {
        CellInfo *owner;
        IdString name, type, hierpath;
        BelId bel;
        PlaceStrength strength;
        ClusterId cluster;
        Region *region;
        PseudoCell *pseudo;
        decltype(CellInfo::params) params;
        decltype(CellInfo::attrs) attrs;
        decltype(CellInfo::ports) ports;
        std::vector<IdString> order;
        ArchCellInfo info;
    };
    struct Net {
        NetInfo *owner;
        IdString name, hierpath, constant;
        PortRef driver;
        indexed_store<PortRef> users;
        decltype(NetInfo::attrs) attrs;
        decltype(NetInfo::wires) wires;
        Region *region;
        bool global;
        std::vector<IdString> aliases;
        const ClockConstraint *clock;
    };
    std::vector<Cell> cells;
    std::vector<Net> nets;
    decltype(Context::ports) ports;
    decltype(Context::net_aliases) aliases;
    decltype(Context::settings) settings;
    decltype(Context::attrs) attrs;
    std::vector<LABInfo> labs;
    explicit HardSnapshot(Context *ctx)
        : ports(ctx->ports), aliases(ctx->net_aliases), settings(ctx->settings), attrs(ctx->attrs), labs(ctx->labs)
    {
        for (const auto &entry : ctx->cells) {
            auto *c = entry.second.get(); std::vector<IdString> order;
            for (const auto &port : c->ports) order.push_back(port.first);
            cells.push_back({c, c->name, c->type, c->hierpath, c->bel, c->belStrength, c->cluster, c->region,
                             c->pseudo_cell.get(), c->params, c->attrs, c->ports, std::move(order),
                             static_cast<const ArchCellInfo &>(*c)});
        }
        for (const auto &entry : ctx->nets) {
            auto *n = entry.second.get();
            nets.push_back({n, n->name, n->hierpath, n->constant_value, n->driver, n->users, n->attrs, n->wires,
                            n->region, n->is_global, n->aliases, n->clkconstr.get()});
        }
    }
    static void port(const PortInfo &a, const PortInfo &b)
    { EXPECT_EQ(a.name, b.name); EXPECT_EQ(a.type, b.type); EXPECT_EQ(a.net, b.net); EXPECT_EQ(a.user_idx, b.user_idx); }
    static void lab(const LABInfo &a, const LABInfo &b, const std::set<uint8_t> &changed = {})
    {
        EXPECT_EQ(a.is_mlab, b.is_mlab); EXPECT_EQ(a.aclr_used, b.aclr_used);
        EXPECT_EQ(a.clk_wires, b.clk_wires); EXPECT_EQ(a.ena_wires, b.ena_wires); EXPECT_EQ(a.aclr_wires, b.aclr_wires);
        EXPECT_EQ(a.sclr_wire, b.sclr_wire); EXPECT_EQ(a.sload_wire, b.sload_wire);
        ASSERT_EQ(a.alms.size(), b.alms.size());
        for (size_t i = 0; i < a.alms.size(); ++i) {
            const auto &x = a.alms[i], &y = b.alms[i];
            EXPECT_EQ(x.comb_out, y.comb_out); EXPECT_EQ(x.sel_clk, y.sel_clk); EXPECT_EQ(x.sel_ena, y.sel_ena);
            EXPECT_EQ(x.sel_aclr, y.sel_aclr); EXPECT_EQ(x.sel_ef, y.sel_ef); EXPECT_EQ(x.ff_in, y.ff_in);
            EXPECT_EQ(x.ff_out, y.ff_out); EXPECT_EQ(x.lut_bels, y.lut_bels); EXPECT_EQ(x.ff_bels, y.ff_bels);
            EXPECT_EQ(x.carry_mode, y.carry_mode); EXPECT_EQ(x.clk_ena_idx, y.clk_ena_idx);
            EXPECT_EQ(x.aclr_idx, y.aclr_idx); EXPECT_EQ(x.l6_mode, y.l6_mode);
            if (!changed.count(uint8_t(i))) EXPECT_EQ(x.unique_input_count, y.unique_input_count);
        }
    }
    void expect(Context *ctx, const std::set<IdString> &moved = {}) const
    {
        ASSERT_EQ(cells.size(), ctx->cells.size()); ASSERT_EQ(nets.size(), ctx->nets.size());
        EXPECT_EQ(ctx->settings, settings); EXPECT_EQ(ctx->attrs, attrs); EXPECT_EQ(ctx->net_aliases, aliases);
        ASSERT_EQ(ctx->ports.size(), ports.size());
        for (const auto &entry : ports) { ASSERT_TRUE(ctx->ports.count(entry.first)); port(ctx->ports.at(entry.first), entry.second); }
        std::map<uint32_t, std::set<uint8_t>> changed;
        size_t index = 0;
        for (const auto &entry : ctx->cells) {
            const auto &old = cells.at(index++); auto *c = entry.second.get();
            SCOPED_TRACE(old.name.str(ctx));
            EXPECT_EQ(c, old.owner); EXPECT_EQ(entry.first, old.name); EXPECT_EQ(c->name, old.name);
            EXPECT_EQ(c->type, old.type); EXPECT_EQ(c->hierpath, old.hierpath); EXPECT_EQ(c->belStrength, old.strength);
            EXPECT_EQ(c->cluster, old.cluster); EXPECT_EQ(c->region, old.region); EXPECT_EQ(c->pseudo_cell.get(), old.pseudo);
            EXPECT_EQ(c->params, old.params); EXPECT_EQ(c->attrs, old.attrs);
            if (!moved.count(old.name)) EXPECT_EQ(c->bel, old.bel);
            else {
                ASSERT_NE(c->bel, old.bel); EXPECT_EQ(ctx->getBoundBelCell(old.bel), nullptr);
                for (auto bel : {c->bel, old.bel}) { const auto &d = ctx->bel_data(bel).lab_data; changed[d.lab].insert(d.alm); }
            }
            if (c->bel != BelId()) EXPECT_EQ(ctx->getBoundBelCell(c->bel), c);
            std::vector<IdString> order; for (const auto &p : c->ports) order.push_back(p.first);
            EXPECT_EQ(order, old.order); ASSERT_EQ(c->ports.size(), old.ports.size());
            for (const auto &p : old.ports) { ASSERT_TRUE(c->ports.count(p.first)); port(c->ports.at(p.first), p.second); }
            EXPECT_EQ(c->constr_children, old.info.constr_children); EXPECT_EQ(c->constr_x, old.info.constr_x);
            EXPECT_EQ(c->constr_y, old.info.constr_y); EXPECT_EQ(c->constr_z, old.info.constr_z); EXPECT_EQ(c->constr_abs_z, old.info.constr_abs_z);
            ASSERT_EQ(c->pin_data.size(), old.info.pin_data.size());
            for (const auto &p : old.info.pin_data) {
                ASSERT_TRUE(c->pin_data.count(p.first)); EXPECT_EQ(c->pin_data.at(p.first).state, p.second.state);
                EXPECT_EQ(c->pin_data.at(p.first).bel_pins, p.second.bel_pins);
            }
            if (c->type == id_MISTRAL_FF) {
                EXPECT_EQ(c->ffInfo.ctrlset, old.info.ffInfo.ctrlset); EXPECT_EQ(c->ffInfo.datain, old.info.ffInfo.datain);
                EXPECT_EQ(c->ffInfo.sdata, old.info.ffInfo.sdata);
            } else if (ctx->is_comb_cell(c->type) || c->type.in(id_MISTRAL_BUF, id_MISTRAL_NOT)) {
                const auto &a = c->combInfo, &b = old.info.combInfo;
                EXPECT_EQ(a.comb_out, b.comb_out); EXPECT_EQ(a.lut_input_count, b.lut_input_count);
                EXPECT_EQ(a.used_lut_input_count, b.used_lut_input_count); EXPECT_EQ(a.lut_bits_count, b.lut_bits_count);
                EXPECT_EQ(a.chain_shared_input_count, b.chain_shared_input_count); EXPECT_EQ(a.is_carry, b.is_carry);
                EXPECT_EQ(a.is_shared, b.is_shared); EXPECT_EQ(a.is_extended, b.is_extended);
                EXPECT_EQ(a.carry_start, b.carry_start); EXPECT_EQ(a.carry_end, b.carry_end); EXPECT_EQ(a.mlab_group, b.mlab_group);
                for (int i = 0; i < b.lut_input_count; ++i) EXPECT_EQ(a.lut_in[i], b.lut_in[i]);
            }
        }
        index = 0;
        for (const auto &entry : ctx->nets) {
            const auto &old = nets.at(index++); auto *n = entry.second.get();
            EXPECT_EQ(n, old.owner); EXPECT_EQ(entry.first, old.name); EXPECT_EQ(n->name, old.name); EXPECT_EQ(n->hierpath, old.hierpath);
            EXPECT_EQ(n->driver.cell, old.driver.cell); EXPECT_EQ(n->driver.port, old.driver.port);
            EXPECT_EQ(n->constant_value, old.constant); EXPECT_EQ(n->attrs, old.attrs); EXPECT_EQ(n->region, old.region);
            EXPECT_EQ(n->is_global, old.global); EXPECT_EQ(n->aliases, old.aliases); EXPECT_EQ(n->clkconstr.get(), old.clock);
            ASSERT_EQ(n->wires.size(), old.wires.size());
            for (const auto &w : old.wires) {
                ASSERT_TRUE(n->wires.count(w.first)); EXPECT_EQ(n->wires.at(w.first).pip, w.second.pip);
                EXPECT_EQ(n->wires.at(w.first).strength, w.second.strength);
            }
            auto a = n->users, b = old.users; ASSERT_EQ(a.capacity(), b.capacity()); ASSERT_EQ(a.entries(), b.entries());
            for (auto u : b.enumerate()) { ASSERT_TRUE(a.count(u.index)); EXPECT_EQ(a.at(u.index).cell, u.value.cell); EXPECT_EQ(a.at(u.index).port, u.value.port); }
            for (size_t i = 0, count = size_t(b.capacity()) + 4; i < count; ++i) EXPECT_EQ(a.add(PortRef{}), b.add(PortRef{}));
        }
        ASSERT_EQ(ctx->labs.size(), labs.size());
        for (size_t i = 0; i < labs.size(); ++i) lab(ctx->labs[i], labs[i], changed[uint32_t(i)]);
        ctx->check();
    }
};

struct HardLog : std::streambuf {
    std::ostream stream;
    std::string text, line;
    int trials = 0, endpoint_rejects = 0;
    bool throw_on_trial;
    explicit HardLog(bool fail = false) : stream(this), throw_on_trial(fail)
    { stream.exceptions(std::ios::badbit | std::ios::failbit); log_streams.emplace_back(&stream, LogLevel::INFO_MSG); }
    ~HardLog() { log_streams.pop_back(); }
    void append(char value)
    {
        text.push_back(value);
        if (value != '\n') { line.push_back(value); return; }
        if (line.find("Hard input locality trial ") != std::string::npos) {
            ++trials; endpoint_rejects += line.find("endpoints=0") != std::string::npos;
            if (throw_on_trial) throw std::runtime_error("real hard-input trial log exception");
        }
        line.clear();
    }
    std::streamsize xsputn(const char *data, std::streamsize count) override
    { for (std::streamsize i = 0; i < count; ++i) append(data[i]); return count; }
    int_type overflow(int_type value) override
    { if (!traits_type::eq_int_type(value, traits_type::eof())) append(traits_type::to_char_type(value)); return traits_type::not_eof(value); }
};

struct TemporaryHardGuide {
    std::filesystem::path directory, file;
    TemporaryHardGuide()
    {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() /
                ("nextpnr-hard-input-guide-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) {
                directory = candidate; file = directory / "guide.json"; return;
            }
        }
        throw std::runtime_error("cannot create exclusive native hard-input guide fixture");
    }
    ~TemporaryHardGuide()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

class HardInputLocalityTest : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clock, *hard_data;
    CellInfo *hard, *launch, *upstream;
    IdString sink_pin;
    CellInfo *ff(const std::string &name, NetInfo *data, bool initial = false)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->pin_data[id_ENA].state = cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = cell->pin_data[id_SLOAD].state = PIN_0;
        cell->params[id_INIT] = Property(initial ? 1 : 0, 1);
        cell->connectPort(id_CLK, clock); if (data) cell->connectPort(id_DATAIN, data);
        cell->addOutput(id_Q); cell->connectPort(id_Q, ctx->createNet(ctx->id(name + "$q")));
        return cell;
    }
    void bind(CellInfo *cell, BelId bel, PlaceStrength strength = STRENGTH_WEAK)
    {
        ASSERT_NE(bel, BelId()); ASSERT_TRUE(ctx->checkBelAvail(bel));
        ctx->bindBel(bel, cell, strength); ASSERT_TRUE(ctx->isBelLocationValid(bel));
    }
    BelId ordinary_origin()
    {
        std::vector<std::pair<int, BelId>> sites;
        for (auto bel : ctx->getBels()) {
            if (ctx->getBelType(bel) != id_MISTRAL_FF || !ctx->checkBelAvail(bel)) continue;
            const auto &d = ctx->bel_data(bel).lab_data; auto at = ctx->getBelLocation(bel);
            if (at.z != 2 || ctx->labs.at(d.lab).is_mlab) continue;
            sites.emplace_back(std::abs(at.x - 25) + std::abs(at.y - 23), bel);
        }
        std::sort(sites.begin(), sites.end(), [&](const auto &a, const auto &b) {
            auto x = ctx->getBelLocation(a.second), y = ctx->getBelLocation(b.second);
            return std::make_tuple(a.first, x.x, x.y) < std::make_tuple(b.first, y.x, y.y);
        });
        if (sites.empty()) { ADD_FAILURE() << "No ordinary LAB origin"; return BelId(); }
        return sites.front().second;
    }
    // Discover fixture occupants from real available native sites. This does
    // not duplicate the worker's candidate list or legality/ranking algorithm.
    BelId near_hard_wire(CellInfo *cell)
    {
        auto wire = ctx->getNetinfoSinkWire(launch->getPort(id_Q), {hard, sink_pin}, 0);
        auto original = ctx->getBelLocation(launch->bel);
        std::vector<std::pair<int, BelId>> sites;
        for (auto bel : ctx->getBels()) {
            if (ctx->getBelType(bel) != id_MISTRAL_FF || !ctx->checkBelAvail(bel)) continue;
            auto at = ctx->getBelLocation(bel);
            if (at.z != 2 || (at.x == original.x && at.y == original.y)) continue;
            int distance = std::abs(at.x - int(wire.node.x())) + std::abs(at.y - int(wire.node.y()));
            sites.emplace_back(distance, bel);
        }
        std::sort(sites.begin(), sites.end(), [&](const auto &a, const auto &b) {
            auto x = ctx->getBelLocation(a.second), y = ctx->getBelLocation(b.second);
            return std::make_tuple(a.first, x.x, x.y) < std::make_tuple(b.first, y.x, y.y);
        });
        for (const auto &site : sites) {
            ctx->bindBel(site.second, cell, STRENGTH_LOCKED);
            bool legal = ctx->isBelLocationValid(site.second); ctx->unbindBel(site.second);
            if (legal) return site.second;
        }
        ADD_FAILURE() << "No real legal fixture site near hard input wire"; return BelId();
    }
    void holes(NetInfo *net)
    {
        std::array<CellInfo *, 3> scratch;
        for (int i = 0; i < 3; ++i) {
            scratch[i] = ctx->createCell(ctx->idf("hole_%s_%d", net->name.c_str(ctx.get()), i), id_MISTRAL_ALUT2);
            scratch[i]->addInput(id_A); scratch[i]->connectPort(id_A, net);
        }
        for (int i : {1, 0, 2}) { scratch[i]->disconnectPort(id_A); ctx->cells.erase(scratch[i]->name); }
    }
    void control(IdString pin, NetInfo *net)
    {
        if (launch->getPort(pin)) launch->disconnectPort(pin);
        launch->pin_data[pin].state = PIN_SIG; launch->connectPort(pin, net);
        ctx->assignArchInfo();
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
        auto a = endpoint(launch, id_Q), b = endpoint(hard, sink_pin);
        auto capture = ctx->getPortClockingInfo(hard, sink_pin, 0);
        auto source_clock = ctx->getPortClockingInfo(launch, id_Q, 0);
        // Intentionally passing guide: actual route may disagree with the
        // display-coordinate predictor. Discovery must still inspect it.
        Json::array segments{
            Json::object{{"type", "clk-to-q"}, {"delay", 0.731}, {"from", a}, {"to", a}},
            Json::object{{"type", "routing"}, {"delay", 1.0}, {"from", a}, {"to", b}, {"net", launch->getPort(id_Q)->name.str(ctx.get())}},
            Json::object{{"type", "setup"}, {"delay", capture.setup.maxDelay() / 1000.0}, {"from", b}, {"to", b}}};
        auto event = [&](ClockEdge edge) { return (edge == FALLING_EDGE ? "negedge " : "posedge ") + clock->name.str(ctx.get()); };
        double window = source_clock.edge == capture.edge ? 10.0 : 5.0;
        return Json::object{{"critical_paths", Json::array{Json::object{{"from", event(source_clock.edge)}, {"to", event(capture.edge)}, {"max_delay", window}, {"path", segments}}}}};
    }
    std::string report() const { return report_json().dump(); }
    guard::Rows rows(TimingAnalyser &timing, CellInfo *cell, IdString pin)
    {
        guard::Rows result; EXPECT_TRUE(timing.get_endpoint_clock_pair_timings(CellPortKey(cell->name, pin), result));
        EXPECT_FALSE(result.empty()); return result;
    }
    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7"; ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 100e6;
        clock = ctx->createNet(ctx->id("renamed_write_clock")); clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>(); clock->clkconstr->period = DelayPair(10000);
        clock->clkconstr->high = clock->clkconstr->low = DelayPair(5000);
        auto *buffer = ctx->createCell(ctx->id("renamed_clock_buffer"), id_MISTRAL_CLKBUF);
        buffer->addOutput(id_Q); buffer->connectPort(id_Q, clock);
        ctx->createNet(ctx->id("$PACKER_GND_NET")); ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        hard = ctx->createCell(ctx->id("arbitrary_fixed_registered_receiver"), id_cyclonev_hps_interface_fpga2sdram);
        auto rd_clock = ctx->id("rd_clk_0"), rd_data = ctx->id("rd_data_0[0]"), wr_clock = ctx->id("wr_clk_1");
        sink_pin = ctx->id("wr_data_1[19]");
        hard->addInput(rd_clock); hard->connectPort(rd_clock, clock);
        hard->addInput(wr_clock); hard->connectPort(wr_clock, clock);
        hard->addOutput(rd_data); hard_data = ctx->createNet(ctx->id("arbitrary_fixed_launch_data")); hard->connectPort(rd_data, hard_data);
        upstream = ff("arbitrary_registered_input", hard_data, true);
        launch = ff("arbitrary_registered_writer", upstream->getPort(id_Q), true);
        hard->addInput(sink_pin); hard->connectPort(sink_pin, launch->getPort(id_Q));
        control(id_ENA, upstream->getPort(id_Q));
        ctx->assignArchInfo();
        for (auto bel : ctx->getBels()) if (ctx->getBelType(bel) == hard->type) { bind(hard, bel, STRENGTH_LOCKED); break; }
        ASSERT_NE(hard->bel, BelId());
        bind(launch, ordinary_origin());
        bind(upstream, near_hard_wire(upstream), STRENGTH_LOCKED);
        ctx->net_aliases[ctx->id("writer_alias")] = launch->getPort(id_Q)->name;
        launch->getPort(id_Q)->aliases.push_back(ctx->id("writer_alias"));
        for (auto *net : {hard_data, upstream->getPort(id_Q), launch->getPort(id_Q)}) holes(net);
        ctx->check();
    }
};

TEST_F(HardInputLocalityTest, PassingGuideConnectedEnableUsesActualHardWireAndPreservesState)
{
    HardSnapshot saved(ctx.get()); HardLog log;
    auto old_wire = ctx->getNetinfoSourceWire(launch->getPort(id_Q));
    auto sink = ctx->getNetinfoSinkWire(launch->getPort(id_Q), {hard, sink_pin}, 0);
    auto display = ctx->getBelLocation(hard->bel);
    ASSERT_TRUE(sink.node.x() != display.x || sink.node.y() != display.y);
    auto old_delay = ctx->estimateDelay(old_wire, sink);
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    auto data = rows(before, launch, id_DATAIN), ena = rows(before, launch, id_ENA);
    auto target = CellPortKey(hard->name, sink_pin); auto slack = before.get_setup_slack(target);
    ASSERT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 1) << log.text;
    ASSERT_GT(log.trials, 0); EXPECT_LE(log.trials, 16); saved.expect(ctx.get(), {launch->name});
    EXPECT_NE(log.text.find("wire=" + ctx->getWireName(sink).str(ctx.get())), std::string::npos) << log.text;
    EXPECT_GE(old_delay - ctx->estimateDelay(ctx->getNetinfoSourceWire(launch->getPort(id_Q)), sink), 250);
    TimingAnalyser after(ctx.get()); after.with_clock_skew = true; after.setup(false, false, true);
    EXPECT_GE(after.get_setup_slack(target), slack + 250);
    EXPECT_TRUE(guard::rows_nonregressing(data, rows(after, launch, id_DATAIN), false));
    EXPECT_TRUE(guard::rows_nonregressing(ena, rows(after, launch, id_ENA), false));
    EXPECT_TRUE(guard::clocks_nonregressing(before, after)); EXPECT_TRUE(guard::holds_nonregressing(guard::holds(before), guard::holds(after)));
}

TEST_F(HardInputLocalityTest, PreloadedBytesStillQualifyAfterGuideReplacementAndDeletion)
{
    EXPECT_TRUE(ctx->hard_input_report.empty()); EXPECT_EQ(ctx->hard_input_budget, 0);
    EXPECT_EQ(ctx->hard_input_radius, 24);
    HardSnapshot initial(ctx.get()); preload_hard_input_locality(ctx.get(), nullptr); initial.expect(ctx.get());
    TemporaryHardGuide guide; auto original = report();
    {
        std::ofstream out(guide.file); ASSERT_TRUE(out); out << original; out.close(); ASSERT_TRUE(out);
    }
    auto spec = guide.file.string() + " 1 24";
    preload_hard_input_locality(ctx.get(), spec.c_str());
    ASSERT_EQ(ctx->hard_input_report, original); ASSERT_EQ(ctx->hard_input_budget, 1);
    ASSERT_EQ(ctx->hard_input_radius, 24); initial.expect(ctx.get());
    {
        // A later filesystem change must not replace the immutable request.
        std::ofstream out(guide.file, std::ios::trunc); ASSERT_TRUE(out);
        out << "{invalid replacement guide"; out.close(); ASSERT_TRUE(out);
    }
    ASSERT_EQ(ctx->hard_input_report, original);
    ASSERT_TRUE(std::filesystem::remove(guide.file)); ASSERT_FALSE(std::filesystem::exists(guide.file));
    auto sink = ctx->getNetinfoSinkWire(launch->getPort(id_Q), {hard, sink_pin}, 0);
    auto wire_delay = ctx->estimateDelay(ctx->getNetinfoSourceWire(launch->getPort(id_Q)), sink);
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    auto data = rows(before, launch, id_DATAIN), ena = rows(before, launch, id_ENA);
    auto target = CellPortKey(hard->name, sink_pin); auto slack = before.get_setup_slack(target);
    HardSnapshot saved(ctx.get()); HardLog log;
    ASSERT_EQ(hard_input_locality(ctx.get(), ctx->hard_input_report, ctx->hard_input_budget,
                                 ctx->hard_input_radius), 1) << log.text;
    EXPECT_GT(log.trials, 0); saved.expect(ctx.get(), {launch->name});
    EXPECT_GE(wire_delay - ctx->estimateDelay(ctx->getNetinfoSourceWire(launch->getPort(id_Q)), sink), 250);
    TimingAnalyser after(ctx.get()); after.with_clock_skew = true; after.setup(false, false, true);
    EXPECT_GE(after.get_setup_slack(target), slack + 250);
    EXPECT_TRUE(guard::rows_nonregressing(data, rows(after, launch, id_DATAIN), false));
    EXPECT_TRUE(guard::rows_nonregressing(ena, rows(after, launch, id_ENA), false));
    EXPECT_TRUE(guard::clocks_nonregressing(before, after));
    EXPECT_TRUE(guard::holds_nonregressing(guard::holds(before), guard::holds(after)));
    HardSnapshot retained(ctx.get()); preload_hard_input_locality(ctx.get(), nullptr);
    EXPECT_TRUE(ctx->hard_input_report.empty()); EXPECT_EQ(ctx->hard_input_budget, 0);
    EXPECT_EQ(ctx->hard_input_radius, 24); retained.expect(ctx.get());
}

TEST_F(HardInputLocalityTest, EveryConnectedNonclockControlHasStrictNativeRows)
{
    for (auto pin : {id_ACLR, id_SCLR, id_SLOAD, id_SDATA}) control(pin, upstream->getPort(id_Q));
    ASSERT_TRUE(ctx->isBelLocationValid(launch->bel));
    HardSnapshot saved(ctx.get()); HardLog log;
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    std::map<IdString, guard::Rows> old;
    for (auto pin : {id_DATAIN, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA}) old.emplace(pin, rows(before, launch, pin));
    ASSERT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 1) << log.text;
    saved.expect(ctx.get(), {launch->name});
    TimingAnalyser after(ctx.get()); after.with_clock_skew = true; after.setup(false, false, true);
    for (const auto &entry : old) EXPECT_TRUE(guard::rows_nonregressing(entry.second, rows(after, launch, entry.first), false));
}

TEST_F(HardInputLocalityTest, InvertedLaunchClockRejectsIncompleteOppositeEdgeHoldCoverage)
{
    launch->pin_data[id_CLK].state = PIN_INV; ctx->assignArchInfo();
    ASSERT_TRUE(ctx->isBelLocationValid(launch->bel));
    ASSERT_EQ(ctx->getPortClockingInfo(launch, id_Q, 0).edge, FALLING_EDGE);
    ASSERT_EQ(ctx->getPortClockingInfo(upstream, id_Q, 0).edge, RISING_EDGE);
    ASSERT_EQ(ctx->getPortClockingInfo(hard, sink_pin, 0).edge, RISING_EDGE);
    ASSERT_EQ(launch->params.at(id_INIT), Property(1, 1));
    ASSERT_EQ(clock->clkconstr->phase_group, IdString());
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    ASSERT_FALSE(before.have_loops);
    ASSERT_TRUE(guard::timed(before.get_setup_slack(CellPortKey(hard->name, sink_pin))));
    ASSERT_FALSE(before.get_timing_result().clock_fmax.empty());
    std::map<CellPortKey, guard::Rows> old;
    for (auto key : {CellPortKey(hard->name, sink_pin), CellPortKey(launch->name, id_DATAIN),
                     CellPortKey(launch->name, id_ENA)}) {
        guard::Rows current;
        ASSERT_TRUE(before.get_endpoint_clock_pair_timings(key, current)); ASSERT_EQ(current.size(), 1);
        const auto &row = current.front();
        ASSERT_EQ(row.launch.clock, clock->name); ASSERT_EQ(row.capture.clock, clock->name);
        ASSERT_EQ(row.launch.edge, key.cell == hard->name ? FALLING_EDGE : RISING_EDGE);
        ASSERT_EQ(row.capture.edge, key.cell == hard->name ? RISING_EDGE : FALLING_EDGE);
        ASSERT_TRUE(row.setup_timed); ASSERT_TRUE(row.setup_window); ASSERT_EQ(*row.setup_window, 5000);
        ASSERT_TRUE(row.setup_margin);
        // Same-net opposite edges have setup timing, but the native API does
        // not establish a hold relation without an explicit phase group.
        ASSERT_FALSE(row.hold_related); ASSERT_FALSE(row.hold_margin);
        old.emplace(key, std::move(current));
    }
    HardSnapshot saved(ctx.get()); HardLog log;
    EXPECT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 0) << log.text;
    EXPECT_EQ(log.trials, 0) << log.text;
    EXPECT_NE(log.text.find("eligible=1 attempted=1 retained=0"), std::string::npos) << log.text;
    EXPECT_EQ(log.text.find("Hard input locality launch="), std::string::npos) << log.text;
    saved.expect(ctx.get());
    EXPECT_EQ(launch->get_pin_state(id_CLK), PIN_INV);
    EXPECT_EQ(ctx->getPortClockingInfo(launch, id_Q, 0).edge, FALLING_EDGE);
    EXPECT_EQ(ctx->getPortClockingInfo(upstream, id_Q, 0).edge, RISING_EDGE);
    EXPECT_EQ(ctx->getPortClockingInfo(hard, sink_pin, 0).edge, RISING_EDGE);
    EXPECT_EQ(launch->params.at(id_INIT), Property(1, 1));
    TimingAnalyser after(ctx.get()); after.with_clock_skew = true; after.setup(false, false, true);
    for (const auto &entry : old) {
        guard::Rows current;
        ASSERT_TRUE(after.get_endpoint_clock_pair_timings(entry.first, current)); ASSERT_EQ(current.size(), 1);
        const auto &row = current.front(), &previous = entry.second.front();
        EXPECT_EQ(row.launch, previous.launch); EXPECT_EQ(row.capture, previous.capture);
        EXPECT_EQ(row.setup_timed, previous.setup_timed); EXPECT_EQ(row.hold_related, previous.hold_related);
        EXPECT_EQ(row.setup_window, previous.setup_window); EXPECT_EQ(row.setup_margin, previous.setup_margin);
        EXPECT_EQ(row.hold_margin, previous.hold_margin);
        EXPECT_EQ(row.max_path_delay, previous.max_path_delay); EXPECT_EQ(row.min_path_delay, previous.min_path_delay);
    }
}

TEST_F(HardInputLocalityTest, SecondaryQEndpointAndRegisteredFeedbackRemainNonregressing)
{
    auto *branch = ff("renamed_secondary_capture", launch->getPort(id_Q)); ctx->assignArchInfo();
    bind(branch, near_hard_wire(branch), STRENGTH_LOCKED);
    auto *feedback = ctx->createCell(ctx->id("renamed_feedback_logic"), id_MISTRAL_ALUT2);
    feedback->addInput(id_A); feedback->addInput(id_B); feedback->addOutput(id_Q);
    feedback->connectPort(id_A, launch->getPort(id_Q)); feedback->connectPort(id_B, upstream->getPort(id_Q));
    feedback->params[id_LUT] = Property(0x8, 4);
    auto *enable = ctx->createNet(ctx->id("renamed_registered_feedback")); feedback->connectPort(id_Q, enable);
    control(id_ENA, enable);
    auto at = ctx->getBelLocation(branch->bel);
    bind(feedback, ctx->getBelByLocation(Loc(at.x, at.y, 0)), STRENGTH_LOCKED);
    HardSnapshot saved(ctx.get()); HardLog log;
    TimingAnalyser before(ctx.get()); before.with_clock_skew = true; before.setup(false, false, true);
    auto ena = rows(before, launch, id_ENA), secondary = rows(before, branch, id_DATAIN);
    ASSERT_FALSE(before.have_loops);
    ASSERT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 1) << log.text;
    saved.expect(ctx.get(), {launch->name});
    TimingAnalyser after(ctx.get()); after.with_clock_skew = true; after.setup(false, false, true);
    EXPECT_FALSE(after.have_loops); EXPECT_TRUE(guard::rows_nonregressing(ena, rows(after, launch, id_ENA), false));
    EXPECT_TRUE(guard::rows_nonregressing(secondary, rows(after, branch, id_DATAIN), false));
}

TEST_F(HardInputLocalityTest, IncomingEnableRegressionRejectsOtherwiseImprovedHardPath)
{
    auto at = ctx->getBelLocation(launch->bel);
    ctx->unbindBel(upstream->bel); bind(upstream, ctx->getBelByLocation(Loc(at.x, at.y, 8)));
    // DATAIN still comes from the fixed hard source; only ENA becomes a short
    // local registered arc which every geometrically useful move worsens.
    launch->disconnectPort(id_DATAIN); launch->connectPort(id_DATAIN, hard_data); ctx->assignArchInfo();
    HardSnapshot saved(ctx.get()); HardLog log;
    EXPECT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 0) << log.text;
    EXPECT_GT(log.trials, 0); EXPECT_GT(log.endpoint_rejects, 0); EXPECT_LE(log.trials, 16); saved.expect(ctx.get());
}

TEST_F(HardInputLocalityTest, IncomingDataRegressionAndEarlierUnrelatedEditArePreserved)
{
    auto at = ctx->getBelLocation(launch->bel);
    ctx->unbindBel(upstream->bel); bind(upstream, ctx->getBelByLocation(Loc(at.x, at.y, 8)));
    control(id_ENA, hard_data);
    auto *prefix = ff("retained_prefix_register", hard_data, true); ctx->assignArchInfo();
    bind(prefix, near_hard_wire(prefix), STRENGTH_LOCKED);
    // A prior graph-prefix edit is part of this transaction's starting state.
    prefix->attrs[ctx->id("earlier_stage_provenance")] = Property("retained");
    ctx->net_aliases[ctx->id("earlier_stage_alias")] = prefix->getPort(id_Q)->name;
    HardSnapshot saved(ctx.get()); HardLog log;
    EXPECT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 0) << log.text;
    EXPECT_GT(log.trials, 0); EXPECT_GT(log.endpoint_rejects, 0); saved.expect(ctx.get());
}

TEST_F(HardInputLocalityTest, ShortSecondaryQBranchRejectsWhileTargetWouldImprove)
{
    auto *branch = ff("short_shared_q_capture", launch->getPort(id_Q)); ctx->assignArchInfo();
    auto at = ctx->getBelLocation(launch->bel);
    bind(branch, ctx->getBelByLocation(Loc(at.x, at.y, 8)));
    HardSnapshot saved(ctx.get()); HardLog log;
    EXPECT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 0) << log.text;
    EXPECT_GT(log.trials, 0); EXPECT_GT(log.endpoint_rejects, 0); saved.expect(ctx.get());
}

TEST_F(HardInputLocalityTest, UnrelatedClockBranchKeepsReferenceFreeMinimumArrival)
{
    auto *other = ctx->createNet(ctx->id("independent_capture_clock")); other->is_global = true;
    other->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
    auto *buffer = ctx->createCell(ctx->id("independent_clock_buffer"), id_MISTRAL_CLKBUF);
    buffer->addOutput(id_Q); buffer->connectPort(id_Q, other);
    auto *branch = ff("independent_branch_capture", launch->getPort(id_Q));
    branch->disconnectPort(id_CLK); branch->connectPort(id_CLK, other); ctx->assignArchInfo();
    bind(branch, near_hard_wire(branch), STRENGTH_LOCKED);
    TimingAnalyser reference(ctx.get()); reference.with_clock_skew = false; reference.setup(false, false, true);
    auto old = rows(reference, branch, id_DATAIN);
    ASSERT_FALSE(old.empty()); for (const auto &row : old) ASSERT_FALSE(row.setup_timed);
    HardSnapshot saved(ctx.get()); HardLog log;
    EXPECT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 0) << log.text;
    EXPECT_GT(log.trials, 0); EXPECT_GT(log.endpoint_rejects, 0); saved.expect(ctx.get());
    TimingAnalyser after(ctx.get()); after.with_clock_skew = false; after.setup(false, false, true);
    EXPECT_TRUE(guard::rows_nonregressing(old, rows(after, branch, id_DATAIN), true));
}

TEST_F(HardInputLocalityTest, UnknownBranchClockRejectsBeforeAnyBinding)
{
    auto *other = ctx->createNet(ctx->id("unconstrained_capture_clock")); other->is_global = true;
    auto *buffer = ctx->createCell(ctx->id("unconstrained_clock_buffer"), id_MISTRAL_CLKBUF);
    buffer->addOutput(id_Q); buffer->connectPort(id_Q, other);
    auto *branch = ff("unknown_clock_capture", launch->getPort(id_Q));
    branch->disconnectPort(id_CLK); branch->connectPort(id_CLK, other); ctx->assignArchInfo();
    bind(branch, near_hard_wire(branch), STRENGTH_LOCKED);
    HardSnapshot saved(ctx.get()); HardLog log;
    EXPECT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 0); EXPECT_EQ(log.trials, 0); saved.expect(ctx.get());
}

TEST_F(HardInputLocalityTest, TrialExceptionRestoresSingleFFAndEveryCacheAfterEarlierEdit)
{
    launch->attrs[ctx->id("earlier_stage_provenance")] = Property("retained");
    auto *prefix = ff("earlier_prefix_cell", hard_data, true); ctx->assignArchInfo();
    bind(prefix, near_hard_wire(prefix), STRENGTH_LOCKED);
    HardSnapshot saved(ctx.get()); HardLog log(true);
    EXPECT_THROW(hard_input_locality(ctx.get(), report(), 1, 24), std::exception);
    ASSERT_GT(log.trials, 0); saved.expect(ctx.get());
}

TEST_F(HardInputLocalityTest, StalePlacementRawEdgeClockAndMissingOwnersFailBeforeTrials)
{
    auto original = report_json();
    for (int change = 0; change < 4; ++change) {
        auto document = original.object_items(); auto paths = document["critical_paths"].array_items();
        auto path = paths[0].object_items(); auto segments = path["path"].array_items();
        auto segment = segments[1].object_items(); auto endpoint = segment["to"].object_items();
        if (change == 0) endpoint["loc"] = json11::Json::array{0, 0};
        if (change == 1) endpoint["cell"] = "$ROUTETHRU_absent_owner";
        if (change == 2) segment["net"] = "absent_net";
        if (change == 3) path["from"] = "negedge " + clock->name.str(ctx.get());
        segment["to"] = endpoint; segments[1] = segment; path["path"] = segments; paths[0] = path;
        document["critical_paths"] = paths;
        HardSnapshot saved(ctx.get()); HardLog log;
        EXPECT_THROW(hard_input_locality(ctx.get(), json11::Json(document).dump(), 1, 24), log_execution_error_exception);
        EXPECT_EQ(log.trials, 0); saved.expect(ctx.get());
    }
}

TEST_F(HardInputLocalityTest, LaterStalePathIsValidatedBeforeEarlierEligibleTrial)
{
    auto document = report_json().object_items(); auto paths = document["critical_paths"].array_items();
    auto path = paths.front().object_items(); auto segments = path["path"].array_items();
    auto segment = segments.front().object_items(); auto owner = segment["from"].object_items();
    owner["cell"] = "missing_later_path_owner"; segment["from"] = owner; segments[0] = segment;
    path["path"] = segments; paths.push_back(path); document["critical_paths"] = paths;
    HardSnapshot saved(ctx.get()); HardLog log;
    EXPECT_THROW(hard_input_locality(ctx.get(), json11::Json(document).dump(), 1, 24), log_execution_error_exception);
    EXPECT_EQ(log.trials, 0); saved.expect(ctx.get());
}

TEST_F(HardInputLocalityTest, CombinationalCycleFailsCoverageWithoutBinding)
{
    auto *loop = ctx->createCell(ctx->id("unrelated_comb_cycle"), id_MISTRAL_ALUT2);
    loop->addInput(id_A); loop->addInput(id_B); loop->addOutput(id_Q); loop->params[id_LUT] = Property(0x8, 4);
    auto *cycle = ctx->createNet(ctx->id("unrelated_cycle_wire"));
    loop->connectPort(id_A, launch->getPort(id_Q)); loop->connectPort(id_B, cycle); loop->connectPort(id_Q, cycle);
    ctx->assignArchInfo(); auto at = ctx->getBelLocation(upstream->bel);
    bind(loop, ctx->getBelByLocation(Loc(at.x, at.y, 0)), STRENGTH_LOCKED);
    HardSnapshot saved(ctx.get()); HardLog log;
    EXPECT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 0); EXPECT_EQ(log.trials, 0); saved.expect(ctx.get());
}

TEST_F(HardInputLocalityTest, TopBoundaryAloneExcludesOtherwiseQualifyingLaunch)
{
    // This is a fresh fixture with no combinational cycle. Removing only the
    // top port must expose the same qualifying native hard-input transaction.
    auto top = ctx->id("top_observed_writer");
    ctx->ports[top] = PortInfo{top, launch->getPort(id_Q), PORT_OUT, {}};
    {
        HardSnapshot boundary(ctx.get()); HardLog log;
        EXPECT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 0);
        EXPECT_EQ(log.trials, 0); boundary.expect(ctx.get());
    }
    ctx->ports.erase(top);
    HardSnapshot unprotected(ctx.get()); HardLog positive;
    ASSERT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 1) << positive.text;
    EXPECT_GT(positive.trials, 0); unprotected.expect(ctx.get(), {launch->name});
}

TEST_F(HardInputLocalityTest, ProtectedLaunchAttributesAloneExcludeOtherwiseQualifyingMove)
{
    ASSERT_EQ(launch->belStrength, STRENGTH_WEAK);
    // The production protection contract is attribute presence. keep and
    // dont_touch do not change native timing or physical legality; BEL names
    // the launch's actual existing site, rather than a conflicting site.
    for (const char *name : {"keep", "dont_touch", "BEL"}) {
        SCOPED_TRACE(name); auto key = ctx->id(name);
        ASSERT_FALSE(launch->attrs.count(key));
        launch->attrs[key] = std::string(name) == "BEL"
            ? Property(ctx->getBelName(launch->bel).str(ctx.get())) : Property(1, 1);
        ASSERT_TRUE(ctx->isBelLocationValid(launch->bel));
        {
            HardSnapshot protected_state(ctx.get()); HardLog log;
            EXPECT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 0);
            EXPECT_EQ(log.trials, 0); protected_state.expect(ctx.get());
        }
        launch->attrs.erase(key);
    }
    // Prove the flags were the only blocker on this same graph and geometry.
    HardSnapshot unprotected(ctx.get()); HardLog positive;
    ASSERT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 1) << positive.text;
    EXPECT_GT(positive.trials, 0); unprotected.expect(ctx.get(), {launch->name});
}

TEST_F(HardInputLocalityTest, ProtectedOtherLABOccupantAloneExcludesLaunch)
{
    // This unrelated weak FF has the same real constrained clock and is fed
    // from the fixed hard source, not from the moved FF or its incoming net.
    // It adds no affected Q branch and no conflicting native control set.
    auto *neighbor = ff("unrelated_same_lab_occupant", hard_data, false);
    ctx->assignArchInfo(); auto at = ctx->getBelLocation(launch->bel);
    bind(neighbor, ctx->getBelByLocation(Loc(at.x, at.y, 8)), STRENGTH_WEAK);
    ASSERT_EQ(neighbor->getPort(id_CLK), launch->getPort(id_CLK));
    ASSERT_TRUE(ctx->isBelLocationValid(launch->bel));
    ASSERT_TRUE(ctx->isBelLocationValid(neighbor->bel));
    auto keep = ctx->id("keep"); neighbor->attrs[keep] = Property(1, 1);
    ASSERT_TRUE(ctx->isBelLocationValid(launch->bel));
    ASSERT_TRUE(ctx->isBelLocationValid(neighbor->bel));
    {
        HardSnapshot protected_lab(ctx.get()); HardLog log;
        EXPECT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 0);
        EXPECT_EQ(log.trials, 0); protected_lab.expect(ctx.get());
    }
    neighbor->attrs.erase(keep);
    HardSnapshot unprotected(ctx.get()); HardLog positive;
    ASSERT_EQ(hard_input_locality(ctx.get(), report(), 1, 24), 1) << positive.text;
    EXPECT_GT(positive.trials, 0); unprotected.expect(ctx.get(), {launch->name});
}
} // namespace
