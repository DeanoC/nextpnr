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


def run_text(command, log):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log.write_text(result.stdout)
    return result


def settings_of(bt_path):
    return dict(re.findall(r"^s (\S+) (\S+)$", bt_path.read_text(), re.M))


def open_flop(routed_path):
    cells = json.loads(routed_path.read_text())["modules"]["top"]["cells"]
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
    assert (opens[0][2], opens[0][3]) == (driven[0][2], driven[0][3]), flops
    return opens[0]


def assert_dedicated_aclr(bt_path, flop):
    name, _, x, y, z = flop
    settings = settings_of(bt_path)
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
    assert settings.get(f"{prefix}:ACLR1_SEL") == "ACLR1", prefix
    assert settings.get(f"{prefix}:ACLR0_SEL") in (None, "DIN3"), prefix
    return alm, half, prefix


def lab_row(document, x, y):
    payload = json.loads(bytes.fromhex(document["modules"]["top"]["attributes"]["FES_LABSTATE_V1"]).decode())
    for row in payload["labs"]:
        if int(row[0]) == x and int(row[1]) == y:
            return payload, row
    raise AssertionError((x, y, "missing LAB state"))


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
    nextpnr = str(args.nextpnr.resolve())
    run([nextpnr, "--device", "5CSEBA6U23I7", "--json", str(out / "synth.json"),
         "--qsf", str(here / "pins.qsf"), "--seed", "1", "--write", str(out / "routed.json"),
         "--rbf", str(out / "core.rbf"), "--compress-rbf"], out / "route.log")
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(out / "core.rbf"),
         str(out / "core.bt")], out / "decompile.log")
    flop = open_flop(out / "routed.json")
    alm, half, prefix = assert_dedicated_aclr(out / "core.bt", flop)
    name, _, x, y, _ = flop

    # A shell routed before this park stored the open half on slot 0.
    # --fes-scaffold restores that V1 index and skips lab_pre_route.
    document = json.loads((out / "routed.json").read_text())
    payload, row = lab_row(document, x, y)
    state = row[3]
    field = 2 + 6 * alm + 4 + half
    assert state[0] == 1 and state[field] == 1, (state[0], state[1], state[field])
    state[field] = 0
    document["modules"]["top"]["attributes"]["FES_LABSTATE_V1"] = json.dumps(payload).encode().hex()
    (out / "stale.json").write_text(json.dumps(document))
    run([nextpnr, "--device", "5CSEBA6U23I7", "--json", str(out / "stale.json"),
         "--fes-scaffold", "--no-pack", "--no-place", "--no-route",
         "--write", str(out / "reloaded.json"), "--rbf", str(out / "reloaded.rbf")],
        out / "reload.log")
    reload_log = (out / "reload.log").read_text()
    assert "FES parked 1 open flip-flop" in reload_log, reload_log
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(out / "reloaded.rbf"),
         str(out / "reloaded.bt")], out / "reload-decompile.log")
    assert_dedicated_aclr(out / "reloaded.bt", flop)
    reloaded = json.loads((out / "reloaded.json").read_text())
    _, reloaded_row = lab_row(reloaded, x, y)
    assert reloaded_row[3][field] == 1, reloaded_row[3][field]

    state[1] = 1
    document["modules"]["top"]["attributes"]["FES_LABSTATE_V1"] = json.dumps(payload).encode().hex()
    (out / "both-slots.json").write_text(json.dumps(document))
    rejected = run_text([nextpnr, "--device", "5CSEBA6U23I7", "--json", str(out / "both-slots.json"),
                         "--fes-scaffold", "--no-pack", "--no-place", "--no-route"],
                        out / "both-slots.log")
    assert rejected.returncode != 0 and "both ACLR slots are used" in rejected.stdout, rejected.stdout
    print(f"PASS: {name} at {prefix} ALM {alm} half {half} selects dedicated ACLR1")
    print("PASS: stale FES_LABSTATE_V1 reloads onto that clear; both live slots are rejected")


if __name__ == "__main__":
    main()
