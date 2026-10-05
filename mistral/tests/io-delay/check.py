#!/usr/bin/env python3
"""Check external setup/hold timing gates through synthesis, packing and routing."""
import argparse
import json
from pathlib import Path
import subprocess


def run(command, log):
    with log.open("w") as stream:
        return subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    synth = out / "synth.json"
    rc = run([str(args.yosys.resolve()), "-p",
              f"read_verilog {here / 'top.v'}; synth_intel_alm -nobram -nodsp -top top; write_json {synth}"],
             out / "synth.log")
    if rc:
        raise RuntimeError(f"Synthesis failed: {out / 'synth.log'}")
    # Synthetic budgets for this register fixture, not SDRAM device parameters.
    base = ("create_clock -period 20 -name memory [get_ports clk]\n"
            "set_input_delay -clock memory -min 1 [get_ports din]\n"
            "set_input_delay -clock memory -max 3 [get_ports din]\n"
            "set_output_delay -clock memory -min 0 [get_ports dout]\n"
            "set_output_delay -clock memory -max 4 [get_ports dout]\n")
    cases = {
        "pass": (base, True),
        "input-setup-fail": (base.replace("-max 3", "-max 40"), False),
        "output-setup-fail": (base.replace("-max 4", "-max 40"), False),
        "output-hold-fail": (base.replace("-min 0", "-min -40"), False),
    }
    results = {}
    for name, (sdc, expected_pass) in cases.items():
        directory = out / name
        directory.mkdir(exist_ok=True)
        constraints = directory / "clocks.sdc"
        constraints.write_text(sdc)
        rc = run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7", "--json", str(synth),
                  "--qsf", str(here / "pins.qsf"), "--sdc", str(constraints), "--seed", "1",
                  "--router", "router2", "--write", str(directory / "routed.json"),
                  "--report", str(directory / "report.json"), "--detailed-timing-report",
                  "--rbf", str(directory / "core.rbf")], directory / "route.log")
        log = (directory / "route.log").read_text()
        if (rc == 0) != expected_pass:
            raise RuntimeError(f"Unexpected result for {name}: exit {rc}; see {directory / 'route.log'}")
        if not expected_pass and not any(token in log for token in ("FAIL at", "Hold/min time violation")):
            raise RuntimeError(f"{name} failed without the expected timing diagnostic")
        if expected_pass:
            checkpoint = json.loads((directory / "routed.json").read_text())["modules"]["top"]
            settings = checkpoint["settings"]
            assert len(json.loads(settings["timing/io_delays"])) == 2
            assert json.loads(settings["timing/io_clocks"])
            report = json.loads((directory / "report.json").read_text())
            # Both external arrival and output setup requirements must appear
            # in actual path reports, not just parser metadata.
            paths = report["critical_paths"]
            segments = [segment for path in paths for segment in path["path"]]
            assert any(s["type"] == "source" and s["delay"] == 3 for s in segments)
            assert any(s["type"] == "setup" and s["delay"] == 4 for s in segments)
            # A fresh process must restore ports and reference clocks without
            # rereading the SDC or repacking the saved design.
            reload_report = directory / "reloaded-report.json"
            reload_rc = run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                             "--json", str(directory / "routed.json"),
                             "--no-pack", "--no-place", "--no-route",
                             "--report", str(reload_report), "--detailed-timing-report",
                             "--rbf", str(directory / "reloaded.rbf")], directory / "reload.log")
            if reload_rc:
                raise RuntimeError(f"Checkpoint reload failed: {directory / 'reload.log'}")
            reloaded = json.loads(reload_report.read_text())["critical_paths"]
            segments = [segment for path in reloaded for segment in path["path"]]
            assert any(s["type"] == "source" and s["delay"] == 3 for s in segments)
            assert any(s["type"] == "setup" and s["delay"] == 4 for s in segments)
            results["checkpoint-reload"] = {"exit_code": reload_rc, "expected_pass": True}
        results[name] = {"exit_code": rc, "expected_pass": expected_pass}
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print("PASS: external input/output setup and output hold gates; saved constraints and path reports")


if __name__ == "__main__":
    main()
