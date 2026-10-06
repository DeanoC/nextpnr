#!/usr/bin/env python3
"""Replay the historical 100MHz ramtest without repacking or changing routes.

Restore legacy PLL/checkpoint metadata in an isolated output directory. Every
replay must reproduce the supplied original RBF hash, including an optional
synthetic-zero IO probe. This is not a board constraint or hardware test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

DUPLICATES = '__legacy_outclk_values__'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pairs(items):
    result, values = {}, []
    for key, value in items:
        if key in result and key != 'outclk':
            raise ValueError('unexpected duplicate JSON key '+key)
        if key == 'outclk':
            values.append(value)
        result[key] = value
    if len(values) > 1:
        result[DUPLICATES] = values
    return result


def migrate(data):
    top = data['modules']['top']
    ram = top['cells']['ram_clock.pll']
    video = top['cells']['video_clock.pll']
    if (ram['parameters']['output_clock_frequency0'] != '100.0 MHz' or
            ram['parameters']['output_clock_frequency1'] != '100.0 MHz' or
            ram['parameters']['phase_shift1'] != '5000 ps' or
            video['parameters']['output_clock_frequency0'] != '74.25 MHz'):
        raise ValueError('this migration is restricted to the historical 100MHz ramtest')
    repairs = []
    for name, cell in top['cells'].items():
        if cell['type'] != 'altera_pll':
            continue
        connections = cell['connections']
        bits_list = connections.get(DUPLICATES, [connections['outclk']])
        count = int(cell['parameters']['number_of_clocks'], 2)
        if len(bits_list) != count or count not in (1, 2):
            raise ValueError('incomplete legacy PLL output data')
        outputs = {}
        for bits in bits_list:
            nets = [n for n in top['netnames'].values() if n['bits'] == bits and
                    n['attributes'].get('ROUTING', '').strip()]
            if len(nets) != 1:
                raise ValueError('missing/ambiguous routed PLL output')
            counters = set(re.findall(r'FPLL_C([0-8])', nets[0]['attributes']['ROUTING']))
            if len(counters) != 1 or not counters <= {'6', '7'}:
                raise ValueError('unexpected historical PLL counter')
            counter = next(iter(counters))
            logical = 'outclk' if counter == '6' else 'outclk[1]'
            if logical in outputs:
                raise ValueError('duplicate physical counter')
            outputs[logical] = bits
        if set(outputs) != ({'outclk'} if count == 1 else {'outclk', 'outclk[1]'}):
            raise ValueError('missing historical C6/C7 output')
        for key in list(connections):
            if key.startswith('outclk') or key == DUPLICATES:
                del connections[key]
        connections.update(outputs)
        cell['port_directions'].pop(DUPLICATES, None)
        cell['port_directions'].update({key: 'output' for key in outputs})
        frozen = json.loads(bytes.fromhex(cell['attributes']['FES_PINMAP_V1']))
        frozen['pins'].pop('outclk[0]', None)
        for key in outputs:
            frozen['pins'][key] = [0, 'C6' if key == 'outclk' else 'C7']
        frozen['count'] = len(frozen['pins'])
        cell['attributes']['FES_PINMAP_V1'] = json.dumps(frozen).encode().hex()
        cell['attributes']['MISTRAL_PLL_COUNTERS'] = '6' if count == 1 else '6,7'
        repairs.append(name)
    clocks = []
    # Values follow the validated original PLL requests and original fmax
    # report, not the duplicate-key ordering or a new placement.
    for net, period, phase, group in [
            ('FPGA_CLK1_50', 20, 0, ''), ('ram_clock.refclk', 20, 0, ''),
            ('ram_clock.pll_outclk', 10, 0, 'ram_clock.pll'),
            ('ram_clock.pll_outclk_1', 10, 5, 'ram_clock.pll'),
            ('ram_clock.clocks[0]', 10, 0, 'ram_clock.pll'),
            ('ram_clock.clocks[1]', 10, 5, 'ram_clock.pll'),
            ('video_clock.pll_outclk', 13.468, 0, ''), ('display.pixel_clk', 13.468, 0, '')]:
        if net not in top['netnames']:
            raise ValueError('missing historical clock '+net)
        clocks.append(dict(net=net, period=[period]*2, high=[period/2]*2,
                           low=[period/2]*2, phase=phase, group=group))
    top['settings']['timing/io_clocks'] = json.dumps(clocks)
    top['settings'].pop('timing/allowFail', None)
    return repairs


def probe_paths(report, top):
    captures = {name: re.fullmatch(r'dq_buf\[(\d+)\]\.dq_sample_MISTRAL_FF_Q', name)[1]
                for name, cell in top['cells'].items() if cell['type'] == 'MISTRAL_FF' and
                re.fullmatch(r'dq_buf\[(\d+)\]\.dq_sample_MISTRAL_FF_Q', name)}
    if len(captures) != 16:
        raise ValueError('expected sixteen historical fabric capture registers')
    reads, outputs = {}, {}
    for net in report['detailed_net_timings']:
        for endpoint in net['endpoints']:
            cell, port = endpoint['cell'], endpoint['port']
            if cell in captures and port == 'DATAIN':
                pin = captures[cell]
                if pin in reads:
                    raise ValueError('ambiguous read boundary')
                reads[pin] = dict(net=net['net'], arrival_ns=endpoint['delay'])
            if cell.startswith('SDRAM_') and port in ('I', 'OE'):
                key = cell+'.'+port
                if key in outputs:
                    raise ValueError('ambiguous output boundary')
                outputs[key] = dict(net=net['net'], arrival_ns=endpoint['delay'])
    if len(reads) != 16 or len(outputs) != 52:
        raise ValueError('incomplete historical read/data/OE boundary coverage')
    return dict(reference='synthetic input launch zero; outputs relative to fabric clock rising; native GPIO boundaries',
                limits='not complete chip-pin requirements; unregistered pad/package, board and fast-corner qualification remain unresolved',
                reads=reads, outputs=outputs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--nextpnr', type=Path, required=True)
    parser.add_argument('--expected-rbf', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--probe', action='store_true')
    args = parser.parse_args()
    root = args.source_root.resolve()
    build = root/'build/fes-ramtest-100'
    if sha(build/'core.rbf') != args.expected_rbf:
        raise ValueError('source bitstream does not match the specified artifact')
    data = json.loads((build/'routed.json').read_text(), object_pairs_hook=pairs)
    repairs = migrate(data)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    receipt = dict(classification='historical artifact replay; no hardware programming or board signoff',
                   original_rbf_sha256=args.expected_rbf, original_fmax=json.loads((build/'timing.json').read_text())['fmax'],
                   source_root=str(root), pll_metadata_migrations=repairs, nextpnr_sha256=sha(args.nextpnr),
                   source_sha256={str(p.relative_to(root)): sha(p) for p in
                                  [build/'routed.json', build/'synth.json', build/'core.rbf', build/'timing.json'] +
                                  [root/'cores/fes-ramtest/rtl'/f for f in ('top.v', 'sdram_addon_port.v', 'ram_pll.v')]},
                   runs={})
    for mode in (['baseline', 'probe'] if args.probe else ['baseline']):
        command = [str(args.nextpnr), '--device', '5CSEBA6U23I7', '--no-pack', '--no-place', '--no-route',
                   '--compress-rbf', '--rbf', str(out/(mode+'.rbf')), '--write', str(out/(mode+'-final.json')),
                   '--report', str(out/(mode+'-timing.json')), '--detailed-timing-report']
        if mode == 'probe':
            top = data['modules']['top']
            # Retain historical top-level port metadata on packed reload.
            top['settings']['timing/io_delays'] = '[]'
            clock = next(c for c in top['cells'].values() if c['type'] == 'MISTRAL_DDROUT' and
                         c['attributes'].get('LOC') == 'PIN_AD20')
            clock['attributes']['NEXTPNR_GPIO_TIMING_PROFILE'] = 'QUARTUS_17_0_2_RAMTEST'
            clock['attributes']['BOARD_MODEL_FAR_C'] = '30P'
            sdc = out/'probe.sdc'
            sdc.write_text('# Synthetic zero delays expose routes; not SDRAM requirements.\n' +
                           ''.join(f'{cmd} -clock {{ram_clock.clocks[0]}} -{bound} 0 [get_ports {{{ports}}}]\n'
                                   for cmd, ports in [('set_input_delay', 'SDRAM_DQ[*]'),
                                                      ('set_output_delay', 'SDRAM_*')]
                                   for bound in ('min', 'max')))
            command += ['--sdc', str(sdc)]
        checkpoint = out/(mode+'-input.json')
        checkpoint.write_text(json.dumps(data))
        command += ['--json', str(checkpoint)]
        log = out/(mode+'.log')
        with log.open('w') as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=300)
        rbf = out/(mode+'.rbf')
        if not rbf.exists() or sha(rbf) != args.expected_rbf:
            raise ValueError(mode+' replay changed the original bitstream or did not emit it')
        timing = json.loads((out/(mode+'-timing.json')).read_text())
        if not timing['timing_summary']['final_analogue_model']:
            raise ValueError('expected final analogue timing')
        if mode == 'baseline' and result.returncode != 0:
            raise ValueError('historical internal timing replay failed')
        receipt['runs'][mode] = dict(returncode=result.returncode, byte_identical_rbf=True,
                                   timing_summary=timing['timing_summary'], log_sha256=sha(log),
                                   report_sha256=sha(out/(mode+'-timing.json')))
        if mode == 'probe':
            receipt['runs'][mode]['boundary_paths'] = probe_paths(timing, data['modules']['top'])
    receipt['script_sha256'] = sha(Path(__file__))
    (out/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print('PASS: historical bitstream preserved in every replay; probe requirements are synthetic')


if __name__ == '__main__':
    main()
