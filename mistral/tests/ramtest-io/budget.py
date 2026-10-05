#!/usr/bin/env python3
"""Generate diagnostic EM63B165 -6 budgets from an actual forwarded-clock report.

Board intervals are required assumptions in ns, not built-in board specifications.
This does not qualify the reference FPGA timing model or prove bus turnaround.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path


def interval(values):
    lo, hi = values
    if not all(math.isfinite(x) for x in values) or lo < 0 or lo > hi:
        raise ValueError("flight/delay intervals must be finite, nonnegative and ordered")
    return lo, hi


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--memory-mhz", type=int, choices=(100, 130), required=True)
    for name in ("clock-flight", "data-flight", "read-flight", "cs-inverter"):
        parser.add_argument("--" + name, nargs=2, type=float, required=True, metavar=("MIN", "MAX"))
    parser.add_argument("--margin", type=float, required=True)
    args = parser.parse_args()
    if not math.isfinite(args.margin) or args.margin < 0:
        raise ValueError("margin must be finite and nonnegative")
    flight = {name: interval(getattr(args, name.replace("-", "_")))
              for name in ("clock-flight", "data-flight", "read-flight", "cs-inverter")}
    report = json.loads(args.report.read_text())
    if not report["timing_summary"]["final_analogue_model"]:
        raise ValueError("budget extraction requires final analogue timing, not a router estimate")
    checkpoint = json.loads(args.checkpoint.read_text())["modules"]["top"]
    clocks = json.loads(checkpoint["settings"]["timing/io_clocks"])
    clock_name = "ram_clock.clocks[0]"
    clock = next(c for c in clocks if c["net"] == clock_name)
    if abs(clock["period"][0] - 1000 / args.memory_mhz) > 0.002:
        raise ValueError("checkpoint clock period does not match the selected memory rate")
    # The RAM tester drives H=0/L=1: chip rise is fabric fall, chip fall is rise.
    cell_name, forwarder = next((n, c) for n, c in checkpoint["cells"].items()
                               if c["type"] == "MISTRAL_DDROUT" and
                               c["attributes"].get("LOC") == "PIN_AD20")
    if int(forwarder["parameters"]["DDR_HIGH"], 2) != 0:
        raise ValueError("this budget expects the tester's inverted forwarder")
    net = next(n for n in report["detailed_net_timings"] if n["net"] == "SDRAM_CLK")
    endpoints = net["endpoints"]
    pad = {}
    for edge, launch in (("rise", "negedge"), ("fall", "posedge")):
        endpoint = next(e for e in endpoints if e["cell"] == cell_name and
                        e["port"] == f"PAD$timing$write${edge}$external")
        if endpoint["source"]["event"] != f"{launch} {clock_name}":
            raise ValueError("forwarder reference clock/edge mismatch")
        # STA arrival includes routed launch clock + complete local mux/pad arc.
        lo, hi = endpoint["delay"]
        if not all(math.isfinite(x) for x in (lo, hi)) or lo > hi:
            raise ValueError("invalid clock-pad timing interval")
        pad[edge] = [lo + flight["clock-flight"][0], hi + flight["clock-flight"][1]]
    cmin, cmax = pad["rise"]
    dmin, dmax = flight["data-flight"]
    rmin, rmax = flight["read-flight"]
    margin = args.margin
    tac = 6.0 if args.memory_mhz == 100 else 5.4  # CL2 / CL3, Rev2.4 Table11
    bounds = {
        "input": [cmin + 2.5 + rmin - margin, cmax + tac + rmax + margin],
        "output": [dmin - cmax - 0.8 - margin, 1.5 + dmax - cmin + margin],
    }
    # One chip is direct, the other inverted: take the stricter bound of both.
    bounds["chip_select"] = [bounds["output"][0],
                             bounds["output"][1] + flight["cs-inverter"][1]]
    # Independent early/late intervals deliberately retain reference-model
    # pessimism. Never substitute an observed minimum to manufacture closure.
    pulse = {
        "high_min": clock["low"][0] + pad["fall"][0] - pad["rise"][1] - margin,
        "low_min": clock["high"][0] + pad["rise"][0] - pad["fall"][1] - margin,
    }
    sdc = ["# Diagnostic assumptions; not measured-board or hardware acceptance."]
    selections = {
        "input": ("set_input_delay", "SDRAM_DQ[*]"),
        "output": ("set_output_delay", "SDRAM_DQ[*] SDRAM_A[*] SDRAM_BA[*] SDRAM_CKE SDRAM_nRAS SDRAM_nCAS SDRAM_nWE"),
        "chip_select": ("set_output_delay", "SDRAM_nCS"),
    }
    for kind, (command, ports) in selections.items():
        for bound, value in zip(("min", "max"), bounds[kind]):
            sdc.append(f"{command} -clock {{{clock_name}}} -clock_fall -{bound} {value:.6f} [get_ports {{{ports}}}]")
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "board.sdc").write_text("\n".join(sdc) + "\n")
    receipt = {
        "status": "diagnostic assumptions only; turnaround and hardware unverified",
        "memory_mhz": args.memory_mhz, "cas_latency": 2 if args.memory_mhz == 100 else 3,
        "datasheet": "https://etron.com/wp-content/uploads/2022/04/EM63B165TSBM_Rev-2.4.pdf",
        "assumptions_ns": flight, "margin_ns": margin,
        "clock_at_chip_ns": pad, "delay_budgets_ns": bounds,
        "chip_clock_pulse_ns": pulse,
        "chip_clock_pulse_proven": min(pulse.values()) >= 2.0,
        "source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (args.report, args.checkpoint)},
    }
    (args.output / "budget.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
