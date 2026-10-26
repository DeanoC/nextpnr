/* Local FF reference profile. Default models and unrelated primitives are unchanged. */
#include "nextpnr.h"
#include "util.h"
#include "ff_core_reference_data.h"

#include <algorithm>
#include <cmath>
#include <limits>

NEXTPNR_NAMESPACE_BEGIN

bool Arch::ff_core_reference(const CellInfo *cell, IdString port, TimingClockingInfo &timing) const
{
    using namespace ::ff_core_reference;
    if (!bool_or_default(getCtx()->settings, getCtx()->id("mistral/ff_core_reference"), false))
        return false;
    if (!port.in(id_Q, id_DATAIN, id_ENA, id_SCLR) || (port != id_Q && !cell->getPort(port)))
        return false; // An inactive calibration port describes no path.
    auto reject = [&](const char *reason) {
        log_error("FF core reference does not cover '%s': %s.\n", cell->name.c_str(getCtx()), reason);
    };
    if (getChipName() != "5CSEBA6U23I7" || cell->type != id_MISTRAL_FF || args.lab_clkb)
        reject("require 5CSEBA6U23I7 and the common-CLKA graph");
    for (auto p : {id_SLOAD, id_SDATA})
        if (cell->getPort(p) || (p == id_SLOAD && cell->get_pin_state(p) != PIN_0))
            reject("synchronous load/data overrides are uncharacterized");
    auto *clock_net = cell->getPort(id_CLK);
    const auto clk_state = cell->get_pin_state(id_CLK);
    if (!clock_net || !clock_net->driver.cell || (clk_state != PIN_SIG && clk_state != PIN_INV))
        reject("missing or unsupported driven clock");
    auto *clock = clock_net->clkconstr.get();
    if (!clock || clock->period.minDelay() != clock->period.maxDelay() ||
        clock->high.minDelay() != clock->high.maxDelay() || clock->low.minDelay() != clock->low.maxDelay() ||
        clock->period.minDelay() < 10000 || clock->high.minDelay() < 5000 || clock->low.minDelay() < 5000 ||
        clock->high.minDelay()+clock->low.minDelay() != clock->period.minDelay() ||
        std::abs(clock->high.minDelay()-clock->low.minDelay()) > 1)
        reject("require an exact clock period >=10ns and nominal 50 percent duty cycle");
    auto active = [&](IdString p, CellPinState inactive, bool invertible) {
        auto *net = cell->getPort(p);
        auto state = cell->get_pin_state(p);
        if (!net) {
            if (state != inactive)
                reject("a disconnected control lacks its explicit inactive constant");
            return false;
        }
        if (!net->driver.cell || (state != PIN_SIG && !(invertible && state == PIN_INV)))
            reject("a control is undriven or has unsupported polarity");
        return true;
    };
    const bool ena = active(id_ENA, PIN_1, true);
    const bool sync = active(id_SCLR, PIN_0, true);
    const bool clear = active(id_ACLR, PIN_1, false);
    if (clear && !bool_or_default(getCtx()->settings, getCtx()->id("mistral/ff_reset_reference"), false))
        reject("an active clear also requires the separate FF reset reference");
    if (cell->getPort(id_DATAIN) && (!cell->getPort(id_DATAIN)->driver.cell || cell->get_pin_state(id_DATAIN) != PIN_SIG))
        reject("data is undriven or has unsupported polarity");
    unsigned mode = (clk_state == PIN_INV ? ClockInv : 0) | (ena ? Enabled : 0) |
                    (ena && cell->get_pin_state(id_ENA) == PIN_INV ? EnableInv : 0) |
                    (sync ? SyncClear : 0) |
                    (sync && cell->get_pin_state(id_SCLR) == PIN_INV ? SyncInv : 0) |
                    (clear ? AsyncClear : 0);
    constexpr unsigned logical_mask = ClockInv|Enabled|EnableInv|SyncClear|SyncInv|AsyncClear;
    unsigned mask = logical_mask;
    int role = port == id_Q ? 0 : port == id_ENA ? 4 : port == id_SCLR ? 5 : -1;
    const bool owner = cell->bel != BelId() && getBoundBelCell(cell->bel) == cell;
    if (owner) {
        if (getBelType(cell->bel) != id_MISTRAL_FF)
            reject("the placed owner is not an FF BEL");
        const auto &info = bel_data(cell->bel).lab_data;
        mode |= labs.at(info.lab).is_mlab ? Mlab : 0;
        mask |= Mlab;
    }
    if (bitstream_configured) {
        if (!owner)
            reject("a configured profile requires its actual placed FF owner");
        using CV = mistral::CycloneV;
        const auto &info = bel_data(cell->bel).lab_data;
        const auto &lab = labs.at(info.lab);
        const auto &alm = lab.alms.at(info.alm);
        const auto block = lab.is_mlab ? CV::MLAB : CV::LAB;
        auto read = [&](CV::bmux_type_t field, int index, int type) {
            CV::bmux_setting_t value{};
            if (!cyclonev->bmux_get(block, cell->bel.pos, field, index, value) || value.type != type)
                reject("missing or mis-typed physical selector");
            return value.s;
        };
        const auto clock_select = read(ALMInfo::ctrl_group(info.idx) == 0 ? CV::TCLK_SEL : CV::BCLK_SEL,
                                       info.alm, CV::MT_MUX);
        const int ci = clock_select == CV::CLK0 ? 0 : clock_select == CV::CLK1 ? 1 : clock_select == CV::CLK2 ? 2 : -1;
        if (ci < 0)
            reject("unsupported physical clock/enable selector");
        const CV::bmux_type_t source[] = {CV::CLK0_SEL, CV::CLK1_SEL, CV::CLK2_SEL};
        const CV::bmux_type_t inversion[] = {CV::CLK0_INV, CV::CLK1_INV, CV::CLK2_INV};
        const CV::bmux_type_t enable[] = {CV::EN0_EN, CV::EN1_EN, CV::EN2_EN};
        const CV::bmux_type_t enable_inv[] = {CV::EN0_NINV, CV::EN1_NINV, CV::EN2_NINV};
        if (read(source[ci], 0, CV::MT_MUX) != CV::CLKA ||
            bool(read(inversion[ci], 0, CV::MT_BOOL)) != (clk_state == PIN_INV) ||
            bool(read(enable[ci], 0, CV::MT_BOOL)) != ena ||
            (ena && bool(read(enable_inv[ci], 0, CV::MT_BOOL)) != (cell->get_pin_state(id_ENA) == PIN_INV)))
            reject("physical clock/enable bits disagree with the logical FF");
        const auto half_clear = info.idx/2 == 0 ? CV::TSCLR_DIS : CV::BSCLR_DIS;
        const bool physical_sync = !read(CV::SCLR_DIS, 0, CV::MT_BOOL) && !read(half_clear, info.alm, CV::MT_BOOL);
        if (physical_sync != sync || (sync && bool(read(CV::SCLR_INV, 0, CV::MT_BOOL)) !=
                                                    (cell->get_pin_state(id_SCLR) == PIN_INV)))
            reject("physical synchronous-clear bits disagree with the logical FF");
        const bool load = read(CV::SLOAD_EN, 0, CV::MT_BOOL) &&
                          read(info.idx/2 == 0 ? CV::TSLOAD_EN : CV::BSLOAD_EN, info.alm, CV::MT_BOOL);
        if (load)
            reject("an uncharacterized synchronous load is physically enabled");
        // Walk actual net-owned edges, not parsed virtual wire names.
        auto cut = [&](IdString p) {
            auto *net = cell->getPort(p);
            PortRef sink; sink.cell = const_cast<CellInfo *>(cell); sink.port = p;
            WireId cursor = getCtx()->getNetinfoSinkWire(net, sink, 0);
            const WireId root = getCtx()->getNetinfoSourceWire(net);
            int hops = 0;
            while (cursor.is_nextpnr_created() && cursor != root) {
                auto found = net->wires.find(cursor);
                if (++hops > 8 || found == net->wires.end() || found->second.pip == PipId())
                    reject("incomplete native FF routing cut");
                cursor = getPipSrcWire(found->second.pip);
            }
            if (cursor == WireId())
                reject("missing native FF routing cut");
            return cursor;
        };
        auto physical = [&](CV::port_type_t p, int bi, int pi) {
            return get_port(block, cell->bel.pos.x(), cell->bel.pos.y(), bi, p, pi);
        };
        // assign_control_sets deliberately shares virtual CLK0 even for
        // physical CLK/ENA1. The RBF's ci above owns the model identity.
        if (cut(id_CLK) != physical(CV::CLKIN, -1, 0))
            reject("clock routing does not reach the characterized dedicated CLKA cut");
        const int enable_inputs[] = {2, 3, 0};
        if (ena && cut(id_ENA) != physical(CV::DATAIN, -1, enable_inputs[ci]))
            reject("enable routing disagrees with the physical selector");
        if (sync && (read(CV::SCLR_SEL, 0, CV::MT_MUX) != CV::DIN3 ||
                     cut(id_SCLR) != physical(CV::DATAIN, -1, 3)))
            reject("synchronous clear does not reach its declared native cut");
        if (sync)
            mode |= SyncDin3;
        const auto clear_index = read(ALMInfo::ctrl_group(info.idx) == 0 ? CV::TCLR_SEL : CV::BCLR_SEL,
                                      info.alm, CV::MT_NUM);
        if (clear_index > 1)
            reject("unsupported physical asynchronous-clear selector");
        const auto clear_source = read(clear_index ? CV::ACLR1_SEL : CV::ACLR0_SEL, 0, CV::MT_MUX);
        if (read(clear_index ? CV::ACLR1_INV : CV::ACLR0_INV, 0, CV::MT_BOOL))
            reject("inverted physical asynchronous clear is uncharacterized");
        if (clear) {
            int input = clear_source == CV::DIN3 ? 3 : clear_source == CV::DIN2 ? 2 : -1;
            if (input < 0 || cut(id_ACLR) != physical(CV::DATAIN, -1, input))
                reject("asynchronous clear does not reach its declared native cut");
        } else if (clear_source != (clear_index ? CV::ACLR1 : CV::ACLR0)) {
            reject("a disconnected clear still selects a live physical input");
        }
        if (cell->getPort(id_DATAIN)) {
            const bool ef = wires_connected(alm.sel_ef[info.idx/2], alm.ff_in[info.idx]);
            const bool lut = wires_connected(alm.comb_out[info.idx/2], alm.ff_in[info.idx]);
            const CV::bmux_type_t pkreg[] = {CV::TPKREG1, CV::TPKREG0, CV::BPKREG1, CV::BPKREG0};
            if (ef == lut || bool(read(pkreg[info.idx], info.alm, CV::MT_BOOL)) != ef)
                reject("data routing disagrees with the physical input selection");
            if (ef) {
                auto selected = read(info.idx/2 == 0 ? CV::TEF_SEL : CV::BEF_SEL, info.alm, CV::MT_MUX);
                if (selected != CV::E && selected != CV::F)
                    reject("unsupported E/F input selection");
                auto pin = info.idx/2 == 0 ? (selected == CV::E ? CV::E0 : CV::F1)
                                          : (selected == CV::E ? CV::E1 : CV::F0);
                if (cut(id_DATAIN) != physical(pin, info.alm, -1))
                    reject("data routing disagrees with the selected E/F input");
                if (port == id_DATAIN)
                    role = selected == CV::E ? 2 : 3;
            } else if (cut(id_DATAIN) != alm.comb_out[info.idx/2]) {
                reject("local LUT data does not end at its declared native cut");
            }
            if (port == id_DATAIN && !ef)
                role = 1;
        }
        mode |= unsigned(ci) << 2;
        mask = 2047;
    }
    Bounds combined{};
    combined.qr_min = combined.qf_min = std::numeric_limits<int>::max();
    combined.qr_max = combined.qf_max = std::numeric_limits<int>::min();
    int setup = std::numeric_limits<int>::min(), hold = setup;
    bool found = false;
    for (const auto &entry : modes) {
        if ((entry.key & mask) != mode)
            continue;
        // This public profile matches the backend's native slow100 model.
        // Other corner data is retained for a separately qualified engine.
        const auto &b = entry.corners[0];
        if (role == 0 && (b.valid & 1)) {
            combined.qr_min = std::min(combined.qr_min, b.qr_min);
            combined.qr_max = std::max(combined.qr_max, b.qr_max);
            combined.qf_min = std::min(combined.qf_min, b.qf_min);
            combined.qf_max = std::max(combined.qf_max, b.qf_max);
            found = true;
        } else if (role != 0) {
            // Before physical routing, both legal data families contribute
            // conservative placement bounds. Never guess a PKREG selection.
            for (int r : {1,2,3,4,5}) {
                if ((role >= 0 && r != role) || (role < 0 && r > 3) || !(b.valid & (1u << r)))
                    continue;
                setup = std::max(setup, b.setup[r-1]);
                hold = std::max(hold, b.hold[r-1]);
                found = true;
            }
        }
    }
    if (!found)
        log_error("FF core reference has no complete sample for '%s' mode 0x%x mask 0x%x port %s.\n",
                  cell->name.c_str(getCtx()), mode, mask, port.c_str(getCtx()));
    if (port == id_Q) {
        timing.clockToQ = DelayQuad{combined.qr_min, combined.qr_max, combined.qf_min, combined.qf_max};
    } else {
        timing.setup = DelayPair{setup, setup};
        timing.hold = DelayPair{hold, hold};
    }
    return true;
}

NEXTPNR_NAMESPACE_END
