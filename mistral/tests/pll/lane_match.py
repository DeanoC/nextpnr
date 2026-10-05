#!/usr/bin/env python3
"""Pack a W21 PLL whose first output drives two clock branches.

FPLL (89,0) is the site W21 reaches. Counters C6, C7, C5 and C8 share its
four vertical lanes, so placing one primary branch on each of four outputs
leaves the second branch of output 0 unmatched. C0 still has a horizontal
lane. Packing must use that split instead of reporting that no dedicated
clock buffer is free.
"""
import argparse
import re
from pathlib import Path

from check import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "pins.qsf").write_text("set_location_assignment PIN_W21 -to CLK_W21\n"
                                  "set_location_assignment PIN_W15 -to LED\n")
    (out / "clocks.sdc").write_text("create_clock -name CLK_W21 -period 20 [get_ports {CLK_W21}]\n")
    run([str(args.yosys.resolve()), "-p",
         f'read_verilog "{fixture / "lane_match.v"}"; '
         "synth_intel_alm -nobram -nolutram -nodsp -top top; "
         f'write_json "{out / "synth.json"}"'], out / "yosys.log")
    log = run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(out / "pins.qsf"), "--sdc", str(out / "clocks.sdc"),
               "--json", str(out / "synth.json"), "--pack-only"], out / "pack.log")
    match = re.search(r"PLL 'pll': .* counters C([0-9,]+), bel (altera_pll\.\d+\.\d+\.\d+)", log)
    assert match, log
    counters, bel = match.group(1), match.group(2)
    assert counters == "6,7,5,0", counters
    assert bel.startswith("altera_pll.89.0."), bel
    assert "no available dedicated PLL/clock-buffer pair" not in log
    print("PASS: W21 four-output PLL with two branches on output 0 uses", counters, "at", bel)


if __name__ == "__main__":
    main()
