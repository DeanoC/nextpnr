#!/usr/bin/env python3
"""Round-trip a routed multiple-output PLL without losing counters or pin maps."""
import argparse
import json
from pathlib import Path
import subprocess


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent / "fixtures/general/mister3"

    def run(command, label):
        with (out / (label + ".log")).open("w") as log:
            result = subprocess.run([str(x) for x in command], stdout=log,
                                    stderr=subprocess.STDOUT, timeout=1200)
        if result.returncode:
            raise RuntimeError(f"{label} failed: see {out / (label + '.log')}")

    run([args.yosys.resolve(), "-p", f"read_verilog {here / 'top.v'}; "
         f"synth_intel_alm -nobram -nolutram -nodsp -top top; write_json {out / 'synth.json'}"], "synth")
    base = [args.nextpnr.resolve(), "--device", "5CSEBA6U23I7", "--timing-allow-fail", "--compress-rbf"]
    run(base + ["--json", out / "synth.json", "--qsf", here / "pins.qsf", "--sdc", here / "clocks.sdc",
                "--write", out / "routed.json", "--rbf", out / "original.rbf"], "route")
    before = json.loads((out / "routed.json").read_text(), object_pairs_hook=unique_object)
    pll = before["modules"]["top"]["cells"]["pll_core"]
    assert all(p in pll["connections"] for p in ("outclk", "outclk[1]", "outclk[2]"))
    frozen = json.loads(bytes.fromhex(pll["attributes"]["FES_PINMAP_V1"]))["pins"]
    assert "outclk[0]" not in frozen, frozen
    run(base + ["--json", out / "routed.json", "--no-pack", "--no-place", "--no-route",
                "--write", out / "reloaded.json", "--rbf", out / "reloaded.rbf"], "reload")
    after = json.loads((out / "reloaded.json").read_text(), object_pairs_hook=unique_object)
    for name in ("pll_core", "pll_hdmi", "pll_audio"):
        a = before["modules"]["top"]["cells"][name]
        b = after["modules"]["top"]["cells"][name]
        assert a["attributes"]["MISTRAL_PLL_COUNTERS"] == b["attributes"]["MISTRAL_PLL_COUNTERS"]
        assert a["attributes"]["FES_PINMAP_V1"] == b["attributes"]["FES_PINMAP_V1"]
    assert (out / "original.rbf").read_bytes() == (out / "reloaded.rbf").read_bytes()
    print("PASS: unique JSON keys, all PLL outputs, frozen pin maps and identical replayed RBF")


if __name__ == "__main__":
    main()
