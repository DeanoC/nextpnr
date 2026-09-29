#!/usr/bin/env python3
"""Legalise a LAB that already holds a BEL-locked flip-flop.

Seal right-PCM bits 6, 7, and 8 left a bottom LUT on comb_pinmap inside a
socket LAB. Seed 6's constant 512 left locked plug_rdata_ff_10, with no
route-through, on fabric F1. This fixture places both shapes and drives
lab_pre_route from placement.
"""

import argparse
import json
import re
import subprocess
from pathlib import Path


OLD_PIN = "69f8237b"


def run(command, log):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)


def strength_of(cell):
    raw = cell["attributes"]["BEL_STRENGTH"]
    bits = "".join(ch for ch in str(raw) if ch in "01")
    return int(bits, 2)


def param_int(value):
    text = str(value).strip()
    if "'b" in text:
        return int(text.split("'b", 1)[1], 2)
    if "'h" in text:
        return int(text.split("'h", 1)[1], 16)
    digits = "".join(ch for ch in text if ch in "01")
    if digits and set(text) <= set("01 \t"):
        return int(digits, 2)
    return int(text, 0)


def half_mask(settings, bel):
    _kind, x, y, z = bel.split(".")
    z = int(z)
    key = f"LAB.{int(x):03d}.{int(y):03d}:LUT_MASK.{z // 6}"
    value = int(settings[key].replace(".", ""), 16)
    return (value >> (32 * (z % 6))) & 0xFFFFFFFF


def route_points(text):
    points = set()
    for line in text.splitlines():
        if not line.startswith("r "):
            continue
        parts = line.split()
        if len(parts) >= 3:
            points.add(parts[1])
            points.add(parts[2])
    return points


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = here.parents[2]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    run(
        [
            str(args.yosys.resolve()),
            "-p",
            f'read_verilog "{here / "top.v"}"; hierarchy -top top; '
            "iopadmap -bits -inpad MISTRAL_IB O:PAD -outpad MISTRAL_OB I:PAD; "
            f'write_json "{out / "input.json"}"',
        ],
        out / "yosys.log",
    )
    run(
        [
            str(args.nextpnr.resolve()),
            "--device",
            "5CSEBA6U23I7",
            "--qsf",
            str(here / "pins.qsf"),
            "--json",
            str(out / "input.json"),
            "--write",
            str(out / "routed.json"),
            "--rbf",
            str(out / "core.rbf"),
        ],
        out / "nextpnr.log",
    )
    run(
        [
            str(args.mistral_cv.resolve()),
            "decomp",
            "5CSEBA6U23I7",
            str(out / "core.rbf"),
            str(out / "core.bt"),
        ],
        out / "decompile.log",
    )

    version = subprocess.run(
        [str(args.nextpnr.resolve()), "--version"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    header = next(line for line in version.stdout.splitlines() if "Version " in line)
    revision = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
    ).strip()
    print(header)
    print(f"source {revision}")

    design = json.loads((out / "routed.json").read_text())
    cells = design["modules"]["top"]["cells"]
    settings = dict(re.findall(r"^s (\S+) (\S+)$", (out / "core.bt").read_text(), re.M))
    points = route_points((out / "core.bt").read_text())

    flop = cells["plug_rdata_ff_10"]
    buffer = cells["plug_rdata_ff_10$ROUTETHRU"]
    lut = cells["seal_pcm_bits_678"]
    const = cells["const0"]
    flop_bel = flop["attributes"]["NEXTPNR_BEL"]
    buffer_bel = buffer["attributes"]["NEXTPNR_BEL"]
    lut_bel = lut["attributes"]["NEXTPNR_BEL"]
    flop_strength = strength_of(flop)
    buffer_strength = strength_of(buffer)
    lut_strength = strength_of(lut)
    lut_mask = half_mask(settings, lut_bel)
    buffer_mask = half_mask(settings, buffer_bel)
    const_lut = param_int(const["parameters"]["LUT"])

    print(f"plug_rdata_ff_10 site {flop_bel} strength {flop_strength}")
    print(f"plug_rdata_ff_10$ROUTETHRU site {buffer_bel} strength {buffer_strength}")
    print(f"seal_pcm_bits_678 site {lut_bel} strength {lut_strength} mask 0x{lut_mask:08x}")
    print(f"const0 LUT {const_lut} route-through mask 0x{buffer_mask:08x}")

    assert flop_bel == "MISTRAL_FF.7.11.10", flop_bel
    assert flop_strength == 6, flop_strength
    assert lut_bel == "MISTRAL_COMB.7.11.1", lut_bel
    assert lut_strength == 6, lut_strength
    assert buffer["type"] == "MISTRAL_BUF", buffer["type"]
    assert buffer_bel == "MISTRAL_COMB.7.11.7", buffer_bel
    assert buffer_strength == 2, buffer_strength
    assert flop["connections"]["DATAIN"] == buffer["connections"]["Q"]
    assert buffer["connections"]["A"] == const["connections"]["Q"]
    assert const["attributes"]["NEXTPNR_BEL"] == "MISTRAL_COMB.7.11.13"
    assert const_lut == 0, const["parameters"]["LUT"]
    # Bottom AND on D (bit 2) and E1 (bit 3). Comb pinmap E0/F0 stores 0xffffff00.
    assert lut_mask == 0xFFF0FFF0, hex(lut_mask)
    assert buffer_mask == 0xF0F0F0F0, hex(buffer_mask)
    for pin in ("LAB.007.011.0:D", "LAB.007.011.0:E1", "LAB.007.011.1:D"):
        assert pin in points, pin
    for pin in ("LAB.007.011.0:E0", "LAB.007.011.0:F0", "LAB.007.011.1:F1"):
        assert pin not in points, pin
    assert "s LAB.007.011:BPKREG1.1 1" not in (out / "core.bt").read_text()
    assert OLD_PIN not in header, header
    assert not revision.startswith(OLD_PIN), revision
    print("PASS: bottom LUT reads D and E1; locked FF data is the route-through of constant 0")


if __name__ == "__main__":
    main()
