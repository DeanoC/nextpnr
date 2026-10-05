#!/usr/bin/env python3
"""Check Cyclone V GPIO input, output and OE registers against Quartus decodes."""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for key in ("yosys", "nextpnr", "mistral-cv", "output"):
    parser.add_argument("--" + key, required=True, type=Path)
a = parser.parse_args()
f = Path(__file__).resolve().parent
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
oracle = json.loads((f / "oracle/mapping.json").read_text())


def run(command, log, success=True):
    result = subprocess.run(
        [str(x) for x in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600
    )
    log.write_text(result.stdout)
    assert (result.returncode == 0) == success, result.stdout[-3000:]
    return result.stdout


def synth(name):
    run(
        [a.yosys, "-p", f"read_verilog {f / (name + '.v')}; synth_intel_alm -top top; write_json {o / (name + '.json')}"],
        o / (name + ".synth.log"),
    )
    return o / (name + ".json")


def route(netlist, qsf, name, success=True):
    command = [a.nextpnr, "--device", "5CSEBA6U23I7", "--qsf", qsf, "--sdc", f / "clocks.sdc", "--json", netlist]
    if success:
        command += ["--rbf", o / (name + ".rbf"), "--write", o / (name + ".routed.json")]
    return run(command, o / (name + ".route.log"), success)


def decoded(name, pins):
    run([a.mistral_cv, "decomp", "5CSEBA6U23I7", o / (name + ".rbf"), o / (name + ".bt")], o / (name + ".decode.log"))
    found = {}
    for line in (o / (name + ".bt")).read_text().splitlines():
        m = re.match(r"^s ((?:GPIO|DQS16)\.\S+) (\S+) ; (\S+)", line)
        if m and m.group(3) in pins and "INPUT_REG4_SEL" not in m.group(1):
            found.setdefault(m.group(3), []).append("s " + m.group(1) + " " + m.group(2))
        m = re.match(r"^i (\S+) (\S) ; (\S+)", line)
        if m and m.group(3) in pins and re.search(r"(CEIN|CEOUT|ACLR|OEIN\.\d)", line):
            found.setdefault(m.group(3), []).append("i " + m.group(1) + " " + m.group(2))
    return {pin: sorted(values) for pin, values in found.items()}


def compare(name):
    design = oracle["designs"][name]
    pins = set(design["pins"].values())
    got = decoded(name, pins)
    want = {pin: values for pin, values in design["decoded"].items() if pin in pins}
    for pin in sorted(set(got) | set(want)):
        assert got.get(pin, []) == want.get(pin, []), (name, pin, got.get(pin), want.get(pin))
    routes = (o / (name + ".bt")).read_text()
    return len(pins), routes


pads = synth("pads")
log = route(pads, f / "pads.qsf", "pads")
assert log.count("I/O register") >= 4 and log.count("Packed SDR") == 4, log
count, routes = compare("pads")
for port in ("CEIN", "CEOUT", "ACLR"):
    assert f":{port} ;" in routes, port
print(f"PASS: {count} pads match Quartus I/O register settings and inverters (in, out, OE, CE, ACLR)")

bus = synth("bus")
log = route(bus, f / "bus.qsf", "bus")
assert log.count("Packed input output output-enable I/O registers") == 4, log
cells = json.loads((o / "bus.routed.json").read_text())["modules"]["top"]["cells"]
assert sum(c["type"] == "MISTRAL_SDRIO" for c in cells.values()) == 4
assert not any(c["type"] == "MISTRAL_FF" for c in cells.values())
count, _ = compare("bus")
print(f"PASS: shared OE register copied into each pad; {count} pads match Quartus with active-low CE/ACLR")

ddr = synth("ddr")
log = route(ddr, f / "ddr.qsf", "ddr")
assert log.count("Packed DDR input") == 2 and log.count("Packed output output-enable I/O registers") == 2, log
cells = json.loads((o / "ddr.routed.json").read_text())["modules"]["top"]["cells"]
assert sum(c["type"] == "MISTRAL_SDRIO" for c in cells.values()) == 2
count, _ = compare("ddr")
print(f"PASS: DDR capture with output and OE registers on bidirectional pads; {count} pads match Quartus")

base = (f / "pads.qsf").read_text()


def reject_netlist(name, edit, expected, qsf=None):
    design = json.loads(pads.read_text())
    edit(design["modules"]["top"])
    path = o / (name + ".json")
    path.write_text(json.dumps(design))
    log = route(path, qsf or f / "pads.qsf", name, success=False)
    assert expected in log, (name, log[-2000:])


def ff_driving(module, pad_port):
    # Find the FF whose Q bit feeds the named pad cell port.
    for cell in module["cells"].values():
        if cell["type"] in ("MISTRAL_IO", "MISTRAL_OB") and cell["connections"].get("PAD") == module["ports"][pad_port]["bits"]:
            bit = cell["connections"]["I"]
            for name, ff in module["cells"].items():
                if ff["type"] == "MISTRAL_FF" and ff["connections"]["Q"] == bit:
                    return ff
    raise AssertionError(pad_port)


def ib_out(module, port):
    for cell in module["cells"].values():
        if cell["type"] == "MISTRAL_IB" and cell["connections"]["PAD"] == module["ports"][port]["bits"]:
            return cell["connections"]["O"]
    raise AssertionError(port)


reject_netlist(
    "sclr",
    lambda m: ff_driving(m, "p4")["connections"].update(SCLR=ib_out(m, "ce")),
    "synchronous clear and load have no I/O register equivalent",
)
reject_netlist(
    "sload",
    lambda m: ff_driving(m, "p1")["connections"].update(SLOAD=ib_out(m, "ce")),
    "synchronous clear and load have no I/O register equivalent",
)
reject_netlist(
    "enable-low",
    lambda m: ff_driving(m, "p4")["connections"].update(ENA=["0"]),
    "clock enable is tied low",
)


def p1_fanout(m):
    # p4's output buffer now also consumes p1's output register.
    q = ff_driving(m, "p1")["connections"]["Q"]
    for cell in m["cells"].values():
        if cell["type"] == "MISTRAL_OB" and cell["connections"]["PAD"] == m["ports"]["p4"]["bits"]:
            cell["connections"]["I"] = q


reject_netlist("fanout", p1_fanout, "may only drive pads requesting the same I/O register")
path = o / "reject-comb.qsf"
path.write_text(base + "set_instance_assignment -name FAST_OUTPUT_REGISTER ON -to p3\n")
log = route(pads, path, "reject-comb", success=False)
assert "requires the pad data to come directly from a MISTRAL_FF" in log, log[-2000:]
path = o / "d1-bidir.qsf"
path.write_text(base + "set_instance_assignment -name D1_DELAY 4 -to p3\n")
log = route(pads, path, "d1-bidir", success=False)
assert "D1_DELAY on a bidirectional FAST_INPUT_REGISTER is not encoded" in log, log[-2000:]


def shared_ff(m):
    # p1's output register now also samples p3, and p3's own sampler is disconnected.
    out = ff_driving(m, "p1")
    p3 = next(
        cell
        for cell in m["cells"].values()
        if cell["type"] == "MISTRAL_IO" and cell["connections"].get("PAD") == m["ports"]["p3"]["bits"]
    )
    sampled = p3["connections"]["O"]
    for cell in m["cells"].values():
        if cell is not out and cell.get("type") == "MISTRAL_FF" and cell["connections"].get("DATAIN") == sampled:
            cell["connections"]["DATAIN"] = ["0"]
    out["connections"]["DATAIN"] = sampled


reject_netlist(
    "shared-ff",
    shared_ff,
    "cannot be both an input register and an output or output-enable register",
)
print("PASS: seven unsupported I/O register requests rejected")


def pack(netlist, qsf, name, success=True):
    command = [
        a.nextpnr,
        "--device",
        "5CSEBA6U23I7",
        "--qsf",
        qsf,
        "--json",
        netlist,
        "--pack-only",
        "--write",
        o / (name + ".packed.json"),
    ]
    return run(command, o / (name + ".pack.log"), success)


def sdr_clocks(path):
    cells = json.loads(path.read_text())["modules"]["top"]["cells"]
    pads = [cell for cell in cells.values() if cell["type"] == "MISTRAL_SDRIO"]
    assert len(pads) == 1, list(cells)
    pad = pads[0]
    assert pad["connections"].get("CLK") and pad["connections"].get("CLKIN")
    buffers = [name for name, cell in cells.items() if cell["type"] == "MISTRAL_CLKBUF"]
    return pad["connections"]["CLK"], pad["connections"]["CLKIN"], buffers


run(
    [
        a.yosys,
        "-p",
        f"read_verilog {f / 'split.v'}; synth_intel_alm -top top -noclkbuf; write_json {o / 'split.json'}",
    ],
    o / "split.synth.log",
)
pack(o / "split.json", f / "split.qsf", "split")
clk, clkin, buffers = sdr_clocks(o / "split.packed.json")
assert clk != clkin, (clk, clkin)
assert any(name.endswith("$sdr_clkbuf") for name in buffers), buffers
assert any(name.endswith("$sdr_in_clkbuf") for name in buffers), buffers
print("PASS: bidirectional input and output registers pack on different unbuffered clocks")

shared = json.loads((o / "split.json").read_text())
clocks = [
    cell["connections"]["CLK"]
    for cell in shared["modules"]["top"]["cells"].values()
    if cell["type"] == "MISTRAL_FF"
]
assert len(clocks) == 2 and clocks[0] != clocks[1], clocks
for cell in shared["modules"]["top"]["cells"].values():
    if cell["type"] == "MISTRAL_FF":
        cell["connections"]["CLK"] = clocks[0]
shared_path = o / "split-shared.json"
shared_path.write_text(json.dumps(shared))
pack(shared_path, f / "split.qsf", "split-shared")
clk, clkin, buffers = sdr_clocks(o / "split-shared.packed.json")
assert clk == clkin, (clk, clkin)
assert sum(name.endswith("$sdr_clkbuf") for name in buffers) == 1, buffers
assert not any(name.endswith("$sdr_in_clkbuf") for name in buffers), buffers
print("PASS: one unbuffered clock is shared by the input and output registers")
