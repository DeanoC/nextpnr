#!/usr/bin/env python3
"""Check DSP signedness configuration against Quartus 17.0.2 references.

Synthesises the probe designs in fixtures/dsp-signedness/rtl with a Yosys whose
intel_alm dsp_map.v forwards A_SIGNED/B_SIGNED, routes them, decompiles the
bitstream and compares every DSP block's settings, operand groups and
sign-replicated groups with the recorded Quartus decompiles. Host-only
configuration evidence; it does not program a board or verify arithmetic.
"""
import argparse
import collections
import json
import re
import subprocess
from pathlib import Path

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "dsp-signedness"
IGNORE = {"CLK0_SEL", "CLK1_SEL", "CLK2_SEL", "ACLR0_SEL", "ACLR1_SEL"}
# name, rtl, chparam, Quartus reference, DATA_INV groups the probe cannot drive
# (the Quartus add-C references carry a C operand on groups 6..9), expected
# number of DSP sites.
CASES = (
    ("u9", "probe.v", "-set W 9 -set SIGNED 0 -set LANES 1", "mul_u9_q", (), 1),
    ("s9", "probe.v", "-set W 9 -set SIGNED 1 -set LANES 1", "mul_s9_q", (), 1),
    ("u27", "probe.v", "-set W 27 -set SIGNED 0 -set LANES 1", "mul_u27_q", (), 1),
    ("s27", "probe.v", "-set W 27 -set SIGNED 1 -set LANES 1", "mul_s27_q", (), 1),
    ("u18", "probe.v", "-set W 18 -set SIGNED 0 -set LANES 1", "mac_u18_q", (6, 7, 8, 9), 1),
    ("s18", "probe.v", "-set W 18 -set SIGNED 1 -set LANES 1", "mac_s18_q", (6, 7, 8, 9), 1),
    # Three unsigned products share one block; three signed products each own
    # one block configured like the single signed product.
    ("u9x3", "probe.v", "-set W 9 -set SIGNED 0 -set LANES 3", None, (), 1),
    ("s9x3", "probe.v", "-set W 9 -set SIGNED 1 -set LANES 3", "mul_s9_q", (), 3),
)


def run(command, log):
    with open(log, "w") as handle:
        subprocess.run(command, check=True, stdout=handle, stderr=subprocess.STDOUT)


def describe(bt_text, drop_groups):
    drivers = {}
    sites = collections.defaultdict(dict)
    for line in bt_text.splitlines():
        if line.startswith("r "):
            _, src, dst = line.split()[:3]
            drivers[dst] = src
        match = re.match(r"^s (DSP\.\d+\.\d+):(\S+) (\S+)$", line)
        if match and match.group(2) not in IGNORE:
            sites[match.group(1)][match.group(2)] = match.group(3)

    def origin(node):
        for _ in range(8):
            if node in drivers:
                node = drivers[node]
            else:
                break
        return node

    groups = collections.defaultdict(lambda: collections.defaultdict(dict))
    for dst, src in drivers.items():
        match = re.match(r"(DSP\.\d+\.\d+)\.(\d+):DATAIN\.(\d+)$", dst)
        if match:
            groups[match.group(1)][int(match.group(2))][int(match.group(3))] = origin(src)
    result = []
    for site in sorted(sites):
        settings = {k: v for k, v in sites[site].items()
                    if not any(k == f"DATA_INV.{g}" for g in drop_groups)}
        used = {g: bits for g, bits in groups.get(site, {}).items() if g not in drop_groups}
        result.append({
            "settings": dict(sorted(settings.items())),
            "groups_used": sorted(used),
            "groups_sign_replicated": sorted(
                g for g, bits in used.items() if len(bits) > 1 and len(set(bits.values())) == 1),
        })
    return result


def reference(name, drop_groups):
    data = json.loads((FIXTURES / "quartus" / f"{name}.dsp.json").read_text())
    sites = []
    for site in data["sites"]:
        settings = {k: v for k, v in site["settings"].items()
                    if not any(k == f"DATA_INV.{g}" for g in drop_groups)}
        sites.append({
            "settings": dict(sorted(settings.items())),
            "groups_used": [g for g in site["groups_used"] if g not in drop_groups],
            "groups_sign_replicated": [g for g in site["groups_sign_replicated"] if g not in drop_groups],
        })
    return sites


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    for name, rtl, chparam, ref_name, drop, site_count in CASES:
        out = args.output.resolve() / name
        out.mkdir(parents=True, exist_ok=True)
        run([str(args.yosys.resolve()), "-q", "-p",
             f"read_verilog {FIXTURES / 'rtl' / rtl}; chparam {chparam} top; "
             f"synth_intel_alm -nobram -nolutram -top top; write_json {out / 'synth.json'}"],
            out / "yosys.log")
        cells = json.loads((out / "synth.json").read_text())["modules"]["top"]["cells"]
        for cell in cells.values():
            if cell["type"].startswith("MISTRAL_MUL"):
                assert {"A_SIGNED", "B_SIGNED"} <= set(cell["parameters"]), (
                    name, "Yosys must forward multiplier signedness", cell["parameters"])
        run([str(args.nextpnr.resolve()), "--json", str(out / "synth.json"),
             "--device", "5CSEBA6U23I7", "--qsf", str(args.qsf.resolve()),
             "--sdc", str(args.sdc.resolve()), "--freq", "50", "--compress-rbf",
             "--rbf", str(out / "top.rbf"), "--write", str(out / "routed.json"),
             "--report", str(out / "timing.json")], out / "route.log")
        log = (out / "route.log").read_text()
        assert "omits A_SIGNED" not in log and "omits B_SIGNED" not in log, name
        report = json.loads((out / "timing.json").read_text())
        for clock in report["fmax"].values():
            assert clock["achieved"] >= clock["constraint"], (name, report["fmax"])
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
             str(out / "top.rbf"), str(out / "top.bt")], out / "decomp.log")
        actual = describe((out / "top.bt").read_text(), drop)
        assert len(actual) == site_count, (name, len(actual))
        if ref_name is not None:
            expected = reference(ref_name, drop)
            assert len(expected) == 1, ref_name
            for site in actual:
                assert site == expected[0], (name, json.dumps(site, indent=1), json.dumps(expected[0], indent=1))
        else:
            site = actual[0]
            assert site["settings"]["MODE"] == "M9X9" and not any(
                k.endswith("_SIGNED") for k in site["settings"]), (name, site)
            assert site["groups_used"] == [0, 2, 6, 7, 8, 9], (name, site)
        print(name, "PASS", f"{len(actual)} site(s)", f"vs {ref_name or 'three-lane rule'}", flush=True)

    # A netlist without the parameters still routes with the signed default
    # and says so.
    out = args.output.resolve() / "legacy"
    out.mkdir(parents=True, exist_ok=True)
    design = json.loads((args.output.resolve() / "u9" / "synth.json").read_text())
    for cell in design["modules"]["top"]["cells"].values():
        if cell["type"].startswith("MISTRAL_MUL"):
            cell["parameters"].pop("A_SIGNED")
            cell["parameters"].pop("B_SIGNED")
    (out / "synth.json").write_text(json.dumps(design))
    run([str(args.nextpnr.resolve()), "--json", str(out / "synth.json"),
         "--device", "5CSEBA6U23I7", "--qsf", str(args.qsf.resolve()),
         "--sdc", str(args.sdc.resolve()), "--freq", "50", "--compress-rbf",
         "--rbf", str(out / "top.rbf")], out / "route.log")
    log = (out / "route.log").read_text()
    assert "omits A_SIGNED" in log and "omits B_SIGNED" in log
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "top.rbf"), str(out / "top.bt")], out / "decomp.log")
    legacy = describe((out / "top.bt").read_text(), ())
    assert legacy == reference("mul_s9_q", ()), "legacy netlists keep the signed configuration"
    print("legacy PASS warned and configured signed", flush=True)


if __name__ == "__main__":
    main()
