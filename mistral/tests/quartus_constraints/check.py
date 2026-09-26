#!/usr/bin/env python3
"""Route a PLL fixture with Quartus SDC/QSF syntax."""
import argparse
import json
from pathlib import Path
import subprocess


def run(command, log, success=True):
    with log.open("w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    assert (result.returncode == 0) == success, log
    return log.read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()

    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    run([
        str(args.yosys.resolve()), "-p",
        f'read_verilog "{fixture / "top.v"}"; '
        'synth_intel_alm -nobram -nolutram -nodsp -top top; '
        f'write_json "{out / "synth.json"}"',
    ], out / "yosys.log")
    design = json.loads((out / "synth.json").read_text())
    assert sum(cell["type"] == "altera_pll"
               for cell in design["modules"]["top"]["cells"].values()) == 1

    command = [
        str(args.nextpnr.resolve()),
        "--device", "5CSEBA6U23I7",
        "--qsf", str(fixture / "pins.qsf"),
        "--sdc", str(fixture / "clocks.sdc"),
        "--freq", "50",
        "--compress-rbf",
    ]
    log = run(command + [
        "--json", str(out / "synth.json"),
        "--rbf", str(out / "top.rbf"),
        "--report", str(out / "timing.json"),
        "--write", str(out / "routed.json"),
    ], out / "route.log")
    assert "Unsupported SDC command" not in log
    assert "Unknown option '-entity'" not in log

    report = json.loads((out / "timing.json").read_text())
    assert report["utilization"]["altera_pll"] == {"used": 1, "available": 6}
    assert report["fmax"]["clk25"]["constraint"] == 25
    assert report["fmax"]["clk25"]["achieved"] >= 25
    print("PASS: Quartus SDC/QSF subset routed one PLL and met 25 MHz")


if __name__ == "__main__":
    main()
