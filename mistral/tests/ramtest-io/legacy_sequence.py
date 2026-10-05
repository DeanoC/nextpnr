#!/usr/bin/env python3
"""Audit historical command opportunities; no P&R or silicon timing signoff."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_trace(text):
    commands, enables = [], []
    for line in text.splitlines():
        match = re.fullmatch(r'CMD time=([\d.]+) kind=\s*(ACT|READ|WRITE) addr=(\d+) a=(\w+) ba=(\d+)', line)
        if match:
            commands.append(dict(time=float(match[1]), kind=match[2], addr=int(match[3]), ages={}))
        match = re.fullmatch(r'AGE port=(\w+) bit=(\d+) ns=([\d.]+)', line)
        if match:
            commands[-1]['ages'][f'{match[1]}[{match[2]}]'] = float(match[3])
        match = re.fullmatch(r'OE time=([\d.]+) value=(\d)', line)
        if match:
            enables.append((float(match[1]), int(match[2])))
    if len(commands) != 96 or len(enables) != 48 or 'FINISHED' not in text:
        raise ValueError('Incomplete six-pattern four-word scan')
    minimum_ages = {}
    for command in commands:
        for port, age in command['ages'].items():
            minimum_ages[port] = min(minimum_ages.get(port, age), age)
    # BL1/CL2: earliest drive at READ+20, latest release at READ+30+tHZ.
    read_start_gaps, write_start_gaps = [], []
    for command in commands:
        if command['kind'] == 'READ':
            previous = [t for t, value in enables if value == 0 and t < command['time']]
            read_start_gaps.append(command['time'] + 20 - max(previous))
    for time, value in enables:
        previous = [c['time'] for c in commands if c['kind'] == 'READ' and c['time'] < time]
        if value and previous:
            write_start_gaps.append(time - (max(previous) + 30 + 5.4))
    assert all(minimum_ages[f'{port}[0]'] == 5 for port in ['nCS', 'nRAS', 'nCAS', 'nWE'])
    return dict(command_count=len(commands), oe_transition_count=len(enables),
                minimum_changed_bit_age_ns=minimum_ages,
                write_release_to_earliest_read_drive_ideal_ns=min(read_start_gaps),
                latest_read_release_to_write_enable_ideal_ns=round(min(write_start_gaps), 3))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--replay-root', type=Path, required=True,
                        help='Parent of baseline-replay-passing and baseline-replay-failing')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    baseline = json.loads((here/'baseline-reference.json').read_text())
    root = args.source_root.resolve()
    for relative, expected in baseline['builds']['passing']['source_sha256'].items():
        if relative.startswith('cores/') and sha(root/relative) != expected:
            raise ValueError(f'Not historical RTL: {relative}')
    args.output.mkdir(parents=True, exist_ok=True)
    source = root/'cores/fes-ramtest'
    traces = {}
    # Cover row A9, row A12, column carry and chip-select carry, plus low addresses.
    for base in [0, 8190, 65534, 131070, 33554430]:
        folder = args.output/str(base)
        folder.mkdir(exist_ok=True)
        command = ['verilator', '--binary', '--timing', '-Wno-fatal', '-DRAM_OSS_HIGH_SPEED',
                   '--top-module', 'legacy_sequence', f'-GBASE={base}', '--Mdir', str(folder/'obj'),
                   str(here/'legacy-sequence.sv'), str(source/'rtl/sdram_addon_port.v'),
                   str(source/'rtl/mem_channel.v'), str(source/'sim/board_models.v')]
        with (folder/'build.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
        trace = subprocess.run([str(folder/'obj/Vlegacy_sequence')], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, check=True, timeout=30).stdout
        (folder/'trace.log').write_text(trace)
        traces[str(base)] = dict(**parse_trace(trace), log_sha256=sha(folder/'trace.log'))
    ages = {}
    for trace in traces.values():
        for port, age in trace['minimum_changed_bit_age_ns'].items():
            ages[port] = min(ages.get(port, age), age)
    expected = {**{f'A[{bit}]': 5 for bit in range(13)},
                **{f'BA[{bit}]': 5 for bit in range(2)},
                **{f'{port}[0]': 5 for port in ['nCS', 'nRAS', 'nCAS', 'nWE']},
                **{f'DQ[{bit}]': 35 for bit in range(16)}}
    if ages != expected:
        raise ValueError(f'Historical launch sequence changed: {ages}')
    rows = {}
    oe_routes = {}
    for label in ['passing', 'failing']:
        build = baseline['builds'][label]
        folder = args.replay_root/f'baseline-replay-{label}'
        if sha(folder/'probe.rbf') != build['original_rbf_sha256']:
            raise ValueError(f'{label} probe no longer matches the historical bitstream')
        module = json.loads((folder/'probe-final.json').read_text())['modules']['top']
        ports = {}
        for port, info in module['ports'].items():
            if port.startswith('SDRAM_'):
                for bit, wire in enumerate(info['bits']):
                    ports[wire] = (port.removeprefix('SDRAM_'), bit)
        paths = build['runs']['probe']['boundary_paths']['outputs']
        oe_routes[label] = round(max(path['arrival_ns'][1] for endpoint, path in paths.items()
                                    if endpoint.endswith('.OE')), 3)
        for endpoint, path in paths.items():
            cell_name, kind = endpoint.rsplit('.', 1)
            cell = module['cells'][cell_name]
            port, bit = ports[cell['connections']['PAD'][0]]
            key = f'{port}[{bit}]'
            if kind == 'I' and key in ages:
                row = rows.setdefault(key, dict(pin=cell['attributes']['LOC'], age_ns=ages[key]))
                # C >= arrival + tIS - age, with unmodelled pad/board delays set to zero.
                # This is a lower bound on required clock delay, NOT a slack verdict.
                late = path['arrival_ns'][1]
                row[label] = dict(native_boundary_late_ns=round(late, 3),
                                  required_clock_delay_lower_bound_ns=round(late+1.5-ages[key], 3))
    for row in rows.values():
        row['late_delta_ns'] = round(row['failing']['native_boundary_late_ns']-
                                    row['passing']['native_boundary_late_ns'], 3)
    receipt = dict(classification='historical sequence and differential native boundary timing; not board signoff',
                   source_root=str(root), traces=traces, boundaries=rows,
                   worst_native_oe_boundary_late_ns=oe_routes,
                   timing_reference='Native boundary arrivals relative to the fabric clock rising edge; ideal chip edge +5 ns',
                   etron_reference='https://etron.com/wp-content/uploads/2022/04/EM63B165TSBM_Rev-2.4.pdf',
                   chip_parameters_ns=dict(tIS=1.5, tLZ_min=0, tHZ_max=5.4),
                   limits=['Real historical controller and mem_channel; scan reduced to four words at five bases.',
                           'No delayed pad waveform, electrical waveform, fast corner, or measured board clock.',
                           'Clock-delay lower bounds omit unregistered output pad/package and board delays.',
                           'STA path maxima need not be sensitized by every simulated address transition.',
                           'Read input route changes and command skew remain possible causes; root cause unproved.'],
                   source_sha256={str(path.relative_to(root)): sha(path) for path in
                                  [source/'rtl/sdram_addon_port.v', source/'rtl/mem_channel.v',
                                   source/'sim/board_models.v']},
                   fixture_sha256=sha(here/'legacy-sequence.sv'), script_sha256=sha(Path(__file__)))
    (args.output/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(dict(scans=len(traces), command_count=sum(t['command_count'] for t in traces.values()),
                         write_to_read_ns=min(t['write_release_to_earliest_read_drive_ideal_ns'] for t in traces.values()),
                         read_to_write_ns=min(t['latest_read_release_to_write_enable_ideal_ns'] for t in traces.values())), indent=2))


if __name__ == '__main__':
    main()
