#!/usr/bin/env python3
"""Check DSP signedness configuration against Quartus 17.0.2 references.

Synthesises the probe designs in fixtures/dsp-signedness/rtl with a Yosys whose
intel_alm dsp_map.v forwards A_SIGNED/B_SIGNED, routes them, decompiles the
bitstream and compares every DSP block's settings, operand groups and
sign-replicated groups with the recorded Quartus decompiles. probe_mixed.v
instantiates signed-by-unsigned cells directly, with the signed operand on
either input. Host-only configuration evidence; it does not program a board or
verify arithmetic.
"""
import argparse
import collections
import json
import re
import subprocess
from pathlib import Path

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "dsp-signedness"
IGNORE = {"CLK0_SEL", "CLK1_SEL", "CLK2_SEL", "ACLR0_SEL", "ACLR1_SEL"}
# name, rtl, chparam, Quartus references, DATA_INV groups the probe cannot
# drive (the Quartus add-C references carry a C operand on groups 6..9), and
# expected number of DSP sites.
CASES = (
    ("u9", "probe.v", "-set W 9 -set SIGNED 0 -set LANES 1", ("mul_u9_q",), (), 1),
    ("s9", "probe.v", "-set W 9 -set SIGNED 1 -set LANES 1", ("mul_s9_q", "atom_ss9_q"), (), 1),
    ("u27", "probe.v", "-set W 27 -set SIGNED 0 -set LANES 1", ("mul_u27_q",), (), 1),
    ("s27", "probe.v", "-set W 27 -set SIGNED 1 -set LANES 1", ("mul_s27_q",), (), 1),
    ("u18", "probe.v", "-set W 18 -set SIGNED 0 -set LANES 1", ("mac_u18_q",), (6, 7, 8, 9), 1),
    ("s18", "probe.v", "-set W 18 -set SIGNED 1 -set LANES 1", ("mac_s18_q",), (6, 7, 8, 9), 1),
    # One signed and one unsigned operand (probe_mixed.v). Quartus moves the
    # signed operand of an inferred mixed product onto X, so the signed-Y
    # references are cyclonev_mac atoms (quartus/muls6.v); the atom controls
    # listed next to inferred references must match them too.
    ("mixed9", "probe_mixed.v", "-set W 9 -set U 8 -set SIGNED_A 1", ("mul_my9_q",), (), 1),
    ("mixed9full", "probe_mixed.v", "-set W 9 -set U 9 -set SIGNED_A 1", ("mul_m9_q", "atom_xs9_q"), (), 1),
    ("mixed9y", "probe_mixed.v", "-set W 9 -set U 9 -set SIGNED_A 0", ("atom_ys9_q",), (), 1),
    ("mixed27", "probe_mixed.v", "-set W 27 -set U 26 -set SIGNED_A 1",
     ("mul_mx27_q", "mul_my27_q", "atom_xs27_q"), (), 1),
    ("mixed27y", "probe_mixed.v", "-set W 27 -set U 27 -set SIGNED_A 0", ("atom_ys27_q",), (), 1),
    # Three unsigned products share one block; three signed products each own
    # one block configured like the single signed product.
    ("u9x3", "probe.v", "-set W 9 -set SIGNED 0 -set LANES 3", (), (), 1),
    ("s9x3", "probe.v", "-set W 9 -set SIGNED 1 -set LANES 3", ("mul_s9_q",), (), 3),
)

# Mixed references no MISTRAL_MUL18X18 configuration reproduces bit for bit.
SKIPPED_REFERENCES = (
    ("mul_m18_q", "a plain 18x18 product, which Quartus maps to its default M18X19 mode; MISTRAL_MUL18X18 "
     "uses M18X18P36, which is why u18 and s18 compare with the mac references."),
    ("mac_sx18_q, mac_sy18_q", "Quartus sets AX_SIGNED but not BX_SIGNED for the zero-extended 17-bit X "
     "operand. This backend sets both from A_SIGNED, so the nearest configurations differ by AX_SIGNED "
     "or BX_SIGNED alone, on an operand whose MSB input is constant zero."),
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
    for ref_name, reason in SKIPPED_REFERENCES:
        print(ref_name, "SKIP", reason, flush=True)

    for name, rtl, chparam, ref_names, drop, site_count in CASES:
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
        if ref_names:
            for ref_name in ref_names:
                expected = reference(ref_name, drop)
                assert len(expected) == 1, ref_name
                for site in actual:
                    assert site == expected[0], (name, ref_name, json.dumps(site, indent=1),
                                                 json.dumps(expected[0], indent=1))
        else:
            site = actual[0]
            assert site["settings"]["MODE"] == "M9X9" and not any(
                k.endswith("_SIGNED") for k in site["settings"]), (name, site)
            assert site["groups_used"] == [0, 2, 6, 7, 8, 9], (name, site)
        print(name, "PASS", f"{len(actual)} site(s)",
              f"vs {', '.join(ref_names) if ref_names else 'three-lane rule'}", flush=True)

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
