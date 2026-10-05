#!/usr/bin/env python3
"""Compare explicit board assumptions against retained fitted timing evidence.

This does not qualify the user's board, reduce FPGA timing envelopes, or
prescribe a PLL phase. Inverter identity and parasitic load remain assumptions.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path


ETRON = 'https://etron.com/wp-content/uploads/2022/04/EM63B165TSBM_Rev-2.4.pdf'
TI = 'https://www.ti.com/lit/ds/symlink/sn74lvc1g04.pdf'
BOARD = 'https://github.com/MiSTer-devel/Hardware_MiSTer/blob/bbd3619620056a0f44476e27f18b442b4f0a5952/releases/sdram_xsds_3.0.pdf'


def evaluate(reference, extra_cap_pf):
    if not math.isfinite(extra_cap_pf) or extra_cap_pf < 0:
        raise ValueError('extra capacitance must be finite and nonnegative')
    rate = reference['rates']['100']
    corners = rate['corners']
    if set(corners) != {'7_slow_1100mv_-40c', '7_slow_1100mv_100c',
                        'MIN_fast_1100mv_-40c', 'MIN_fast_1100mv_100c'}:
        raise ValueError('expected all four fitted corners')
    for data in corners.values():
        if data['output_channels'] != 52 or len(data['read_capture_windows_ns']) != 16:
            raise ValueError('expected complete FES input/output coverage')
    lo, hi = rate['common_absolute_capture_window_ns']
    if not all(math.isfinite(x) for x in (lo, hi)) or rate['common_window_exists'] != (lo <= hi):
        raise ValueError('inconsistent capture window')
    allowance = min(c['cs_inverter_max_setup_delay_ns'] for c in corners.values())
    models = {}
    for name, delay, qualified_load in [('generic-assumption', 5.0, None),
                                         ('TI-SN74LVC1G04-15pF', 3.3, 15),
                                         ('TI-SN74LVC1G04-50pF', 4.2, 50)]:
        inverter_load = 5.5 + extra_cap_pf
        covered = qualified_load is None or inverter_load <= qualified_load
        models[name] = dict(max_delay_ns=delay, load_test_pf=qualified_load,
                            assumed_inverter_output_load_pf=inverter_load,
                            load_within_test_value=covered,
                            fitted_cs_setup_remaining_ns=round(allowance-delay, 6) if covered else None)
    return dict(classification='conditional standard-board comparison; no board qualification or hardware signoff',
                sources=dict(schematic=BOARD, memory_capacitance_and_test_conditions=ETRON, inverter=TI),
                input_loads_pf=dict(two_memory_control_inputs=11.0, two_memory_dq_inputs=12.0,
                                    control_with_extra=11.0+extra_cap_pf, dq_with_extra=12.0+extra_cap_pf),
                extra_capacitance_assumption_pf=extra_cap_pf,
                capacitance_conditions='Etron Table14, 3.3V/25C, sampled; excludes FPGA and PCB/connector capacitance',
                inverter_conditions='TI15/50pF, 3.3V +/-0.3V, -40..85C; input rise/fall <=2.5ns; exact installed part unverified',
                chip_timing_conditions='Etron AC test load30pF, 1ns input transitions; Notes9/10 require slow-slew compensation',
                preserved_native_pad_envelope_pf=30,
                cs_inverter_setup_allowance_ns=allowance, inverter_comparisons=models,
                read_capture_window_ns=[lo, hi], read_window_width_ns=round(hi-lo, 6),
                read_phase_selection_supported=lo <= hi,
                decision='no supported candidate: no common read window' if lo > hi else 'read window exists; other gates still required',
                next_evidence=['exact inverter identity and load/slew conditions',
                               'native clock/data correlation and corner bounds',
                               'capture phase plus controller consumption cycle', 'setup/hold and bus turnaround'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--extra-cap-pf', type=float, required=True,
                        help='Illustrative PCB/connector load, not a built-in board estimate')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = evaluate(json.loads(args.reference.read_text()), args.extra_cap_pf)
    result['reference_sha256'] = hashlib.sha256(args.reference.read_bytes()).hexdigest()
    result['audit_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(result['decision'])


if __name__ == '__main__':
    main()
