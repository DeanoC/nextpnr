/* Native-cut SDR GPIO reference. All use requires an explicit model opt-in. */
#include "nextpnr.h"
#include "util.h"
#include "gpio_core_reference_data.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

NEXTPNR_NAMESPACE_BEGIN

namespace {
using CV = mistral::CycloneV;
using Site = ::gpio_core_reference::Site;

const Site *reference_site(const Context *ctx, const CellInfo *cell)
{
    if (!bool_or_default(ctx->settings, ctx->id("mistral/gpio_core_reference"), false))
        return nullptr;
    auto reject = [&](const char *reason) {
        log_error("GPIO core reference does not cover '%s': %s.\n", cell->name.c_str(ctx), reason);
    };
    if (cell->type.in(id_MISTRAL_SDRIN,id_MISTRAL_DDRIN,id_MISTRAL_DDRBIDIR) ||
        (cell->type == id_MISTRAL_DDROUT &&
         (cell->getPort(id_I) || cell->getPort(id_D_H) || cell->getPort(id_D_L))))
        reject("this registered GPIO fabric mode has no native reference");
    if (!cell->type.in(id_MISTRAL_SDROUT,id_MISTRAL_SDRIO))
        return nullptr; // Constant clock forwarding retains its separate model.
    if (ctx->getChipName() != "5CSEBA6U23I7")
        reject("require the characterized 5CSEBA6U23I7 package");
    const bool bidir = cell->type == id_MISTRAL_SDRIO;
    if (bidir) {
        for (auto key : {"IOREG_IN", "IOREG_OUT", "IOREG_OE"})
            if (int_or_default(cell->params, ctx->id(key), 0) != 1)
                reject("require all three SDR input/output/OE registers");
    }
    for (auto key : {"IOREG_IN_DDR", "IOREG_IN_CE", "IOREG_OUT_CE", "IOREG_OE_CE",
                     "IOREG_IN_ACLR", "IOREG_OUT_ACLR", "IOREG_OE_ACLR",
                     "IOREG_CEIN_INV", "IOREG_CEOUT_INV", "IOREG_ACLR_INV"})
        if (int_or_default(cell->params, ctx->id(key), 0))
            reject("DDR, clock enables, clears or control inversion are uncharacterized");
    for (auto port : {id_CEIN, id_CEOUT, id_ACLR})
        if (cell->getPort(port))
            reject("require disconnected inactive clock-enable/clear controls");
    const auto io = ctx->get_io_electrical(cell);
    if (io.lvcmos || io.bus_hold || io.weak_pullup || io.clamp_diode || io.slow_slew ||
        io.drive_strength != CV::V3P3_LVTTL_16MA_LVCMOS_2MA || io.d1_delay >= 0 || io.d3_delay >= 0 ||
        io.d5_delay >= 0 || io.d5_oe_delay >= 0)
        reject("require the fitted default 3.3-V LVTTL electrical and delay-chain settings");
    const auto loc = cell->attrs.find(id_LOC);
    if (loc == cell->attrs.end() || !loc->second.is_string)
        reject("missing package-pin assignment");
    const Site *site = nullptr;
    for (const auto &candidate : ::gpio_core_reference::sites)
        if (loc->second.as_string() == std::string("PIN_")+candidate.pin)
            site = &candidate;
    if (!site || site->valid != unsigned(bidir ? 7 : 1))
        reject("no complete fitted native reference for this package pin and register mode");
    for (auto port : {id_CLK, id_CLKIN}) {
        if (port == id_CLKIN && !bidir)
            continue;
        auto *net = cell->getPort(port);
        if (!net || !net->driver.cell || cell->get_pin_state(port) != PIN_SIG)
            reject("require driven non-inverted input and output clocks");
        auto *clock = net->clkconstr.get();
        // Model/endpoint discovery precedes clock inference. Every selected
        // GPIO declares mandatory primitive clock checks, which require the
        // waveform after inference. Validate its exact domain whenever known.
        if (!clock)
            continue;
        if (clock->period.minDelay() != clock->period.maxDelay() ||
            clock->high.minDelay() != clock->high.maxDelay() || clock->low.minDelay() != clock->low.maxDelay() ||
            clock->period.minDelay() < 10000 || clock->high.minDelay() < 5000 || clock->low.minDelay() < 5000 ||
            clock->high.minDelay()+clock->low.minDelay() != clock->period.minDelay() ||
            std::abs(clock->high.minDelay()-clock->low.minDelay()) > 1)
            log_error("GPIO core reference clock %s.%s requires an exact period >=10ns and nominal 50 percent duty: period %d..%d high %d..%d low %d..%d; configured=%d.\n",cell->name.c_str(ctx),port.c_str(ctx),clock?clock->period.minDelay():-1,clock?clock->period.maxDelay():-1,clock?clock->high.minDelay():-1,clock?clock->high.maxDelay():-1,clock?clock->low.minDelay():-1,clock?clock->low.maxDelay():-1,int(ctx->bitstream_configured));
    }
    for (auto port : {id_I, id_OE})
        if (cell->getPort(port) && (!cell->getPort(port)->driver.cell || cell->get_pin_state(port) != PIN_SIG))
            reject("fabric data/OE is undriven or inverted");
    auto *package = ctx->cyclonev->pin_find_name(site->pin);
    if (!package)
        reject("the selected Mistral database has no reference package pin");
    const auto bel = ctx->get_io_pin_bel(package);
    const bool owner = cell->bel != BelId() && ctx->getBoundBelCell(cell->bel) == cell;
    if (cell->bel != BelId() && (!owner || cell->bel != bel))
        reject("the placed GPIO owner disagrees with the package-pin assignment");
    if (!ctx->bitstream_configured)
        return site;
    if (!owner || ctx->bel_data(bel).block_index != site->bi || bel.pos.x() != site->x || bel.pos.y() != site->y)
        reject("a configured profile requires its actual physical GPIO owner");
    const auto lane = ctx->cyclonev->p2p_to(CV::pnode_coords{CV::GPIO,bel.pos,CV::PNONE,int8_t(site->bi),-1});
    if (!lane || lane.bt() != CV::DQS16)
        reject("missing physical DQS lane");
    for (size_t i = 0; i < std::size(::gpio_core_reference::fields); ++i) {
        const auto &field = ::gpio_core_reference::fields[i];
        CV::bmux_setting_t setting{};
        if (!ctx->cyclonev->bmux_get(CV::DQS16,lane.p(),field.name,lane.bi(),setting) || setting.type != field.type)
            reject("missing or mis-typed physical GPIO selector");
        uint32_t value = setting.s;
        if (setting.type == CV::MT_RAM) {
            if (setting.r.size() > 4)
                reject("unexpected wide GPIO delay selector");
            value = 0;
            for (size_t byte = 0; byte < setting.r.size(); ++byte)
                value |= uint32_t(setting.r[byte]) << (8*byte);
        }
        if (value != unsigned(site->settings[i]))
            reject("physical GPIO selectors differ from the fitted reference");
    }
    const std::array<IdString,5> ports{id_I,id_OE,id_Q,id_CLK,id_CLKIN};
    const CV::port_type_t physical[]{CV::DATAOUT,CV::OEIN,CV::DATAIN,CV::CLKOUT,CV::CLKIN};
    const CV::rnode_type_t types[]{CV::GOUT,CV::GOUT,CV::GIN,CV::DCMUX,CV::DCMUX};
    for (size_t i = 0; i < ports.size(); ++i) {
        auto port = ports[i];
        auto *net = cell->getPort(port);
        if (!net)
            continue;
        const auto &cut = site->cuts[i];
        auto expected = ctx->get_port(CV::GPIO,site->x,site->y,site->bi,physical[i],i == 2 ? 3 : 0);
        const WireId measured{CV::rnode_coords{types[i],uint8_t(cut.x),uint8_t(cut.y),uint16_t(cut.index)}};
        auto pin = cell->pin_data.find(port);
        if (expected != measured || pin == cell->pin_data.end() || pin->second.bel_pins.size() != 1 ||
            ctx->getBelPinWire(bel,pin->second.bel_pins[0]) != expected || !net->wires.count(expected))
            reject("a connected GPIO port lacks its measured native net-owned routing cut");
        if (port == id_Q) {
            if (net->driver.cell != cell || net->driver.port != port || ctx->getNetinfoSourceWire(net) != expected)
                reject("GPIO Q is not the declared native fabric source");
        } else {
            PortRef sink; sink.cell = const_cast<CellInfo *>(cell); sink.port = port;
            if (ctx->getNetinfoSinkWire(net,sink,0) != expected)
                reject("GPIO sink routing does not reach the declared native cut");
        }
    }
    return site;
}

int role(IdString port) { return port == id_I ? 0 : port == id_OE ? 1 : port == id_Q ? 2 : -1; }
} // namespace

bool Arch::gpio_core_reference(const CellInfo *cell, IdString port, TimingClockingInfo &timing) const
{
    const auto *site = reference_site(getCtx(),cell);
    if (!site)
        return false;
    const int r = role(port);
    if (r < 0 || !cell->getPort(port))
        return false;
    if (!(site->valid & (1u << r)))
        log_error("GPIO core reference has no '%s' observation for '%s'.\n",port.c_str(getCtx()),cell->name.c_str(getCtx()));
    const auto &b = site->arcs[0][r]; // Public native engine is slow100 only.
    timing.clock_port = port == id_Q ? id_CLKIN : id_CLK;
    timing.edge = RISING_EDGE;
    if (port == id_Q)
        timing.clockToQ = DelayQuad{b.a,b.b,b.c,b.d};
    else {
        timing.setup = DelayPair{b.a,b.a};
        timing.hold = DelayPair{b.b,b.b};
    }
    return true;
}

bool Arch::gpio_core_clock_prefix(const CellInfo *cell, IdString data_port, DelayQuad &prefix) const
{
    const auto *site = reference_site(getCtx(),cell);
    if (!site)
        return false;
    const int r = role(data_port);
    if (r < 0 || !(site->valid & (1u << r)))
        log_error("GPIO core reference has no clock-prefix role for '%s'.\n",cell->name.c_str(getCtx()));
    int rmin=std::numeric_limits<int>::max(), fmin=rmin;
    int rmax=std::numeric_limits<int>::min(), fmax=rmax;
    // The legacy pad/primitive envelopes cover all four corners. Translate
    // them conservatively using an interval over the same four-corner prefix.
    for (const auto &corner : site->clocks) {
        rmin=std::min(rmin,corner[r].a); rmax=std::max(rmax,corner[r].b);
        fmin=std::min(fmin,corner[r].c); fmax=std::max(fmax,corner[r].d);
    }
    prefix=DelayQuad{rmin,rmax,fmin,fmax};
    return true;
}

NEXTPNR_NAMESPACE_END
