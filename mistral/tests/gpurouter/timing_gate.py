#!/usr/bin/env python3
"""Check which analysis fails a GPU-routed Mistral run on timing.

The router1 legality check at the end of the GPU router times the design
with the pip-delay table. With --rbf, build_bitstream() then runs the
analogue signoff, which is the gate: the legality check only reports, so a
table-model miss that the analogue repair closes does not fail the run.
Without --rbf no signoff runs, and the legality check must stay the gate.

A 32-bit multiply-accumulate on one clock cannot meet 400 MHz. For each
flow the script checks the exit status and which check reported the miss
as an error, then checks that 10 MHz passes with --rbf. It exits 0 when
every check holds and 1 otherwise.
"""

import argparse
from pathlib import Path
import subprocess
import sys

TOP = '''module top(input clk, input [2:0] din, output [2:0] dout);
    reg [31:0] a = 0, b = 0, acc = 0;
    always @(posedge clk) begin
        a <= {a[28:0], din};
        b <= {b[28:0], a[31:29] ^ din};
        acc <= acc + a * b;
    end
    assign dout = acc[31:29];
endmodule
'''

PINS = (("V11", "clk"), ("Y24", "din[0]"), ("W24", "din[1]"), ("W21", "din[2]"),
        ("W15", "dout[0]"), ("AA24", "dout[1]"), ("V16", "dout[2]"))

SIGNOFF = "Running signoff timing analysis..."


def run(command, log):
    with log.open("w") as handle:
        return subprocess.run([str(part) for part in command], stdout=handle, stderr=subprocess.STDOUT).returncode


def fmax_errors(text):
    return [line for line in text.splitlines() if line.startswith("ERROR: Max frequency for clock")]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--gpu-device", type=int)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    (out / "top.v").write_text(TOP)
    script = f"read_verilog {out / 'top.v'}; synth_intel_alm -nobram -nolutram -nodsp -top top; " \
             f"write_json {out / 'synth.json'}"
    if run([args.yosys.resolve(), "-p", script], out / "synth.log"):
        raise SystemExit(f"yosys failed; see {out / 'synth.log'}")
    (out / "pins.qsf").write_text("".join(
        f"set_location_assignment PIN_{pin} -to {port}\n"
        f"set_instance_assignment -name IO_STANDARD \"3.3-V LVTTL\" -to {port}\n" for pin, port in PINS))

    def route(name, freq, rbf):
        command = [args.nextpnr.resolve(), "--device", "5CSEBA6U23I7", "--qsf", out / "pins.qsf",
                   "--json", out / "synth.json", "--freq", freq, "--seed", "1", "--router", "gpu"]
        if args.gpu_device is not None:
            command += ["--gpu-device", str(args.gpu_device)]
        if rbf:
            command += ["--rbf", out / f"{name}.rbf"]
        log = out / f"{name}.log"
        code = run(command, log)
        return code, log.read_text()

    failures = []

    code, text = route("no-rbf", "400", False)
    errors = fmax_errors(text)
    print(f"400 MHz without --rbf: exit {code}, {len(errors)} Fmax errors")
    if code == 0:
        failures.append("a timing miss without --rbf exited 0")
    if not errors:
        failures.append("without --rbf the legality check did not report the miss as an error")

    code, text = route("rbf", "400", True)
    before, _, after = text.partition(SIGNOFF)
    print(f"400 MHz with --rbf: exit {code}, {len(fmax_errors(before))} Fmax errors before signoff, "
          f"{len(fmax_errors(after))} at signoff")
    if code == 0:
        failures.append("a signoff miss with --rbf exited 0")
    if not after:
        failures.append("with --rbf no signoff ran")
    if fmax_errors(before):
        failures.append("with --rbf the legality check reported the miss as an error")
    if not fmax_errors(after):
        failures.append("with --rbf the signoff did not report the miss as an error")

    code, text = route("pass", "10", True)
    print(f"10 MHz with --rbf: exit {code}")
    if code != 0 or "Program finished normally." not in text:
        failures.append(f"a design that meets 10 MHz exited {code}")

    if failures:
        print(f"FAIL: {'; '.join(failures)}")
        return 1
    print("PASS: the signoff gates --rbf runs and the legality check gates runs without it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
