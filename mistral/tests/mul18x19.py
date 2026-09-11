#!/usr/bin/env python3
"""Route one native Cyclone V 18x19 DSP mode.

The locked 060 JSON supplies the board clock, HPS GP and IO constraints.  The
fixture widens its single 9x9 cell into the dual-product interface used by the
native DSP modes so the backend can be tested without changing the pinned
Yosys or misteross trees.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess


def make_fixture(path: Path, mode: str) -> dict:
    design = json.loads(path.read_text())
    cells = design["modules"]["top"]["cells"]
    names = [name for name, cell in cells.items() if cell["type"] == "MISTRAL_MUL9X9"]
    assert len(names) == 1, names
    mul = cells[names[0]]
    mul["type"] = "MISTRAL_MUL18X19" if mode == "parallel" else "MISTRAL_MUL18X19_COMBINED"
    mul["attributes"]["src"] = mul["attributes"].get("src", "") + f"|dsp-mode-test:18x19-{mode}"

    # Keep every input driven by an existing fixture net.  Reusing the nets is
    # intentional: it exercises fanout into all four physical operand groups
    # while avoiding un-driven JSON nets that would be discarded by nextpnr.
    a = list(mul["connections"]["A"])
    b = list(mul["connections"]["B"])
    mul["connections"]["A"] = a + [a[bit % len(a)] for bit in range(9)]
    mul["connections"]["B"] = b + [b[bit % len(b)] for bit in range(10)]
    mul["port_directions"]["C"] = "input"
    mul["port_directions"]["D"] = "input"
    mul["connections"]["C"] = [a[bit % len(a)] for bit in range(18)]
    mul["connections"]["D"] = [b[bit % len(b)] for bit in range(19)]

    if mode == "combined":
        # Exercise the add/sub control as a retained constant.  The bitstream
        # must program SUB_INV rather than leave the silicon default floating.
        mul["port_directions"]["SUB"] = "input"
        mul["connections"]["SUB"] = ["0"]

    used = [
        bit
        for cell in cells.values()
        for bits in cell.get("connections", {}).values()
        for bit in bits
        if isinstance(bit, int)
    ]
    next_net = max(used) + 1
    result_width = 74 if mode == "parallel" else 38
    missing_y_bits = result_width - len(mul["connections"]["Y"])
    assert missing_y_bits >= 0
    mul["connections"]["Y"] = list(mul["connections"]["Y"]) + list(
        range(next_net, next_net + missing_y_bits)
    )
    return design


def run(command, log: Path):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("nextpnr", "mistral-cv", "fixture", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--mode", choices=("parallel", "combined"), default="parallel")
    args = parser.parse_args()

    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fixture = out / "synth.json"
    fixture.write_text(json.dumps(make_fixture(args.fixture, args.mode)))
    cell_type = "MISTRAL_MUL18X19" if args.mode == "parallel" else "MISTRAL_MUL18X19_COMBINED"
    run(
        [
            str(args.nextpnr.resolve()),
            "--json",
            str(fixture),
            "--device",
            "5CSEBA6U23I7",
            "--qsf",
            str(args.qsf.resolve()),
            "--sdc",
            str(args.sdc.resolve()),
            "--freq",
            "50",
            "--compress-rbf",
            "--rbf",
            str(out / "top.rbf"),
            "--write",
            str(out / "routed.json"),
            "--report",
            str(out / "timing.json"),
            "--detailed-timing-report",
        ],
        out / "route.log",
    )

    report = json.loads((out / "timing.json").read_text())
    util = report["utilization"]
    assert util[cell_type] == {"used": 1, "available": 112}, util
    assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    for kind in ("MISTRAL_MUL9X9", "MISTRAL_MUL18X18", "MISTRAL_MUL27X27"):
        assert util[kind]["used"] == 0
    assert util["MISTRAL_M10K"]["used"] == 0
    assert util["altera_pll"]["used"] == 0
    clock = report["fmax"]["product.FPGA_CLK1_50"]
    assert clock["constraint"] == 50 and clock["achieved"] >= 50, clock

    run(
        [
            str(args.mistral_cv.resolve()),
            "decomp",
            "5CSEBA6U23I7",
            str(out / "top.rbf"),
            str(out / "top.bt"),
        ],
        out / "decomp.log",
    )
    bt = (out / "top.bt").read_text()
    expected_mode = "M18X19" if args.mode == "parallel" else "M18X19_COMBINED"
    if args.mode == "parallel":
        # Mistral marks M18X19 as the default mode and omits that setting from
        # decomp output; the signedness settings still identify the site.
        sites = re.findall(r"^s (DSP\.\d+\.\d+):BY_SIGNED ", bt, re.M)
    else:
        sites = re.findall(r"^s (DSP\.\d+\.\d+):MODE " + expected_mode + r"$", bt, re.M)
    assert len(sites) == 1, sites
    site = sites[0]

    # The four operand pairs occupy AX/AY and BX/BY groups.  The 19th bit of
    # each Y operand is on a currently unnamed DSP GOUT endpoint; assert that
    # it is routed as a BEL pin as well as checking all ordinary DATAIN slices.
    for group in (0, 1, 2, 3, 6, 7, 8, 9):
        assert re.search(r"^r \S+ " + re.escape(f"{site}.{group}:DATAIN") + r"\.\d+$", bt, re.M), group
    assert re.search(r"^r \S+ " + re.escape(f"{site}:UNK_IN.94") + r"$", bt, re.M)
    assert re.search(r"^r \S+ " + re.escape(f"{site}:UNK_IN.30") + r"$", bt, re.M)
    if args.mode == "combined":
        settings = dict(re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$", bt, re.M))
        assert settings.get("SUB_INV") == "1", settings
        assert not re.search(r"^r \S+ " + re.escape(f"{site}:SUB") + r"$", bt, re.M)
    assert (out / "top.rbf").read_bytes()
    print("PASS", sites[0], args.mode)


if __name__ == "__main__":
    main()
