#!/usr/bin/env python3
"""Build and inspect the HPS PLL reset/relock fixture; never programs hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--invert", action="store_true", help="exercise a fabric-inverted reset")
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    def run(command, name):
        with (out / name).open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

    source = fixture / "reset.v"
    if args.invert:
        source = out / "reset-inverted.v"
        source.write_text((fixture / "reset.v").read_text().replace(".rst(reset_sync)", ".rst(~reset_sync)"))
    run([str(args.yosys.resolve()), "-p",
         f'read_verilog "{fixture / "pll_meter.v"}" "{source}"; '
         'synth_intel_alm -nobram -nolutram -nodsp -top top; '
         f'write_json "{out / "synth.json"}"'], "yosys.log")
    if args.invert:
        synth = json.loads((out / "synth.json").read_text())["modules"]["top"]
        reset = synth["cells"]["pll"]["connections"]["rst"]
        assert any(cell["type"] == "MISTRAL_NOT" and cell["connections"]["Q"] == reset
                   for cell in synth["cells"].values()), "inverted reset fixture lost its inverter"
    run([str(args.nextpnr.resolve()), "--json", str(out / "synth.json"), "--device", "5CSEBA6U23I7",
         "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(fixture / "clocks.sdc"),
         "--freq", "50", "--compress-rbf", "--rbf", str(out / "top.rbf"),
         "--write", str(out / "routed.json"), "--report", str(out / "timing.json")], "route.log")
    report = json.loads((out / "timing.json").read_text())
    util = report["utilization"]
    assert util["altera_pll"] == {"used": 1, "available": 6}
    assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    assert util["MISTRAL_CLKENA"]["used"] == 2
    assert util["MISTRAL_MUL9X9"]["used"] == util["MISTRAL_M10K"]["used"] == 0
    assert util.get("MISTRAL_MLAB", {"used": 0})["used"] == 0
    for clock, frequency in (("clk25", 25), ("meter.refclk", 50)):
        assert report["fmax"][clock]["constraint"] == frequency
        assert report["fmax"][clock]["achieved"] >= frequency
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(out / "top.rbf"), str(out / "top.bt")], "decomp.log")
    bt = (out / "top.bt").read_text()
    routed = json.loads((out / "routed.json").read_text())["modules"]["top"]
    pll = routed["cells"]["pll"]
    assert pll["connections"]["rst"], "driven reset was removed"
    assert "FPLL.000.014:NRESET0" in bt
    assert "i FPLL.000.014:NRESET0 1" not in bt
    assert any(line.startswith("r ") and "FPLL.000.014:NRESET0" in line for line in bt.splitlines()), "reset is not routed"
    assert "s FPLL.000.014:FPLL_ENABLE 1" in bt
    assert "s FPLL.000.014:FBCLK_MUX_2 1" in bt
    assert "s FPLL.000.073:PL_AUX_BG_POWERDOWN 1" in bt
    assert "s CMUXHG.000.035:INPUT_SEL.2 16" in bt
    assert "o OPT_B ffffff40.2dffffff" in bt
    print("PASS: diagnostic host checks", report["fmax"])
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
