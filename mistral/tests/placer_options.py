#!/usr/bin/env python3
"""Check Mistral HeAP defaults and overrides with real deterministic placements.

No routing or GPU is required. Exponent and beta overrides must change the
placement of this timing-sensitive fixture. Defaults must match explicit
Mistral defaults, and serialized settings must describe the selected values.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys

TOP = '''module top(input clk, input [2:0] din, output [2:0] dout);
    reg [15:0] a = 0, b = 0, acc = 0;
    always @(posedge clk) begin
        a <= {a[12:0], din};
        b <= {b[12:0], a[15:13] ^ din};
        acc <= acc + a[7:0] * b[7:0];
    end
    assign dout = acc[15:13];
endmodule
'''
PINS = (("V11", "clk"), ("Y24", "din[0]"), ("W24", "din[1]"), ("W21", "din[2]"),
        ("W15", "dout[0]"), ("AA24", "dout[1]"), ("V16", "dout[2]"))


def run(command, log):
    with log.open("w") as handle:
        result = subprocess.run([str(part) for part in command], stdout=handle, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"command exited {result.returncode}; see {log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "top.v").write_text(TOP)
    script = f'read_verilog "{out / "top.v"}"; synth_intel_alm -nobram -nolutram -nodsp -top top; ' \
             f'write_json "{out / "synth.json"}"'
    run([args.yosys.resolve(), "-p", script], out / "synth.log")
    (out / "pins.qsf").write_text("".join(
        f"set_location_assignment PIN_{pin} -to {port}\n"
        f'set_instance_assignment -name IO_STANDARD "3.3-V LVTTL" -to {port}\n' for pin, port in PINS))

    results = {}
    variants = {"default": [],
                "explicit-default": ["--placer-heap-critexp", "7", "--placer-heap-beta", "0.5"],
                "exponent": ["--placer-heap-critexp", "2"],
                "beta": ["--placer-heap-beta", "0.8"],
                "weight": ["--placer-heap-timingweight", "30"]}
    for name, options in variants.items():
        output = out / f"{name}.json"
        run([args.nextpnr.resolve(), "--device", "5CSEBA6U23I7", "--qsf", out / "pins.qsf",
             "--json", out / "synth.json", "--freq", "130", "--seed", "2", "--no-route",
             "--write", output, *options], out / f"{name}.log")
        module = json.loads(output.read_text())["modules"]["top"]
        placements = {name: cell["attributes"]["NEXTPNR_BEL"] for name, cell in module["cells"].items()}
        results[name] = (placements, module["settings"])
        print(f"{name}: {len(placements)} placed cells", flush=True)

    failures = []
    baseline = results["default"][0]
    if baseline != results["explicit-default"][0]:
        failures.append("Mistral defaults differ from explicit exponent7/beta0.5")
    for name in ("exponent", "beta", "weight"):
        changed = sum(baseline[cell] != bel for cell, bel in results[name][0].items())
        print(f"{name}: {changed} cells moved from default")
        if not changed:
            failures.append(f"{name} override did not affect actual placement")
    for name, (_, settings) in results.items():
        expected = {"criticalityExponent": 2 if name == "exponent" else 7,
                    "beta": 0.8 if name == "beta" else 0.5,
                    "timingWeight": 30 if name == "weight" else 10}
        for key, value in expected.items():
            actual = float(settings["placerHeap/" + key])
            if abs(actual - value) > 1e-6:
                failures.append(f"{name}: serialized {key}={actual}, expected {value}")
    if failures:
        print("FAIL: " + "; ".join(failures))
        return 1
    print("PASS: Mistral defaults and explicit HeAP options control real placement")
    return 0


if __name__ == "__main__":
    sys.exit(main())
