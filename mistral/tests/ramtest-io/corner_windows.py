#!/usr/bin/env python3
"""Audit correlated windows in the retained Quartus SDR reference fit.

Results describe that fit's clock routes, not the native FES PLL clock routes.
No production timing bounds or SDC constraints are replaced by this analysis.
"""
import argparse
import collections
import copy
import hashlib
import json
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gpio-timing"))
from pad_summary import arc, summarize


def analyze(evidence, flight, margin, distortion):
    summarize(evidence)  # Validate complete early/late, corner and transition coverage.
    if evidence["device"] != "5CSEBA6U23I7" or evidence["output_load_pf"] != 30:
        raise ValueError("expected the qualified device and 30pF SDR reference")
    reports = evidence["variants"]["ramtest-sdr-pads"]["reports"]
    rows = collections.defaultdict(lambda: collections.defaultdict(list))
    for name, paths in reports.items():
        output = "/output-" in name
        if not output and "/input-" not in name:
            continue
        if "-rise-" not in name and "-fall-" not in name:
            continue  # Generic worst-path queries duplicate explicit transition queries.
        kind = "hold" if name.endswith("hold.rpt") else "setup"
        for path in paths:
            if any(not math.isfinite(p[key]) for p in path["points"]
                   for key in ("total_ns", "incremental_ns")):
                raise ValueError("nonfinite fitted timing data")
            extracted = arc(path, kind, output)
            if extracted is None:
                continue
            family, data = extracted
            if output and path["To Node"] == "SDRAM_CLK":
                # This fixture varies DDR data. Select only the transfers that
                # form the tester's H=0/L=1 waveform; separate constant-forward
                # fits established the same local delay for these transfers.
                if data["clock_edge"] == data["pad_transition"]:
                    continue
                points = [p for p in path["points"] if p["section"] == "arrival" and p["stage"] == "data"]
                index = next(i for i, p in enumerate(points) if p["node"].endswith("|muxsel"))
                origin = next(p for p in path["points"] if p["section"] == "arrival")
                # PLL compensation can change accumulated time without an
                # IC/CELL increment. Include the full clock prefix, and remove
                # only the ideal launch edge, rather than summing data ICs.
                prefix = points[index]["total_ns"] - origin["total_ns"]
                key = ("clock", data["pad_transition"], kind)
            elif not output and family == "read_sdr":
                points = [p for p in path["points"] if p["section"] == "required" and
                          p["stage"] == "clock" and p["type"] in ("IC", "CELL")]
                prefix = points[-2]["total_ns"] - points[0]["total_ns"]
                key = ("read", path["From Node"], kind)
            else:
                continue
            if not math.isfinite(prefix) or not math.isfinite(data["value_ps"]):
                raise ValueError("nonfinite fitted timing data")
            rows[name.split("/")[0]][key].append((data["value_ps"] / 1000, prefix))
    if len(rows) != 4:
        raise ValueError("missing fitted corners")
    result = {}
    for rate, tac in ((100, 6.0), (130, 5.4)):
        period = 1000 / rate
        corners = {}
        for corner, paths in sorted(rows.items()):
            clock = {}
            for edge in ("R", "F"):
                for kind in ("setup", "hold"):
                    values = paths[("clock", edge, kind)]
                    if len(values) != 1:
                        raise ValueError("missing or ambiguous constant clock edge")
                    clock[edge, kind] = values[0]
            def arrival(edge, kind):
                return sum(clock[edge, kind])
            pulses = {
                "local_high_min": period / 2 + clock["F", "hold"][0] - clock["R", "setup"][0],
                "local_low_min": period / 2 + clock["R", "hold"][0] - clock["F", "setup"][0],
                "fitted_high_min": period / 2 + arrival("F", "hold") - arrival("R", "setup"),
                "fitted_low_min": period / 2 + arrival("R", "hold") - arrival("F", "setup"),
            }
            # Declare independent 0..flight bounds on clock and return data.
            # The fitted clock uncertainty is retained; no common-path credit.
            for key in pulses:
                pulses[key] -= distortion + margin
            pins = {key[1] for key in paths if key[0] == "read"}
            if len(pins) != 16:
                raise ValueError("expected all sixteen fitted SDR DQ captures")
            windows = {}
            for pin in sorted(pins):
                setup = paths["read", pin, "setup"]
                hold = paths["read", pin, "hold"]
                if not setup or not hold:
                    raise ValueError("missing input setup/hold pair")
                lower = period / 2 + arrival("R", "setup") + tac + 2 * flight + margin
                lower += max(value - prefix for value, prefix in setup)
                upper = 1.5 * period + arrival("R", "hold") + 2.5 - margin
                upper += min(-value - prefix for value, prefix in hold)
                windows[pin] = [round(lower, 6), round(upper, 6)]
            corners[corner] = {"pulse_lower_bounds_ns": pulses,
                               "read_capture_windows_ns": windows,
                               "all_pin_window_ns": [max(w[0] for w in windows.values()),
                                                     min(w[1] for w in windows.values())]}
        common = [max(c["all_pin_window_ns"][0] for c in corners.values()),
                  min(c["all_pin_window_ns"][1] for c in corners.values())]
        result[str(rate)] = {"corners": corners, "common_absolute_capture_window_ns": common,
                            "common_window_exists": common[0] <= common[1]}
    return {"classification": "correlated fitted-reference diagnostic; native FES clocks and hardware unqualified",
            "flight_assumption_ns": [0, flight], "margin_ns": margin,
            "clock_edge_distortion_assumption_ns": distortion,
            "clock_reference": "input-clock rising edge; preserve one absolute read latency across corners",
            "rates": result}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--flight-max", type=float, required=True)
    parser.add_argument("--margin", type=float, required=True)
    parser.add_argument("--clock-distortion", type=float, required=True,
                        help="maximum board rise/fall flight difference, ns; an assumption")
    parser.add_argument("--check-rejections", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if any(not math.isfinite(x) or x < 0 for x in (args.flight_max, args.margin, args.clock_distortion)):
        raise ValueError("flight and margin must be finite and nonnegative")
    evidence = json.loads(args.evidence.read_text())
    result = analyze(evidence, args.flight_max, args.margin, args.clock_distortion)
    if args.check_rejections:
        for defect in ("corner", "clock", "pin", "device", "load", "finite"):
            bad = copy.deepcopy(evidence)
            reports = bad["variants"]["ramtest-sdr-pads"]["reports"]
            if defect == "device": bad["device"] = "other"
            elif defect == "load": bad["output_load_pf"] = 100
            elif defect == "corner":
                for name in list(reports):
                    if name.startswith("7_slow_1100mv_100c/"): del reports[name]
            elif defect in ("clock", "pin"):
                for name, paths in reports.items():
                    paths[:] = [p for p in paths if p["To Node"] != "SDRAM_CLK"] if defect == "clock" else [
                        p for p in paths if p["From Node"] != "SDRAM_DQ[0]"]
            else:
                path = next(p for p in reports["7_slow_1100mv_100c/output-rise-setup.rpt"]
                            if p["To Node"] == "SDRAM_CLK")
                next(p for p in path["points"] if p["node"].endswith("|muxsel"))["incremental_ns"] = float("nan")
            try:
                analyze(bad, args.flight_max, args.margin, args.clock_distortion)
            except (ValueError, KeyError):
                continue
            raise AssertionError("accepted defective evidence: " + defect)
        result["rejection_cases"] = 6
        translated = copy.deepcopy(evidence)
        for paths in translated["variants"]["ramtest-sdr-pads"]["reports"].values():
            for path in paths:
                active = None
                for point in path["points"]:
                    section = (point["section"], point["stage"])
                    if point["node"] == "FPGA_CLK1_50~input|o" and (
                            section == ("required", "clock") or
                            (path["To Node"] == "SDRAM_CLK" and point["section"] == "arrival")):
                        point["incremental_ns"] += 1.0
                        active = point["section"]
                    if point["section"] == active:
                        point["total_ns"] += 1.0
        shifted = analyze(translated, args.flight_max, args.margin, args.clock_distortion)
        for rate, data in result["rates"].items():
            other = shifted["rates"][rate]
            assert data["common_absolute_capture_window_ns"] == other["common_absolute_capture_window_ns"]
            for corner, values in data["corners"].items():
                for key, value in values["pulse_lower_bounds_ns"].items():
                    assert abs(value - other["corners"][corner]["pulse_lower_bounds_ns"][key]) < 1e-9
        result["common_clock_translation_check"] = "1ns shared prefix shift cancels from pulses and read windows"
    result["evidence_sha256"] = hashlib.sha256(args.evidence.read_bytes()).hexdigest()
    result["audit_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    for rate, data in result["rates"].items():
        pulse = min(min(c["pulse_lower_bounds_ns"].values()) for c in data["corners"].values())
        print(rate, "MHz: minimum reference pulse", round(pulse, 3), "ns; common capture window", data["common_absolute_capture_window_ns"])
