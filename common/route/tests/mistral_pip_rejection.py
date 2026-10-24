#!/usr/bin/env python3
"""Compile Mistral pip rules and verify router1 applies them during routing."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
src = (root / 'mistral/arch.h').read_text()
assert 'fes_shell_keepout' not in src
start = src.index('    const char *pip_rejection_reason(')
end = src.index('    void netHaloShellTiles(', start)
router1 = (root / 'common/route/router1.cc').read_text()
signal_route = router1[router1.index('    bool route_arc('):router1.index('    bool route_const_arc(')]
constant_route = router1[router1.index('    bool route_const_arc('):router1.index('    delay_t find_slack_thresh(')]
for route in (signal_route, constant_route):
    assert route.count('ctx->pipAllowedForNet(pip, net_info)') >= 2
    assert route.index('if (!ctx->pipAllowedForNet(pip, net_info))') < route.index('if (!ctx->checkPipAvail(pip))')

code = r'''
#include <cassert>
#include <cstring>
#include <vector>
struct Cell { int type = 0; };
struct NetInfo { struct { Cell *cell = nullptr; } driver; };
struct WireId { int id; explicit WireId(int i): id(i) {} bool is_nextpnr_created() const { return true; } };
struct PipId { int src = 0, dst = 1; };
struct Membership { bool value = false; bool count(PipId) const { return value; } };
struct BaseArch {
    bool pip_free = true;
    NetInfo *bound_net = nullptr;
    bool checkPipAvail(PipId) const { return pip_free; }
    virtual bool checkPipAvailForNet(PipId pip, const NetInfo *net) const {
        return pipAllowedForNet(pip, net) && (bound_net == nullptr || bound_net == net);
    }
    virtual bool pipAllowedForNet(PipId, const NetInfo *) const { return true; }
    virtual bool pipOutsideNetHalo(PipId) const { return false; }
    virtual bool netConstrainedToHalo(const NetInfo *) const { return false; }
};
struct Arch : BaseArch {
    bool blocked = false, preserves = true, pll_sink = true;
    bool fes_fence_active = false, fes_has_reserved_rect = false;
    bool slot_driver = false, slot_user = true, socket = false, halo = true, shell = false;
    bool shell_pin = false;
    int id_altera_pll = 1;
    Membership pll_private_pips, pll_private_ref_pips;
    bool is_pip_blocked(PipId) const { return blocked; }
    bool net_feeds_pll(const NetInfo *) const { return pll_sink; }
    bool fes_pip_preserves_cram(PipId) const { return preserves; }
    bool fes_cell_is_slot(Cell *) const { return slot_driver; }
    bool fes_net_touches_slot(const NetInfo *) const { return slot_user; }
    bool fes_pip_in_socket(PipId) const { return socket; }
    bool fes_pip_in_plug_halo(PipId) const { return halo; }
    bool fes_pip_hits_bound_shell_pin(PipId) const { return shell_pin; }
    bool fes_pip_reaches_net_shell_tile(PipId p, const NetInfo *) const {
        return shell || fes_pip_hits_bound_shell_pin(p);
    }
''' + src[start:end] + r'''
};
void check(Arch a, const char *expected) {
    NetInfo n; PipId p;
    auto reason = a.pip_rejection_reason(p, &n);
    assert(reason && std::strcmp(reason, expected) == 0);
    assert(!a.pip_allowed_for_net(p, &n));
}
bool route_to_gin(const Arch &arch, const NetInfo *net, const std::vector<PipId> &path) {
    constexpr int GIN_WIRE = 42;
    if (path.empty() || path.back().dst != GIN_WIRE)
        return false;
    for (PipId pip : path) {
        if (!arch.pipAllowedForNet(pip, net))
            return false;
    }
    return true;
}
int main() {
    Arch a; NetInfo n; PipId p;
    assert(a.pipAllowedForNet(p, &n));
    a.blocked = true; check(a, "reserved_route_requires_other_source"); a = Arch();

    a.pll_private_pips.value = true;
    check(a, "pll_private_output_requires_pll_driver");
    Cell pll; pll.type = a.id_altera_pll;
    n.driver.cell = &pll;
    assert(a.pipAllowedForNet(p, &n));
    a = Arch();
    a.pll_private_ref_pips.value = true; a.pll_sink = false;
    check(a, "pll_private_reference_requires_pll_sink"); a = Arch();

    a.preserves = false; check(a, "fes_scaffold_cram_preservation"); a = Arch();
    a.fes_fence_active = a.fes_has_reserved_rect = true;
    a.slot_driver = true; a.halo = false;
    check(a, "fes_slot_route_outside_plug_halo_and_shell");
    assert(a.pipOutsideNetHalo(p));
    assert(a.netConstrainedToHalo(&n));
    // A slot route may enter its own shell tile while remaining halo constrained.
    a.shell = true; assert(a.pipAllowedForNet(p, &n));
    assert(a.pipOutsideNetHalo(p));
    // A wire already touching a bound shell pin is not an outside-halo pip.
    a.shell = false; a.shell_pin = true;
    assert(!a.pipOutsideNetHalo(p));
    assert(a.pipAllowedForNet(p, &n));
    a.shell_pin = false; a.halo = true;
    assert(!a.pipOutsideNetHalo(p));
    assert(a.pipAllowedForNet(p, &n));
    a.halo = false;
    a.slot_driver = false; a.slot_user = false; a.socket = true;
    assert(!a.netConstrainedToHalo(&n));
    check(a, "fes_non_slot_route_enters_socket");

    // Occupancy remains separate: router1 may consider this pip for rip-up,
    // while the ownership-aware check still reports its current owner.
    a = Arch(); NetInfo owner;
    a.bound_net = &owner;
    assert(a.pipAllowedForNet(p, &n));
    assert(!a.checkPipAvailForNet(p, &n));

    // Coleco v2's audio.locked shape: a PLL-driven output reaches a GIN through
    // its private pips, with a sink in the active FES slot. The old shell-wide
    // keepout rejected this route; the PLL and net-specific fences admit it.
    a = Arch();
    a.fes_fence_active = a.fes_has_reserved_rect = true;
    a.slot_user = true;
    a.socket = true;
    a.pll_private_pips.value = true;
    Cell audio_pll; audio_pll.type = a.id_altera_pll;
    NetInfo audio_locked; audio_locked.driver.cell = &audio_pll;
    const std::vector<PipId> audio_locked_to_gin = {{0, 1}, {1, 42}};
    assert(route_to_gin(a, &audio_locked, audio_locked_to_gin));
}
'''
with tempfile.TemporaryDirectory(prefix='mistral-pip-rejection-') as tmp:
    cpp, exe = Path(tmp) / 'test.cc', Path(tmp) / 'test'
    cpp.write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Mistral pip rejection and PLL-to-GIN route regression passed')
