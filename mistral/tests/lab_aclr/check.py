#!/usr/bin/env python3
"""A flip-flop with no async clear must not inherit a LAB clear another flop uses."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def run(command, log):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run([str(args.yosys.resolve()), "-p",
         f"read_verilog {here / 'top.v'}; synth_intel_alm -nobram -nodsp -top top; "
         f"write_json {out / 'synth.json'}"], out / "synth.log")
    run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7", "--json", str(out / "synth.json"),
         "--qsf", str(here / "pins.qsf"), "--seed", "1", "--write", str(out / "routed.json"),
         "--rbf", str(out / "core.rbf"), "--compress-rbf"], out / "route.log")
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(out / "core.rbf"),
         str(out / "core.bt")], out / "decompile.log")
    cells = json.loads((out / "routed.json").read_text())["modules"]["top"]["cells"]
    settings = dict(re.findall(r"^s (\S+) (\S+)$", (out / "core.bt").read_text(), re.M))
    flops = []
    for name, cell in cells.items():
        if cell["type"] != "MISTRAL_FF":
            continue
        aclr = cell["connections"].get("ACLR", ["", ""])
        driven = bool(aclr and aclr[0])
        bel = cell["attributes"]["NEXTPNR_BEL"]
        _, x, y, z = bel.split(".")
        flops.append((name, driven, int(x), int(y), int(z)))
    assert len(flops) == 2, flops
    opens = [item for item in flops if not item[1]]
    driven = [item for item in flops if item[1]]
    assert len(opens) == 1 and len(driven) == 1, flops
    name, _, x, y, z = opens[0]
    assert (x, y) == (driven[0][2], driven[0][3]), (flops, "open and reset flops split across LABs")
    # BEL z is creation order: two COMBs then four FFs per ALM.
    ff_index = (z % 6) - 2
    assert ff_index in range(4), (name, z)
    alm = z // 6
    half = ff_index // 2
    clr = "TCLR_SEL" if half == 0 else "BCLR_SEL"
    kinds = [kind for kind in ("LAB", "MLAB")
             if any(key.startswith(f"{kind}.{x:03d}.{y:03d}:") for key in settings)]
    assert len(kinds) == 1, (x, y, kinds)
    prefix = f"{kinds[0]}.{x:03d}.{y:03d}"
    clr_key = f"{prefix}:{clr}.{alm}"
    assert settings.get(clr_key) == "1", (clr_key, settings.get(clr_key), name)
    aclr1 = settings.get(f"{prefix}:ACLR1_SEL")
    assert aclr1 == "ACLR1", (prefix, aclr1)
    aclr0 = settings.get(f"{prefix}:ACLR0_SEL")
    assert aclr0 in (None, "DIN3"), (prefix, aclr0)
    print(f"PASS: {name} at {prefix} ALM {alm} half {half} selects dedicated ACLR1")


if __name__ == "__main__":
    main()
