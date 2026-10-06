#!/usr/bin/env python3
"""Audit qualified registered SDRAM timing channels, including clock cuts.

Coverage is separate from positive slack and from board/hardware qualification.
Use the checkpoint and detailed report from the same compiler invocation.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

from constraint_coverage import inventory


def enabled(cell, name):
    value = cell.get('parameters', {}).get(name, '0')
    return int(value, 2) != 0 if isinstance(value, str) else value != 0


def channels(cell, input_direction):
    kind = cell['type']
    if input_direction:
        if kind == 'MISTRAL_DDRIN':
            return {'high': 'posedge', 'low': 'negedge'}
        if kind == 'MISTRAL_SDRIO' and enabled(cell, 'IOREG_IN'):
            return ({'high': 'posedge', 'low': 'negedge'} if enabled(cell, 'IOREG_IN_DDR')
                    else {'rise': 'posedge'})
    else:
        if kind == 'MISTRAL_SDROUT':
            return {'data': 'posedge'}
        if kind == 'MISTRAL_DDROUT':
            return ({'rise': 'posedge', 'fall': 'negedge'} if enabled(cell, 'DDR_HIGH')
                    else {'fall': 'posedge', 'rise': 'negedge'})
        if kind == 'MISTRAL_SDRIO' and enabled(cell, 'IOREG_OUT') and enabled(cell, 'IOREG_OE'):
            return {'data': 'posedge', 'oe': 'posedge'}
    return {}


def audit(module, report):
    result = inventory(module)
    if report.get('timing_summary', {}).get('final_analogue_model') is not True:
        raise ValueError('Requires a final analogue report')
    endpoints = {}
    for net in report['detailed_net_timings']:
        for endpoint in net['endpoints']:
            endpoints.setdefault((endpoint['cell'], endpoint['port']), []).append(endpoint)

    def clock_bits(name):
        return module.get('netnames', {}).get(name, {}).get('bits')

    def matches(event, edge, bits):
        parts = event.split(' ', 1) if isinstance(event, str) else []
        return len(parts) == 2 and parts[0] == edge and bits and clock_bits(parts[1]) == bits

    rows = []
    for row in result['ports']:
        if row['exempt_structural_constant']:
            continue
        cell = module['cells'].get(row['pad_cell'], {})
        input_direction = row['direction'] == 'input'
        expected = channels(cell, input_direction) if cell else {}
        constraint = row['constraint']
        failures = []
        if not expected:
            failures.append('unsupported or unregistered pad direction')
        if row['timing_profile'] != 'QUARTUS_17_0_2_RAMTEST':
            failures.append('missing qualified pad profile')
        if constraint is None:
            failures.append('missing IO delay declaration')
        direction = 'read' if input_direction else 'write'
        for channel, edge in expected.items():
            prefix = f'PAD$timing${direction}${channel}'
            local = prefix + '$register'
            external = prefix + '$external'
            sink = local if input_direction else external
            source = external if input_direction else local
            candidates = endpoints.get((row['pad_cell'], sink), [])
            errors = list(failures)
            if len(candidates) != 1:
                errors.append('missing or ambiguous channel endpoint')
            endpoint = candidates[0] if len(candidates) == 1 else {}
            if endpoint:
                if endpoint.get('source', {}).get('cell') != row['pad_cell'] or endpoint['source'].get('port') != source:
                    errors.append('incorrect channel source')
                register_event = endpoint.get('event') if input_direction else endpoint.get('source', {}).get('event')
                clock_pin = 'CLKIN' if input_direction and cell['type'] == 'MISTRAL_SDRIO' else 'CLK'
                if not matches(register_event, edge, cell['connections'].get(clock_pin)):
                    errors.append('incorrect register clock or edge')
                external_event = endpoint.get('source', {}).get('event') if input_direction else endpoint.get('event')
                if constraint and not matches(external_event, 'negedge' if constraint['fall'] else 'posedge',
                                              clock_bits(constraint['clock'])):
                    errors.append('incorrect external reference clock or edge')
                delay = endpoint.get('delay', [])
                if len(delay) != 2 or not all(isinstance(x, (int, float)) and not isinstance(x, bool) and
                                             math.isfinite(x) for x in delay) or delay[0] > delay[1]:
                    errors.append('invalid channel arrival interval')
                for check in ('setup', 'hold'):
                    slack = endpoint.get(check + '_slack_ns')
                    if endpoint.get(check + '_checked') is not True or not isinstance(slack, (int, float)) or isinstance(slack, bool) or not math.isfinite(slack):
                        errors.append(f'{check} check absent, cut or invalid')
            rows.append(dict(port=row['port'], direction=row['direction'], channel=channel,
                             pad_cell=row['pad_cell'], covered=not errors, errors=errors,
                             endpoint=endpoint))
        if not expected:
            rows.append(dict(port=row['port'], direction=row['direction'], channel=None,
                             pad_cell=row['pad_cell'], covered=False, errors=failures, endpoint={}))
    covered = all(row['covered'] for row in rows) and result['all_active_directions_declared']
    valid_rows = [row for row in rows if row['covered']]
    result.update(classification='registered IO check coverage; not board or hardware signoff',
                  channels=rows, expected_channels=len(rows), checked_channels=len(valid_rows),
                  all_channels_checked=covered,
                  all_channel_slacks_nonnegative=covered and all(
                      row['endpoint'][check + '_slack_ns'] >= 0 for row in valid_rows for check in ('setup', 'hold')),
                  timing_summary=report['timing_summary'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(json.loads(args.checkpoint.read_text())['modules']['top'], json.loads(args.report.read_text()))
    result['source_sha256'] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in (args.checkpoint, args.report, Path(__file__))}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: result[key] for key in ('active_directions', 'declared_directions',
          'expected_channels', 'checked_channels', 'all_channels_checked', 'all_channel_slacks_nonnegative')}, indent=2))
    raise SystemExit(0 if result['all_channels_checked'] else 1)


if __name__ == '__main__':
    main()
