#!/usr/bin/env python3
"""Fresh pack/place/route of the original 100 MHz synthesis with address targets.

No frozen placement, routing, PLL migrations, route selection or RTL changes.
Retain unsuccessful compiler results as diagnostics; never label them signoff.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--nextpnr', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    parser.add_argument('--seed', type=int, default=2)
    parser.add_argument('--router', choices=['gpu', 'router2'], default='gpu')
    parser.add_argument('--gpu-cpu', action='store_true', help='Use the GPU router CPU reference backend')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = args.source_root.resolve()
    source = root/'build/fes-ramtest-100/synth.json'
    qsf = root/'cores/fes-ramtest/constraints.qsf'
    reference = load(here/'baseline-reference.json')['builds']['failing']
    if sha(source) != reference['source_sha256']['build/fes-ramtest-100/synth.json']:
        raise ValueError('Expected the original failing artifact\'s unchanged synthesis')
    build_inputs = load(root/'build/fes-ramtest-100/build-inputs.json')
    if sha(qsf) != build_inputs['source_inputs']['cores/fes-ramtest/constraints.qsf']:
        raise ValueError('Pin constraints differ from the original synthesis provenance')
    module = load(source)['modules']['top']
    for section in ('cells', 'netnames'):
        for name, item in module[section].items():
            if any(key in item.get('attributes', {}) for key in ('ROUTING', 'NEXTPNR_BEL', 'FES_PINMAP_V1')):
                raise ValueError(f'{name}: input must be fresh synthesis, not a physical checkpoint')
    if module.get('settings'):
        raise ValueError('Unexpected settings in the original synthesis')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out/'runner-source.py').write_bytes(Path(__file__).read_bytes())
    (out/'synth.json').write_bytes(source.read_bytes())
    (out/'pins.qsf').write_bytes(qsf.read_bytes())
    (out/'address.sdc').write_bytes((here/'address-target.sdc').read_bytes())
    executable = args.nextpnr.resolve()
    executable_sha = sha(executable)
    command = [str(executable), '--device', '5CSEBA6U23I7', '--json', str(out/'synth.json'),
               '--qsf', str(out/'pins.qsf'), '--sdc', str(out/'address.sdc'), '--freq', '74.25',
               '--router', args.router, '--seed', str(args.seed), '--compress-rbf',
               '--write', str(out/'final.json'), '--report', str(out/'timing.json'),
               '--detailed-timing-report', '--timing-report-paths', '128', '--rbf', str(out/'core.rbf')]
    if args.gpu_cpu:
        if args.router != 'gpu':
            raise ValueError('--gpu-cpu requires --router gpu')
        command.append('--gpu-cpu')
    (out/'command.json').write_text(json.dumps(command, indent=2)+'\n')
    started = time.monotonic()
    with (out/'build.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=1200)
    elapsed = time.monotonic()-started
    report = load(out/'timing.json')
    final = load(out/'final.json')['modules']['top']
    if not report['timing_summary']['final_analogue_model']:
        raise ValueError('Final analogue timing is required')
    if sum(c['type'] == 'MISTRAL_FF' and bool(re.fullmatch(r'dq_buf\[\d+\]\.dq_sample_MISTRAL_FF_Q', n))
           for n, c in final['cells'].items()) != 16:
        raise ValueError('Historical fabric capture implementation changed')
    address = {}
    for net in report['detailed_net_timings']:
        for endpoint in net['endpoints']:
            cell = final['cells'][endpoint['cell']]
            if cell['type'] != 'MISTRAL_OB' or not endpoint['cell'].startswith('SDRAM_A_MISTRAL_OB_PAD'):
                continue
            if endpoint['port'] != 'I' or net['event'] != 'posedge ram_clock.clocks[0]':
                raise ValueError('Unexpected address launch relationship')
            match = re.fullmatch(r'SDRAM_A_MISTRAL_OB_PAD(?:_(\d+))?', endpoint['cell'])
            bit = int(match[1] or 0)
            if bit in address:
                raise ValueError('Ambiguous address endpoint')
            early, late = endpoint['delay']
            address[bit] = dict(cell=endpoint['cell'], net=net['net'], arrival_ns=[early, late],
                                setup_margin_ns=6.5-late, hold_margin_ns=early)
    if set(address) != set(range(13)):
        raise ValueError('Missing address timing endpoints')
    clocks = report['timing_summary']['clocks']
    complete_timing = all(c['setup_wns_ns'] >= 0 and c['hold_wns_ns'] >= 0 for c in clocks.values())
    receipt = dict(classification='fresh constrained pack/place/route; native address optimization, not board signoff',
                   source_root=str(root), seed=args.seed, router=args.router, gpu_cpu=args.gpu_cpu,
                   command=command, nextpnr_sha256=executable_sha, script_sha256=sha(out/'runner-source.py'),
                   elapsed_seconds=elapsed, compiler_returncode=result.returncode,
                   all_address_targets_met=all(row['setup_margin_ns'] >= -1e-5 for row in address.values()),
                   complete_timing_pass=result.returncode == 0 and complete_timing,
                   address=address, timing_summary=report['timing_summary'],
                   core_rbf_sha256=sha(out/'core.rbf'), core_rbf_bytes=(out/'core.rbf').stat().st_size,
                   file_sha256={name: sha(out/name) for name in
                                ('runner-source.py', 'synth.json', 'pins.qsf', 'address.sdc', 'command.json',
                                 'final.json', 'timing.json', 'build.log')},
                   limits=['All placement and routing are newly generated; no route-only causal isolation.',
                           '6.5 ns is an empirical native GPIO optimization target, not a complete chip-pin budget.',
                           'Uncharacterized pad/package/board and HPS minimum-delay qualifications remain separate.',
                           'Hardware validation is required for this new artifact.'])
    (out/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps({key: receipt[key] for key in ('compiler_returncode', 'all_address_targets_met',
                                                 'complete_timing_pass', 'address', 'timing_summary')}, indent=2))
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
