#!/usr/bin/env python3
"""Check SDC clock groups, clock false paths and clock multicycles."""
import argparse
import json
import re
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for key in ("yosys", "nextpnr", "output"):
    parser.add_argument("--" + key, required=True, type=Path)
a = parser.parse_args()
f = Path(__file__).resolve().parent
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)


def run(command, log, success=True):
    result = subprocess.run(
        [str(x) for x in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=1200
    )
    log.write_text(result.stdout)
    assert (result.returncode == 0) == success, result.stdout[-3000:]
    return result.stdout


run(
    [a.yosys, "-p", f"read_verilog {f / 'top.v'}; synth_intel_alm -nodsp -top top; write_json {o / 'top.json'}"],
    o / "synth.log",
)
base = (f / "base.sdc").read_text()
FAST, SLOW = "clocks[0]", "clocks[1]"


def route(name, extra, success=True):
    sdc = o / (name + ".sdc")
    sdc.write_text(base + extra)
    command = [a.nextpnr, "--device", "5CSEBA6U23I7", "--qsf", f / "pins.qsf", "--sdc", sdc, "--json", o / "top.json",
               "--seed", "1", "--timing-allow-fail"]
    if success:
        command += ["--report", o / (name + ".json")]
    log = run(command, o / (name + ".log"), success)
    return log, (json.loads((o / (name + ".json")).read_text()) if success else None)


def untimed(log, launch, capture):
    return f"Max delay posedge {launch} -> posedge {capture}" in log


def hold(log, launch, capture):
    if launch == capture:
        return f"Hold/min time violation for clock 'posedge {launch}'" in log
    return f"Hold/min time violation for path 'posedge {launch}' -> 'posedge {capture}'" in log


# The two PLL outputs share a 20 ns period with a 90 degree offset, so
# nextpnr times the crossings as related clocks.
log, report = route("base", "")
assert not untimed(log, FAST, SLOW) and not untimed(log, SLOW, FAST)
base_fast = report["fmax"][FAST]["achieved"]

log, report = route("groups", "set_clock_groups -asynchronous -group [get_clocks {clocks[0]}] "
                    "-group [get_clocks {clocks[1]}]\n")
assert untimed(log, FAST, SLOW) and untimed(log, SLOW, FAST), log[-2000:]
assert not hold(log, FAST, SLOW) and not hold(log, SLOW, FAST), log[-2000:]
groups_fast = report["fmax"][FAST]["achieved"]
assert groups_fast > base_fast, (groups_fast, base_fast)
print(f"PASS: set_clock_groups cuts both crossings ({FAST}: {base_fast:.2f} -> {groups_fast:.2f} MHz)")

log, report = route("wildcard", "set_clock_groups -exclusive -group [get_clocks {*s[0]}] -group {clocks[1]}\n")
assert untimed(log, FAST, SLOW) and untimed(log, SLOW, FAST)
log, report = route("single-group", "set_clock_groups -asynchronous -group [get_clocks {clocks[0]}]\n")
assert untimed(log, FAST, SLOW) and untimed(log, SLOW, FAST)
print("PASS: wildcard and brace-list groups; a single group is exclusive with every other clock")

log, report = route("false-path", "set_false_path -from [get_clocks {clocks[1]}] -to [get_clocks {clocks[0]}]\n")
assert untimed(log, SLOW, FAST) and not untimed(log, FAST, SLOW), log[-2000:]
assert not hold(log, SLOW, FAST), log[-2000:]
print("PASS: clock-to-clock set_false_path cuts only its direction")

log, report = route("false-self", "set_false_path -from [get_clocks {clocks[0]}] -to [get_clocks {clocks[0]}]\n")
assert not hold(log, FAST, FAST), log[-2000:]
print("PASS: a clock false-path to itself cuts hold on that clock")

multicycle = ("set_clock_groups -asynchronous -group [get_clocks {clocks[0]}] -group [get_clocks {clocks[1]}]\n"
              "set_multicycle_path -setup 2 -from [get_clocks {clocks[0]}] -to [get_clocks {clocks[0]}]\n"
              "set_multicycle_path -hold 1 -from [get_clocks {clocks[0]}] -to [get_clocks {clocks[0]}]\n")
log, report = route("multicycle", multicycle)
path = next(p for p in report["critical_paths"] if p["from"] == p["to"] == "posedge " + FAST)
delay = sum(step["delay"] for step in path["path"])
achieved = report["fmax"][FAST]["achieved"]
assert abs(achieved - 2000.0 / delay) < 0.05 * achieved, (achieved, delay)
print(f"PASS: set_multicycle_path -setup 2 reports {achieved:.2f} MHz for a {delay:.3f} ns path")

# -setup and -hold on one command must still relax setup. Hold stays single-cycle.
combined = ("set_clock_groups -asynchronous -group [get_clocks {clocks[0]}] -group [get_clocks {clocks[1]}]\n"
            "set_multicycle_path -setup 2 -hold 1 -from [get_clocks {clocks[0]}] -to [get_clocks {clocks[0]}]\n")
log, report = route("setup-and-hold", combined)
combined_achieved = report["fmax"][FAST]["achieved"]
assert abs(combined_achieved - achieved) < 0.05, (combined_achieved, achieved)
assert "hold checks keep the single-cycle relationship" in log
print(f"PASS: -setup 2 -hold 1 on one command still reports {combined_achieved:.2f} MHz")

for name, extra, expected in [
    ("mc-cells", "set_multicycle_path -setup 2 -to [get_ports {Q[0]}]\n", "supports only clock -from/-to"),
    ("mc-zero", "set_multicycle_path -setup 0 -from [get_clocks {clocks[0]}]\n", "at least 1"),
    ("groups-kind", "set_clock_groups -group [get_clocks {clocks[0]}]\n", "needs -asynchronous"),
    ("groups-empty", "set_clock_groups -asynchronous\n", "at least one -group"),
    ("get-clocks-option", "set_clock_groups -asynchronous -group [get_clocks -of_objects x]\n",
     "unsupported argument '-of_objects'"),
]:
    log, _ = route(name, extra, success=False)
    assert expected in log, (name, log[-2000:])
print("PASS: five unsupported clock exception forms rejected")

# A fabric clock with no create_clock has a null clkconstr. -setup 2 must use
# the target period instead of crashing, and must double the reported fmax.
free = o / "free.json"
run([a.yosys, "-p", f"read_verilog {f / 'free.v'}; synth_intel_alm -nodsp -top free; write_json {free}"],
    o / "free-synth.log")


def route_free(name, extra):
    sdc = o / (name + ".sdc")
    sdc.write_text(extra)
    return run([a.nextpnr, "--device", "5CSEBA6U23I7", "--qsf", f / "free.qsf", "--sdc", sdc, "--json", free,
                "--seed", "1", "--timing-allow-fail", "--report", o / (name + ".json")], o / (name + ".log"))


def clock_fmax(log):
    found = re.findall(r"Max frequency for clock\s+'([^']+)': ([0-9.]+) MHz", log)
    assert found, log[-2000:]
    names = {name for name, _ in found}
    assert len(names) == 1, found
    # An estimate is printed before the routed fmax. The last line is routed.
    return found[-1][0], float(found[-1][1])


plain = route_free("free-base", "\n")
clock, base_fmax = clock_fmax(plain)
relaxed = route_free("free-mc", "set_multicycle_path -setup 2 -from [get_clocks {%s}] -to [get_clocks {%s}]\n" %
                     (clock, clock))
_, mc_fmax = clock_fmax(relaxed)
assert abs(mc_fmax - 2 * base_fmax) < 0.05 * base_fmax, (clock, base_fmax, mc_fmax)
print(f"PASS: multicycle on unconstrained clock '{clock}' reports {base_fmax:.2f} -> {mc_fmax:.2f} MHz")
