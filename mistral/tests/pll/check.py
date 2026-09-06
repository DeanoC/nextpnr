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
    for name, parameter, value in (("frequency", "output_clock_frequency0", "30.0 MHz"),
                                   ("mode", "operation_mode", "normal"),
                                   ("fractional", "fractional_vco_multiplier", "true"),
                                   ("phase", "phase_shift0", "100 ps"),
                                   ("duty", "duty_cycle0", format(40, "032b")),
                                   ("count", "number_of_clocks", format(2, "032b"))):
        invalid = copy.deepcopy(design)
        invalid["modules"]["top"]["cells"]["pll"]["parameters"][parameter] = value
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(path)], out / f"invalid-{name}.log", success=False)
        assert "unsupported parameter" in log, log
    for name in ("reset", "fanout", "port"):
        invalid = copy.deepcopy(design)
        pll = invalid["modules"]["top"]["cells"]["pll"]
        if name == "reset":
            pll["connections"]["rst"] = ["1"]
            expected = "rst tied to zero"
        elif name == "fanout":
            # A second sink on the unbuffered PLL clock is unsupported.
            ff = next(c for c in invalid["modules"]["top"]["cells"].values() if c["type"] == "MISTRAL_FF")
            ff["connections"]["CLK"] = pll["connections"]["outclk"]
            expected = "outclk must feed exactly one clock buffer"
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
    assert "dedicated reference from PIN_V11" in log, log
    digest = hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest()
    print(f"PASS: one PLL, 25 MHz generated clock, direct C6 configuration; RBF sha256 {digest}")


if __name__ == "__main__":
    main()
