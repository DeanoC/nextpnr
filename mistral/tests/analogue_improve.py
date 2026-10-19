#!/usr/bin/env python3
"""Exercise analogue improvement of a design that already meets signoff.

Use a passing menu DDR checkpoint from mistral/tests/gpurouter/menu_ddr_seed1
(an HPS f2sdram design, like the FES menu of nextpnr issue 231). A routed
reload is locked as a scaffold, which no candidate may move, so the test keeps
the checkpoint's placement, drops its routing and routes it again on the CPU
reference backend, with ordinary table repair disabled:

- improvement disabled: no improvement pass runs;
- default settings: the design passes the analogue check, so candidate passes
  run on its near-critical sinks; no clock may end with less setup slack, a
  lower Fmax or negative hold slack than with improvement disabled, and the
  returned checkpoint must reproduce its bitstream without routing;
- immediate budget expiry: no improvement pass runs and the bitstream is the
  one improvement disabled gives.

Only the checkpoint's placement is used, so any build may make it:

  python3 mistral/tests/gpurouter/menu_ddr_seed1.py \\
    --nextpnr build/nextpnr-mistral --gpu-cpu --output /tmp/menu-improve-base
  python3 mistral/tests/analogue_improve.py --nextpnr build/nextpnr-mistral \\
    --checkpoint /tmp/menu-improve-base/routed.json --output /tmp/menu-improve-check
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
    parser.add_argument('--timeout', type=int, default=600)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    original = json.loads(args.checkpoint.read_text())
    nextpnr = str(args.nextpnr.resolve())
    results = {}
    for name, settings in [('disabled', dict(analogueImproveRounds=0)),
                           ('improve', {}),
                           ('expired', dict(analogueTimeBudget='0.000001'))]:
        out = args.output/name
        out.mkdir()
        design = json.loads(json.dumps(original))
        top = design['modules']['top']
        top['attributes']['step'] = 'place'
        for net in top['netnames'].values():
            net.get('attributes', {}).pop('ROUTING', None)
        module = top['settings']
        module['gpurouter/repairRounds'] = 0
        module['gpurouter/cpu'] = '1'
        module.pop('gpurouter/telemetryPath', None)
        for key in ('analogueImproveRounds', 'analogueImproveBand', 'analogueTimeBudget', 'analogueSlack'):
            module.pop('gpurouter/'+key, None)
        for key, value in settings.items():
            module['gpurouter/'+key] = value
        (out/'input.json').write_text(json.dumps(design)+'\n')
        cmd = [nextpnr, '--device', '5CSEBA6U23I7', '--json', str(out/'input.json'),
               '--no-pack', '--no-place', '--router', 'gpu', '--gpu-cpu', '--compress-rbf',
               '--rbf', str(out/'core.rbf'), '--write', str(out/'final.json'),
               '--report', str(out/'timing.json'), '--detailed-timing-report']
        with (out/'build.log').open('w') as log:
            code = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout).returncode
        text = (out/'build.log').read_text()
        assert code == 0, (name, code, text[-3000:])
        assert 'Analogue signoff check' in text, name
        report = json.loads((out/'timing.json').read_text())
        assert report['timing_summary']['final_analogue_model'], name
        clocks = report['timing_summary']['clocks']
        assert clocks and all(c['setup_wns_ns'] >= 0 and c['hold_wns_ns'] >= 0 for c in clocks.values()), name
        ran = 'near-critical sinks tried' in text
        assert ran == (name == 'improve'), (name, ran)
        rbf = hashlib.sha256((out/'core.rbf').read_bytes()).hexdigest()
        results[name] = dict(clocks=clocks, fmax={k: v['achieved'] for k, v in report['fmax'].items()},
                             rbf_sha256=rbf, rounds=text.count('Analogue signoff improvement round'))
        print('PASS', name, flush=True)

    off, on = results['disabled'], results['improve']
    assert results['expired']['rbf_sha256'] == off['rbf_sha256']
    for clock, timing in off['clocks'].items():
        assert on['clocks'][clock]['setup_wns_ns'] >= timing['setup_wns_ns'], clock
    for clock, fmax in off['fmax'].items():
        assert on['fmax'][clock] >= fmax, clock
    if on['rbf_sha256'] != off['rbf_sha256']:
        # A kept pass changed the routing: the returned checkpoint alone must
        # reproduce the bitstream, without any simulator state of the passes.
        out = args.output/'improve'
        replay = [nextpnr, '--device', '5CSEBA6U23I7', '--json', str(out/'final.json'),
                  '--no-pack', '--no-place', '--no-route', '--compress-rbf', '--rbf', str(out/'replayed.rbf')]
        with (out/'replay.log').open('w') as log:
            assert subprocess.run(replay, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout).returncode == 0
        assert hashlib.sha256((out/'replayed.rbf').read_bytes()).hexdigest() == on['rbf_sha256']
    print('PASS improvement keeps every clock: ' + ', '.join(
        f"{k} {off['fmax'][k]:.2f} -> {on['fmax'][k]:.2f} MHz" for k in off['fmax']), flush=True)
    (args.output/'results.json').write_text(json.dumps(results, indent=2)+'\n')


if __name__ == '__main__':
    main()
