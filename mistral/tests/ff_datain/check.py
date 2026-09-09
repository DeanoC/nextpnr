#!/usr/bin/env python3
"""Check that every MISTRAL_FF data value selects a real physical source."""

import argparse
import json
from pathlib import Path
import subprocess


def run(command, log):
    with log.open("w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    assert result.returncode == 0, log.read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    run(
        [
            str(args.yosys.resolve()),
            "-p",
            f'read_verilog "{fixture / "top.v"}"; hierarchy -top top; '
            'iopadmap -bits -inpad MISTRAL_IB O:PAD -outpad MISTRAL_OB I:PAD; '
            f'write_json "{output / "input.json"}"',
        ],
        output / "yosys.log",
    )
    run(
        [
            str(args.nextpnr.resolve()),
            "--device", "5CSEBA6U23I7",
            "--qsf", str(fixture / "pins.qsf"),
            "--json", str(output / "input.json"),
            "--write", str(output / "routed.json"),
            "--rbf", str(output / "core.rbf"),
        ],
        output / "nextpnr.log",
    )

    cells = json.loads((output / "routed.json").read_text())["modules"]["top"]["cells"]
    constants = {
        int(cell["parameters"]["LUT"], 2): cell["connections"]["Q"]
        for cell in cells.values()
        if cell["type"] == "MISTRAL_CONST"
    }
    assert cells["ff_zero"]["connections"]["DATAIN"] == constants[0], cells["ff_zero"]
    assert cells["ff_one"]["connections"]["DATAIN"] == constants[1], cells["ff_one"]
    assert cells["ff_signal"]["connections"]["DATAIN"] == cells["$iopadmap$top.signal"]["connections"]["O"]

    netnames = list(json.loads((output / "routed.json").read_text())["modules"]["top"]["netnames"].values())
    for ff_name, endpoint, active_name in (
        ("ff_zero", "WIRE.7.11.FFIN[26]", "active_zero"),
        ("ff_one", "WIRE.7.11.FFIN[30]", "active_one"),
        ("ff_signal", "WIRE.7.11.FFIN[34]", "active_signal"),
    ):
        bit = cells[ff_name]["connections"]["DATAIN"][0]
        route = next(
            net["attributes"]["ROUTING"]
            for net in netnames
            if bit in net["bits"] and "ROUTING" in net.get("attributes", {})
        )
        assert endpoint in route, (ff_name, route)
        active_bit = cells[active_name]["connections"]["Q"][0]
        assert active_bit != bit, (ff_name, active_name)

    bitstream = output / "core.bt"
    run(
        [str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(output / "core.rbf"), str(bitstream)],
        output / "decompile.log",
    )
    settings = set(bitstream.read_text().splitlines())
    for alm in (6, 7, 8):
        assert f"s LAB.007.011:BPKREG1.{alm} 1" in settings, (alm, settings)


if __name__ == "__main__":
    main()
