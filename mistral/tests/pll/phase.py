#!/usr/bin/env python3
"""Check the bounded 25 MHz quarter-cycle PLL profile and related-clock timing."""
import argparse
import copy
import gzip
import hashlib
import json
from pathlib import Path

from check import run
from clock_bits import check_ff_clock
from fractional import settings

# Complete non-default selected-FPLL settings from Quartus 17.0.2:
# 50 MHz reference, dual 25 MHz, output1 delayed by 10000 ps.
ORACLE = dict(line.split() for line in """
BWCTRL 7
C6_COUT_EN 1
C7_COUT_EN 1
CLKIN_0_SRC 4
CNT_IN_SRC.6 0
CNT_IN_SRC.7 0
CNT_PRESET.7 04
CP_CURRENT 1
CTRL_OVERRIDE_SETTING 0
DPRIO0_CNT_HI_DIV.6 06
DPRIO0_CNT_HI_DIV.7 06
DPRIO0_CNT_LO_DIV.6 06
DPRIO0_CNT_LO_DIV.7 06
FBCLK_MUX_2 1
FPLL_ENABLE 1
FRACTIONAL_DIVISION_SETTING 00000001
LOCK_FILTER_CFG_SETTING 019
M_CNT_HI_DIV_SETTING 06
M_CNT_LO_DIV_SETTING 06
NREVERT_INVERT 1
TCLK_SEL 0
UNLOCK_FILTER_CFG_SETTING 2
VCO0PH_EN 1
VCO_DIV 0
VCO_PH0_EN 1
VCO_PH1_EN 1
VCO_PH2_EN 1
VCO_PH3_EN 1
VCO_PH4_EN 1
VCO_PH5_EN 1
VCO_PH6_EN 1
VCO_PH7_EN 1
""".strip().splitlines())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--oracle-bt", type=Path)
    parser.add_argument("--degrees", type=int, choices=(90, 180, 270), default=90)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    oracle = dict(ORACLE)
    oracle["CNT_PRESET.7"] = {90: "04", 180: "07", 270: "0a"}[args.degrees]
    if args.degrees in (180, 270):
        reference = fixture / "fixtures" / "phase" / str(args.degrees)
        hashes = json.loads((reference / "sha256.json").read_text())
        compressed = (reference / "quartus.rbf.gz").read_bytes()
        assert hashlib.sha256(compressed).hexdigest() == hashes["rbf.gz"]
        raw = gzip.decompress(compressed)
        assert hashlib.sha256(raw).hexdigest() == hashes["rbf"]
        reference_rbf = out / "quartus-reference.rbf"
        reference_rbf.write_bytes(raw)
        reference_bt = out / "quartus-reference.bt"
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
             str(reference_rbf), str(reference_bt)], out / "quartus-decomp.log")
        saved_settings = dict(line.split() for line in (reference / "fpll.txt").read_text().splitlines())
        assert settings(reference_bt.read_text()) == saved_settings == oracle
        print(f"PASS: bundled Quartus {args.degrees}-degree reference hashes and complete FPLL settings")
    shifted_clock = f"phase{args.degrees}"
    if args.oracle_bt:
        assert settings(args.oracle_bt.read_text()) == oracle
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(fixture / "clocks.sdc"),
               "--freq", "50", "--compress-rbf"]
    design = None
    # Capture must be the first strictly later edge, including wrap around.
    budgets = {90: (10, 30, 30, 10), 180: (20, 20, 40, 40), 270: (30, 10, 10, 30)}[args.degrees]
    cases = (("forward", "phase0", "posedge", shifted_clock, "posedge", budgets[0]),
             ("reverse", shifted_clock, "posedge", "phase0", "posedge", budgets[1]),
             ("forward-fall", "phase0", "posedge", shifted_clock, "negedge", budgets[2]),
             ("reverse-fall", shifted_clock, "posedge", "phase0", "negedge", budgets[3]))
    for name, source_clock, launch_edge, target_clock, capture_edge, budget in cases:
        case = out / name
        case.mkdir(exist_ok=True)
        text = (fixture / "phase.v").read_text()
        text = text.replace("@(posedge phase0) launch", f"@({launch_edge} {source_clock}) launch")
        text = text.replace("@(posedge phase90) capture", f"@({capture_edge} {target_clock}) capture")
        text = text.replace("phase90", shifted_clock).replace("10000 ps", f"{args.degrees // 90 * 10000} ps")
        source = case / "phase.v"
        source.write_text(text)
        run([str(args.yosys.resolve()), "-p", f'read_verilog "{source}"; '
             'synth_intel_alm -nobram -nolutram -nodsp -top top; '
             f'write_json "{case / "synth.json"}"'], case / "yosys.log")
        if design is None:
            design = json.loads((case / "synth.json").read_text())
        run(command + ["--json", str(case / "synth.json"), "--rbf", str(case / "top.rbf"),
                       "--report", str(case / "timing.json"), "--write", str(case / "routed.json")], case / "route.log")
        report = json.loads((case / "timing.json").read_text())
        util = report["utilization"]
        assert util["altera_pll"] == {"used": 1, "available": 6}
        assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
        for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
            assert util.get(kind, {"used": 0})["used"] == 0
        routed = json.loads((case / "routed.json").read_text())
        cells = routed["modules"]["top"]["cells"].values()
        assert sum(c["type"] == "MISTRAL_CLKBUF" for c in cells) == 2
        paths = [p for p in report["critical_paths"]
                 if p["from"] == launch_edge + " " + source_clock
                 and p["to"] == capture_edge + " " + target_clock]
        assert len(paths) == 1, report["critical_paths"]
        assert abs(paths[0]["max_delay"] - budget) < 0.002, paths[0]
        path_delay = sum(segment["delay"] for segment in paths[0]["path"])
        assert path_delay < budget, paths[0]
        fmax = report["fmax"][source_clock]
        expected_fmax = 1000 * (budget / 40) / path_delay
        assert abs(fmax["constraint"] - 25) < 0.001 and fmax["achieved"] >= 25
        assert abs(fmax["achieved"] - expected_fmax) < expected_fmax * 0.0001, (fmax, expected_fmax)
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
             str(case / "top.rbf"), str(case / "top.bt")], case / "decomp.log")
        bt = (case / "top.bt").read_text()
        assert settings(bt) == oracle
        check_ff_clock(bt, routed, "launch", launch_edge == "negedge")
        check_ff_clock(bt, routed, "capture", capture_edge == "negedge")
        print(f"PASS: {name}: {launch_edge} {source_clock} -> {capture_edge} {target_clock}, "
              f"budget {budget} ns, delay {path_delay} ns")
        print("RBF sha256", hashlib.sha256((case / "top.rbf").read_bytes()).hexdigest())

    for name, changes in (
        ("phase45", {"phase_shift1": "5000 ps"}),
        ("negative", {"phase_shift1": "-10000 ps"}),
        ("full-cycle", {"phase_shift1": "40000 ps"}),
        ("inexact", {"phase_shift1": "10001 ps"}),
        ("reference", {"reference_clock_frequency": "26 MHz"}),
        ("fractional", {"fractional_vco_multiplier": "true",
                        "output_clock_frequency0": "12.288 MHz", "output_clock_frequency1": "24.576 MHz"}),
        ("duty", {"duty_cycle1": f"{25:032b}"}),
        ("single", {"number_of_clocks": f"{1:032b}", "phase_shift0": "10000 ps"}),
        ("frequency", {"output_clock_frequency1": "40 MHz"}),
    ):
        invalid = copy.deepcopy(design)
        invalid["modules"]["top"]["cells"]["pll"]["parameters"].update(changes)
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(path)], out / f"invalid-{name}.log", success=False)
        assert "ERROR:" in log and ("phase" in log.lower() or (name == "reference" and "reference frequency" in log)), log
    sdc = out / "conflict.sdc"
    sdc.write_text((fixture / "clocks.sdc").read_text() +
                   "\ncreate_clock -period 40 [get_nets {" + shifted_clock + "}]\n")
    conflict_command = list(command)
    conflict_command[conflict_command.index("--sdc") + 1] = str(sdc)
    log = run(conflict_command + ["--json", str(out / "forward" / "synth.json")],
              out / "conflict.log", success=False)
    assert "shifted output must use the PLL-derived phase constraint" in log, log
    # A user constraint on the inverted buffer output must not be silently
    # erased when folding that buffer into a falling-edge FF clock pin.
    falling = json.loads((out / "forward-fall" / "synth.json").read_text())["modules"]["top"]
    inverted = {tuple(c["connections"]["Q"]) for c in falling["cells"].values()
                if c["type"] == "MISTRAL_NOT"}
    buffers = [c for c in falling["cells"].values() if c["type"] == "MISTRAL_CLKBUF"
               and tuple(c["connections"]["A"]) in inverted]
    assert len(buffers) == 1
    names = [name for name, net in falling["netnames"].items()
             if net["bits"] == buffers[0]["connections"]["Q"]]
    assert names
    inverse_sdc = out / "inverse-conflict.sdc"
    inverse_sdc.write_text((fixture / "clocks.sdc").read_text() +
                          "\ncreate_clock -period 40 [get_nets {" + names[0] + "}]\n")
    conflict_command[conflict_command.index("--sdc") + 1] = str(inverse_sdc)
    log = run(conflict_command + ["--json", str(out / "forward-fall" / "synth.json")],
              out / "inverse-conflict.log", success=False)
    assert "explicit clock constraint cannot describe the PLL phase" in log, log
    print("PASS: unsupported phase profiles and contradictory output constraints rejected")


if __name__ == "__main__":
    main()
