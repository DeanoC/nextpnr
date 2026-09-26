#!/usr/bin/env python3
"""Route a TDP M10K whose disabled B port has a constant clock."""

import argparse
import copy
import json
from pathlib import Path
import re
import subprocess


DEVICE = "5CSEBA6U23I7"
CELL = "MISTRAL_M10K_TDP"
HPS = "cyclonev_hps_interface_mpu_general_purpose"


def run(command, log, timeout=120):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name("true_dual_port.v").resolve()
    script = output / "synth.ys"
    script.write_text(
        f"read_verilog {source}\n"
        "chparam -set WIDTH 20 -set SAME_CLOCK 1 top\n"
        "synth_intel_alm -nolutram -nodsp -top top\n"
        f"select -assert-count 1 t:{CELL}\n"
        f"write_json {output / 'base.json'}\n"
    )
    run([str(args.yosys.resolve()), "-Q", "-T", "-s", str(script)], output / "synth.log")
    design = json.loads((output / "base.json").read_text())
    cells = design["modules"]["top"]["cells"]
    name, cell = next((n, c) for n, c in cells.items() if c["type"] == CELL)
    base_design = copy.deepcopy(design)

    invalid = copy.deepcopy(base_design)
    invalid_cell = invalid["modules"]["top"]["cells"][name]
    invalid_cell["connections"]["CLK2"] = ["0"]
    invalid_cell["port_directions"]["CLK2"] = "input"
    invalid_fixture = output / "invalid-active-b.json"
    invalid_fixture.write_text(json.dumps(invalid))
    invalid_result = subprocess.run(
        [str(args.nextpnr.resolve()), "--device", DEVICE, "--qsf", str(args.qsf.resolve()),
         "--json", str(invalid_fixture)], capture_output=True, text=True, timeout=120)
    assert invalid_result.returncode != 0
    assert "constant CLK2 is only valid when B1EN is tied low" in invalid_result.stdout + invalid_result.stderr

    design = copy.deepcopy(base_design)
    cell = design["modules"]["top"]["cells"][name]
    # This is the shape emitted by Yosys for an unused true-dual-port side:
    # retain the TDP primitive, but tie the B clock and enable low. Before the
    # packer fix CLK2 became a $PACKER_GND_NET sink on M10K CLKIN[1].
    cell["connections"]["CLK2"] = ["0"]
    cell["port_directions"]["CLK2"] = "input"
    cell["connections"]["B1EN"] = ["0"]
    cell["port_directions"]["B1EN"] = "input"
    # Also exercise the hard clock inverter. The packer must route the source
    # clock once and carry the inversion in TOP_CLK_INV.
    source_clock = cell["connections"]["CLK1"][0]
    inverted_clock = 10001
    cells = design["modules"]["top"]["cells"]
    cells["constant_clock_inverter"] = {
        "hide_name": 0, "type": "MISTRAL_NOT", "parameters": {}, "attributes": {},
        "port_directions": {"A": "input", "Q": "output"},
        "connections": {"A": [source_clock], "Q": [inverted_clock]},
    }
    cell["connections"]["CLK1"] = [inverted_clock]
    cell["port_directions"]["CLK1"] = "input"
    # An inverted constant must be folded as a constant too. Otherwise its
    # temporary soft GND/VCC net would still be assigned to the M10K TCLK pin.
    constant_inverter = 10002
    cells["disabled_clock_constant_inverter"] = {
        "hide_name": 0, "type": "MISTRAL_NOT", "parameters": {}, "attributes": {},
        "port_directions": {"A": "input", "Q": "output"},
        "connections": {"A": ["0"], "Q": [constant_inverter]},
    }
    cell["connections"]["CLK2"] = [constant_inverter]
    cell["port_directions"]["CLK2"] = "input"
    fixture = output / "synth.json"
    fixture.write_text(json.dumps(design))

    run([str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(args.qsf.resolve()), "--sdc", str(args.sdc.resolve()),
         "--json", str(fixture), "--compress-rbf", "--rbf", str(output / "top.rbf"),
         "--write", str(output / "routed.json"), "--report", str(output / "timing.json")],
        output / "route.log")
    assert (output / "top.rbf").stat().st_size > 0
    report = json.loads((output / "timing.json").read_text())
    assert report["utilization"]["MISTRAL_M10K"]["used"] == 1
    assert report["utilization"][HPS]["used"] == 1
    assert report["fmax"] and all(clock["achieved"] >= clock["constraint"] == 50
                                   for clock in report["fmax"].values())
    assert any(path["from"] == "negedge clock_b" and
               any(arc["type"] == "clk-skew" and arc["from"]["cell"] == name and
                   arc["from"]["port"] == "CLK1" for arc in path["path"])
               for path in report["critical_paths"]), "missing inverted M10K clock edge"

    routed = json.loads((output / "routed.json").read_text())["modules"]["top"]["cells"]
    packed = routed[name]
    assert packed["type"] == "MISTRAL_M10K"
    assert packed["connections"].get("CLK2") == [], packed

    run([str(args.mistral_cv.resolve()), "decomp", DEVICE, str(output / "top.rbf"),
         str(output / "top.bt")], output / "decomp.log")
    _, x, y, _ = packed["attributes"]["NEXTPNR_BEL"].split(".")
    site = f"M10K.{int(x):03d}.{int(y):03d}"
    bitstream = (output / "top.bt").read_text()
    fields = dict(re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$",
                             bitstream, re.MULTILINE))
    assert fields.get("TOP_CLK_SEL") == "1", fields
    assert fields.get("TOP_CLK_INV") == "1", fields
    assert "BOT_CLK_SEL" not in fields, fields
    assert "BOT_1_CORECLK_SEL" not in fields, fields
    assert re.search(r"^r \S+ " + re.escape(site + ":CLKIN.0") + r"$", bitstream, re.MULTILINE)
    assert not re.search(r"^r \S+ " + re.escape(site + ":CLKIN.1") + r"$", bitstream, re.MULTILINE)
    print("PASS: constant disabled TDP clock is folded off TCLK and uses single-clock selectors")


if __name__ == "__main__":
    main()
