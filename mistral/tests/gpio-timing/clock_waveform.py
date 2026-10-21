#!/usr/bin/env python3
"""Audit same-corner AD20 pad pulse losses; diagnostic fixed-load references only."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
from characterize import paths
from pad_summary import arc

CORNERS = {'7_slow_1100mv_-40c': 'Slow 1100mV -40C Model',
           '7_slow_1100mv_100c': 'Slow 1100mV 100C Model',
           'MIN_fast_1100mv_-40c': 'Fast 1100mV -40C Model',
           'MIN_fast_1100mv_100c': 'Fast 1100mV 100C Model'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def ps(value):
    value = float(value)
    require(math.isfinite(value), 'nonfinite delay')
    return round(value * 1000)


def pulse_rows(text):
    result = {}
    for block in re.split(r'^Path #\d+:', text, flags=re.M)[1:]:
        fields = {}
        for key in ('Node', 'Type', 'Actual Width', 'Required Width', 'Slack'):
            match = re.search(r'^;\s*' + key + r'\s*;\s*([^;]+);', block, re.M)
            require(match is not None, 'missing pulse property ' + key)
            fields[key] = match[1].strip()
        kind = {'High Pulse Width': 'high', 'Low Pulse Width': 'low'}.get(fields['Type'])
        require(kind is not None and kind not in result and fields['Node'] == 'DDR_OUT', 'unexpected pulse identity')
        rows = {'late': [], 'early': []}
        section = None
        credit = 0
        for line in block.splitlines():
            if '; Late Clock Arrival Path' in line:
                section = 'late'
            elif '; Early Clock Arrival Path' in line:
                section = 'early'
            cols = [c.strip() for c in line.split(';')[1:-1]]
            if section and len(cols) == 5:
                if cols[4] == 'clock pessimism removed':
                    require(section == 'early' and credit == 0, 'unexpected pessimism credit')
                    credit = ps(cols[1])
                if cols[3] in ('IC', 'CELL'):
                    rows[section].append({'total_ps': ps(cols[0]), 'increment_ps': ps(cols[1]),
                                          'transition': cols[2], 'type': cols[3], 'node': cols[4]})
        require(all(rows.values()), 'missing pulse path')
        raw_width = rows['early'][-1]['total_ps'] - rows['late'][-1]['total_ps']
        actual = ps(fields['Actual Width'])
        require(abs(raw_width + credit - actual) <= 2, 'pulse total/credit mismatch')
        require(abs(actual - ps(fields['Required Width']) - ps(fields['Slack'])) <= 2, 'pulse slack mismatch')
        result[kind] = dict(rows=rows, raw_width_ps=raw_width, vendor_actual_ps=actual,
                            removed_credit_ps=credit, vendor_requirement_ps=ps(fields['Required Width']))
    require(set(result) == {'high', 'low'}, 'missing high/low pulse check')
    return result


def extract(root):
    root = Path(root)
    evidence = json.loads((root/'evidence.json').read_text())
    variant = 'clock-forward-inverted'
    require(evidence['device'] == '5CSEBA6U23I7' and evidence['ddr_output_pin'] == 'AD20', 'wrong device/pin')
    require(evidence['all_transitions'] and evidence['forwarded_clock_polarities'] == {variant: True}, 'wrong polarity/coverage')
    require(set(evidence['variants']) == {variant}, 'unexpected variants')
    load = evidence['output_load_pf']
    require(load in (0, 15, 30), 'uncharacterized point load')
    project = root/variant
    hashes = {}

    def read(name, corner=None):
        raw = (project/name).read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        hashes[name] = digest
        original = evidence['variants'][variant]['hashes'].get(name)
        require(original is not None or name.startswith('pulse-check/'), 'missing source/report hash: ' + name)
        if original is not None:
            require(digest == original, 'changed retained input/report: ' + name)
        text = raw.decode('latin-1')
        if corner:
            require(CORNERS[corner] in text, 'wrong timing corner: ' + name)
        return text

    qsf = read('top.qsf')
    require('PIN_AD20 -to DDR_OUT' in qsf and '5CSEBA6U23I7' in qsf, 'wrong physical assignment')
    caps = re.findall(r'-name BOARD_MODEL_FAR_C (\S+) -to DDR_OUT', qsf)
    require((not caps and load == 0) or (len(caps) == 1 and abs(float(caps[0])*1e12-load) < 1e-8), 'load mismatch')
    rtl = read(variant+'.v')
    require('WITH_PLL=0, INVERTED=1, MINIMAL=1' in rtl, 'unsupported fixture')
    require('create_clock -period 10 -name memory' in read('clocks.sdc'), 'wrong source waveform')
    fit = read('output_files/top.fit.rpt')
    row = next((line for line in fit.splitlines() if line.startswith('; DDR_OUT ;')), '')
    require('AD20' in row and '16mA' in row and '3.3-V LVTTL' in row, 'missing fitted pad configuration')
    scenarios = []
    for corner in CORNERS:
        arcs = {}
        for edge, transition in (('rise', 'R'), ('fall', 'F')):
            for mode, query in (('early', 'hold'), ('late', 'setup')):
                name = f'{corner}/output-{edge}-{query}.rpt'
                parsed = paths(read(name, corner))
                require(len(parsed) == 1, 'ambiguous edge path')
                path = parsed[0]
                _, value = arc(path, query, True)
                require(value['pad_transition'] == transition, 'wrong pad transition')
                require(value['clock_edge'] == ('F' if edge == 'rise' else 'R'), 'wrong launch edge')
                data = [p for p in path['points'] if p['section'] == 'arrival' and p['stage'] == 'data']
                require(data[-1]['location'] == 'PIN_AD20', 'wrong endpoint')
                mux = [i for i,p in enumerate(data) if p['node'].endswith('|muxsel')]
                require(len(mux) == 1, 'missing/ambiguous mux ingress')
                arcs[edge, mode] = (round(value['value_ps']), data, mux[0])
        vendor = pulse_rows(read(f'pulse-check/{corner}/pulse.rpt', corner))
        losses = {}
        raw = {}
        for kind, late_edge, early_edge in (('high', 'rise', 'fall'), ('low', 'fall', 'rise')):
            late, early = arcs[late_edge, 'late'], arcs[early_edge, 'early']
            for mode, reference in (('late', late), ('early', early)):
                rows = vendor[kind]['rows'][mode]
                data = reference[1]
                require(len(rows) == len(data), 'different pulse and transfer paths')
                for actual, expected in zip(rows, data):
                    require((actual['node'], actual['type'], actual['transition']) ==
                            (expected['node'], expected['type'], expected['transition']), 'pulse path identity mismatch')
                    require(abs(actual['increment_ps']-ps(expected['incremental_ns'])) <= 1, 'pulse path delay mismatch')
            losses[kind] = late[0] - early[0]
            ingress = vendor[kind]['rows']['early'][early[2]]['total_ps'] - vendor[kind]['rows']['late'][late[2]]['total_ps']
            require(abs(ingress-losses[kind]-vendor[kind]['raw_width_ps']) <= 2, 'ingress/pad decomposition mismatch')
            raw[kind] = {k:v for k,v in vendor[kind].items() if k != 'rows'}
            raw[kind]['ingress_width_ps'] = ingress
        scenarios.append(dict(corner=corner, local_loss_ps=losses, vendor_pulses=raw))
    hashes['../evidence.json'] = hashlib.sha256((root/'evidence.json').read_bytes()).hexdigest()
    return dict(load_pf=load, load_scope='default zero load' if load == 0 else 'declared fixed load',
                scenarios=scenarios, worst_local_loss_ps={k: max((s['local_loss_ps'][k]+9)//10*10 for s in scenarios) for k in ('high','low')},
                hashes=hashes)


def evaluate(reference, load_pf, ingress_high_ps, ingress_low_ps, uncertainty_ps):
    """Use conservative ingress pulse bounds; no route or CCPP credit is invented."""
    require(reference['format'] == 1 and reference['physical_signoff'] is False and
            reference['inverted'] is True and reference['device'] == '5CSEBA6U23I7' and
            reference['pin'] == 'AD20', 'wrong reference format/configuration')
    require(all(math.isfinite(v) and v >= 0 for v in (load_pf, ingress_high_ps, ingress_low_ps, uncertainty_ps)), 'invalid waveform inputs')
    matches = [r for r in reference['loads'] if r['load_pf'] == load_pf]
    require(len(matches) == 1, 'load not sampled; interpolation is unsupported')
    losses = matches[0]['worst_local_loss_ps']
    # Inverted forwarder: an input low pulse becomes an output high pulse.
    return dict(high_min_ps=ingress_low_ps-losses['high']-uncertainty_ps,
                low_min_ps=ingress_high_ps-losses['low']-uncertainty_ps,
                physical_signoff=False, classification='conditional point-load diagnostic')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    loads = [extract(args.root/f'load{load}') for load in (0,15,30)]
    result = dict(format=1, device='5CSEBA6U23I7', pin='AD20', inverted=True,
                  physical_signoff=False, native_model_changed=False, interpolation_supported=False, loads=loads)
    result['ideal_5ns_ingress_diagnostics'] = {str(load): evaluate(result,load,5000,5000,0) for load in (0,15,30)}
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['ideal_5ns_ingress_diagnostics'],indent=2))
