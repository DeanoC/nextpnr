#!/usr/bin/env python3
"""Extract complete pad reference arcs relative to GPIO clock routing ingress."""
import argparse
import collections
import json
from pathlib import Path


def total(points):
    return sum(p['incremental_ns'] for p in points)


def close(a, b, label):
    # Reports round each incremental and accumulated point to 1 ps.
    if abs(a-b) > 0.002:
        raise ValueError('Incomplete or inconsistent '+label)


def arc(path, kind, output):
    points = path['points']
    if output:
        # TimeQuest represents the DDIO output mux's clock-to-pin transfer
        # as a data path from the clock port, not a register uTco path.
        data = [p for p in points if p['section'] == 'arrival' and p['stage'] == 'data']
        mux = [i for i, p in enumerate(data) if p['type'] == 'IC' and
               p['location'].startswith('DDIOOUTCELL') and p['node'].endswith('|muxsel')]
        if mux:
            if len(mux) != 1:
                raise ValueError('Ambiguous DDR clock mux ingress')
            ingress = data[mux[0]]
            local = data[mux[0]+1:]
            if (not local or local[0]['type'] != 'CELL' or
                    local[0]['location'] != ingress['location'] or
                    not local[0]['node'].endswith('|dataout') or
                    local[-1]['node'] != path['To Node'] or
                    not local[-1]['location'].startswith('PIN_') or
                    not any(p['type'] == 'CELL' and p['location'].startswith('IOOBUF') for p in local)):
                raise ValueError('Incomplete DDR mux-to-pin path')
            value = total(local)
            close(value, local[-1]['total_ns']-ingress['total_ns'], 'DDR mux-to-pin path')
            edge = 'F' if 'INVERTED' in path['Launch Clock'] else 'R'
            if not ingress['transition'] or ingress['transition'][0] != edge:
                raise ValueError('DDR output phase disagrees with mux clock transition')
            return ('write_ddr_low' if edge == 'F' else 'write_ddr_high'), dict(
                value_ps=round(value*1000, 3), clock_edge=edge, data_transition=local[-1]['transition'],
                pad_transition=local[-1]['transition'][-1],
                clock_reference='DDIO output clock mux routing ingress',
                clock_local_ps=0, data_local_ps=round(value*1000, 3))
        if any(p['location'].startswith('DDIOOUTCELL') and p['node'].endswith('|dataout') for p in data):
            raise ValueError('Missing DDR clock mux ingress')
    section = 'arrival' if output else 'required'
    endpoint = path['From Node'] if output else path['To Node']
    clocks = [p for p in points if p['section'] == section and p['stage'] == 'clock' and
              p['type'] in ['IC', 'CELL']]
    registers = [p for p in clocks if p['type'] == 'CELL' and p['node'] == endpoint and
                 p['location'].startswith(('DDIOOUTCELL', 'DDIOOECELL') if output else 'DDIOINCELL')]
    if not registers:
        if any(p['location'].startswith('DDIO') for p in clocks) or any(
                p['section'] == 'arrival' and p['stage'] == 'data' and
                p['location'].startswith('DDIO') and
                (p['type'] == 'uTco' if output else p['node'] == endpoint) for p in points):
            raise ValueError('Incomplete register clock path')
        return None  # Combinational or fabric-register paths are other boundaries.
    if len(registers) != 1 or len(clocks) < 2 or registers[0] != clocks[-1]:
        raise ValueError('Ambiguous register clock boundary')
    register = registers[0]
    ingress = clocks[-2]
    if (ingress['type'] != 'IC' or ingress['location'] != register['location'] or
            not ingress['node'].endswith('|clk')):
        raise ValueError('Missing GPIO clock routing ingress')
    close(register['total_ns']-ingress['total_ns'], register['incremental_ns'], 'local clock arc')
    data = [p for p in points if p['section'] == 'arrival' and p['stage'] == 'data']
    if not data:
        raise ValueError('Missing pad data path')
    if output:
        if (data[0]['type'] != 'uTco' or data[0]['node'] != endpoint or
                data[-1]['node'] != path['To Node'] or not data[-1]['location'].startswith('PIN_') or
                not any(p['type'] == 'CELL' and p['location'].startswith('IOOBUF') for p in data)):
            raise ValueError('Incomplete register-to-pin path')
        value = register['incremental_ns']+total(data)
        close(value, data[-1]['total_ns']-ingress['total_ns'], 'clock-to-pin path')
        family = 'write_oe' if register['location'].startswith('DDIOOECELL') else 'write_data'
        edge = register['transition'][0]
    else:
        if (data[0]['type'] != 'IC' or not data[0]['location'].startswith('IOIBUF') or
                data[-1]['node'] != endpoint or data[-1]['location'] != register['location'] or
                not any(p['type'] == 'CELL' and p['location'].startswith('IOIBUF') for p in data)):
            raise ValueError('Incomplete pin-to-register path')
        close(total(data), data[-1]['total_ns']-data[0]['total_ns']+data[0]['incremental_ns'],
              'pin-to-register path')
        intrinsic = [p for p in points if p['section'] == 'required' and p['node'] == endpoint and
                     p['type'] == ('uTh' if kind == 'hold' else 'uTsu')]
        if len(intrinsic) != 1:
            raise ValueError('Missing intrinsic capture check')
        delta = total(data)-register['incremental_ns']
        value = total(intrinsic)+(-delta if kind == 'hold' else delta)
        family = 'read_ddr_low' if '~DFFLO' in endpoint else (
            'read_ddr_high' if 'dataout_h[' in endpoint else 'read_sdr')
        # DFFLO's internal inversion still captures on the falling ingress edge.
        edge = 'F' if family == 'read_ddr_low' else 'R'
        if ('INVERTED' in path['Latch Clock']) != (edge == 'F'):
            raise ValueError('Capture edge disagrees with TimeQuest latch clock')
    if edge not in ['R', 'F']:
        raise ValueError('Missing clock edge')
    return family, dict(value_ps=round(value*1000, 3), clock_edge=edge,
                        data_transition=data[-1]['transition'],
                        pad_transition=data[-1]['transition'][-1] if output else data[0]['transition'][0],
                        clock_local_ps=round(register['incremental_ns']*1000, 3),
                        data_local_ps=round(total(data)*1000, 3))


def summarize(evidence):
    observations = collections.defaultdict(list)
    for variant, data in evidence['variants'].items():
        for report, paths in data['reports'].items():
            output = '/output-' in report
            if not output and '/input-' not in report:
                continue
            kind = 'hold' if report.endswith('hold.rpt') else 'setup'
            for path in paths:
                extracted = arc(path, kind, output)
                if extracted is None:
                    continue
                family, row = extracted
                key = family+'_'+('early' if kind == 'hold' else 'late') if output else family+'_'+kind
                observations[key].append(dict(row, variant=variant, report=report,
                                              source=path['From Node'], target=path['To Node'],
                                              report_sha256=data['hashes'][report]))
    if not observations:
        raise ValueError('No complete registered pad paths')
    result = {}
    for key, rows in sorted(observations.items()):
        if len({r['report'].split('/')[0] for r in rows}) != 4:
            raise ValueError('Missing corner coverage for '+key)
        if evidence.get('all_transitions', False):
            coverage = collections.defaultdict(set)
            expected = {}
            for row in rows:
                identity = (row['variant'], row['report'].split('/')[0], row['source'], row['target'])
                inverted = evidence.get('forwarded_clock_polarities', {}).get(row['variant'])
                wanted = {'rise', 'fall'}
                if inverted is not None:
                    if not isinstance(inverted, bool):
                        raise ValueError('Forwarded clock polarity must be boolean')
                    edge = row['clock_edge']
                    pad_edge = ('F' if edge == 'R' else 'R') if inverted else edge
                    if row['pad_transition'] != pad_edge:
                        raise ValueError('Forwarded clock polarity disagrees with pad transition')
                    wanted = {'rise' if pad_edge == 'R' else 'fall'}
                expected[identity] = wanted
                coverage[identity]  # Require all physically possible transitions at each launch phase.
                for transition, edge in [('rise', 'R'), ('fall', 'F')]:
                    if '-'+transition+'-' in row['report']:
                        if row['pad_transition'] != edge:
                            raise ValueError('Pad transition disagrees with explicit query')
                        coverage[identity].add(transition)
            if any(value != expected[identity] for identity, value in coverage.items()):
                raise ValueError('Incomplete explicit transition coverage for '+key)
        result[key] = dict(count=len(rows), minimum=min(rows, key=lambda r: r['value_ps']),
                           maximum=max(rows, key=lambda r: r['value_ps']))
    for family in {key.rsplit('_', 1)[0] for key in result}:
        suffixes = ['early', 'late'] if family.startswith('write_') else ['setup', 'hold']
        if any(family+'_'+s not in result for s in suffixes):
            raise ValueError('Incomplete early/late or setup/hold coverage for '+family)
    return dict(classification='fitted pad reference observations; not a production model or hardware signoff',
                clock_reference='GPIO register clock or DDIO output mux routing ingress; upstream network excluded',
                device=evidence['device'], io_standard=evidence['io_standard'],
                forwarded_clock_polarities=evidence.get('forwarded_clock_polarities', {}),
                all_transitions=evidence.get('all_transitions', False),
                ddr_output_pin=evidence.get('ddr_output_pin', 'W15'),
                output_load_pf=evidence.get('output_load_pf', 0), bounds=result,
                input_hashes={v: {k: h for k, h in d['hashes'].items()
                                 if k.endswith('.v') or k in ['top.qsf', 'clocks.sdc']}
                              for v, d in evidence['variants'].items()})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = summarize(json.loads(args.evidence.read_text()))
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS: complete pad arcs extracted relative to clock routing ingress at all four corners')
