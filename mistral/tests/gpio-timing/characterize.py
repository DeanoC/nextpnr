#!/usr/bin/env python3
"""Retain fitted GPIO timing evidence; never infer a production timing model."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess


def run(command, cwd, log):
    with log.open('w') as stream:
        subprocess.run([str(x) for x in command], cwd=cwd, stdout=stream,
                       stderr=subprocess.STDOUT, check=True)


def paths(report):
    result = []
    for block in re.split(r'^Path #\d+:', report, flags=re.M)[1:]:
        row = {}
        for key in ['From Node', 'To Node', 'Launch Clock', 'Latch Clock', 'Slack']:
            match = re.search(r'^;\s*' + key + r'\s*;\s*([^;]+);', block, re.M)
            if not match:
                raise ValueError('Missing path property ' + key)
            row[key] = match[1].strip()
        points = []
        section = None
        stage = None
        for line in block.splitlines():
            if '; Data Arrival Path' in line:
                section = 'arrival'
                stage = None
            elif '; Data Required Path' in line:
                section = 'required'
                stage = None
            if re.search(r';\s*clock path\s*;', line):
                stage = 'clock'
            elif re.search(r';\s*data path\s*;', line):
                stage = 'data'
            cols = [x.strip() for x in line.split(';')[1:-1]]
            if section and len(cols) == 7 and re.fullmatch(r'-?\d+\.\d+', cols[0]) and cols[3] in ['IC', 'CELL', 'uTsu', 'uTh', 'uTco']:
                points.append(dict(section=section, stage=stage, total_ns=float(cols[0]),
                                   incremental_ns=float(cols[1]), transition=cols[2],
                                   type=cols[3], location=cols[5], node=cols[6]))
        if not points:
            raise ValueError('Timing path has no physical points')
        row['points'] = points
        result.append(row)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quartus-bin', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reuse-fits', action='store_true')
    parser.add_argument('--variants', nargs='+', choices=['ddr', 'pads', 'bus', 'ddr-output-data', 'ramtest-pads'],
                        default=['ddr', 'pads', 'bus'])
    parser.add_argument('--output-load-pf', type=float, default=0,
                        help='External reference load on interface pads (not a board estimate)')
    parser.add_argument('--ddr-output-pin', choices=['W15', 'AD20'], default='W15')
    parser.add_argument('--all-transitions', action='store_true',
                        help='Query rise/fall pad transitions separately in addition to worst paths')
    args = parser.parse_args()
    if not math.isfinite(args.output_load_pf) or args.output_load_pf < 0:
        parser.error('--output-load-pf must be finite and nonnegative')
    here = Path(__file__).resolve().parent
    fixtures = here.parent / 'io-registers'
    out = args.output.resolve()
    evidence = {'classification': 'fitted reference evidence, not a production model or hardware signoff',
                'device': '5CSEBA6U23I7', 'io_standard': '3.3-V LVTTL',
                'output_load_pf': args.output_load_pf, 'all_transitions': args.all_transitions,
                'ddr_output_pin': 'AD20' if args.variants == ['ramtest-pads'] else args.ddr_output_pin,
                'variants': {}}
    for variant in dict.fromkeys(args.variants):
        project = out / variant
        project.mkdir(parents=True, exist_ok=True)
        if variant == 'ramtest-pads':
            rtl = (here/'ramtest-pads.v').read_text()
            qsf = (here/'ramtest-pads.qsf').read_text()
            clock_port = 'FPGA_CLK1_50'
        elif variant == 'ddr-output-data':
            oracle = here.parent / variant / 'oracle'
            rtl = (oracle/'top.v').read_text()
            qsf = (oracle/'top.qsf').read_text().replace('VERILOG_FILE top.v', 'VERILOG_FILE '+variant+'.v')
            qsf = qsf.replace('PIN_W15 -to DDR_OUT', 'PIN_'+args.ddr_output_pin+' -to DDR_OUT')
            clock_port = 'FPGA_CLK1_50'
        else:
            rtl = (fixtures / (variant+'.v')).read_text()
            qsf = (fixtures / 'oracle' / (variant+'.qsf')).read_text().replace(
                '../'+variant+'.v', variant+'.v').replace('../clocks.sdc', 'clocks.sdc')
            clock_port = 'clk'
        inputs = {variant+'.v': rtl, 'top.qsf': qsf,
                  'top.qpf': 'PROJECT_REVISION = "top"\n',
                  'clocks.sdc': f'create_clock -period 10 -name memory [get_ports {clock_port}]\n'
                  f'set_input_delay -clock memory -min 0 [remove_from_collection [all_inputs] [get_ports {clock_port}]]\n'
                  f'set_input_delay -clock memory -max 1 [remove_from_collection [all_inputs] [get_ports {clock_port}]]\n'
                  'set_input_delay -clock memory -min 0 [get_ports {dq[*] p1 p2 p3}]\n'
                  'set_input_delay -clock memory -max 1 [get_ports {dq[*] p1 p2 p3}]\n'
                  'set_output_delay -clock memory -min 0 [all_outputs]\n'
                  'set_output_delay -clock memory -max 1 [all_outputs]\n'}
        if variant == 'ramtest-pads':
            inputs['clocks.sdc'] = inputs['clocks.sdc'].replace('dq[*] p1 p2 p3', 'SDRAM_DQ[*]')
        if args.output_load_pf:
            # The obsolete OUTPUT_PIN_LOAD is ignored by TimeQuest 17.0.
            # Use a lumped far-end board capacitance (units are farads).
            ports = {'ddr': ['dq[0]', 'dq[1]'],
                     'pads': ['p1', 'p2', 'p3', 'p4', 'p6', 'p8'],
                     'ddr-output-data': ['DDR_OUT'],
                     'ramtest-pads': re.findall(r'^set_location_assignment PIN_\w+ -to (SDRAM_\S+)$', qsf, re.M),
                     'bus': [f'dq[{i}]' for i in range(4)]}[variant]
            inputs['top.qsf'] += ''.join('set_instance_assignment -name BOARD_MODEL_FAR_C '
                                        f'{args.output_load_pf*1e-12:.12g} -to {port}\n' for port in ports)
        if args.reuse_fits:
            for name, body in inputs.items():
                def normalized(text):
                    return '\n'.join(line for line in text.splitlines() if line.strip() and
                                     not line.startswith('set_global_assignment -name LAST_QUARTUS_VERSION '))
                if not (project / name).exists() or normalized((project / name).read_text()) != normalized(body):
                    raise ValueError('Reuse requires identical retained inputs: '+str(project/name))
        else:
            for name, body in inputs.items():
                (project / name).write_text(body)
            run([args.quartus_bin / 'quartus_sh', '--flow', 'compile', 'top'], project, project / 'compile.log')
        if args.output_load_pf:
            fit = (project / 'output_files/top.fit.rpt').read_text(encoding='latin-1')
            if 'Ignored BOARD_MODEL_FAR_C' in fit:
                raise ValueError('Quartus ignored the requested output load')
        query = {'ddr-output-data': 'ddr-output-paths.tcl', 'ramtest-pads': 'ramtest-paths.tcl'}.get(variant, 'paths.tcl')
        query_text = (here/query).read_text()
        if args.all_transitions:
            input_ports = 'SDRAM_DQ[*]' if variant == 'ramtest-pads' else 'dq[*] p1 p2 p3 p5 p7'
            output_ports = {'ddr-output-data': 'DDR_OUT', 'ramtest-pads': 'SDRAM_*'}.get(
                variant, 'dq[*] p1 p2 p3 p4 p6 p8')
            extra = ('foreach corner [get_available_operating_conditions] {\n'
                     '    set_operating_conditions $corner\n    update_timing_netlist\n'
                     '    foreach kind {setup hold} {\n    foreach transition {rise fall} {\n')
            if variant != 'ddr-output-data':
                extra += ('        report_timing -$kind -${transition}_from [get_ports {'+input_ports+'}] '
                          '-npaths 500 -nworst 100 -detail full_path '
                          '-file $corner/input-$transition-$kind.rpt\n')
            extra += ('        report_timing -$kind -${transition}_to [get_ports {'+output_ports+'}] '
                      '-npaths 500 -nworst 100 -detail full_path '
                      '-file $corner/output-$transition-$kind.rpt\n    }\n    }\n}\n')
            query_text = query_text.replace('project_close', extra+'project_close')
        (project/'paths.tcl').write_text(query_text)
        # A reused fit may have previously been queried with different options.
        # Do not collect stale reports from those old queries.
        for report in project.glob('*_*mv_*c/*.rpt'):
            report.unlink()
        run([args.quartus_bin / 'quartus_sta', '-t', 'paths.tcl'], project, project / 'paths.log')
        if args.output_load_pf and re.search(
                r'Ignored.*(?:BOARD_MODEL_FAR_C|Board Model Far C|capacitance assignment)',
                (project/'paths.log').read_text(encoding='latin-1'), re.I):
            raise ValueError('TimeQuest ignored the requested board capacitance')
        reports = {}
        for report in sorted(project.glob('*_*mv_*c/*.rpt')):
            reports[str(report.relative_to(project))] = paths(report.read_text())
        if len({x.split('/')[0] for x in reports}) != 4:
            raise ValueError('Expected all four slow/fast temperature corners')
        if not any(reports.values()):
            raise ValueError('No GPIO paths captured')
        hashes = {str(p.relative_to(project)): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(project.rglob('*')) if p.is_file() and
                  (p.name in inputs or p.suffix == '.rpt' or p.name == 'paths.tcl')}
        evidence['variants'][variant] = {'hashes': hashes, 'reports': reports}
    (out / 'evidence.json').write_text(json.dumps(evidence, indent=2)+'\n')
    print('PASS: retained GPIO paths, source hashes and all four corners for '+', '.join(args.variants))


if __name__ == '__main__':
    main()
