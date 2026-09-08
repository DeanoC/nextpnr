#!/usr/bin/env python3
"""Timeout-bounded router2 regression using the retained mixed-width M10K netlist."""
import argparse
import gzip
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nextpnr', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--seed', type=int, action='append')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fixture = Path(__file__).with_name('m10k-mixed.json.gz')
    (out/'synth.json').write_bytes(gzip.decompress(fixture.read_bytes()))
    (out/'pins.qsf').write_text('set_location_assignment PIN_V11 -to FPGA_CLK1_50\n')
    (out/'clocks.sdc').write_text('create_clock -name FPGA_CLK1_50 -period 20.000 [get_ports {FPGA_CLK1_50}]\n')
    for seed in args.seed or [1, 2]:
        case = out/f'seed-{seed}'
        case.mkdir(exist_ok=True)
        start = time.monotonic()
        command = [str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7', '--seed', str(seed),
                   '--qsf', str(out/'pins.qsf'), '--sdc', str(out/'clocks.sdc'),
                   '--json', str(out/'synth.json'), '--compress-rbf', '--rbf', str(case/'top.rbf'),
                   '--write', str(case/'routed.json'), '--report', str(case/'timing.json')]
        # Do not pass --router: exercise Mistral's default router2.
        with (case/'route.log').open('w') as log:
            try:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=args.timeout)
            except subprocess.TimeoutExpired:
                raise SystemExit(f'FAIL: seed {seed}: router exceeded {args.timeout}s; see {case/"route.log"}')
        report = json.loads((case/'timing.json').read_text())
        assert report['utilization']['MISTRAL_M10K']['used'] == 1
        assert report['fmax'] and all(c['achieved'] >= c['constraint'] == 50 for c in report['fmax'].values())
        assert (case/'top.rbf').stat().st_size > 0
        print(f'PASS: seed {seed}: default router, one M10K, RBF and 50 MHz in {time.monotonic()-start:.1f}s', flush=True)


if __name__ == '__main__':
    main()
