#!/usr/bin/env python3
"""Check the MiSTer-compatible HPS I2C hard-block route to the ADV7513."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess


CELL_TYPE = "cyclonev_hps_interface_peripheral_i2c"
BEL = f"{CELL_TYPE}.52.60.0"
REFERENCE_SHA256 = "1567e5ea4db1f18b9f23b48e7a4b7604024a998ddf1378bf77fe5968e00c64d1"


def run(command, log, success=True):
    with log.open("w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    assert (result.returncode == 0) == success, log.read_text()
    return log.read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--reference-rbf", type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    run(
        [
            str(args.yosys.resolve()),
            "-p",
            f'read_verilog "{fixture / "top.v"}"; '
            "synth_intel_alm -nobram -nolutram -nodsp -top top; "
            f'write_json "{output / "synth.json"}"',
        ],
        output / "yosys.log",
    )
    design = json.loads((output / "synth.json").read_text())
    cells = design["modules"]["top"]["cells"]
    i2c = [cell for cell in cells.values() if cell["type"] == CELL_TYPE]
    assert len(i2c) == 1, i2c
    assert i2c[0]["port_directions"] == {
        "out_clk": "output",
        "out_data": "output",
        "scl": "input",
        "sda": "input",
    }, i2c[0]
    assert set(i2c[0]["connections"]) == {"out_clk", "out_data", "scl", "sda"}
    assert i2c[0]["attributes"]["BEL"] == BEL

    io_cells = {name: cell for name, cell in cells.items() if cell["type"] == "MISTRAL_IO"}
    assert set(io_cells) == {"scl_pad", "sda_pad"}, io_cells
    i2c_connections = i2c[0]["connections"]
    for pad, enable, feedback in (
        (io_cells["scl_pad"], "out_clk", "scl"),
        (io_cells["sda_pad"], "out_data", "sda"),
    ):
        assert pad["connections"]["I"] == ["0"], pad
        assert pad["connections"]["OE"] == i2c_connections[enable], (pad, i2c[0])
        assert pad["connections"]["O"] == i2c_connections[feedback], (pad, i2c[0])

    command = [
        str(args.nextpnr.resolve()),
        "--device",
        "5CSEBA6U23I7",
        "--qsf",
        str(fixture / "pins.qsf"),
    ]
    run(
        command
        + [
            "--json",
            str(output / "synth.json"),
            "--rbf",
            str(output / "top.rbf"),
            "--report",
            str(output / "report.json"),
            "--write",
            str(output / "routed.json"),
        ],
        output / "route.log",
    )
    report = json.loads((output / "report.json").read_text())
    assert report["utilization"][CELL_TYPE] == {"used": 1, "available": 4}
    routed = json.loads((output / "routed.json").read_text())
    routed_cells = routed["modules"]["top"]["cells"]
    routed_i2c = next(cell for cell in routed_cells.values() if cell["type"] == CELL_TYPE)
    assert routed_i2c["attributes"]["NEXTPNR_BEL"] == BEL
    ground = next(
        cell for cell in routed_cells.values()
        if cell["type"] == "MISTRAL_CONST" and int(cell["parameters"]["LUT"], 2) == 0
    )
    for name in ("scl_pad", "sda_pad"):
        assert routed_cells[name]["connections"]["I"] == ground["connections"]["Q"], (
            routed_cells[name], ground
        )

    routes = run(
        [str(args.mistral_cv.resolve()), "routes", "5CSEBA6U23I7", str(output / "top.rbf")],
        output / "routes.log",
    ).splitlines()
    for expected in (fixture / "expected-routes.txt").read_text().splitlines():
        assert expected in routes, (expected, routes)
    for dataout in ("GPIO.006.000.0:DATAOUT.0 ; U10", "GPIO.004.000.2:DATAOUT.0 ; AA4"):
        assert any(line.endswith(dataout) for line in routes), (dataout, routes)
    for unsafe in (
        "HPS_PERIPHERAL_I2C.052.060:OUT_CLK GPIO.006.000.0:DATAOUT.0",
        "HPS_PERIPHERAL_I2C.052.060:OUT_DATA GPIO.004.000.2:DATAOUT.0",
    ):
        assert not any(line.startswith(unsafe) for line in routes), (unsafe, routes)

    if args.reference_rbf:
        reference = args.reference_rbf.resolve()
        reference_routes = run(
            [str(args.mistral_cv.resolve()), "routes", "5CSEBA6U23I7", str(reference)],
            output / "quartus-reference-routes.log",
        ).splitlines()
        for oracle in (fixture / "quartus-routes.txt").read_text().splitlines():
            assert oracle in reference_routes, (oracle, reference_routes)
        digest = hashlib.sha256(reference.read_bytes()).hexdigest()
        assert digest == REFERENCE_SHA256, digest
        (output / "quartus-reference-sha256.txt").write_text(digest + "\n")

    for y in (58, 59, 60, 61):
        at_site = copy.deepcopy(design)
        site_i2c = next(cell for cell in at_site["modules"]["top"]["cells"].values() if cell["type"] == CELL_TYPE)
        site_i2c["attributes"]["BEL"] = f"{CELL_TYPE}.52.{y}.0"
        site_path = output / f"site-{y}.json"
        site_path.write_text(json.dumps(at_site))
        run(command + ["--json", str(site_path), "--no-route"], output / f"site-{y}.log")

    for name, bad_bel, bad_type, expected in (
        ("unavailable", f"{CELL_TYPE}.52.57.0", CELL_TYPE, "No Bel named"),
        ("mismatched", BEL, "cyclonev_hps_interface_mpu_general_purpose", "does not match cell"),
    ):
        invalid = copy.deepcopy(design)
        invalid_i2c = next(cell for cell in invalid["modules"]["top"]["cells"].values() if cell["type"] == CELL_TYPE)
        invalid_i2c["attributes"]["BEL"] = bad_bel
        invalid_i2c["type"] = bad_type
        invalid_path = output / f"invalid-{name}.json"
        invalid_path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(invalid_path)], output / f"invalid-{name}.log", success=False)
        assert expected in log, log

    print("PASS: four HPS I2C BELs; exact y60 U10/AA4 low-or-release route")


if __name__ == "__main__":
    main()
