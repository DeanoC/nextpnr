#!/usr/bin/env python3
"""Route mixed-width M10K SDP with byte-enabled 20-bit writes."""

import argparse
import json
from pathlib import Path
import re
import subprocess


CELL_TYPE = "MISTRAL_M10K"


def run(command, log, *, timeout=120, success=True):
    with log.open("w") as stream:
        result = subprocess.run(
            command,
            stdout=stream,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
    if (result.returncode == 0) != success:
        raise AssertionError(f"{command!r}\n{log.read_text()}")
    return log.read_text()


def synthesize(args, output, write_lanes, read_lanes):
    source = Path(__file__).with_name("mixed_width.v").resolve()
    script = output / "synth.ys"
    script.write_text(
        f"read_verilog {source}\n"
        f"chparam -set UNIT 10 -set WLANES {write_lanes} -set RLANES {read_lanes} top\n"
        "synth_intel_alm -nolutram -nodsp -top top\n"
        "select -assert-count 1 t:MISTRAL_M10K\n"
        f"write_json {output / 'base.json'}\n"
    )
    run([str(args.yosys.resolve()), "-Q", "-T", "-s", str(script)], output / "synth.log")
    design = json.loads((output / "base.json").read_text())
    cells = design["modules"]["top"]["cells"]
    name, cell = next((n, c) for n, c in cells.items() if c["type"] == CELL_TYPE)
    hps = next(c for c in cells.values() if c["type"] == "cyclonev_hps_interface_mpu_general_purpose")

    # Locked Yosys does not expose a mixed-width+byte-enable mapping yet.
    # Add the two real ports to an otherwise authentic mixed-width fixture so
    # this backend regression can exercise only nextpnr's packing contract.
    cell["parameters"]["CFG_BYTE_ENABLE"] = "00000000000000000000000000000001"
    cell["connections"]["A1BE"] = [hps["connections"]["gp_out"][5], hps["connections"]["gp_out"][6]]
    cell["port_directions"]["A1BE"] = "input"
    path = output / "synth.json"
    path.write_text(json.dumps(design))
    return design, name, path


def route_case(args, output, write_lanes, read_lanes):
    case = output / f"w{write_lanes * 10}r{read_lanes * 10}"
    case.mkdir(parents=True, exist_ok=True)
    _design, name, fixture = synthesize(args, case, write_lanes, read_lanes)
    run(
        [
            str(args.nextpnr.resolve()),
            "--device",
            "5CSEBA6U23I7",
            "--freq",
            "50",
            "--qsf",
            str(args.qsf.resolve()),
            "--sdc",
            str(args.sdc.resolve()),
            "--json",
            str(fixture),
            "--compress-rbf",
            "--rbf",
            str(case / "top.rbf"),
            "--write",
            str(case / "routed.json"),
            "--report",
            str(case / "timing.json"),
        ],
        case / "route.log",
    )
    report = json.loads((case / "timing.json").read_text())
    assert report["utilization"][CELL_TYPE] == {"available": 553, "used": 1}, report["utilization"][CELL_TYPE]
    assert report["utilization"]["altera_pll"]["used"] == 1
    assert report["utilization"]["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    assert report["fmax"] and all(
        clock["achieved"] >= clock["constraint"] for clock in report["fmax"].values()
    )
    assert (case / "top.rbf").stat().st_size > 0

    routed = json.loads((case / "routed.json").read_text())["modules"]["top"]["cells"][name]
    assert int(routed["parameters"]["CFG_BYTE_ENABLE"], 2) == 1
    assert len(routed["connections"]["A1BE"]) == 2
    _, x, y, _ = routed["attributes"]["NEXTPNR_BEL"].split(".")
    site = f"M10K.{int(x):03d}.{int(y):03d}"
    run(
        [
            str(args.mistral_cv.resolve()),
            "decomp",
            "5CSEBA6U23I7",
            str(case / "top.rbf"),
            str(case / "top.bt"),
        ],
        case / "decomp.log",
    )
    bitstream = (case / "top.bt").read_text()
    fields = dict(
        re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$", bitstream, re.MULTILINE)
    )
    write_bits = write_lanes * 10
    read_bits = read_lanes * 10
    assert fields.get("A_DATA_WIDTH") == str(write_bits), fields
    assert fields.get("B_DATA_WIDTH") == str(read_bits), fields
    assert fields.get("TOP_CE0_SEL") == "1", fields
    assert fields.get("TOP_CORECLK_SEL") == "1", fields
    assert fields.get("BOT_CORECLK_SEL") == "1", fields
    assert fields.get("BOT_INCLK_SEL") == "1", fields
    assert fields.get("BOT_1_INCLK_SEL", "0") == ("0" if 40 in (write_bits, read_bits) else "1"), fields
    assert fields.get("A_DATA_FLOW_THRU", "0") == ("0" if 40 in (write_bits, read_bits) else "1"), fields
    assert fields.get("B_DATA_FLOW_THRU", "0") == ("0" if 40 in (write_bits, read_bits) else "1"), fields
    for pin in ("CLKIN.0", "CLKIN.1", "WREN.0", "ENABLE.0", "ENABLE.1", "BYTEENABLEA.0", "BYTEENABLEA.1"):
        assert re.search(r"^r \S+ " + re.escape(site + ":" + pin) + r"$", bitstream, re.MULTILINE), pin
    assert not re.search(r"^r \S+ " + re.escape(site + ":BYTEENABLEB") + r"$", bitstream, re.MULTILINE)
    print(f"PASS: w{write_bits}r{read_bits}: mixed byte-enabled M10K, masks, selectors, RBF and timing")


def reject_case(args, output, write_lanes, read_lanes):
    case = output / f"reject-w{write_lanes * 10}r{read_lanes * 10}"
    case.mkdir(parents=True, exist_ok=True)
    _design, _name, fixture = synthesize(args, case, write_lanes, read_lanes)
    log = run(
        [
            str(args.nextpnr.resolve()),
            "--device",
            "5CSEBA6U23I7",
            "--qsf",
            str(args.qsf.resolve()),
            "--json",
            str(fixture),
        ],
        case / "route.log",
        success=False,
    )
    assert "mixed-width byte enables require a 20-bit write port" in log, log


def reject_extra_mask(args, output):
    case = output / "reject-extra-mask"
    case.mkdir(parents=True, exist_ok=True)
    design, name, fixture = synthesize(args, case, 2, 1)
    cell = design["modules"]["top"]["cells"][name]
    cell["connections"]["A1BE"].append(cell["connections"]["A1BE"][0])
    fixture.write_text(json.dumps(design))
    log = run(
        [
            str(args.nextpnr.resolve()),
            "--device",
            "5CSEBA6U23I7",
            "--qsf",
            str(args.qsf.resolve()),
            "--json",
            str(fixture),
        ],
        case / "route.log",
        success=False,
    )
    assert "mixed-width byte masks must have exactly two bits" in log, log


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    route_case(args, output, 2, 1)
    route_case(args, output, 2, 4)
    reject_case(args, output, 1, 2)
    reject_case(args, output, 4, 1)
    reject_extra_mask(args, output)
    print("PASS: unsupported mixed byte-enable shapes rejected")


if __name__ == "__main__":
    main()
