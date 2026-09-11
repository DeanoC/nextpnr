#!/usr/bin/env python3
"""Route an M10K with an unregistered (combinational) read port.

The fixture is synthesized as a direct MISTRAL_M10K and then edited like the
native memory_libmap output: CFG_ASYNC_READ=1 and no CLK2 read clock. The
packer materialises an omitted read enable as a constant-high route to the
physical ENABLE[0] pin, including the read-only shape whose write clock folds
away as a constant.
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

    # A dynamic read enable would make the port clocked, so it is malformed in
    # the asynchronous contract.  A constant-high B1EN remains valid and is
    # covered below as a separate case.
    invalid = copy.deepcopy(base)
    invalid_cell = invalid["modules"]["top"]["cells"][name]
    invalid_cell["parameters"]["CFG_ASYNC_READ"] = "00000000000000000000000000000001"
    invalid_cell["connections"]["B1EN"] = [invalid_cell["connections"]["B1ADDR"][0]]
    invalid_case = output / "invalid-enable"
    invalid_case.mkdir(exist_ok=True)
    invalid_fixture = invalid_case / "synth.json"
    invalid_fixture.write_text(json.dumps(invalid))
    result = subprocess.run(
        [str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(qsf), "--sdc", str(args.sdc.resolve()),
         "--json", str(invalid_fixture)], capture_output=True, text=True, timeout=120)
    (invalid_case / "route.log").write_text(result.stdout + result.stderr)
    assert result.returncode != 0, "CFG_ASYNC_READ must reject a dynamic B1EN"
    assert "CFG_ASYNC_READ requires B1EN tied high" in result.stdout + result.stderr

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
    # The mapper omits B1EN for a combinational read. The packer keeps that
    # logical omission at the input boundary but adds an internal VCC-backed
    # connection so the physical ENABLE[0] route is explicit.
    assert "B1EN" in packed["connections"]

    run([str(args.mistral_cv.resolve()), "decomp", DEVICE, str(case / "top.rbf"),
         str(case / "top.bt")], case / "decomp.log")
    _, x, y, _ = packed["attributes"]["NEXTPNR_BEL"].split(".")
    site = f"M10K.{int(x):03d}.{int(y):03d}"
    bitstream = (case / "top.bt").read_text()
    fields = dict(re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$",
                             bitstream, re.MULTILINE))
    assert fields.get("B_OUTPUT_SEL", "async") == "async", fields
    # The flow-through read path still has no independent logical clock, but
    # Cyclone V's split M10K clock mux needs the shared CLK1 route on both
    # physical clock sinks. The explicit constant-high enable path selects
    # the bottom core/input clock tree as well as the second-half muxes.
    assert fields.get("BOT_CLK_SEL", "0") == "1", fields
    assert fields.get("BOT_CORECLK_SEL", "0") == "1", fields
    assert fields.get("BOT_INCLK_SEL", "0") == "1", fields
    assert fields.get("BOT_1_CORECLK_SEL", "0") == "1", fields
    assert fields.get("BOT_1_INCLK_SEL", "0") == "1", fields
    assert fields.get("BOT_1_OUTCLK_SEL", "0") == "1", fields
    assert fields.get("BOT_CLK_INV", "0") == "0", fields
    for clock_pin in ("CLKIN.0", "CLKIN.1"):
        assert re.search(r"^r \S+ " + re.escape(site + ":" + clock_pin) + r"$",
                         bitstream, re.MULTILINE), clock_pin
    assert re.search(r"^r \S+ " + re.escape(site + ":ENABLE.0") + r"$",
                     bitstream, re.MULTILINE), bitstream
    assert not re.search(r"^r \S+ " + re.escape(site + ":RDEN.0") + r"$",
                         bitstream, re.MULTILINE), bitstream
    # Constant-high byte enables are part of the flow-through mode's default
    # source. Keep CFG_BYTE_ENABLE set, but do not route the two constant masks
    # through fabric; dynamic or low masks still use BYTEENABLEA pins.
    for pin in ("BYTEENABLEA.0", "BYTEENABLEA.1"):
        assert not re.search(r"^r \S+ " + re.escape(site + ":" + pin) + r"$",
                             bitstream, re.MULTILINE), pin

    # A ROM-like async memory can have its write clock folded away by
    # memory_libmap. The inactive A1EN and constant CLK1 must be accepted
    # without creating a fabric clock route for the unused write half.
    readonly_design = copy.deepcopy(design)
    readonly_cell = readonly_design["modules"]["top"]["cells"][name]
    readonly_cell["connections"]["CLK1"] = ["0"]
    readonly_cell["connections"]["A1EN"] = ["1"]
    readonly_case = output / "async-readonly"
    readonly_case.mkdir(exist_ok=True)
    route(args, qsf, readonly_case, readonly_design)
    readonly_report = json.loads((readonly_case / "timing.json").read_text())
    assert readonly_report["utilization"][CELL]["used"] == 1
    assert readonly_report["fmax"] and all(clock["achieved"] >= clock["constraint"] == 50
                                               for clock in readonly_report["fmax"].values())
    readonly_packed = json.loads((readonly_case / "routed.json").read_text())["modules"]["top"]["cells"][name]
    assert not readonly_packed["connections"].get("CLK1")
    run([str(args.mistral_cv.resolve()), "decomp", DEVICE, str(readonly_case / "top.rbf"),
         str(readonly_case / "top.bt")], readonly_case / "decomp.log")
    _, rx, ry, _ = readonly_packed["attributes"]["NEXTPNR_BEL"].split(".")
    readonly_site = f"M10K.{int(rx):03d}.{int(ry):03d}"
    readonly_bitstream = (readonly_case / "top.bt").read_text()
    assert re.search(r"^r \S+ " + re.escape(readonly_site + ":ENABLE.0") + r"$",
                     readonly_bitstream, re.MULTILINE), readonly_bitstream
    assert not re.search(r"^r \S+ " + re.escape(readonly_site + ":CLKIN.0") + r"$",
                         readonly_bitstream, re.MULTILINE), readonly_bitstream
    assert not re.search(r"^r \S+ " + re.escape(readonly_site + ":CLKIN.1") + r"$",
                         readonly_bitstream, re.MULTILINE), readonly_bitstream

    # A nonconstant mask must keep the normal byte-enable routes. Reuse a
    # live read-address bit for one lane and tie the other low so this case
    # covers both dynamic and constant-low inputs without changing geometry.
    mixed_design = copy.deepcopy(design)
    mixed_cell = mixed_design["modules"]["top"]["cells"][name]
    mixed_cell["connections"]["A1BE"] = [mixed_cell["connections"]["B1ADDR"][0], "0"]
    mixed_case = output / "async-mixed-mask"
    mixed_case.mkdir(exist_ok=True)
    route(args, qsf, mixed_case, mixed_design)
    mixed_report = json.loads((mixed_case / "timing.json").read_text())
    assert mixed_report["utilization"][CELL]["used"] == 1
    assert mixed_report["fmax"] and all(clock["achieved"] >= clock["constraint"] == 50
                                           for clock in mixed_report["fmax"].values())
    run([str(args.mistral_cv.resolve()), "decomp", DEVICE, str(mixed_case / "top.rbf"),
         str(mixed_case / "top.bt")], mixed_case / "decomp.log")
    mixed_packed = json.loads((mixed_case / "routed.json").read_text())["modules"]["top"]["cells"][name]
    _, mx, my, _ = mixed_packed["attributes"]["NEXTPNR_BEL"].split(".")
    mixed_site = f"M10K.{int(mx):03d}.{int(my):03d}"
    mixed_bitstream = (mixed_case / "top.bt").read_text()
    for pin in ("BYTEENABLEA.0", "BYTEENABLEA.1"):
        assert re.search(r"^r \S+ " + re.escape(mixed_site + ":" + pin) + r"$",
                         mixed_bitstream, re.MULTILINE), pin
    assert re.search(r"^r \S+ " + re.escape(mixed_site + ":ENABLE.0") + r"$",
                     mixed_bitstream, re.MULTILINE), mixed_bitstream
    assert not re.search(r"^r \S+ " + re.escape(mixed_site + ":RDEN.0") + r"$",
                         mixed_bitstream, re.MULTILINE), mixed_bitstream

    # Explicit constant-high B1EN is accepted as the equivalent primitive
    # spelling and must use the same physical enable route.
    explicit_design = copy.deepcopy(base)
    explicit_cell = explicit_design["modules"]["top"]["cells"][name]
    explicit_cell["parameters"]["CFG_ASYNC_READ"] = "00000000000000000000000000000001"
    for clear in ("ACLR0", "ACLR1"):
        explicit_cell["connections"][clear] = ["0"]
        explicit_cell["port_directions"][clear] = "input"
    explicit_case = output / "async-explicit-enable"
    explicit_case.mkdir(exist_ok=True)
    route(args, qsf, explicit_case, explicit_design)
    explicit_packed = json.loads((explicit_case / "routed.json").read_text())["modules"]["top"]["cells"][name]
    assert "B1EN" in explicit_packed["connections"]
    run([str(args.mistral_cv.resolve()), "decomp", DEVICE, str(explicit_case / "top.rbf"),
         str(explicit_case / "top.bt")], explicit_case / "decomp.log")
    _, ex, ey, _ = explicit_packed["attributes"]["NEXTPNR_BEL"].split(".")
    explicit_site = f"M10K.{int(ex):03d}.{int(ey):03d}"
    explicit_bitstream = (explicit_case / "top.bt").read_text()
    assert re.search(r"^r \S+ " + re.escape(explicit_site + ":ENABLE.0") + r"$",
                     explicit_bitstream, re.MULTILINE), explicit_bitstream
    print("PASS: asynchronous-read M10K, explicit constant-high enable, routed mixed mask, 50 MHz timing")


if __name__ == "__main__":
    main()
