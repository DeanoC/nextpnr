#!/usr/bin/env python3
"""Audit controlled DQ input selectors against the decoded native tester."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from pad_summary import summarize
from clock_summary import summarize as clock_summary

SELECTORS = ('RB_T1_SEL_IREG_CFF_DELAY', 'SET_T3_FOR_CDATA0IN', 'SET_T3_FOR_CDATA1IN')


def settings(text, pins):
    # Decompilation omits defaults. Check pin presence independently so a
    # truncated or unrelated bitstream cannot masquerade as a zero selector.
    result = {}
    for pin in pins:
        if not re.search(r'^.* ; ' + re.escape(pin) + r'$', text, re.M):
            raise ValueError('Missing decoded pin ' + pin)
        values = {}
        for selector in SELECTORS:
            matches = re.findall(r'^s DQS16\.[^:]+:' + selector + r'\.\d+ (\S+) ; ' + re.escape(pin) + r'$', text, re.M)
            if len(matches) > 1:
                raise ValueError('Ambiguous decoded selector ' + pin + '/' + selector)
            values[selector] = matches[0] if matches else '0'
            if values[selector] != '0':
                raise ValueError('Nonzero input selector ' + pin + '/' + selector)
        result[pin] = values
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=Path)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--database-doc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    evidence = json.loads((args.project/'evidence.json').read_text())
    if not evidence.get('zero_input_delays') or set(evidence['variants']) != {'ramtest-pads', 'ramtest-sdr-pads'}:
        raise ValueError('Expected controlled SDR and DDR evidence')
    docs = args.database_doc.read_text()
    for selector in SELECTORS:
        row = next(line for line in docs.splitlines() if re.search(r'\|\s*'+selector+r'\s*\|', line))
        if row.split('|')[5].strip() != '0':
            raise ValueError('Database default is not zero: ' + selector)
    decoded = {}
    fabric = {}
    clock_requirements = {}
    for variant in evidence['variants']:
        folder = args.project/variant
        qsf = (folder/'top.qsf').read_text()
        pins = re.findall(r'^set_location_assignment PIN_(\w+) -to SDRAM_DQ\[\d+\]$', qsf, re.M)
        if len(pins) != 16 or len(set(pins)) != 16:
            raise ValueError('Expected sixteen distinct DQ pins')
        for index in range(16):
            for name in ['D1_DELAY', 'D3_DELAY']:
                assignment = f'set_instance_assignment -name {name} 0 -to SDRAM_DQ[{index}]'
                if qsf.splitlines().count(assignment) != 1:
                    raise ValueError('Missing controlled assignment ' + assignment)
        controlled = settings((folder/'top.bt').read_text(), pins)
        native = settings(args.native.read_text(), pins)
        if controlled != native:
            raise ValueError('Native/reference input selectors differ')
        decoded[variant] = dict(settings=controlled, decoded_sha256=hashlib.sha256((folder/'top.bt').read_bytes()).hexdigest(),
                                rbf_sha256=hashlib.sha256((folder/'output_files/top.rbf').read_bytes()).hexdigest())
        rows = []
        for report, paths in evidence['variants'][variant]['reports'].items():
            if '/fabric-' not in report:
                continue
            for path in paths:
                points = path['points']
                clocks = [p for p in points if p['section'] == 'arrival' and p['stage'] == 'clock' and
                          p['type'] == 'CELL' and p['location'].startswith('DDIOIN')]
                cells = [p for p in points if p['section'] == 'arrival' and p['stage'] == 'data' and
                         p['type'] in ['CELL', 'uTco'] and p['location'].startswith('DDIOIN')]
                if clocks and cells:
                    rows.append(dict(report=report, source=path['From Node'],
                                     value_ps=round(sum(p['incremental_ns'] for p in clocks+cells)*1000, 3),
                                     report_sha256=evidence['variants'][variant]['hashes'][report]))
        if len({r['report'].split('/')[0] for r in rows}) != 4 or max(r['value_ps'] for r in rows) > 870:
            raise ValueError('Incomplete or uncovered native input-to-fabric observations')
        fabric[variant] = dict(count=len(rows), maximum=max(rows, key=lambda r: r['value_ps']), envelope_ps=870)
        clocks = clock_summary(folder)
        limits = {'DDIOINCELL': (1540, 170, 190), 'DDIOOUTCELL': (1540, 790, 770),
                  'DDIOOECELL': (1540, 810, 780)}
        for family, bounds in limits.items():
            for kind, limit in zip(['period', 'high', 'low'], bounds):
                if clocks['bounds'][family+'_ingress_'+kind+'_ns']['maximum']['required_ps'] > limit:
                    raise ValueError('Uncovered primitive clock requirement')
        clock_requirements[variant] = dict(check_count=clocks['check_count'],
                                          bounds={k: v for k, v in clocks['bounds'].items()
                                                  if k.startswith('DDIO') and '_ingress_' in k},
                                          hashes=clocks['hashes'], query_sha256=clocks['query_sha256'])
    # Exercise the two failures that originally escaped qualification.
    sample = next(iter(decoded.values()))['settings']
    pin = next(iter(sample))
    for text in ['', args.native.read_text()+f'\ns DQS16.004.000:{SELECTORS[0]}.0 10 ; {pin}\n']:
        try:
            settings(text, list(sample))
        except ValueError:
            continue
        raise AssertionError('Accepted missing pin or nonzero selector')
    result = summarize(evidence)
    result.update(input_delay_configuration=decoded,
                  input_clock_to_fabric=fabric,
                  clock_requirements=clock_requirements,
                  native_decoded_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest(),
                  database_defaults_sha256=hashlib.sha256(args.database_doc.read_bytes()).hexdigest(),
                  negative_checks=['missing decoded pin', 'nonzero decoded selector'])
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS: all 16 DQ zero selectors match native SDR/DDR references; two rejection checks')


if __name__ == '__main__':
    main()
