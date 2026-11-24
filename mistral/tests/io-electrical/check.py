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
    command = [nextpnr or a.nextpnr, "--device", "5CSEBA6U23I7", "--qsf", qsf, "--json", netlist, "--seed", "1"]
    if sdc:
        command += ["--sdc", f / "clocks.sdc"]
    if success:
        command += ["--rbf", o / (name + ".rbf")]
    return run(command, o / (name + ".route.log"), success)


def settings(name):
    run([a.mistral_cv, "decomp", "5CSEBA6U23I7", o / (name + ".rbf"), o / (name + ".bt")], o / (name + ".decode.log"))
    found = {}
    for line in (o / (name + ".bt")).read_text().splitlines():
        m = re.match(r"^s ((?:GPIO|DQS16)\.\S+) (\S+) ; (\S+)", line)
        if not m or IGNORED.search(m.group(1)):
            continue
        found.setdefault(m.group(3), []).append(m.group(1) + " " + m.group(2))
    return {pin: sorted(values) for pin, values in found.items()}


def compare_settings(name, want):
    got = settings(name)
    for pin in sorted(set(got) | set(want)):
        assert got.get(pin, []) == want.get(pin, []), (name, pin, got.get(pin), want.get(pin))
    return len(set(got) | set(want))


def compare(name):
    return compare_settings(name, oracle["designs"][name]["settings"])


top = synth("top.v", "top")
log = route(top, f / "pins.qsf", "top")
assert "Packed SDR input register" in log and "Packed SDR output register" in log
pins = compare("top")
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
reject("d3-fine", "set_instance_assignment -name D3_FINE_DELAY 1 -to i_plain", "D3_FINE_DELAY is not supported")
reject("open-drain", "set_instance_assignment -name OPEN_DRAIN_OUTPUT ON -to o_plain",
       "OPEN_DRAIN_OUTPUT is not supported")
reject("wildcard-location", "set_location_assignment PIN_AH22 -to o_*", "not a wildcard")
print("PASS: 16 invalid electrical requests rejected")

# Always gate the "default output unchanged" contract without needing
# --reference-nextpnr:
# 1. A design with no electrical assignments must decode to the exact default
#    GPIO/DQS16 settings on every pad (3.3-V LVTTL, 16 mA, fast slew, weak
#    pull-up/bus-hold/clamp off, default delay chains).
# 2. Adding explicit default electrical assignments must produce byte-identical
#    RBF output to having no electrical assignments at all.
PLAIN_DEFAULT_SETTINGS = {
    "AA15": [
        "DQS16.060.000:RB_T9_SEL_EREG_CFF_DELAY.1 1f",
        "GPIO.064.000:DRIVE_STRENGTH.1 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.064.000:IOCSR_STD.1 DIS",
        "GPIO.064.000:USE_WEAK_PULLUP.1 0",
    ],
    "AC24": [
        "DQS16.089.008:RB_T9_SEL_EREG_CFF_DELAY.10 1f",
        "GPIO.089.008:DRIVE_STRENGTH.2 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.089.008:IOCSR_STD.2 DIS",
        "GPIO.089.008:USE_WEAK_PULLUP.2 0",
    ],
    "AD26": [
        "DQS16.089.008:RB_T9_SEL_EREG_CFF_DELAY.7 1f",
        "GPIO.089.006:DRIVE_STRENGTH.3 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.089.006:IOCSR_STD.3 DIS",
        "GPIO.089.006:USE_WEAK_PULLUP.3 0",
    ],
    "AE25": [
        "DQS16.089.008:RB_T9_SEL_EREG_CFF_DELAY.6 1f",
        "GPIO.089.006:DRIVE_STRENGTH.2 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.089.006:IOCSR_STD.2 DIS",
        "GPIO.089.006:USE_WEAK_PULLUP.2 0",
    ],
    "AF23": [
        "DQS16.076.000:RB_T9_SEL_EREG_CFF_DELAY.5 1f",
        "GPIO.078.000:DRIVE_STRENGTH.1 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.078.000:IOCSR_STD.1 DIS",
        "GPIO.078.000:USE_WEAK_PULLUP.1 0",
    ],
    "AF25": [
        "DQS16.084.000:RB_T9_SEL_EREG_CFF_DELAY.4 1f",
        "GPIO.086.000:DRIVE_STRENGTH.0 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.086.000:USE_WEAK_PULLUP.0 0",
    ],
    "AF27": [
        "DQS16.084.000:RB_T9_SEL_EREG_CFF_DELAY.2 1f",
        "GPIO.088.000:DRIVE_STRENGTH.2 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.088.000:IOCSR_STD.2 DIS",
        "GPIO.088.000:USE_WEAK_PULLUP.2 0",
    ],
    "AF28": [
        "DQS16.084.000:RB_T9_SEL_EREG_CFF_DELAY.3 1f",
        "GPIO.088.000:DRIVE_STRENGTH.3 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.088.000:IOCSR_STD.3 DIS",
        "GPIO.088.000:USE_WEAK_PULLUP.3 0",
    ],
    "AG23": [
        "DQS16.076.000:RB_T9_SEL_EREG_CFF_DELAY.4 1f",
        "GPIO.078.000:DRIVE_STRENGTH.0 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.078.000:USE_WEAK_PULLUP.0 0",
    ],
    "AG25": [
        "DQS16.084.000:RB_T9_SEL_EREG_CFF_DELAY.5 1f",
        "GPIO.086.000:DRIVE_STRENGTH.1 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.086.000:IOCSR_STD.1 DIS",
        "GPIO.086.000:USE_WEAK_PULLUP.1 0",
    ],
    "AG26": [
        "DQS16.084.000:RB_T9_SEL_EREG_CFF_DELAY.14 1f",
        "GPIO.082.000:DRIVE_STRENGTH.2 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.082.000:IOCSR_STD.2 DIS",
        "GPIO.082.000:USE_WEAK_PULLUP.2 0",
    ],
    "AG28": [
        "DQS16.084.000:RB_T9_SEL_EREG_CFF_DELAY.6 1f",
        "GPIO.086.000:DRIVE_STRENGTH.2 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.086.000:IOCSR_STD.2 DIS",
        "GPIO.086.000:USE_WEAK_PULLUP.2 0",
    ],
    "AH24": [
        "DQS16.076.000:RB_T9_SEL_EREG_CFF_DELAY.3 1f",
        "GPIO.080.000:DRIVE_STRENGTH.3 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.080.000:USE_WEAK_PULLUP.3 0",
    ],
    "AH26": [
        "DQS16.084.000:OEREG_HR_CLK_EN.11 1",
        "DQS16.084.000:OUTREG_OUTPUT_SEL.11 SEL_SDR",
        "DQS16.084.000:RBOE_LVL_FR_CLK_EN.11 1",
        "GPIO.084.000:DRIVE_STRENGTH.3 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.084.000:IOCSR_STD.3 DIS",
        "GPIO.084.000:USE_WEAK_PULLUP.3 0",
    ],
    "AH27": [
        "DQS16.084.000:RB_T9_SEL_EREG_CFF_DELAY.7 1f",
        "GPIO.086.000:DRIVE_STRENGTH.3 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.086.000:IOCSR_STD.3 DIS",
        "GPIO.086.000:USE_WEAK_PULLUP.3 0",
    ],
    "C12": ["GPIO.040.081:USE_WEAK_PULLUP.1 0"],
    "D11": ["GPIO.032.081:USE_WEAK_PULLUP.1 0"],
    "D12": [
        "DQS16.036.081:RB_FIFO_WCLK_EN.0 1",
        "DQS16.036.081:RB_FIFO_WCLK_INV.0 1",
        "GPIO.040.081:USE_WEAK_PULLUP.0 0",
    ],
    "D8": ["GPIO.038.081:USE_WEAK_PULLUP.3 0"],
    "E8": ["GPIO.038.081:USE_WEAK_PULLUP.2 0"],
    "V11": ["GPIO.032.000:USE_WEAK_PULLUP.0 0"],
    "V12": ["GPIO.040.000:USE_WEAK_PULLUP.0 0"],
    "W12": ["GPIO.040.000:USE_WEAK_PULLUP.1 0"],
    "Y15": [
        "DQS16.060.000:RB_T9_SEL_EREG_CFF_DELAY.0 1f",
        "GPIO.064.000:DRIVE_STRENGTH.0 V3P3_LVTTL_16MA_LVCMOS_2MA",
        "GPIO.064.000:IOCSR_STD.0 DIS",
        "GPIO.064.000:USE_WEAK_PULLUP.0 0",
    ],
}

plain_lines = [
    line
    for line in base_qsf.splitlines()
    if line.startswith("set_location_assignment") or "FAST_" in line
]
(o / "plain.qsf").write_text("\n".join(plain_lines) + "\n")
route(top, o / "plain.qsf", "plain")
plain_pins = compare_settings("plain", PLAIN_DEFAULT_SETTINGS)
print(f"PASS: {plain_pins} pads without electrical assignments match exact default GPIO/DQS16 settings")

plain_defaults_lines = plain_lines + [
    'set_instance_assignment -name IO_STANDARD "3.3-V LVTTL" -to *',
    "set_instance_assignment -name CURRENT_STRENGTH_NEW 16MA -to o_*",
    "set_instance_assignment -name CURRENT_STRENGTH_NEW 16MA -to b_*",
    "set_instance_assignment -name CURRENT_STRENGTH_NEW 16MA -to t_oe",
    "set_instance_assignment -name SLEW_RATE 1 -to o_*",
    "set_instance_assignment -name SLEW_RATE 1 -to b_*",
    "set_instance_assignment -name SLEW_RATE 1 -to t_oe",
    "set_instance_assignment -name WEAK_PULL_UP_RESISTOR OFF -to *",
    "set_instance_assignment -name ENABLE_BUS_HOLD_CIRCUITRY OFF -to *",
    "set_instance_assignment -name CLAMPING_DIODE OFF -to *",
]
(o / "plain-defaults.qsf").write_text("\n".join(plain_defaults_lines) + "\n")
route(top, o / "plain-defaults.qsf", "plain-defaults")
plain_bytes = (o / "plain.rbf").read_bytes()
defaults_bytes = (o / "plain-defaults.rbf").read_bytes()
assert plain_bytes == defaults_bytes, "RBF bytes differ between no electrical assignments and explicit defaults"
print("PASS: no electrical assignments gives byte-identical RBF bytes with explicit default assignments")

if a.reference_nextpnr:
    route(top, o / "plain.qsf", "plain-ref", nextpnr=a.reference_nextpnr)
    ref_bytes = (o / "plain-ref.rbf").read_bytes()
    assert plain_bytes == ref_bytes, "RBF bytes differ without electrical assignments"
    print("PASS: no electrical assignments gives byte-identical RBF bytes with the reference binary")
