#!/usr/bin/env python3
"""Check the default or a selected 25/50/100 MHz triple PLL profile; host checks only."""
import argparse
import copy
import gzip
import hashlib
import json
from pathlib import Path
import re

from check import run


def fpll_settings(text):
    """Compare every emitted setting at every FPLL, including auxiliary blocks."""
    return dict(re.findall(r"^s (FPLL\.\S+) (.+)$", text, re.M))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--frequencies", nargs=3, default=('25', '50', '100'),
                        help="exact output frequencies in MHz")
    parser.add_argument("--oracle-fixture", type=Path,
                        help="directory containing top.rbf.gz and sha256.json")
    parser.add_argument("--oracle-bt", type=Path)
    parser.add_argument("--skip-negative", action="store_true")
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run([str(args.yosys.resolve()), "-p",
         f'read_verilog "{fixture / "triple.v"}"; '
         'synth_intel_alm -nobram -nolutram -nodsp -top top; '
         f'write_json "{out / "synth.json"}"'], out / "yosys.log")
    design = json.loads((out / "synth.json").read_text())
    cells = design["modules"]["top"]["cells"]
    assert sum(c["type"] == "altera_pll" for c in cells.values()) == 1
    for index, frequency in enumerate(args.frequencies):
        cells["pll"]["parameters"][f"output_clock_frequency{index}"] = f"{frequency} MHz"
    (out / "synth.json").write_text(json.dumps(design))
    sdc = out / "clocks.sdc"
    sdc.write_text("create_clock -name FPGA_CLK1_50 -period 20 [get_ports {FPGA_CLK1_50}]\n")
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(sdc),
               "--freq", "50", "--compress-rbf"]
    run(command + ["--json", str(out / "synth.json"), "--rbf", str(out / "top.rbf"),
                   "--report", str(out / "timing.json"), "--write", str(out / "routed.json")], out / "route.log")
    report = json.loads((out / "timing.json").read_text())
    util = report["utilization"]
    assert util["altera_pll"] == {"used": 1, "available": 6}
    assert util["MISTRAL_CLKENA"]["used"] == 3
    assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
        assert util.get(kind, {"used": 0})["used"] == 0
    for index, frequency in enumerate(map(float, args.frequencies)):
        clock = report["fmax"][f"clocks[{index}]"]
        assert abs(clock["constraint"] - frequency) <= frequency * 0.00005, clock
        assert clock["achieved"] >= frequency, clock
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "top.rbf"), str(out / "top.bt")], out / "decomp.log")
    bt = (out / "top.bt").read_text()
    assert len(re.findall(r"^s FPLL.*:FPLL_ENABLE 1$", bt, re.M)) == 1
    settings = fpll_settings(bt)
    assert settings
    for lane, select in ((1, "17"), (2, "16"), (3, "15")):
        assert f"s CMUXHG.000.035:INPUT_SEL.{lane} {select}" in bt.splitlines()
        assert f"s CMUXHG.000.035:TESTSYN_ENOUT_SELECT.{lane} PRE_SYNENB" in bt.splitlines()
    routed = json.loads((out / "routed.json").read_text())["modules"]["top"]["cells"]
    # BEL creation preserves existing lane2/lane3 indices, then adds lane1.
    for index, bel in enumerate(("MISTRAL_CLKENA.0.35.0", "MISTRAL_CLKENA.0.35.1", "MISTRAL_CLKENA.0.35.2")):
        net = [cells["pll"]["connections"]["outclk"][index]]
        name = next(name for name, cell in cells.items()
                    if cell["type"] == "MISTRAL_CLKBUF" and cell["connections"]["A"] == net)
        assert routed[name]["attributes"]["NEXTPNR_BEL"] == bel, routed[name]
    reference = args.oracle_fixture or fixture / "fixtures" / "triple"
    hashes = json.loads((reference / "sha256.json").read_text())
    compressed = (reference / "top.rbf.gz").read_bytes()
    assert hashlib.sha256(compressed).hexdigest() == hashes["rbf.gz"]
    raw = gzip.decompress(compressed)
    assert hashlib.sha256(raw).hexdigest() == hashes["rbf"]
    (out / "quartus.rbf").write_bytes(raw)
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "quartus.rbf"), str(out / "quartus.bt")], out / "quartus-decomp.log")
    for oracle_path in [out / "quartus.bt"] + ([args.oracle_bt] if args.oracle_bt else []):
        oracle = fpll_settings(oracle_path.read_text())
        assert oracle, "oracle contains no FPLL settings"
        differences = {key: (oracle.get(key), settings.get(key))
                       for key in oracle.keys() | settings.keys() if oracle.get(key) != settings.get(key)}
        assert not differences, f"FPLL oracle mismatch (oracle, actual): {differences}"

    def reject(name, invalid, reasons, custom_command=None):
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run((custom_command or command) + ["--json", str(path)],
                  out / f"invalid-{name}.log", success=False)
        assert "ERROR" in log and any(reason in log for reason in reasons), log

    if not args.skip_negative:
        for name, parameter, value, reasons in (
            ("five-outputs", "number_of_clocks", format(5, "032b"), ("number_of_clocks",)),
            ("reference25", "reference_clock_frequency", "25.0 MHz", ("multi-output", "reference")),
            ("fractional", "fractional_vco_multiplier", "true", ("multi-output", "fractional")),
            ("frequency0", "output_clock_frequency0", "7.0 MHz", ("multi-output", "frequenc")),
            ("frequency1", "output_clock_frequency1", "7.0 MHz", ("multi-output", "frequenc")),
            ("phase1", "phase_shift1", "10000 ps", ("phase",)),
            ("duty0", "duty_cycle0", format(25, "032b"), ("multi-output",)),
            ("duty1", "duty_cycle1", format(25, "032b"), ("multi-output", "frequencies/duties")),
            ("frequency2", "output_clock_frequency2", "7.0 MHz", ("multi-output", "frequenc")),
            ("phase2", "phase_shift2", "100 ps", ("multi-output", "phase")),
            ("duty2", "duty_cycle2", format(25, "032b"), ("multi-output", "duty")),
            ("missing-frequency2", "output_clock_frequency2", None, ("output_clock_frequency2",)),
        ):
            invalid = copy.deepcopy(design)
            params = invalid["modules"]["top"]["cells"]["pll"]["parameters"]
            if value is None:
                del params[parameter]
            else:
                params[parameter] = value
            reject(name, invalid, reasons)
        for name in ("extra-port", "disconnected-third", "direct-third-sink"):
            invalid = copy.deepcopy(design)
            invalid_cells = invalid["modules"]["top"]["cells"]
            pll = invalid_cells["pll"]
            if name == "extra-port":
                pll["connections"]["phase_en"] = ["0"]
                pll["port_directions"]["phase_en"] = "input"
                reasons = ("unsupported port",)
            else:
                third = pll["connections"]["outclk"][2]
                buffer = next(c for c in invalid_cells.values()
                              if c["type"] == "MISTRAL_CLKBUF" and c["connections"]["A"] == [third])
                if name == "disconnected-third":
                    # Leave the PLL output with no buffer sink, retaining its output port.
                    buffer["connections"]["A"] = ["0"]
                else:
                    ff = next(c for c in invalid_cells.values()
                              if c["type"] == "MISTRAL_FF"
                              and c["connections"]["CLK"] == buffer["connections"]["Q"])
                    ff["connections"]["CLK"] = [third]
                reasons = ("outclk[2]", "clock buffer")
            reject(name, invalid, reasons)
        conflicting_sdc = out / "conflicting-third.sdc"
        conflicting_sdc.write_text(sdc.read_text() +
                                  f"create_clock -period {2000 / float(args.frequencies[-1])} [get_nets {{clocks[2]}}]\n")
        conflicting_command = command.copy()
        conflicting_command[conflicting_command.index("--sdc") + 1] = str(conflicting_sdc)
        reject("conflicting-third", design, ("conflicting clock constraint",), conflicting_command)
    print("PASS: triple PLL host checks", report["fmax"])
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
