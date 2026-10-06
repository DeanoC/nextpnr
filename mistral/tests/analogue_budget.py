#!/usr/bin/env python3
"""Exercise repair cancellation and final timing on a real routed checkpoint.

Use a passing menu DDR checkpoint from mistral/tests/gpurouter/menu_ddr_seed1.
The test disables ordinary table repair, forces an unattainable analogue
margin, and verifies that cancelled work produces a legal, replayable routing.
Early expiry preserves the exact RBF; later expiry may keep an improved
round. An impossible clock then verifies the final gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nextpnr', type=Path, required=True)
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    original = json.loads(args.checkpoint.read_text())
    results = {}
    for name, candidates, budget, rounds, fail in [
            ('disabled', 0, 0, 0, False),
            ('early', 0, 0.000001, 100, False),
            ('reroute', 0, 5, 100, False),
            # Imported routes may be fixed until a full reroute creates
            # movable wires. Allow that first round before testing candidates.
            ('candidate', 4, 20, 100, False),
            ('timing-failure', 0, 0.000001, 100, True)]:
        out = args.output/name
        out.mkdir()
        design = json.loads(json.dumps(original))
        settings = design['modules']['top']['settings']
        # Decimal points avoid interpreting all-0/1 strings as binary JSON properties.
        for key, value in dict(repairRounds=0, analogueRounds=rounds,
                               analogueCandidates=candidates, analogueSlack=1000000,
                               analogueRipSlack=1000000, analogueTimeBudget=budget).items():
            settings['gpurouter/'+key] = str(float(value)) if key not in (
                'repairRounds', 'analogueRounds', 'analogueCandidates') else int(value)
        settings['gpurouter/cpu'] = '1'
        settings.pop('gpurouter/telemetryPath', None)
        if fail:
            (out/'clocks.sdc').write_text(
                'create_clock -name impossible -period 0.1 [get_nets {ddr.scanout.clk}]\n')
        (out/'input.json').write_text(json.dumps(design)+'\n')
        cmd = [str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7', '--json', str(out/'input.json'),
               '--no-pack', '--no-place', '--router', 'gpu', '--gpu-cpu', '--compress-rbf',
               '--rbf', str(out/'core.rbf'), '--write', str(out/'final.json'),
               '--report', str(out/'timing.json'), '--detailed-timing-report']
        if fail:
            cmd += ['--sdc', str(out/'clocks.sdc')]
        with (out/'build.log').open('w') as log:
            code = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=120).returncode
        text = (out/'build.log').read_text()
        assert code == (1 if fail else 0), (name, code, text[-3000:])
        summary = json.loads((out/'timing.json').read_text())['timing_summary']
        assert summary['final_analogue_model']
        clocks = summary['clocks'].values()
        assert all(c['setup_wns_ns'] >= 0 and c['hold_wns_ns'] >= 0 for c in clocks) != fail
        if name != 'disabled':
            assert 'Analogue repair time budget exhausted' in text, name
        if name == 'reroute':
            assert 're-routing' in text
        if name == 'candidate':
            assert 'Setting up the GPU router for candidate routes' in text
        expiry_location = ('rerouting' if 'exhausted during rerouting' in text else
                           'candidate selection' if 'exhausted during candidate selection' in text else
                           'between rounds' if 'Analogue repair time budget exhausted' in text else 'disabled')
        if expiry_location in ('rerouting', 'candidate selection'):
            assert 'restoring the routing' in text
        final = json.loads((out/'final.json').read_text())['modules']['top']
        bindings = {n: net.get('attributes', {}).get('ROUTING') for n, net in final['netnames'].items()}
        rbf = hashlib.sha256((out/'core.rbf').read_bytes()).hexdigest()
        if name == 'disabled':
            base_bindings, base_rbf = bindings, rbf
        elif name in ('early', 'timing-failure'):
            assert bindings == base_bindings and rbf == base_rbf, name
        else:
            # A completed round may improve the saved best before cancellation.
            # Reload its final checkpoint without routing: no partial trial or
            # hidden simulator state may be needed to reproduce the bitstream.
            replay = [str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7',
                      '--json', str(out/'final.json'), '--no-pack', '--no-place', '--no-route',
                      '--compress-rbf', '--rbf', str(out/'replayed.rbf')]
            with (out/'replay.log').open('w') as log:
                replay_code = subprocess.run(replay, stdout=log, stderr=subprocess.STDOUT, timeout=120).returncode
            assert replay_code == 0, name
            assert hashlib.sha256((out/'replayed.rbf').read_bytes()).hexdigest() == rbf, name
        results[name] = dict(compiler_returncode=code, timing_summary=summary, rbf_sha256=rbf,
                             expiry_location=expiry_location)
        print('PASS', name, flush=True)
    (args.output/'results.json').write_text(json.dumps(results, indent=2)+'\n')


if __name__ == '__main__':
    main()
