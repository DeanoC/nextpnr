#!/usr/bin/env python3
"""Check emitted duty-cycle counters and both half-cycle timing budgets on host."""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from check import run
from clock_bits import check_ff_clock
from fractional import settings


# Complete non-default selected-FPLL settings from Quartus 17.0.2 (25 MHz,
# 25% duty from 50 MHz). 75% swaps only the C6 high and low counters.
ORACLE = dict(line.split() for line in """
BWCTRL 7
C6_COUT_EN 1
CLKIN_0_SRC 4
CNT_IN_SRC.6 0
CP_CURRENT 1
CTRL_OVERRIDE_SETTING 0
DPRIO0_CNT_HI_DIV.6 03
DPRIO0_CNT_LO_DIV.6 09
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
    parser.add_argument("--duty", type=int, choices=(25, 75), required=True)
    parser.add_argument("--oracle-bt", type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    oracle = dict(ORACLE)
    if args.duty == 75:
        oracle.update({"DPRIO0_CNT_HI_DIV.6": "09", "DPRIO0_CNT_LO_DIV.6": "03"})
    if args.oracle_bt:
        assert settings(args.oracle_bt.read_text()) == oracle
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(fixture / "clocks.sdc"),
               "--freq", "50", "--compress-rbf"]
    for reverse in (False, True):
        case = out / ("fall-rise" if reverse else "rise-fall")
        case.mkdir(exist_ok=True)
        text = (fixture / "duty.v").read_text().replace(".duty_cycle0(25)", f".duty_cycle0({args.duty})")
        if reverse:
            text = text.replace("posedge", "EDGE").replace("negedge", "posedge").replace("EDGE", "negedge")
        source = case / "duty.v"
        source.write_text(text)
        run([str(args.yosys.resolve()), "-p", f'read_verilog "{source}"; '
             'synth_intel_alm -nobram -nolutram -nodsp -top top; '
             f'write_json "{case / "synth.json"}"'], case / "yosys.log")
        run(command + ["--json", str(case / "synth.json"), "--rbf", str(case / "top.rbf"),
                       "--report", str(case / "timing.json"), "--write", str(case / "routed.json")], case / "route.log")
        report = json.loads((case / "timing.json").read_text())
        util = report["utilization"]
        assert util["altera_pll"] == {"used": 1, "available": 6}
        assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
        for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
            assert util.get(kind, {"used": 0})["used"] == 0
        start, end = ("negedge", "posedge") if reverse else ("posedge", "negedge")
        paths = [p for p in report["critical_paths"]
                 if p["from"] == start + " duty_clock" and p["to"] == end + " duty_clock"]
        assert len(paths) == 1, report["critical_paths"]
        budget = 40 * ((100 - args.duty) if reverse else args.duty) / 100
        assert abs(paths[0]["max_delay"] - budget) < 0.002, paths[0]
        fmax = report["fmax"]["duty_clock"]
        assert abs(fmax["constraint"] - 25) < 0.001 and fmax["achieved"] >= 25
        # Reported Fmax must scale with the actual high/low fraction, not the
        # old unconditional half-period assumption for opposite-edge paths.
        path_delay = sum(segment["delay"] for segment in paths[0]["path"])
        expected_fmax = 1000 * budget / 40 / path_delay
        assert abs(fmax["achieved"] - expected_fmax) < expected_fmax * 0.0001, (fmax, expected_fmax)
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
             str(case / "top.rbf"), str(case / "top.bt")], case / "decomp.log")
        bt = (case / "top.bt").read_text()
        assert settings(bt) == oracle
        routed = json.loads((case / "routed.json").read_text())
        check_ff_clock(bt, routed, "launch", reverse)
        check_ff_clock(bt, routed, "capture", not reverse)
        print(f"PASS: duty {args.duty}% {start}->{end}, budget {budget} ns", fmax)
        print("RBF sha256", hashlib.sha256((case / "top.rbf").read_bytes()).hexdigest())

    # Reject illegal percentages and frequencies whose checked shared feedback
    # profiles cannot express the requested high/low counts exactly.
    design = json.loads((out / "rise-fall" / "synth.json").read_text())
    for name, changes, expected in (
        ("zero", {"duty_cycle0": f"{0:032b}"}, "integer percent from 1 to 99"),
        ("hundred", {"duty_cycle0": f"{100:032b}"}, "integer percent from 1 to 99"),
        ("inexact", {"output_clock_frequency0": "100 MHz", "duty_cycle0": f"{25:032b}"},
         "unsupported PLL output frequency/duty"),
        ("fractional", {"output_clock_frequency0": "12.288 MHz", "fractional_vco_multiplier": "true"},
         "fractional-N profiles require 50 percent duty cycle"),
    ):
        invalid = copy.deepcopy(design)
        invalid["modules"]["top"]["cells"]["pll"]["parameters"].update(changes)
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(path)], out / f"invalid-{name}.log", success=False)
        assert expected in log, log
    # Folding an inverted clock buffer must not silently discard placement.
    constrained = copy.deepcopy(design)
    cells = constrained["modules"]["top"]["cells"]
    inverter_outputs = {tuple(c["connections"]["Q"]) for c in cells.values() if c["type"] == "MISTRAL_NOT"}
    inverse_buffers = [c for c in cells.values() if c["type"] == "MISTRAL_CLKBUF"
                       and tuple(c["connections"]["A"]) in inverter_outputs]
    assert len(inverse_buffers) == 1
    inverse_buffers[0]["attributes"]["BEL"] = "MISTRAL_CLKENA.89.35.0"
    path = out / "constrained-buffer.json"
    path.write_text(json.dumps(constrained))
    log = run(command + ["--json", str(path)], out / "constrained-buffer.log", success=False)
    assert "placement constraint prevents folding" in log, log
    # Same period but a contradictory 50% waveform must not override the PLL.
    sdc = out / "conflict.sdc"
    sdc.write_text((fixture / "clocks.sdc").read_text() +
                   "\ncreate_clock -period 40 [get_nets {duty_clock}]\n")
    conflict_command = list(command)
    conflict_command[conflict_command.index("--sdc") + 1] = str(sdc)
    log = run(conflict_command + ["--json", str(out / "rise-fall" / "synth.json")],
              out / "conflict.log", success=False)
    assert "conflicting clock constraint" in log, log
    print("PASS: invalid duty requests and conflicting output waveform rejected")


if __name__ == "__main__":
    main()
