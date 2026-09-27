#!/usr/bin/env python3
"""Reject Cyclone V M10K asynchronous reads before placement or timing."""

import argparse
import json
from pathlib import Path
import subprocess


DEVICE = "5CSEBA6U23I7"
ERROR = "Cyclone V M10K does not support asynchronous reads"


def synthesize(args, case, source, parameters=""):
    script = case / "synth.ys"
    script.write_text(
        f"read_verilog {source}\n"
        f"{parameters}"
        "synth_intel_alm -nolutram -nodsp -top top\n"
        f"write_json {case / 'base.json'}\n"
    )
    with (case / "synth.log").open("w") as stream:
        subprocess.run([str(args.yosys.resolve()), "-Q", "-T", "-s", str(script)],
                       stdout=stream, stderr=subprocess.STDOUT, check=True)
    return json.loads((case / "base.json").read_text())


def pack(args, qsf, case, design, rejected):
    case.mkdir(exist_ok=True)
    fixture = case / "synth.json"
    fixture.write_text(json.dumps(design))
    result = subprocess.run(
        [str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(qsf), "--sdc", str(args.sdc.resolve()),
         "--json", str(fixture), "--pack-only"],
        capture_output=True, text=True, timeout=120)
    log = result.stdout + result.stderr
    (case / "pack.log").write_text(log)
    if rejected:
        assert result.returncode != 0, (case, log)
        assert ERROR in log, (case, log)
    else:
        assert result.returncode == 0, (case, log)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    qsf = output / "pins.qsf"
    qsf.write_text(args.qsf.read_text().replace("-to LED[0]", "-to LED"))

    source = Path(__file__).with_name("async_read.v").resolve()
    for width, abits in ((10, 10), (20, 9), (40, 8)):
        case = output / f"sdp{width}"
        case.mkdir(exist_ok=True)
        design = synthesize(args, case, source,
                            f"chparam -set WIDTH {width} top\n")
        cell = next(c for c in design["modules"]["top"]["cells"].values()
                    if c["type"] == "MISTRAL_M10K")
        assert int(cell["parameters"]["CFG_ABITS"], 2) == abits
        for enable in ("explicit", "omitted"):
            variant = json.loads(json.dumps(design))
            ram = next(c for c in variant["modules"]["top"]["cells"].values()
                       if c["type"] == "MISTRAL_M10K")
            ram["parameters"]["CFG_ASYNC_READ"] = f"{int(enable == 'explicit'):032b}"
            if enable == "omitted":
                ram["connections"].pop("B1EN", None)
                ram["port_directions"].pop("B1EN", None)
            pack(args, qsf, output / f"sdp{width}-{enable}", variant, True)
        pack(args, qsf, output / f"sdp{width}-sync", design, False)

    tdp_source = Path(__file__).with_name("async_tdp.v").resolve()
    tdp_case = output / "tdp"
    tdp_case.mkdir(exist_ok=True)
    tdp = synthesize(args, tdp_case, tdp_source)
    tdp_cell = next(c for c in tdp["modules"]["top"]["cells"].values()
                    if c["type"] == "MISTRAL_M10K_TDP")
    assert int(tdp_cell["parameters"]["CFG_ASYNC_READ"], 2) == 1
    pack(args, qsf, output / "tdp-async", tdp, True)
    tdp_cell["parameters"]["CFG_ASYNC_READ"] = f"{0:032b}"
    pack(args, qsf, output / "tdp-sync", tdp, False)
    print("PASS: async SDP 10/20/40 and TDP rejected; synchronous modes packed")


if __name__ == "__main__":
    main()
