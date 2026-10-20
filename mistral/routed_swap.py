#!/usr/bin/env python3
"""Isolated full-route acceptance for Mistral occupied-LUT swaps (default off).

Baseline artifacts are never modified. `retained` is the only selected output
bundle; failed/unsafe trials remain in candidate for diagnosis. Exit status and
timing qualification always describe retained, not a rejected candidate.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ARTIFACTS = ('diagnostic.rbf', 'routed.json', 'timing.json', 'endpoints.json')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    tmp.replace(path)


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def rows(snapshot):
    if snapshot.get('complete') is not True or snapshot.get('unsupported') != []:
        raise ValueError('incomplete native endpoint coverage')
    if snapshot.get('schema') != 6 or snapshot.get('final_analogue') is not True:
        raise ValueError('not a native final analogue snapshot')
    if snapshot.get('unsupported_details') != {}:
        raise ValueError('unresolved or missing native interface gap details')
    if not isinstance(snapshot.get('physical_obligations'), list) or snapshot['physical_obligations']:
        raise ValueError('unresolved or missing physical interface obligations')
    if not isinstance(snapshot.get('reset_contracts'), dict) or snapshot['reset_contracts']:
        # Structure alone is never physical reset qualification. No qualified
        # reset model is implemented, so no declaration can bypass this gate.
        raise ValueError('unqualified or missing reset contracts')
    if not isinstance(snapshot.get('forwarded_clocks'), dict) or snapshot['forwarded_clocks']:
        raise ValueError('unqualified or missing forwarded clock contracts')
    if not isinstance(snapshot.get('constant_outputs'), dict):
        raise ValueError('missing constant output channel contracts')
    if not snapshot.get('clocks') or not snapshot.get('endpoints'):
        raise ValueError('empty clock or endpoint coverage')
    endpoints = snapshot['endpoints']
    if not isinstance(endpoints, dict):
        raise ValueError('invalid endpoint inventory')
    endpoint_keys = {}
    for key in endpoints:
        identity = json.loads(key)
        if (not isinstance(identity, list) or len(identity) != 2 or
                not all(isinstance(v, str) and v for v in identity) or tuple(identity) in endpoint_keys):
            raise ValueError('invalid or duplicate endpoint identity')
        endpoint_keys[tuple(identity)] = key
    for clock in snapshot['clocks'].values():
        if len(clock) != 8 or not all(finite(v) for v in clock[:7]) or not isinstance(clock[7], str):
            raise ValueError('invalid clock constraint')
    constraints = snapshot.get('io_constraints')
    if not isinstance(constraints, list):
        raise ValueError('invalid IO constraints')
    seen = set()
    for constraint in constraints:
        if (not isinstance(constraint, dict) or
                not isinstance(constraint.get('port'), str) or not constraint['port'] or
                not isinstance(constraint.get('clock'), str) or constraint['clock'] not in snapshot['clocks'] or
                type(constraint.get('input')) is not bool or type(constraint.get('fall')) is not bool or
                not finite(constraint.get('min')) or not finite(constraint.get('max')) or
                constraint['min'] > constraint['max']):
            raise ValueError('invalid IO constraint')
        key = (constraint['port'], constraint['input'])
        if key in seen:
            raise ValueError('ambiguous IO constraints')
        seen.add(key)
    outputs = {c['port'] for c in constraints if not c['input']}
    contracts = snapshot.get('output_contracts')
    if not isinstance(contracts, dict) or set(contracts) != outputs:
        raise ValueError('missing output boundary contracts')
    for contract in contracts.values():
        if (not isinstance(contract, dict) or not isinstance(contract.get('net'), str) or
                not contract['net'] or type(contract.get('direction')) is not int or
                contract['direction'] not in (1, 2) or
                not isinstance(contract.get('cells'), dict) or not contract['cells']):
            raise ValueError('invalid output boundary contract')
        for cell in contract['cells'].values():
            if (not isinstance(cell, dict) or not isinstance(cell.get('type'), str) or not cell['type'] or
                    any(not isinstance(cell.get(f), dict) for f in ('ports', 'params', 'pins', 'attrs')) or
                    not cell['ports']):
                raise ValueError('invalid output cell contract')
            for connection in cell['ports'].values():
                if (not isinstance(connection, list) or len(connection) != 2 or
                        type(connection[0]) is not int or connection[0] not in (0, 1, 2) or
                        not isinstance(connection[1], str)):
                    raise ValueError('invalid output port binding')
    registered = snapshot.get('registered_contracts')
    primitive = snapshot.get('primitive_clocks')
    if not isinstance(registered, dict) or not isinstance(primitive, dict):
        raise ValueError('missing registered boundary or primitive clock contracts')
    for key, requirement in primitive.items():
        identity = json.loads(key)
        if (not isinstance(requirement, dict) or not isinstance(identity, list) or len(identity) != 2 or
                requirement.get('clock') not in snapshot['clocks'] or
                not isinstance(requirement.get('limits'), list) or not isinstance(requirement.get('route'), list) or
                len(requirement['limits']) != 3 or len(requirement['route']) != 2 or
                not all(finite(v) and v >= 0 for v in requirement['limits'] + requirement['route']) or
                requirement['route'][0] > requirement['route'][1]):
            raise ValueError('invalid primitive clock contract')
        if not isinstance(requirement.get('margins'), list) or len(requirement['margins']) != 3 or not all(finite(v) and v >= 0 for v in requirement['margins']):
            raise ValueError('invalid primitive clock margins')
        wave = snapshot['clocks'][requirement['clock']]
        distortion = requirement['route'][1] - requirement['route'][0]
        if any(a < b for a, b in zip((wave[0], wave[2] - distortion, wave[4] - distortion), requirement['limits'])):
            raise ValueError('primitive clock waveform outside model')
    channel_endpoints = set()
    for key, contract in registered.items():
        identity = json.loads(key)
        if (not isinstance(contract, dict) or not isinstance(identity, list) or len(identity) != 3 or type(identity[2]) is not bool or
                not all(isinstance(v, str) and v for v in identity[:2]) or
                any(not isinstance(contract.get(f), dict) for f in ('params', 'pins', 'attrs', 'ports', 'channels')) or
                not contract['channels'] or not isinstance(contract.get('net'), str) or not contract['net'] or
                not isinstance(contract.get('bel'), str) or not contract['bel'] or not isinstance(contract.get('type'), str)):
            raise ValueError('invalid registered boundary contract')
        if (not isinstance(contract.get('port'), str) or (contract['port'], identity[2]) not in seen or
                not isinstance(contract['ports'].get(identity[1]), list) or
                len(contract['ports'][identity[1]]) != 2 or contract['ports'][identity[1]][1] != contract['net'] or
                any(not isinstance(contract['attrs'].get(k), str) or not contract['attrs'][k]
                    for k in ('LOC', 'NEXTPNR_GPIO_TIMING_PROFILE') + (() if identity[2] else ('BOARD_MODEL_FAR_C',)))):
            raise ValueError('missing registered physical binding')
        if not identity[2] and contracts[contract['port']]['net'] != contract['net']:
            raise ValueError('registered output net mismatch')
        for name, channel in contract['channels'].items():
            if not isinstance(channel, dict):
                raise ValueError('invalid registered channel')
            endpoint = channel.get('endpoint')
            if (not name or not isinstance(endpoint, list) or len(endpoint) != 2 or
                    not all(isinstance(v, str) and v for v in endpoint) or endpoint[0] != identity[0] or
                    type(channel.get('edge')) is not int or channel['edge'] not in (0, 1) or
                    not isinstance(channel.get('clock_port'), str)):
                raise ValueError('invalid registered channel identity')
            endpoint_key = endpoint_keys.get(tuple(endpoint))
            if endpoint_key is None or endpoint_key in channel_endpoints:
                raise ValueError('missing or duplicate registered channel endpoint')
            channel_endpoints.add(endpoint_key)
            for field in ('setup', 'hold', 'clock_to_q_rise', 'clock_to_q_fall'):
                interval = channel.get(field)
                if not isinstance(interval, list) or len(interval) != 2 or not all(finite(v) for v in interval) or interval[0] > interval[1]:
                    raise ValueError('invalid registered channel model')
            clock_key = next((k for k in primitive if json.loads(k) == [identity[0], channel['clock_port']]), None)
            if clock_key is None:
                raise ValueError('missing primitive clock requirements')
            domain = [primitive[clock_key]['clock'], channel['edge']]
            if any(row['capture' if identity[2] else 'launch'] != domain for row in snapshot['endpoints'][endpoint_key]):
                raise ValueError('registered channel clock mismatch')
    # No private endpoint can escape its physical pad/channel contract.
    if any('$timing$' in identity[1] and key not in channel_endpoints for identity, key in endpoint_keys.items()):
        raise ValueError('unbound registered endpoint')
    for key, proof in snapshot['constant_outputs'].items():
        identity = json.loads(key)
        if (not isinstance(identity, list) or len(identity) != 2 or identity[1] != 'I' or
                not isinstance(identity[0], str) or not identity[0] or key in endpoints or
                not isinstance(proof, dict) or proof.get('reason') != 'constant-open-drain-data' or
                type(proof.get('value')) is not int or proof['value'] != 0 or
                proof.get('scope') != 'I-only; OE and input feedback remain independent' or
                proof.get('oe_endpoint') != [identity[0], 'OE'] or
                (identity[0], 'OE') not in endpoint_keys or
                not isinstance(proof.get('chain'), dict) or not proof['chain'] or
                not isinstance(proof.get('cell'), dict)):
            raise ValueError('invalid constant output channel proof')
        cell = proof['cell']
        bindings = [c for boundary in contracts.values() for name, c in boundary['cells'].items()
                    if name == identity[0]]
        if (cell.get('type') != 'MISTRAL_IO' or cell.get('params') != {} or
                not bindings or any(json.dumps(c, sort_keys=True) != json.dumps(cell, sort_keys=True) for c in bindings)):
            raise ValueError('unbound constant output channel')
    static = snapshot.get('static_endpoints')
    if not isinstance(static, dict):
        raise ValueError('missing static endpoint proofs')
    for endpoint, proof in static.items():
        if (not isinstance(proof, dict) or endpoint in snapshot['endpoints'] or proof.get('reason') != 'constant-no-launch-transition' or
                type(proof.get('value')) is not int or proof['value'] not in (0, 1) or
                not proof.get('chain') or not proof.get('capture') or not proof.get('sink')):
            raise ValueError('invalid static endpoint proof')
    result = {}
    for endpoint, values in snapshot['endpoints'].items():
        if not values:
            raise ValueError('empty endpoint rows')
        for row in values:
            key = (endpoint, tuple(row['launch']), tuple(row['capture']))
            if key in result:
                raise ValueError('duplicate endpoint clock pair')
            if type(row['setup_timed']) is not bool or type(row['hold_related']) is not bool:
                raise ValueError('invalid timing classification')
            for field, enabled in [('setup', row['setup_timed']), ('setup_window', row['setup_timed']),
                                   ('hold', row['hold_related']), ('min_path', True), ('max_path', True)]:
                if enabled and not finite(row[field]):
                    raise ValueError('missing/nonfinite endpoint timing')
                if not enabled and row[field] is not None:
                    raise ValueError('unexpected timed margin')
            result[key] = row
    return result


def compare(before, after):
    """Strict nonregression, including positive margins; no report intersection."""
    try:
        old, new = rows(before), rows(after)
        if before['clocks'] != after['clocks']:
            return False, 'clock constraints changed'
        if before['io_constraints'] != after['io_constraints']:
            return False, 'IO boundary constraints changed'
        # Preserve JSON types too: Python otherwise equates True with 1.
        if json.dumps(before['output_contracts'], sort_keys=True) != json.dumps(after['output_contracts'], sort_keys=True):
            return False, 'output boundary semantics changed'
        if json.dumps(before['registered_contracts'], sort_keys=True) != json.dumps(after['registered_contracts'], sort_keys=True):
            return False, 'registered boundary semantics changed'
        def clock_contracts(snapshot):
            return {k: {f: v for f, v in c.items() if f not in ('route', 'margins')} for k, c in snapshot['primitive_clocks'].items()}
        if json.dumps(clock_contracts(before), sort_keys=True) != json.dumps(clock_contracts(after), sort_keys=True):
            return False, 'primitive clock requirements changed'
        for key, value in before['primitive_clocks'].items():
            if any(b < a for a, b in zip(value['margins'], after['primitive_clocks'][key]['margins'])):
                return False, 'primitive clock margin regression'
        for field in ('constant_outputs', 'reset_contracts', 'physical_obligations'):
            if json.dumps(before[field], sort_keys=True) != json.dumps(after[field], sort_keys=True):
                return False, 'interface contract changed: ' + field
        if before['static_endpoints'] != after['static_endpoints']:
            return False, 'static endpoint semantics/coverage changed'
        if old.keys() != new.keys():
            return False, 'endpoint clock-pair coverage changed'
        improved = False
        for key, a in old.items():
            b = new[key]
            for field in ('setup_timed', 'hold_related', 'setup_window'):
                if a[field] != b[field]:
                    return False, 'timing classification/window changed: ' + str(key)
            for field, enabled in [('setup', a['setup_timed']), ('hold', a['hold_related'])]:
                if enabled and b[field] < a[field]:
                    return False, field + ' regression: ' + str(key)
            if not a['setup_timed'] and (b['max_path'] > a['max_path'] or b['min_path'] < a['min_path']):
                return False, 'unrelated path extrema regressed: ' + str(key)
            improved |= a['setup_timed'] and b['setup'] >= a['setup'] + 20
        return (True, 'complete routed endpoint nonregression with >=20ps setup gain') if improved else (False, 'no routed setup gain')
    except (ValueError, KeyError, TypeError) as error:
        return False, 'coverage failure: ' + str(error)


def completed(directory, receipt):
    if receipt.get('returncode') not in (0, 1) or receipt.get('exception'):
        raise ValueError('route did not complete normally')
    for name in ARTIFACTS:
        if not (directory / name).is_file() or not (directory / name).stat().st_size:
            raise ValueError('missing artifact: ' + name)
    report = json.loads((directory / 'timing.json').read_text())
    if report['timing_summary']['final_analogue_model'] is not True or not report['fmax']:
        raise ValueError('missing final analogue signoff')
    # Force full JSON parsing before selecting a bundle.
    json.loads((directory / 'routed.json').read_text())
    snapshot = json.loads((directory / 'endpoints.json').read_text())
    if snapshot.get('final_analogue') is not True:
        raise ValueError('missing native analogue inventory')
    return report, snapshot


def select(output, name):
    tmp = output / 'retained.tmp'
    tmp.symlink_to(name, target_is_directory=True)
    tmp.replace(output / 'retained')


def guarded_candidate(output, baseline_receipt, run_candidate):
    baseline, old = completed(output / 'baseline', baseline_receipt)
    select(output, 'baseline')
    decision = {'selected': 'baseline', 'accepted': False, 'reason': 'candidate not completed',
                'baseline_coverage': {'complete': old.get('complete'),
                    'endpoints': len(old.get('endpoints', {})), 'unsupported': len(old.get('unsupported', []))}}
    write_json(output / 'decision.json', decision)
    try:
        receipt = run_candidate()
        candidate, new = completed(output / 'candidate', receipt)
        decision['candidate_coverage'] = {'complete': new.get('complete'),
            'endpoints': len(new.get('endpoints', {})), 'unsupported': len(new.get('unsupported', []))}
        accepted, reason = compare(old, new)
        if accepted:
            if baseline['fmax'].keys() != candidate['fmax'].keys():
                accepted, reason = False, 'clock summary coverage changed'
            else:
                for key, a in baseline['fmax'].items():
                    b = candidate['fmax'][key]
                    if (a['constraint'] != b['constraint'] or not finite(b['achieved']) or
                            b['achieved'] < a['achieved']):
                        accepted, reason = False, 'clock summary regression: ' + key
        decision.update(accepted=accepted, reason=reason)
        if accepted:
            select(output, 'candidate')
            decision['selected'] = 'candidate'
    except Exception as error:
        decision['reason'] = 'candidate failure: ' + str(error)
    selected = output / decision['selected']
    decision['artifacts'] = {name: digest(selected / name) for name in ARTIFACTS}
    decision['retained_returncode'] = json.loads((selected / 'receipt.json').read_text())['returncode']
    decision['timing_summary'] = json.loads((selected / 'timing.json').read_text())['timing_summary']
    write_json(output / 'decision.json', decision)
    return decision


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'json', 'qsf', 'sdc', 'guidance', 'output'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--device', required=True)
    p.add_argument('--budget', type=int, default=64)
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--freq', type=float, default=100)
    p.add_argument('--router', choices=('gpu', 'router1', 'router2'), default='gpu')
    p.add_argument('--runner-root', type=Path, help='FES phase-lease runner module root (required for gpu)')
    p.add_argument('--timeout', type=float, default=600)
    p.add_argument('--route-budget', type=float, default=1200)
    a = p.parse_args()
    if not 1 <= a.budget <= 64 or not 0 < a.timeout <= 600 or a.route_budget < 2 * a.timeout:
        p.error('budget must be 1..64, timeout 0..600, route-budget >= twice timeout')
    if a.router == 'gpu' and not a.runner_root:
        p.error('GPU routes require the FES phase-lease runner')
    for key in ('GPU_LEASE_IDS', 'GPU_LEASE_VENDOR', 'HIP_VISIBLE_DEVICES', 'CUDA_VISIBLE_DEVICES', 'ROCR_VISIBLE_DEVICES'):
        if os.environ.get(key):
            p.error('GPU device/vendor pin is not permitted: ' + key)
    output = a.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    inputs = output / 'inputs'
    inputs.mkdir()
    sources = {'compiler': a.binary, 'placed.json': a.json, 'constraints.qsf': a.qsf,
               'clocks.sdc': a.sdc, 'guidance.json': a.guidance}
    for name, source in sources.items():
        shutil.copy2(source, inputs / name)
        (inputs / name).chmod(0o555 if name == 'compiler' else 0o444)
    write_json(output / 'inputs.json', {name: {'source': str(source.resolve()), 'sha256': digest(inputs / name)}
                                      for name, source in sources.items()})
    env = dict(os.environ, FES_GPU_LEASE_OWNER='pcxt-agent', GPU_LEASE_MAX_PER_OWNER='2', NEXTPNR_REQUIRE_GPU='1')
    runner = subprocess.run
    if a.router == 'gpu':
        root = a.runner_root.resolve()
        sys.path[:0] = [str(root), str(root / 'scripts')]
        from fes_build_common import _run_command
        runner = _run_command
    base = [str(inputs / 'compiler'), '--device', a.device, '--json', str(inputs / 'placed.json'),
            '--qsf', str(inputs / 'constraints.qsf'), '--sdc', str(inputs / 'clocks.sdc'),
            '--seed', str(a.seed), '--freq', str(a.freq), '--router', a.router,
            '--placer-heap-timingweight', '2000', '--placer-heap-critexp', '5', '--no-pack', '--no-place',
            '--rbf', 'diagnostic.rbf', '--compress-rbf', '--write', 'routed.json', '--report', 'timing.json',
            '--detailed-timing-report', '--timing-report-paths', '1024', '--routed-endpoints', 'endpoints.json']
    charged = 0.0

    def run(name):
        nonlocal charged
        directory = output / name
        directory.mkdir()
        command = base.copy()
        if name == 'candidate':
            command += ['--critical-swap-budget', str(a.budget), '--critical-cohort-report',
                        str(inputs / 'guidance.json'), '--critical-swap-routed-trial']
        receipt = {'command': command, 'binary_sha256': digest(inputs / 'compiler'),
                   'timeout': a.timeout, 'environment': {k: env[k] for k in
                   ('FES_GPU_LEASE_OWNER', 'GPU_LEASE_MAX_PER_OWNER', 'NEXTPNR_REQUIRE_GPU')}}
        start = time.monotonic()
        try:
            if charged + a.timeout > a.route_budget:
                raise ValueError('route work budget exhausted')
            with (directory / 'nextpnr.log').open('w') as log:
                options = {'gpu_label': 'pcxt265-guard-' + name} if a.router == 'gpu' else {}
                result = runner(command, cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=a.timeout, **options)
                receipt['returncode'] = result.returncode
        except Exception as error:
            receipt['exception'] = str(error)
        finally:
            receipt['charged_seconds'] = time.monotonic() - start  # includes every lease wait
            charged += receipt['charged_seconds']
            receipt['artifacts'] = {name: digest(directory / name) for name in ARTIFACTS if (directory / name).is_file()}
            write_json(directory / 'receipt.json', receipt)
        return receipt

    baseline = run('baseline')
    try:
        decision = guarded_candidate(output, baseline, lambda: run('candidate'))
        print(json.dumps(decision, indent=2))
        return decision['retained_returncode']
    finally:
        write_json(output / 'budget.json', {'charged_seconds': charged, 'allowance_seconds': a.route_budget,
                                         'accounting': 'full wall time, including lease waits'})


if __name__ == '__main__':
    sys.exit(main())
