#!/usr/bin/env python3
"""Exercise ordinary Mistral HeAP control-set search with real placements.

Two clock/polarity domains share reset and enable inputs. Compare the default
with --placer-heap-no-ctrl-set and repeat the default to check determinism.
Runtime is reported, not asserted, because it depends on the host.
"""

import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import subprocess
import time

from placer_options import run

TOP = '''module top #(parameter WIDTH = 512)
    (input clk, input rst, en, din, output [1:0] dout);
    reg [WIDTH-1:0] a = 0, b = 0, c = 0, d = 0;
    always @(posedge clk)
        a <= {a[WIDTH-2:0], a[WIDTH-1] ^ din};
    always @(negedge clk)
        if (en) b <= {b[WIDTH-2:0], b[WIDTH-1] ^ din};
    always @(posedge clk)
        if (rst) c <= 0; else c <= {c[WIDTH-2:0], c[WIDTH-1] ^ din};
    always @(negedge clk or posedge rst)
        if (rst) d <= 0; else if (en) d <= {d[WIDTH-2:0], d[WIDTH-1] ^ din};
    assign dout = {(^a) ^ (^b), (^c) ^ (^d)};
endmodule
'''
PINS = (("V11", "clk"), ("W24", "rst"),
        ("W21", "en"), ("W15", "din"), ("AA24", "dout[0]"), ("V16", "dout[1]"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--width", type=int, default=512,
                        help="FFs per bank (four banks; default: 512)")
    args = parser.parse_args()
    if args.width < 2:
        parser.error("--width must be at least 2")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "top.v").write_text(TOP)
    run([args.yosys.resolve(), "-p",
         f'read_verilog "{out / "top.v"}"; chparam -set WIDTH {args.width} top; '
         'synth_intel_alm -nobram -nolutram -nodsp -top top; '
         f'write_json "{out / "synth.json"}"'], out / "synth.log")
    (out / "pins.qsf").write_text("".join(
        f"set_location_assignment PIN_{pin} -to {port}\n"
        f'set_instance_assignment -name IO_STANDARD "3.3-V LVTTL" -to {port}\n'
        for pin, port in PINS))
    results = {}
    times = {}
    for name, options in (("default", []), ("repeat", []),
                          ("disabled", ["--placer-heap-no-ctrl-set"])):
        start = time.monotonic()
        log = out / f"{name}.log"
        command = [str(part) for part in [args.nextpnr.resolve(), "--device", "5CSEBA6U23I7",
                   "--qsf", out / "pins.qsf", "--json", out / "synth.json", "--seed", "5",
                   "--placer-heap-timingweight", "2000", "--placer-heap-critexp", "5",
                   "--no-route", "--write", out / f"{name}.json", *options]]
        with log.open("w") as handle:
            subprocess.run(command, stdout=handle, stderr=subprocess.STDOUT, check=True, timeout=600)
        module = json.loads((out / f"{name}.json").read_text())["modules"]["top"]
        results[name] = {n: c["attributes"]["NEXTPNR_BEL"] for n, c in module["cells"].items()}
        assert sum(c["type"] == "MISTRAL_FF" for c in module["cells"].values()) == 4 * args.width
        # Different enable/reset signatures must be allowed to coexist in a
        # LAB; affinity must not become an exclusive control-set constraint.
        lab_controls = defaultdict(set)
        for cell in module["cells"].values():
            if cell["type"] != "MISTRAL_FF":
                continue
            lab = tuple(cell["attributes"]["NEXTPNR_BEL"].split(".")[1:3])
            lab_controls[lab].add(tuple(tuple(cell["connections"][port])
                                        for port in ("CLK", "ENA", "ACLR", "SCLR", "SLOAD")))
        assert any(len(controls) > 1 for controls in lab_controls.values()), "no mixed-control LABs exercised"
        text = log.read_text()
        assert "post-placement validity check failed" not in text
        passes = re.findall(r"at iteration #\d+, type (\S+):", text)
        assert passes and set(passes) == {"ALL"}, "ordinary placement split LUT/FF passes"
        times[name] = {"wall_seconds": time.monotonic() - start}
        for label, pattern in (("heap_seconds", r"HeAP Placer Time: ([\d.]+)s"),
                               ("legalisation_seconds", r"of which strict legalisation: ([\d.]+)s")):
            times[name][label] = float(re.search(pattern, text)[1])
        print(name, times[name], flush=True)
    assert results["default"] == results["repeat"], "placement is not deterministic"
    assert results["default"] != results["disabled"], "control-set search did not affect placement"
    (out / "summary.json").write_text(json.dumps(times, indent=2) + "\n")
    print("PASS: deterministic, legal placements with control-set search enabled and disabled")


if __name__ == "__main__":
    main()
