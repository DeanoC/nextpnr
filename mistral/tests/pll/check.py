#!/usr/bin/env python3
"""Exercise the first PLL profile, generated clock and emitted configuration."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess


def run(command, log, success=True):
    with log.open("w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    assert (result.returncode == 0) == success, log
    return log.read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run([str(args.yosys.resolve()), "-p",
         f'read_verilog "{fixture / "top.v"}"; '
         'synth_intel_alm -nobram -nolutram -nodsp -top top; '
         f'write_json "{out / "synth.json"}"'], out / "yosys.log")
    design = json.loads((out / "synth.json").read_text())
    assert sum(c["type"] == "altera_pll" for c in design["modules"]["top"]["cells"].values()) == 1
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "pins.qsf"), "--sdc", str(fixture / "clocks.sdc"),
               "--freq", "50", "--compress-rbf"]
    run(command + ["--json", str(out / "synth.json"), "--rbf", str(out / "top.rbf"),
                   "--report", str(out / "timing.json"), "--write", str(out / "routed.json")], out / "route.log")
    report = json.loads((out / "timing.json").read_text())
    assert report["utilization"]["altera_pll"] == {"used": 1, "available": 6}
    for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
        assert report["utilization"].get(kind, {"used": 0})["used"] == 0
    assert report["fmax"]["clk25"]["constraint"] == 25
    assert report["fmax"]["clk25"]["achieved"] >= 25
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "top.rbf"), str(out / "top.bt")], out / "decomp.log")
    bt = (out / "top.bt").read_text()
    assert len(re.findall(r"^s FPLL.*:FPLL_ENABLE 1$", bt, re.M)) == 1
    settings = dict(re.findall(r"^s FPLL\.000\.014:(\S+) (\S+)$", bt, re.M))
    for key, value in {"FPLL_ENABLE": "1", "FBCLK_MUX_2": "1", "CLKIN_0_SRC": "4",
                       "M_CNT_HI_DIV_SETTING": "06", "M_CNT_LO_DIV_SETTING": "06",
                       "DPRIO0_CNT_HI_DIV.6": "06", "DPRIO0_CNT_LO_DIV.6": "06",
                       "C6_COUT_EN": "1", "VCO_DIV": "0", "BWCTRL": "7", "CP_CURRENT": "1"}.items():
        assert settings[key] == value, (key, settings)
    assert "s CMUXHG.000.035:INPUT_SEL.2 16" in bt
    assert "i FPLL.000.014:NRESET0 1" in bt
    assert "s FPLL.000.073:PL_AUX_BG_POWERDOWN 1" in bt
    assert "PLL_FEEDBACK_ENABLE" not in bt
    assert "o OPT_B ffffff40.2dffffff" in bt
    # The general solver accepts every configuration Quartus implements
    # (mistral/tests/pll/solver.py); these remain unsupported and must fail.
    for name, parameter, value, expected in (
            ("frequency", "output_clock_frequency0", "7.0000001 MHz", "at most six decimal places"),
            ("too-fast", "output_clock_frequency0", "600.0 MHz", "exceed the -7 global clock limit"),
            ("mode", "operation_mode", "normal", "unsupported parameter"),
            ("fractional", "fractional_vco_multiplier", "true", "fractional-N mode supports reference clocks"),
            ("phase", "phase_shift0", "-100 ps", "unsupported PLL output frequency/duty/phase"),
            ("duty", "duty_cycle0", format(0, "032b"), "duty cycle must be an integer percent"),
            ("count", "number_of_clocks", format(10, "032b"), "number_of_clocks must"),
            # 2^32+1 and 2^32+50 fit in the parameter bit vector but not in int.
            # Narrowing them used to yield a legal 1 and a legal 50.
            ("count-wide", "number_of_clocks", format(2 ** 32 + 1, "033b"),
             "number_of_clocks must be an integer from 1 to 9"),
            ("duty-wide", "duty_cycle0", format(2 ** 32 + 50, "033b"),
             "duty_cycle0 must be an integer percentage"),
            # as_int64() drops bits above 63, so these used to read as 1 and 50.
            ("count-65", "number_of_clocks", format(2 ** 64 + 1, "065b"),
             "number_of_clocks must be an integer from 1 to 9"),
            ("duty-65", "duty_cycle0", format(2 ** 64 + 50, "065b"),
             "duty_cycle0 must be an integer percentage")):
        invalid = copy.deepcopy(design)
        pll = invalid["modules"]["top"]["cells"]["pll"]
        pll["parameters"][parameter] = value
        if name == "fractional":
            # 25 MHz is a valid generic fractional rate; use an unsupported
            # reference to exercise the fractional-only guard.
            pll["parameters"]["reference_clock_frequency"] = "25.0 MHz"
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(path)], out / f"invalid-{name}.log", success=False)
        assert expected in log, log
    for name in ("reset", "fanout", "port"):
        invalid = copy.deepcopy(design)
        pll = invalid["modules"]["top"]["cells"]["pll"]
        if name == "reset":
            pll["connections"]["rst"] = ["1"]
            expected = "rst must be tied low or driven by a signal"
        elif name == "fanout":
            # A second sink on the unbuffered PLL clock is unsupported.
            ff = next(c for c in invalid["modules"]["top"]["cells"].values() if c["type"] == "MISTRAL_FF")
            ff["connections"]["CLK"] = pll["connections"]["outclk"]
            expected = "must feed only clock buffers"
        else:
            pll["connections"]["phase_en"] = ["0"]
            pll["port_directions"]["phase_en"] = "input"
            expected = "unsupported port"
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(path)], out / f"invalid-{name}.log", success=False)
        assert expected in log, log
    for name, text, expected in (
        ("reference-clock", "create_clock -period 25 [get_ports FPGA_CLK1_50]\n", "conflicting clock constraint"),
        ("output-clock", "create_clock -period 20 [get_nets clk25]\n", "conflicting clock constraint"),
    ):
        path = out / f"invalid-{name}.sdc"
        path.write_text(text)
        invalid_command = command.copy()
        invalid_command[invalid_command.index("--sdc") + 1] = str(path)
        log = run(invalid_command + ["--json", str(out / "synth.json")], out / f"invalid-{name}.log", success=False)
        assert expected in log, log
    qsf = out / "invalid-reference-pin.qsf"
    qsf.write_text((fixture / "pins.qsf").read_text().replace("PIN_V11", "TEMP_PIN")
                   .replace("PIN_W15", "PIN_V11").replace("TEMP_PIN", "PIN_W15"))
    invalid_command = command.copy()
    invalid_command[invalid_command.index("--qsf") + 1] = str(qsf)
    log = run(invalid_command + ["--json", str(out / "synth.json")], out / "invalid-reference-pin.log", success=False)
    assert "has no dedicated clock path to an FPLL" in log, log
    digest = hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest()
    print(f"PASS: one PLL, 25 MHz generated clock, direct C6 configuration; RBF sha256 {digest}")


if __name__ == "__main__":
    main()
