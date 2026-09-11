#!/usr/bin/env python3
"""Route an M10K with an unregistered (combinational) read port.

The locked Yosys tree does not yet emit this primitive shape.  The fixture is
therefore synthesized as a direct MISTRAL_M10K and then edited like the
future memory_libmap output: CFG_ASYNC_READ=1 and no B1EN/CLK2 read controls.
The packer must add an internal high RDEN[0] route for this shape.
"""

import argparse
import copy
import json
from pathlib import Path
import re
import subprocess


DEVICE = "5CSEBA6U23I7"
CELL = "MISTRAL_M10K"


def run(command, log, timeout=120):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def route(args, qsf, case, design):
    fixture = case / "synth.json"
    fixture.write_text(json.dumps(design))
    run([str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(qsf), "--sdc", str(args.sdc.resolve()),
         "--json", str(fixture), "--compress-rbf", "--rbf", str(case / "top.rbf"),
         "--write", str(case / "routed.json"), "--report", str(case / "timing.json")],
        case / "route.log")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    # The board QSF names the one-bit vector LED[0].  This direct primitive
    # fixture is intentionally scalar in Yosys JSON, so adapt only the target
    # name while retaining the board pin and I/O standard.
    qsf = output / "pins.qsf"
    qsf.write_text(args.qsf.read_text().replace("-to LED[0]", "-to LED"))

    synth = output / "synth.ys"
    source = Path(__file__).with_suffix(".v").resolve()
    synth.write_text(
        f"read_verilog {source}\n"
        "synth_intel_alm -nolutram -nodsp -top top\n"
        f"select -assert-count 1 t:{CELL}\n"
        f"write_json {output / 'base.json'}\n"
    )
    run([str(args.yosys.resolve()), "-Q", "-T", "-s", str(synth)], output / "synth.log")
    base = json.loads((output / "base.json").read_text())
    cells = base["modules"]["top"]["cells"]
    name, cell = next((n, c) for n, c in cells.items() if c["type"] == CELL)
    assert cell["port_directions"].get("B1EN") == "input"

    # The locked M10K library cell predates the byte-enable mapper and has no
    # A1BE port.  Add the two always-enabled write lanes at the JSON boundary
    # so this regression covers the 20-bit byte-enable selector path used by
    # the hardware fixture without changing that pinned library.
    cell["parameters"]["CFG_BYTE_ENABLE"] = "00000000000000000000000000000001"
    cell["connections"]["A1BE"] = ["1", "1"]
    cell["port_directions"]["A1BE"] = "input"
    assert len(cell["connections"]["A1BE"]) == 2

    # This is the future Yosys output contract.  A read enable or a second
    # clock would make the port clocked, so an explicit B1EN is malformed.
    invalid = copy.deepcopy(base)
    invalid_cell = invalid["modules"]["top"]["cells"][name]
    invalid_cell["parameters"]["CFG_ASYNC_READ"] = "00000000000000000000000000000001"
    invalid_case = output / "invalid-enable"
    invalid_case.mkdir(exist_ok=True)
    invalid_fixture = invalid_case / "synth.json"
    invalid_fixture.write_text(json.dumps(invalid))
    result = subprocess.run(
        [str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(qsf), "--sdc", str(args.sdc.resolve()),
         "--json", str(invalid_fixture)], capture_output=True, text=True, timeout=120)
    (invalid_case / "route.log").write_text(result.stdout + result.stderr)
    assert result.returncode != 0, "CFG_ASYNC_READ must reject a connected B1EN"
    assert "CFG_ASYNC_READ" in result.stdout + result.stderr

    invalid_clear = copy.deepcopy(base)
    invalid_clear_cell = invalid_clear["modules"]["top"]["cells"][name]
    invalid_clear_cell["parameters"]["CFG_ASYNC_READ"] = "00000000000000000000000000000001"
    invalid_clear_cell["connections"].pop("B1EN")
    invalid_clear_cell["port_directions"].pop("B1EN")
    invalid_clear_cell["connections"]["ACLR1"] = ["1"]
    invalid_clear_cell["port_directions"]["ACLR1"] = "input"
    invalid_clear_case = output / "invalid-clear"
    invalid_clear_case.mkdir(exist_ok=True)
    invalid_clear_fixture = invalid_clear_case / "synth.json"
    invalid_clear_fixture.write_text(json.dumps(invalid_clear))
    result = subprocess.run(
        [str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(qsf), "--sdc", str(args.sdc.resolve()),
         "--json", str(invalid_clear_fixture)], capture_output=True, text=True, timeout=120)
    (invalid_clear_case / "route.log").write_text(result.stdout + result.stderr)
    assert result.returncode != 0, "CFG_ASYNC_READ must reject an active ACLR"
    assert "inactive ACLR1" in result.stdout + result.stderr

    design = copy.deepcopy(base)
    async_cell = design["modules"]["top"]["cells"][name]
    async_cell["parameters"]["CFG_ASYNC_READ"] = "00000000000000000000000000000001"
    async_cell["connections"].pop("B1EN")
    async_cell["port_directions"].pop("B1EN")
    # Explicit constant-zero clears are legal and must remain inactive after
    # constant folding, just like omitted controls.
    for clear in ("ACLR0", "ACLR1"):
        async_cell["connections"][clear] = ["0"]
        async_cell["port_directions"][clear] = "input"
    case = output / "async"
    case.mkdir(exist_ok=True)
    route(args, qsf, case, design)

    assert (case / "top.rbf").stat().st_size > 0
    report = json.loads((case / "timing.json").read_text())
    assert report["utilization"][CELL]["used"] == 1
    assert report["fmax"] and all(clock["achieved"] >= clock["constraint"] == 50
                                   for clock in report["fmax"].values())
    assert any(any(arc["type"] == "logic" and arc["delay"] == 1.5 and
                   arc["from"]["cell"] == name and arc["from"]["port"].startswith("B1ADDR") and
                   arc["to"]["cell"] == name and arc["to"]["port"].startswith("B1DATA")
                   for arc in path["path"])
                   for path in report["critical_paths"]), "missing asynchronous M10K timing arc"

    routed = json.loads((case / "routed.json").read_text())["modules"]["top"]["cells"]
    packed = routed[name]
    assert packed["parameters"]["CFG_ASYNC_READ"][-1] == "1"
    assert packed["parameters"]["CFG_BYTE_ENABLE"][-1] == "1"
    # The mapper omits B1EN for a combinational read, but Cyclone V still
    # requires the physical read-enable lane to be high.  The packer adds an
    # internal soft-VCC connection so the route is visible in the bitstream;
    # it is not a user-controlled port.
    assert "B1EN" in packed["connections"]

    run([str(args.mistral_cv.resolve()), "decomp", DEVICE, str(case / "top.rbf"),
         str(case / "top.bt")], case / "decomp.log")
    _, x, y, _ = packed["attributes"]["NEXTPNR_BEL"].split(".")
    site = f"M10K.{int(x):03d}.{int(y):03d}"
    bitstream = (case / "top.bt").read_text()
    fields = dict(re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$",
                             bitstream, re.MULTILINE))
    assert fields.get("B_OUTPUT_SEL", "async") == "async", fields
    # Byte-enable wiring is a write-side concern here.  Both bottom read
    # clock selectors must retain their single-clock defaults because the
    # asynchronous port has no CLKIN[1] route.
    assert fields.get("BOT_CORECLK_SEL", "0") == "0", fields
    assert fields.get("BOT_INCLK_SEL", "0") == "0", fields
    assert not re.search(r"^r \S+ " + re.escape(site + ":CLKIN.1") + r"$",
                         bitstream, re.MULTILINE)
    assert re.search(r"^r \S+ " + re.escape(site + ":RDEN.0") + r"$",
                     bitstream, re.MULTILINE), bitstream
    for pin in ("BYTEENABLEA.0", "BYTEENABLEA.1"):
        assert re.search(r"^r \S+ " + re.escape(site + ":" + pin) + r"$",
                         bitstream, re.MULTILINE), pin
    print("PASS: one asynchronous-read M10K, RDEN.0 tied high, no read clock, 50 MHz timing")


if __name__ == "__main__":
    main()
