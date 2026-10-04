#!/usr/bin/env python3
"""Check Cyclone V control block atoms and QSF device options against Quartus."""
import argparse
import copy
import gzip
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
mapping = json.loads((f / "oracle/mapping.json").read_text())
DEVICE = "5CSEBA6U23I7"


def run(command, log, success=True):
    result = subprocess.run([str(x) for x in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            timeout=900)
    log.write_text(result.stdout)
    assert (result.returncode == 0) == success, result.stdout[-3000:]
    return result.stdout


def synth(source, name):
    out = o / (name + ".json")
    run([a.yosys, "-p", f"read_verilog {source}; synth_intel_alm -nobram -nolutram -nodsp -top top; write_json {out}"],
        o / (name + "-synth.log"))
    return out


def pnr(netlist, qsf, name, success=True, extra=()):
    args = [a.nextpnr, "--device", DEVICE, "--json", netlist, "--qsf", qsf]
    if success:
        args += ["--rbf", o / (name + ".rbf"), "--write", o / (name + "-routed.json")]
    else:
        args += ["--no-route"]
    return run(args + list(extra), o / (name + "-pnr.log"), success)


def decode(rbf, name):
    bt = o / (name + ".bt")
    run([a.mistral_cv, "decomp", DEVICE, rbf, bt], o / (name + "-decode.log"))
    return bt.read_text().splitlines()


def oracle(name):
    rbf = o / ("oracle-" + name + ".rbf")
    rbf.write_bytes(gzip.open(f / "oracle" / (name + ".rbf.gz")).read())
    return decode(rbf, "oracle-" + name)


def ctrl_ports(lines):
    return sorted({p for line in lines if line.startswith("r ") for p in re.findall(r"CTRL\.000\.002:\S+", line)})


def settings(lines):
    # Non-routing state with the design-dependent checksum usercode removed.
    return {line.split(" ;")[0] for line in lines
            if line[:2] in ("o ", "i ", "s ") and not line.startswith(("s LAB", "s MLAB", "o JTAG_ID"))}


def option(lines, name):
    return next((line.split()[2] for line in lines if line.startswith("o " + name + " ")), None)


def qsf(name, *assignments):
    path = o / (name + ".qsf")
    text = (f / "pins.qsf").read_text()
    path.write_text(text + "".join(x + "\n" for x in assignments))
    return path


# 1. Every supported CTRL atom in one design: the same block ports, the same
#    global-clock entries into the CTRL clock muxes and the same CRC divider
#    as the Quartus compile of the identical RTL; no CTRL setting or inverter.
atoms = synth(f / "top.v", "atoms")
atoms_qsf = qsf("atoms", "set_global_assignment -name ERROR_CHECK_FREQUENCY_DIVISOR 2")
log = pnr(atoms, atoms_qsf, "atoms")
assert "no characterized interface timing" in log
assert "Trimming port 'altera_reserved_tdo'" in log, log[-2000:]
ours = decode(o / "atoms.rbf", "atoms")
quartus = oracle("atoms")
assert ctrl_ports(quartus) == mapping["atoms_oracle"]["ctrl_ports"]
assert ctrl_ports(ours) == ctrl_ports(quartus), (ctrl_ports(ours), ctrl_ports(quartus))
for route in mapping["atoms_oracle"]["global_clock_routes"]:
    assert route in ours, route
assert option(ours, "CRC_DIVIDE_ORDER") == option(quartus, "CRC_DIVIDE_ORDER") == "1"
assert option(ours, "JTAG_ID") is None  # nextpnr keeps usercode FFFFFFFF
for lines in (ours, quartus):
    assert not [line for line in lines if line[:2] in ("i ", "s ") and "CTRL" in line]
routed = json.loads((o / "atoms-routed.json").read_text())["modules"]["top"]["cells"]
for cell_type in mapping["atoms"]:
    cells = [c for c in routed.values() if c["type"] == cell_type]
    assert len(cells) == 1 and cells[0]["attributes"]["NEXTPNR_BEL"].startswith(cell_type + ".0.2."), cell_type
assert "tck" not in next(c for c in routed.values() if c["type"] == "cyclonev_jtag")["connections"]
print("PASS: chip ID, CRC, OPREG, user/core JTAG and oscillator ports match the Quartus routes")

# 2. QSF device options: the change from the plain design matches Quartus,
#    setting for setting (option bits, dedicated pins, unused pads).
blinky = synth(f / "blinky.v", "blinky")
pnr(blinky, qsf("blinky"), "blinky")
base = settings(decode(o / "blinky.rbf", "blinky"))
quartus_base = settings(oracle("blinky"))
for name in ("options", "bushold"):
    pnr(blinky, qsf(name, *(f / (name + ".qsf")).read_text().splitlines()), name)
    ours = decode(o / (name + ".rbf"), name)
    quartus = oracle(name)
    ours_delta = (settings(ours) - base, base - settings(ours))
    quartus_delta = (settings(quartus) - quartus_base, quartus_base - settings(quartus))
    assert ours_delta == quartus_delta, (sorted(ours_delta[0] ^ quartus_delta[0])[:20],
                                         sorted(ours_delta[1] ^ quartus_delta[1])[:20])
    assert option(ours, "JTAG_ID") == ("0c0ffee5" if name == "options" else None)
    print(f"PASS: {name}: {len(ours_delta[0])} settings identical to the Quartus delta")
assert option(quartus, "JTAG_ID") is not None  # Quartus checksum usercode by default


# 3. Accepted variants.
log = pnr(blinky, qsf("ignored", "set_global_assignment -name SOME_QUARTUS_SETTING whatever",
                      "set_global_assignment -name CRC_ERROR_CHECKING ON",
                      "set_global_assignment -name ENABLE_INIT_DONE_OUTPUT OFF",
                      "set_global_assignment -name RESERVE_ALL_UNUSED_PINS_WEAK_PULLUP "
                      "\"As input tri-stated with weak pull-up\""), "ignored")
assert (o / "ignored.rbf").read_bytes() == (o / "blinky.rbf").read_bytes()
design = json.loads(atoms.read_text())
crc = next(c for c in design["modules"]["top"]["cells"].values() if c["type"] == "cyclonev_crcblock")
crc["parameters"]["oscillator_divider"] = "00000000000000000000000000000100"
divider = o / "divider.json"
divider.write_text(json.dumps(design))
log = pnr(divider, atoms_qsf, "divider")
assert "as in Quartus the atom's divider is used" in log
assert option(decode(o / "divider.rbf", "divider"), "CRC_DIVIDE_ORDER") == "2"
print("PASS: default and unknown assignments leave the bitstream unchanged; atom CRC divider wins")


# 4. Rejected requests.
def reject(name, expected, edit=None, assignments=(), source=atoms):
    netlist = source
    if edit is not None:
        design = json.loads(source.read_text())
        edit(design["modules"]["top"]["cells"])
        netlist = o / (name + ".json")
        netlist.write_text(json.dumps(design))
    log = pnr(netlist, qsf(name, *assignments), name, success=False)
    assert expected in log, (name, log[-2000:])


def cell(cells, cell_type):
    return next(c for c in cells.values() if c["type"] == cell_type)


def duplicate(cells):
    second = copy.deepcopy(cell(cells, "cyclonev_chipidblock"))
    del second["connections"]["regout"]
    cells["chipid2"] = second


def retype(cell_type):
    def edit(cells):
        cell(cells, "cyclonev_chipidblock")["type"] = cell_type
    return edit


def set_param(cell_type, key, value):
    def edit(cells):
        cell(cells, cell_type)["parameters"][key] = value
    return edit


def move_port(cell_type, src, dst):
    def edit(cells):
        c = cell(cells, cell_type)
        c["connections"][dst] = c["connections"].pop(src)
        c["port_directions"][dst] = c["port_directions"].pop(src)
    return edit


def jtag_tck_from_logic(cells):
    jtag = cell(cells, "cyclonev_jtag")
    jtag["connections"]["tck"] = cell(cells, "cyclonev_chipidblock")["connections"]["shiftnld"]


def crcerror_to_pad(cells):
    led = next(c for c in cells.values() if c["type"] == "MISTRAL_OB")
    led["connections"]["I"] = cell(cells, "cyclonev_crcblock")["connections"]["crcerror"]


reject("rublock", "Active Serial configuration scheme", retype("cyclonev_rublock"))
reject("asmiblock", "Active Serial configuration scheme", retype("cyclonev_asmiblock"))
reject("duplicate", "The device has one cyclonev_chipidblock", duplicate)
reject("tdoutap", "Quartus rejects the tdoutap port", move_port("cyclonev_jtag", "tdouser", "tdoutap"))
reject("unknown-port", "port bogus is not supported", move_port("cyclonev_chipidblock", "shiftnld", "bogus"))
reject("jtag-pin", "port tck is the dedicated JTAG pin", jtag_tck_from_logic)
reject("divider", "oscillator_divider must be", set_param("cyclonev_crcblock", "oscillator_divider", "3"))
reject("crc-param", "triple_adj_err_correction must be",
       set_param("cyclonev_crcblock", "triple_adj_err_correction", "true"))
reject("unknown-param", "unsupported parameter bogus", set_param("cyclonev_jtag", "bogus", "1"))
reject("crcerror-pad", "crcerror drives a pad directly", crcerror_to_pad)
for name, assignments, expected in (
        ("usercode-checksum", ["STRATIX_JTAG_USER_CODE 12345678"], "needs USE_CHECKSUM_AS_USERCODE OFF"),
        ("checksum-on", ["USE_CHECKSUM_AS_USERCODE ON"], "does not compute the Quartus checksum"),
        ("usercode-hex", ["STRATIX_JTAG_USER_CODE 12345G78", "USE_CHECKSUM_AS_USERCODE OFF"],
         "1 to 8 hexadecimal digits"),
        ("unused-ground", ["RESERVE_ALL_UNUSED_PINS_WEAK_PULLUP \"AS OUTPUT DRIVING GROUND\""],
         "Unsupported value"),
        ("crc-divisor", ["ERROR_CHECK_FREQUENCY_DIVISOR 3"], "ERROR_CHECK_FREQUENCY_DIVISOR must be"),
        ("open-drain", ["INIT_DONE_OPEN_DRAIN OFF"], "Unsupported value 'OFF' for INIT_DONE_OPEN_DRAIN"),
        ("pin-conflict", ["ENABLE_INIT_DONE_OUTPUT ON"], "reserves pin AA20 for INIT_DONE")):
    extra = ["set_location_assignment PIN_AA20 -to LED[7]"] if name == "pin-conflict" else []
    reject(name, expected, assignments=["set_global_assignment -name " + x for x in assignments] + extra,
           source=blinky)
print("PASS: 17 unsupported atoms, connections, parameters and option values rejected")
