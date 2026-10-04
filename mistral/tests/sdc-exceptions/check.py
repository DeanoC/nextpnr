#!/usr/bin/env python3
"""Check SDC clock groups, clock false paths and clock multicycles."""
import argparse
import json
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


# The two PLL outputs share a 20 ns period with a 90 degree offset, so
# nextpnr times the crossings as related clocks.
log, report = route("base", "")
assert not untimed(log, FAST, SLOW) and not untimed(log, SLOW, FAST)
base_fast = report["fmax"][FAST]["achieved"]

log, report = route("groups", "set_clock_groups -asynchronous -group [get_clocks {clocks[0]}] "
                    "-group [get_clocks {clocks[1]}]\n")
assert untimed(log, FAST, SLOW) and untimed(log, SLOW, FAST), log[-2000:]
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
print("PASS: clock-to-clock set_false_path cuts only its direction")

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
