#!/usr/bin/env python3
"""Trace actual FES controller read consumption under explicit return assumptions.

Trial controller changes are made only in the output directory. This diagnostic
neither changes FES nor chooses a physical capture phase or board timing budget.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fes-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verilator', type=Path, default=shutil.which('verilator'))
    args = parser.parse_args()
    if args.verilator is None:
        parser.error('Verilator is required')
    here = Path(__file__).resolve().parent
    root = args.fes_root.resolve()/'cores/fes-ramtest'
    source = root/'rtl/sdram_addon_port.v'
    models = root/'sim/board_models.v'
    pll = root/'rtl/ram_pll.v'
    top = root/'rtl/top.v'
    if any(f'.phase_shift1("{phase} ps")' not in pll.read_text() for phase in (5000, 6538)) or any(
            assignment not in top.read_text() for assignment in ('dq_sample <= dq_pin', 'dq_oss_hold <= dq_rise')):
        raise ValueError('Capture wrapper/PLL changed; re-audit the diagnostic clock model')
    original = source.read_text()
    trigger = "wait_count == 14'd4 + OUT_LATENCY"
    if original.count(trigger) != 1:
        raise ValueError('Unexpected controller capture schedule; re-audit RTL')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cases = []
    for rate, advance in [(100, 0), (130, 0), (130, 1), (130, 2)]:
        folder = out/f'{rate}-advance{advance}'
        folder.mkdir(exist_ok=True)
        rtl = source
        if advance:
            rtl = folder/'controller-trial.v'
            rtl.write_text(original.replace(trigger, f"wait_count == 14'd{4-advance} + OUT_LATENCY"))
        command = [str(args.verilator), '--binary', '--timing', '-Wno-fatal',
                   '-DRAM_OSS_HIGH_SPEED', '--top-module', 'trace', f'-GRATE={rate}',
                   '--Mdir', str(folder/'obj'), str(here/'capture-cycle.sv'), str(rtl), str(models)]
        with (folder/'build.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
        for delay in [0, 3, 6, 9]:
            for tac in [0.5, 6 if rate == 100 else 5.4]:
                result = subprocess.run([str(folder/'obj/Vtrace'), f'+return_delay={delay}', f'+tac={tac}'],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                        check=True, timeout=30)
                logfile = folder/f'delay{delay}-tac{tac}.log'
                logfile.write_text(result.stdout)
                row = re.search(r'RESULT rate=(\d+) data=(\w+) capture=([\d.]+) launch=([\d.]+) '
                                r'valid_from=([\d.]+) valid_until=([\d.]+)', result.stdout)
                if row is None or int(row[1]) != rate:
                    raise ValueError('Missing controller trace')
                capture, launch, start, end = map(float, row.group(3, 4, 5, 6))
                expected_valid = start < capture < end
                observed_valid = row[2] == 'beef'
                if expected_valid != observed_valid:
                    raise AssertionError('Controller consumed a different sample than its trace')
                cases.append(dict(rate_mhz=rate, trial_advance_cycles=advance,
                                  tac_assumption_ns=tac, return_delay_assumption_ns=delay,
                                  consumed_capture_after_launch_ns=round(capture-launch, 3),
                                  valid_word=observed_valid,
                                  valid_interval_after_launch_ns=[round(start-launch, 3), round(end-launch, 3)],
                                  log_sha256=sha(logfile), controller_sha256=sha(rtl)))
    def case(rate, advance, delay, tac):
        return next(c for c in cases if (c['rate_mhz'], c['trial_advance_cycles'],
                                        c['return_delay_assumption_ns'], c['tac_assumption_ns']) ==
                                       (rate, advance, delay, tac))['valid_word']
    assert case(100, 0, 0, 0.5)
    assert not case(130, 0, 0, 0.5)
    assert case(130, 2, 0, 0.5)
    # A fixed advance is not a physical remedy: a delayed return reverses it.
    assert case(130, 0, 9, 5.4)
    assert not case(130, 2, 9, 5.4)
    version = subprocess.run([str(args.verilator), '--version'], stdout=subprocess.PIPE,
                             text=True, check=True).stdout.strip()
    receipt = dict(classification='ideal-clock controller cycle diagnostic; assumed return delays; no hardware signoff',
                   waveform='CAS2 at100, CAS3 at130, burst1; poison outside launch+delay+tAC through launch+T+delay+2.5ns',
                   return_delay='lumped illustrative return delay; not measured board flight or fitted silicon bound',
                   verilator=version, source_sha256=sha(source), board_models_sha256=sha(models),
                   pll_sha256=sha(pll), capture_wrapper_sha256=sha(top),
                   testbench_sha256=sha(here/'capture-cycle.sv'), cases=cases)
    (out/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print('PASS: 32 controller-cycle traces; fast and delayed returns require different sample cycles')


if __name__ == '__main__':
    main()
