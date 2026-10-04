#!/usr/bin/env python3
"""Check Cyclone V 3.3 V I/O electrical options against Quartus decodes."""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for key in ("yosys", "nextpnr", "mistral-cv", "output"):
    parser.add_argument("--" + key, required=True, type=Path)
parser.add_argument(
    "--reference-nextpnr",
    type=Path,
    help="optional nextpnr without this feature; a design with no electrical "
    "assignments must produce byte-identical RBF bytes",
)
a = parser.parse_args()
f = Path(__file__).resolve().parent
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
oracle = json.loads((f / "oracle/mapping.json").read_text())
IGNORED = re.compile(r"INPUT_REG4_SEL")


def run(command, log, success=True):
    result = subprocess.run(
        [str(x) for x in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600
    )
    log.write_text(result.stdout)
    assert (result.returncode == 0) == success, result.stdout[-3000:]
    return result.stdout


def synth(design, name):
    run(
        [a.yosys, "-p", f"read_verilog {f / design}; synth_intel_alm -top top; write_json {o / (name + '.json')}"],
        o / (name + ".synth.log"),
    )
    return o / (name + ".json")


def route(netlist, qsf, name, nextpnr=None, success=True, sdc=True):
    command = [nextpnr or a.nextpnr, "--device", "5CSEBA6U23I7", "--qsf", qsf, "--json", netlist]
    if sdc:
        command += ["--sdc", f / "clocks.sdc"]
    if success:
        command += ["--rbf", o / (name + ".rbf")]
    return run(command, o / (name + ".route.log"), success)


def settings(name, sdr_outputs=()):
    run([a.mistral_cv, "decomp", "5CSEBA6U23I7", o / (name + ".rbf"), o / (name + ".bt")], o / (name + ".decode.log"))
    found = {}
    for line in (o / (name + ".bt")).read_text().splitlines():
        m = re.match(r"^s ((?:GPIO|DQS16)\.\S+) (\S+) ; (\S+)", line)
        if not m or IGNORED.search(m.group(1)):
            continue
        # nextpnr keeps its existing OE-delay default on a packed SDR output.
        if m.group(3) in sdr_outputs and "RB_T9_SEL_EREG_CFF_DELAY" in m.group(1):
            continue
        found.setdefault(m.group(3), []).append(m.group(1) + " " + m.group(2))
    return {pin: sorted(values) for pin, values in found.items()}


def compare(name, sdr_outputs=()):
    got = settings(name, sdr_outputs)
    want = oracle["designs"][name]["settings"]
    for pin in sorted(set(got) | set(want)):
        assert got.get(pin, []) == want.get(pin, []), (name, pin, got.get(pin), want.get(pin))
    return len(set(got) | set(want))


top = synth("top.v", "top")
log = route(top, f / "pins.qsf", "top")
assert "Packed SDR input register" in log and "Packed SDR output register" in log
pins = compare("top", sdr_outputs={oracle["designs"]["top"]["pins"]["o_reg_d5"]})
print(f"PASS: {pins} pads match the Quartus electrical and delay-chain settings")

bus = synth("bus.v", "bus")
route(bus, f / "bus.qsf", "bus", sdc=False)
pins = compare("bus")
print(f"PASS: wildcard, whole-bus and exact assignments match Quartus precedence on {pins} pads")

base_qsf = (f / "pins.qsf").read_text()


def reject(name, extra, expected, qsf=None):
    path = o / (name + ".qsf")
    path.write_text((qsf if qsf is not None else base_qsf) + extra + "\n")
    log = route(top, path, name, success=False)
    assert expected in log, (name, log[-2000:])


reject("standard", 'set_instance_assignment -name IO_STANDARD "2.5 V" -to o_plain', 'IO_STANDARD "2.5 V"')
reject("drive", "set_instance_assignment -name CURRENT_STRENGTH_NEW 12MA -to o_plain", "CURRENT_STRENGTH_NEW '12MA'")
reject("lvcmos-drive", "set_instance_assignment -name CURRENT_STRENGTH_NEW 4MA -to o_lvcmos", "3.3-V LVCMOS")
reject("slew", "set_instance_assignment -name SLEW_RATE 2 -to o_plain", "SLEW_RATE must be 0")
reject("pull", "set_instance_assignment -name WEAK_PULL_UP_RESISTOR MAYBE -to i_plain", "must be ON or OFF")
reject(
    "hold-and-pull",
    "set_instance_assignment -name WEAK_PULL_UP_RESISTOR ON -to i_bushold",
    "cannot both be ON",
)
reject(
    "termination",
    'set_instance_assignment -name OUTPUT_TERMINATION "SERIES 50 OHM WITHOUT CALIBRATION" -to o_plain',
    "not available on the 3.3 V",
)
reject("d3-range", "set_instance_assignment -name D3_DELAY 8 -to i_d3", "from 0 to 7")
reject("d5-range", "set_instance_assignment -name D5_DELAY 32 -to o_d5", "from 0 to 31")
reject("d3-output", "set_instance_assignment -name D3_DELAY 1 -to o_plain", "combinational input path")
reject("d5-input", "set_instance_assignment -name D5_DELAY 1 -to i_plain", "D5_DELAY is only supported")
reject("d5oe-sdr", "set_instance_assignment -name D5_OE_DELAY 1 -to o_reg_d5", "D5_OE_DELAY is only supported")
reject("d4", "set_instance_assignment -name D4_DELAY 1 -to i_plain", "D4_DELAY is not supported")
reject("wildcard-location", "set_location_assignment PIN_AH22 -to o_*", "not a wildcard")
print("PASS: 14 invalid electrical requests rejected")

if a.reference_nextpnr:
    # Without electrical assignments the feature must not change any bit.
    plain = "\n".join(
        line
        for line in base_qsf.splitlines()
        if line.startswith("set_location_assignment") or "FAST_" in line or "IO_STANDARD \"3.3-V LVTTL\"" in line
    )
    (o / "plain.qsf").write_text(plain + "\n")
    route(top, o / "plain.qsf", "plain-new")
    route(top, o / "plain.qsf", "plain-ref", nextpnr=a.reference_nextpnr)
    new = (o / "plain-new.rbf").read_bytes()
    ref = (o / "plain-ref.rbf").read_bytes()
    assert new == ref, "RBF bytes differ without electrical assignments"
    print("PASS: no electrical assignments gives byte-identical RBF bytes with the reference binary")
