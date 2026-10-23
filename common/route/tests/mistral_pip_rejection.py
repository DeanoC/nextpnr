#!/usr/bin/env python3
"""Compile Mistral pip rules and verify router1 filters net-forbidden pips."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
src = (root / 'mistral/arch.h').read_text()
start = src.index('    const char *pip_rejection_reason(')
end = src.index('    void netHaloShellTiles(', start)
router1 = (root / 'common/route/router1.cc').read_text()
signal_route = router1[router1.index('    bool route_arc('):router1.index('    bool route_const_arc(')]
constant_route = router1[router1.index('    bool route_const_arc('):router1.index('    delay_t find_slack_thresh(')]
assert signal_route.count('ctx->pipAllowedForNet(pip, net_info)') >= 2
assert constant_route.count('ctx->pipAllowedForNet(pip, net_info)') >= 2
code = r'''
#include <cassert>
#include <cstring>
#include <unordered_set>
struct Cell { int type = 0; };
struct NetInfo { struct { Cell *cell = nullptr; } driver; bool is_global = false; int name = 1; };
struct WireId { int id; explicit WireId(int i): id(i) {} bool is_nextpnr_created() const { return true; } };
struct PipId { int src = 0, dst = 1; };
struct Membership { bool value = false; bool count(PipId) const { return value; } };
struct BaseArch {
    bool checkPipAvail(PipId) const { return true; }
    virtual bool pipAllowedForNet(PipId, const NetInfo *) const { return true; }
    virtual bool pipOutsideNetHalo(PipId) const { return false; }
    virtual bool netConstrainedToHalo(const NetInfo *) const { return false; }
};
struct Arch : BaseArch {
    bool blocked = false, preserves = true, pll_sink = true;
    bool fes_fence_active = false, fes_has_reserved_rect = false;
    bool fes_shell_keepout = false;
    bool slot_driver = false, slot_user = true, socket = false, halo = true, shell = false;
    bool shell_pin = false;
    std::unordered_set<int> fes_shell_socket_nets;
    int id_altera_pll = 1;
    Membership pll_private_pips, pll_private_ref_pips;
    bool is_pip_blocked(PipId) const { return blocked; }
    bool net_feeds_pll(const NetInfo *) const { return pll_sink; }
    bool fes_pip_preserves_cram(PipId) const { return preserves; }
    bool fes_cell_is_slot(Cell *) const { return slot_driver; }
    bool fes_net_touches_slot(const NetInfo *) const { return slot_user; }
    bool fes_pip_in_socket(PipId) const { return socket; }
    bool fes_shell_net_may_enter_socket(const NetInfo *net) const {
        return net == nullptr || net->is_global || fes_shell_socket_nets.count(net->name) != 0;
    }
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
int main() {
    Arch a; NetInfo n; PipId p;
    assert(a.pip_allowed_for_net(p, &n));
    a.blocked = true; check(a, "reserved_route_requires_other_source"); a = Arch();
    a.pll_private_pips.value = true; check(a, "pll_private_output_requires_pll_driver"); a = Arch();
    a.pll_private_ref_pips.value = true; a.pll_sink = false;
    check(a, "pll_private_reference_requires_pll_sink"); a = Arch();
    a.preserves = false; check(a, "fes_scaffold_cram_preservation"); a = Arch();
    a.fes_fence_active = a.fes_has_reserved_rect = true;
    a.slot_driver = true; a.halo = false;
    check(a, "fes_slot_route_outside_plug_halo_and_shell");
    assert(a.pipOutsideNetHalo(p));
    assert(a.netConstrainedToHalo(&n));
    // The shell tile is a per-net exception, so the pip stays outside the halo.
    a.shell = true; assert(a.pip_allowed_for_net(p, &n));
    assert(a.pipOutsideNetHalo(p));
    // A wire that already touches a bound shell pin is not an outside-halo pip.
    a.shell = false; a.shell_pin = true;
    assert(!a.pipOutsideNetHalo(p));
    assert(a.pip_allowed_for_net(p, &n));
    a.shell_pin = false; a.halo = true;
    assert(!a.pipOutsideNetHalo(p));
    assert(a.pip_allowed_for_net(p, &n));
    a.halo = false;
    a.slot_driver = false; a.slot_user = false; a.socket = true;
    assert(!a.netConstrainedToHalo(&n));
    check(a, "fes_non_slot_route_enters_socket");

    a = Arch(); a.fes_shell_keepout = true; a.socket = true;
    assert(!a.pipAllowedForNet(p, &n));
    n.is_global = true;
    assert(a.pipAllowedForNet(p, &n));
}
'''
with tempfile.TemporaryDirectory(prefix='mistral-pip-rejection-') as tmp:
    cpp, exe = Path(tmp) / 'test.cc', Path(tmp) / 'test'
    cpp.write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Mistral router1 shell pip keepout regression passed')
