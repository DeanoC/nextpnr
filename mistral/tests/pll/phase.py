#!/usr/bin/env python3
"""Check the bounded 25 MHz 0/90-degree PLL profile and related-clock timing."""
import argparse
import copy
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
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if args.oracle_bt:
        assert settings(args.oracle_bt.read_text()) == ORACLE
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(fixture / "clocks.sdc"),
               "--freq", "50", "--compress-rbf"]
    design = None
    # Capture must be the first strictly later edge, including wrap around.
    cases = (("forward", "phase0", "posedge", "phase90", "posedge", 10),
             ("reverse", "phase90", "posedge", "phase0", "posedge", 30),
             ("forward-fall", "phase0", "posedge", "phase90", "negedge", 30),
             ("reverse-fall", "phase90", "posedge", "phase0", "negedge", 10))
    for name, source_clock, launch_edge, target_clock, capture_edge, budget in cases:
        case = out / name
        case.mkdir(exist_ok=True)
        text = (fixture / "phase.v").read_text()
        text = text.replace("@(posedge phase0) launch", f"@({launch_edge} {source_clock}) launch")
        text = text.replace("@(posedge phase90) capture", f"@({capture_edge} {target_clock}) capture")
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
        assert settings(bt) == ORACLE
        check_ff_clock(bt, routed, "launch", launch_edge == "negedge")
        check_ff_clock(bt, routed, "capture", capture_edge == "negedge")
        print(f"PASS: {name}: {launch_edge} {source_clock} -> {capture_edge} {target_clock}, "
              f"budget {budget} ns, delay {path_delay} ns")
        print("RBF sha256", hashlib.sha256((case / "top.rbf").read_bytes()).hexdigest())

    for name, changes in (
        ("phase45", {"phase_shift1": "5000 ps"}),
        ("negative", {"phase_shift1": "-10000 ps"}),
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
        assert "ERROR:" in log and "phase" in log.lower(), log
    sdc = out / "conflict.sdc"
    sdc.write_text((fixture / "clocks.sdc").read_text() +
                   "\ncreate_clock -period 40 [get_nets {phase90}]\n")
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
