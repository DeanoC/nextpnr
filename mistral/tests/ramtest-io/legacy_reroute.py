#!/usr/bin/env python3
"""Reroute three historical address nets with all other physical resources frozen."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess

from legacy_checkpoint import probe_paths

NETS = {f'sdram.sdram_a[{bit}]' for bit in (9, 10, 12)}
ENDPOINTS = {f'SDRAM_A_MISTRAL_OB_PAD_{bit}.I' for bit in (9, 10, 12)}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text())


def bit_map(before, after, require_names=True):
    result = {}
    if require_names and before['netnames'].keys() != after['netnames'].keys():
        raise ValueError('Logical nets changed')
    for name, net in before['netnames'].items():
        if name not in after['netnames']:
            continue
        bits = after['netnames'][name]['bits']
        if len(bits) != len(net['bits']):
            raise ValueError('Net width changed')
        for old, new in zip(net['bits'], bits):
            if old in result and result[old] != new:
                raise ValueError('Net alias changed')
            result[old] = new
    return result


def routes(net):
    parts = net.get('attributes', {}).get('ROUTING', '').split(';')
    # Strength increases when freezing a checkpoint; compare actual wires/pips.
    return sorted((parts[i], parts[i+1]) for i in range(0, len(parts)-2, 3))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True, help='baseline-replay-failing directory')
    parser.add_argument('--nextpnr', type=Path, required=True)
    parser.add_argument('--decoder', type=Path, required=True, help='mistral-cv executable')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reuse', action='store_true', help='Validate existing route/probe runs instead of repeating them')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent
    reference = load(here/'baseline-reference.json')
    original_sha = reference['builds']['failing']['original_rbf_sha256']
    if sha(args.baseline/'baseline.rbf') != original_sha:
        raise ValueError('Expected exact original failing artifact')
    data = load(args.baseline/'baseline-input.json')
    for name in NETS:
        data['modules']['top']['netnames'][name]['attributes'].pop('ROUTING')
    settings = data['modules']['top']['settings']
    settings['router1/cleanupReroute'] = '0'
    settings['router1/fullCleanupReroute'] = '0'
    settings['router2/estimateWeight'] = '0'
    out = args.output
    common = [str(args.nextpnr), '--device', '5CSEBA6U23I7', '--no-pack', '--no-place', '--compress-rbf']
    route_cmd = common + ['--json', str(out/'input.json'), '--router', 'router2', '--seed', '2',
                          '--rbf', str(out/'candidate.rbf'), '--write', str(out/'final.json'),
                          '--report', str(out/'timing.json'), '--detailed-timing-report']
    if args.reuse:
        if load(out/'input.json') != data:
            raise ValueError('Existing route input is not the controlled experiment')
    else:
        (out/'input.json').write_text(json.dumps(data))
        with (out/'route.log').open('w') as log:
            subprocess.run(route_cmd, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    if 'Program finished normally.' not in (out/'route.log').read_text():
        raise ValueError('Route run did not finish successfully')
    before = load(args.baseline/'baseline-final.json')['modules']['top']
    final = load(out/'final.json')
    after = final['modules']['top']
    mapping = bit_map(before, after)
    if before['cells'].keys() != after['cells'].keys():
        raise ValueError('Cells changed')
    for name, cell in before['cells'].items():
        other = after['cells'][name]
        for field in ('type', 'parameters', 'port_directions'):
            if cell[field] != other[field]:
                raise ValueError(f'{name}: {field} changed')
        if {p: [mapping.get(b, b) for b in bits] for p, bits in cell['connections'].items()} != other['connections']:
            raise ValueError(f'{name}: connectivity changed')
        for attr in ('NEXTPNR_BEL', 'LOC', 'FES_PINMAP_V1'):
            if cell['attributes'].get(attr) != other['attributes'].get(attr):
                raise ValueError(f'{name}: physical placement/pin map changed')
    changed = {n for n in before['netnames'] if routes(before['netnames'][n]) != routes(after['netnames'][n])}
    if changed != NETS:
        raise ValueError(f'Unexpected changed routing: {changed}')
    timing = load(out/'timing.json')['timing_summary']
    if timing != load(args.baseline/'baseline-timing.json')['timing_summary'] or not timing['final_analogue_model']:
        raise ValueError('Internal timing changed or final model missing')
    probe = copy.deepcopy(final)
    module = probe['modules']['top']
    source = data['modules']['top']
    mapping = bit_map(source, module, require_names=False)
    # Plain packed reload drops ports; restore metadata with renumbered net bits.
    module['ports'] = {n: {**p, 'bits': [mapping[b] for b in p['bits']]} for n, p in source['ports'].items()}
    module['settings']['timing/io_delays'] = '[]'
    clock = next(c for c in module['cells'].values() if c['type'] == 'MISTRAL_DDROUT' and
                 c['attributes'].get('LOC') == 'PIN_AD20')
    clock['attributes']['NEXTPNR_GPIO_TIMING_PROFILE'] = 'QUARTUS_17_0_2_RAMTEST'
    clock['attributes']['BOARD_MODEL_FAR_C'] = '30P'
    if args.reuse:
        if load(out/'probe-input.json') != probe:
            raise ValueError('Existing probe input changed')
    else:
        (out/'probe-input.json').write_text(json.dumps(probe))
        cmd = common + ['--no-route', '--json', str(out/'probe-input.json'),
                        '--sdc', str(args.baseline/'probe.sdc'), '--rbf', str(out/'probe.rbf'),
                        '--write', str(out/'probe-final.json'), '--report', str(out/'probe-timing.json'),
                        '--detailed-timing-report']
        with (out/'probe.log').open('w') as log:
            result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=300)
        if result.returncode not in (0, 1):
            raise ValueError('Probe failed unexpectedly')
    if sha(out/'probe.rbf') != sha(out/'candidate.rbf'):
        raise ValueError('Probe changed physical artifact')
    report = load(out/'probe-timing.json')
    if not report['timing_summary']['final_analogue_model']:
        raise ValueError('Probe missing final analogue model')
    paths = probe_paths(report, module)
    old_paths = reference['builds']['failing']['runs']['probe']['boundary_paths']
    if paths['reads'] != old_paths['reads'] or {k for k in paths['outputs'] if paths['outputs'][k] != old_paths['outputs'][k]} != ENDPOINTS:
        raise ValueError('Timing outside the selected three boundaries changed')
    for rbf, decoded in [(out/'candidate.rbf', out/'decoded.bt'),
                         (args.baseline/'baseline.rbf', out/'reference-decoded.bt')]:
        subprocess.run([str(args.decoder), 'decomp', '5CSEBA6U23I7', str(rbf), str(decoded)],
                       check=True, timeout=120)
    def nonroutes(path):
        return sorted(line for line in path.read_text().splitlines() if not line.startswith('r '))
    configuration = nonroutes(out/'decoded.bt')
    if configuration != nonroutes(out/'reference-decoded.bt'):
        raise ValueError('Decoded nonrouting bitstream configuration changed')
    comparison = {}
    for endpoint in sorted(ENDPOINTS):
        new = paths['outputs'][endpoint]['arrival_ns'][1]
        old = old_paths['outputs'][endpoint]['arrival_ns'][1]
        passing = reference['builds']['passing']['runs']['probe']['boundary_paths']['outputs'][endpoint]['arrival_ns'][1]
        comparison[endpoint] = dict(passing_ns=round(passing, 3), failing_ns=round(old, 3),
                                    candidate_ns=round(new, 3), improvement_ns=round(old-new, 3))
    if not all(row['improvement_ns'] > 0 for row in comparison.values()):
        raise ValueError('Candidate did not improve every selected boundary')
    receipt = dict(classification='controlled route-only hardware-test candidate; no hardware result or board signoff',
                   original_failing_rbf_sha256=original_sha, candidate_rbf_sha256=sha(out/'candidate.rbf'),
                   candidate_rbf_bytes=(out/'candidate.rbf').stat().st_size, candidate_rbf_path=str(out/'candidate.rbf'),
                   changed_routes=sorted(changed), unchanged_cells=len(before['cells']),
                   unchanged_decoded_configuration_lines=len(configuration), unchanged_read_boundaries=16,
                   unchanged_other_output_boundaries=49, internal_timing=timing, address_comparison=comparison,
                   route_command=route_cmd, nextpnr_sha256=sha(args.nextpnr), decoder_sha256=sha(args.decoder),
                   script_sha256=sha(Path(__file__)),
                   file_sha256={f: sha(out/f) for f in ('input.json', 'final.json', 'timing.json', 'route.log',
                                                       'probe-input.json', 'probe-final.json', 'probe-timing.json', 'probe.log',
                                                       'decoded.bt', 'reference-decoded.bt')},
                   limits=['Native GPIO boundary timing excludes unqualified unregistered pad and board delays.',
                           'Synthetic zero-delay probe is not a SDRAM timing requirement or hardware failure.',
                           'All logic, placement, pin maps, PLL/GPIO configuration and other routes are unchanged.',
                           'Hardware comparison with the original failing artifact is required to test causality.'])
    (out/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(comparison, indent=2))


if __name__ == '__main__':
    main()
