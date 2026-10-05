#!/usr/bin/env python3
"""Check AD20 constant DDR clock-forwarder phase timing and checkpoint replay."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    def run(command, folder, name, success=True, diagnostic=None):
        result = subprocess.run([str(x) for x in command], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=1200)
        (folder / (name + ".log")).write_text(result.stdout)
        if (result.returncode == 0) != success or (diagnostic and diagnostic not in result.stdout):
            raise RuntimeError(f"{name}: unexpected result; see {folder / (name + '.log')}")

    # Synthetic data timing checks expose both forwarded pad phases; they are
    # not generated-clock declarations or SDRAM clock waveform requirements.
    base = ("create_clock -period 40 -name memory [get_ports FPGA_CLK1_50]\n"
            "set_output_delay -clock memory -min 0 [get_ports DDR_OUT]\n"
            "set_output_delay -clock memory -max 1 [get_ports DDR_OUT]\n")
    for inverted in (False, True):
        folder = out / ("inverted" if inverted else "normal")
        folder.mkdir(exist_ok=True)
        synth = folder / "synth.json"
        rtl = here.parent / "ddr-output" / "top.v"
        run([args.yosys.resolve(), "-p", f"read_verilog {rtl}; chparam -set MINIMAL 1 "
             f"-set INVERTED {int(inverted)} top; synth_intel_alm -nobram -nodsp -top top; write_json {synth}"],
            folder, "synth")
        qsf = folder / "pins.qsf"
        qsf.write_text((here.parent / "ddr-output" / "pins.qsf").read_text().replace("PIN_W15", "PIN_AD20") +
                       "set_instance_assignment -name NEXTPNR_GPIO_TIMING_PROFILE QUARTUS_17_0_2_RAMTEST -to DDR_OUT\n"
                       "set_instance_assignment -name BOARD_MODEL_FAR_C 30P -to DDR_OUT\n")
        sdc = folder / "clocks.sdc"
        sdc.write_text(base)
        run([args.nextpnr.resolve(), "--device", "5CSEBA6U23I7", "--json", synth, "--qsf", qsf,
             "--sdc", sdc, "--seed", "1", "--router", "router2", "--write", folder / "routed.json",
             "--report", folder / "report.json", "--detailed-timing-report"], folder, "route")
        run([args.nextpnr.resolve(), "--device", "5CSEBA6U23I7", "--json", folder / "routed.json",
             "--no-pack", "--no-place", "--no-route", "--report", folder / "reload.json",
             "--detailed-timing-report", "--rbf", folder / "reload.rbf"], folder, "reload")
        checkpoint = json.loads((folder / "routed.json").read_text())
        assert "PAD$timing$" not in json.dumps(checkpoint)
        for filename in ("report.json", "reload.json"):
            report = json.loads((folder / filename).read_text())
            endpoints = [ep for net in report["detailed_net_timings"] for ep in net["endpoints"]]
            for edge in ("rise", "fall"):
                endpoint = next(ep for ep in endpoints if ep["port"] == f"PAD$timing$write${edge}$external")
                source_falls = (edge == "rise") == inverted
                assert endpoint["source"]["event"].startswith("negedge" if source_falls else "posedge"), filename
            assert "PAD$timing$write$" in json.dumps(report["critical_paths"]), filename
        sdc.write_text(base.replace("-period 40", "-period 1") +
                       "set_false_path -from [get_clocks memory] -to [get_clocks memory]\n")
        run([args.nextpnr.resolve(), "--device", "5CSEBA6U23I7", "--json", synth, "--qsf", qsf,
             "--sdc", sdc, "--timing-allow-fail"], folder, "clock-cut", False, "outside its timing model")
    print("PASS: both AD20 forwarder polarities, launch phases, native clock guard and fresh checkpoint")


if __name__ == "__main__":
    main()
