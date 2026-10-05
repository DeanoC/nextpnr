#!/usr/bin/env python3
"""Check native DDR-capture/SDR-data/OE pad timing through packing and checkpoint reload."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--sdr", action="store_true", help="Use the high-speed FES SDR capture pattern")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    def run(command, name, success=True, diagnostic=None):
        result = subprocess.run([str(x) for x in command], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=1200)
        (out / (name + ".log")).write_text(result.stdout)
        if (result.returncode == 0) != success:
            raise RuntimeError(f"{name}: unexpected exit {result.returncode}; see {out / (name + '.log')}")
        if diagnostic and diagnostic not in result.stdout:
            raise RuntimeError(f"{name}: missing diagnostic {diagnostic!r}")

    synth = out / "synth.json"
    rtl = here.parent / "io-registers" / "ddr.v"
    qsf = here / "pins.qsf"
    if args.sdr:
        text = rtl.read_text()
        start = text.index('        altddio_in #')
        end = text.index('    end endgenerate', start)
        text = text[:start] + ('        reg sample;\n        always @(posedge clk) sample <= pin;\n'
                              '        assign q[2*i] = sample;\n        assign q[2*i+1] = sample;\n') + text[end:]
        rtl = out / "sdr.v"
        rtl.write_text(text)
        qsf = out / "sdr.qsf"
        qsf.write_text((here / "pins.qsf").read_text() +
                       "set_instance_assignment -name FAST_INPUT_REGISTER ON -to dq[*]\n")
    run([args.yosys.resolve(), "-p",
         f"read_verilog {rtl}; synth_intel_alm -nobram -nodsp -top top; write_json {synth}"], "synth")
    # Synthetic host regression budgets; these are not real SDRAM constraints.
    base = ("create_clock -period 20 -name memory [get_ports clk]\n"
            "set_input_delay -clock memory -min 1 [get_ports {dq[*]}]\n"
            "set_input_delay -clock memory -max 3 [get_ports {dq[*]}]\n"
            "set_output_delay -clock memory -min 0 [get_ports {dq[*]}]\n"
            "set_output_delay -clock memory -max 4 [get_ports {dq[*]}]\n")
    cases = {
        "pass": (base, True, None),
        "setup-fail": (base.replace("-max 4", "-max 40"), False, "FAIL at"),
        "clock-cut-fail": (base.replace("-period 20", "-period 1") +
                           "set_false_path -from [get_clocks memory] -to [get_clocks memory]\n",
                           False, "outside its timing model"),
    }
    for name, (text, success, diagnostic) in cases.items():
        sdc = out / (name + ".sdc")
        sdc.write_text(text)
        command = [args.nextpnr.resolve(), "--device", "5CSEBA6U23I7", "--json", synth,
                   "--qsf", qsf, "--sdc", sdc, "--seed", "1", "--router", "router2"]
        if name == "clock-cut-fail":
            command += ["--timing-allow-fail"]
        if success:
            command += ["--write", out / "routed.json", "--report", out / "report.json",
                        "--detailed-timing-report"]
        run(command, name, success, diagnostic)
    checkpoint = json.loads((out / "routed.json").read_text())
    # Timing aliases belong to the analyser, not the persisted routed design.
    assert "PAD$timing$" not in json.dumps(checkpoint)
    run([args.nextpnr.resolve(), "--device", "5CSEBA6U23I7", "--json", out / "routed.json",
         "--no-pack", "--no-place", "--no-route", "--report", out / "reload.json",
         "--detailed-timing-report", "--rbf", out / "reload.rbf"], "reload")
    for filename in ("report.json", "reload.json"):
        report = json.loads((out / filename).read_text())
        paths = json.dumps(report["detailed_net_timings"])
        assert "PAD$timing$read$" in paths, filename
        assert "PAD$timing$write$" in paths, filename
        assert "$oe$" in paths, filename
    print("PASS: native registered DQ timing, data setup gate, clock cuts/allow-fail guard and fresh checkpoint")


if __name__ == "__main__":
    main()
