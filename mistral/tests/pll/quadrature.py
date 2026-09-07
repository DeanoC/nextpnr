#!/usr/bin/env python3
"""Check fixed quadrature PLL settings and every ordered rising/falling clock crossing."""
import argparse
import copy
import gzip
import hashlib
import json
from pathlib import Path
import re

from check import run


def fpll_settings(text):
    """Compare every emitted setting at every FPLL, including auxiliary blocks."""
    return dict(re.findall(r"^s (FPLL\.\S+) (.+)$", text, re.M))


def crossing_source(phases, group=None, reference_mhz=50, output_mhz=25):
    """Use only live launch/capture registers so isolated routes stay compact."""
    count = len(phases)
    pairs = [(source, target, edge) for source in range(count)
             for target in range(count) if source != target
             for edge in ("posedge", "negedge")]
    selected = [(i, pair) for i, pair in enumerate(pairs)
                if group is None or i % (2 * (count - 1)) == group]
    lines = ["module top(input wire FPGA_CLK1_50);",
             f"wire [{count - 1}:0] clocks; wire locked; wire [31:0] gpo;"]
    for i, (source, target, edge) in selected:
        lines += [f"reg launch{i} = 0, capture{i} = 0;",
                  f"always @(posedge clocks[{source}]) launch{i} <= gpo[{i}];",
                  f"always @({edge} clocks[{target}]) capture{i} <= launch{i};"]
    parameters = [f'.reference_clock_frequency("{reference_mhz}.0 MHz")',
                  f'.number_of_clocks({count})', '.operation_mode("direct")',
                  '.fractional_vco_multiplier("false")']
    for i, phase in enumerate(phases):
        parameters += [f'.output_clock_frequency{i}("{output_mhz}.0 MHz")',
                       f'.phase_shift{i}("{phase * 1000:g} ps")', f'.duty_cycle{i}(50)']
    lines += ["altera_pll #(" + ", ".join(parameters) +
              ") pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(clocks), .locked(locked));"]
    captures = ", ".join(f"capture{i}" for i, _ in reversed(selected))
    lines += ["cyclonev_hps_interface_mpu_general_purpose hps_gp (.gp_in({" +
              f"{31 - len(selected)}'b0, locked, " + captures + "}), .gp_out(gpo));", "endmodule"]
    return "\n".join(lines) + "\n"


def main(phases=(0, 10, 20, 30), profile=None, reference_mhz=50, output_mhz=25, oracle_fixture=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    count = len(phases)
    period_ns = 1000 / output_mhz
    out = args.output.resolve() / profile if profile else args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source_file = out / "top.v"
    source_file.write_text(crossing_source(phases, reference_mhz=reference_mhz, output_mhz=output_mhz))
    run([str(args.yosys.resolve()), "-p",
         f'read_verilog "{source_file}"; '
         'synth_intel_alm -nobram -nolutram -nodsp -top top; '
         f'write_json "{out / "synth.json"}"'], out / "yosys.log")
    design = json.loads((out / "synth.json").read_text())
    cells = design["modules"]["top"]["cells"]
    assert sum(c["type"] == "altera_pll" for c in cells.values()) == 1
    assert sum(c["type"] == "cyclonev_hps_interface_mpu_general_purpose" for c in cells.values()) == 1
    sdc = out / "clocks.sdc"
    sdc.write_text(f"create_clock -name FPGA_CLK1_50 -period {1000 / reference_mhz:g} [get_ports {{FPGA_CLK1_50}}]\n")
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(sdc),
               "--freq", str(reference_mhz), "--compress-rbf"]
    run(command + ["--json", str(out / "synth.json"), "--rbf", str(out / "top.rbf"),
                   "--report", str(out / "timing.json"), "--write", str(out / "routed.json")], out / "route.log")
    report = json.loads((out / "timing.json").read_text())
    util = report["utilization"]
    assert util["altera_pll"] == {"used": 1, "available": 6}
    assert util["MISTRAL_CLKENA"]["used"] == count
    assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
        assert util.get(kind, {"used": 0})["used"] == 0
    def check_path(path):
        source = int(re.search(r"clocks\[(\d)\]", path["from"])[1])
        target = int(re.search(r"clocks\[(\d)\]", path["to"])[1])
        offset = period_ns / 2 if path["to"].startswith("negedge") else 0
        budget = (phases[target] - phases[source] + offset) % period_ns or period_ns
        assert abs(path["max_delay"] - budget) < 0.002, path
        delay = sum(segment["delay"] for segment in path["path"])
        assert 0 < delay < budget, path
        return source, 1000 * (budget / period_ns) / delay

    # Reports retain only the worst destination per source. Verify the combined
    # design's aggregate Fmax against that path, then isolate compact groups so
    # every ordered pair and both capture edges become visible in a report.
    assert len(report["critical_paths"]) == count
    for path in report["critical_paths"]:
        source, expected = check_path(path)
        clock = report["fmax"][f"clocks[{source}]"]
        assert abs(clock["constraint"] - output_mhz) < 0.001 and clock["achieved"] >= output_mhz, clock
        assert abs(clock["achieved"] - expected) < expected * 0.0001, (clock, expected)
    observed = set()
    for group in range(2 * (count - 1)):
        case = out / f"crossings{group}"
        case.mkdir(exist_ok=True)
        source_file = case / "top.v"
        source_file.write_text(crossing_source(phases, group, reference_mhz, output_mhz))
        run([str(args.yosys.resolve()), "-p", f'read_verilog "{source_file}"; '
             'synth_intel_alm -nobram -nolutram -nodsp -top top; '
             f'write_json "{case / "synth.json"}"'], case / "yosys.log")
        run(command + ["--json", str(case / "synth.json"),
                       "--report", str(case / "timing.json")], case / "route.log")
        group_report = json.loads((case / "timing.json").read_text())
        assert len(group_report["critical_paths"]) == count
        for path in group_report["critical_paths"]:
            source, expected = check_path(path)
            observed.add((path["from"], path["to"]))
            clock = group_report["fmax"][f"clocks[{source}]"]
            assert abs(clock["constraint"] - output_mhz) < 0.001 and clock["achieved"] >= output_mhz, clock
            assert abs(clock["achieved"] - expected) < expected * 0.0001, (clock, expected)
    expected_crossings = {(f"posedge clocks[{source}]", f"{edge} clocks[{target}]")
                          for source in range(count) for target in range(count) if source != target
                          for edge in ("posedge", "negedge")}
    assert observed == expected_crossings, (observed, expected_crossings)
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "top.rbf"), str(out / "top.bt")], out / "decomp.log")
    bt = (out / "top.bt").read_text()
    assert len(re.findall(r"^s FPLL.*:FPLL_ENABLE 1$", bt, re.M)) == 1
    settings = fpll_settings(bt)
    assert settings
    for lane, select in list(enumerate(("14", "17", "16", "15")))[4 - count:]:
        assert f"s CMUXHG.000.035:INPUT_SEL.{lane} {select}" in bt.splitlines()
        assert f"s CMUXHG.000.035:TESTSYN_ENOUT_SELECT.{lane} PRE_SYNENB" in bt.splitlines()
    routed = json.loads((out / "routed.json").read_text())["modules"]["top"]["cells"]
    # BEL creation preserves lane2/lane3/lane1 indices, then appends lane0.
    for index, bel in enumerate(("MISTRAL_CLKENA.0.35.0", "MISTRAL_CLKENA.0.35.1", "MISTRAL_CLKENA.0.35.2",
                                 "MISTRAL_CLKENA.0.35.3")[:count]):
        net = [cells["pll"]["connections"]["outclk"][index]]
        name = next(name for name, cell in cells.items()
                    if cell["type"] == "MISTRAL_CLKBUF" and cell["connections"]["A"] == net)
        assert routed[name]["attributes"]["NEXTPNR_BEL"] == bel, routed[name]
    reference = Path(oracle_fixture) if oracle_fixture else (fixture / "fixtures" / "phase-select" / profile if profile else fixture / "fixtures" / "quadrature")
    hashes = json.loads((reference / "sha256.json").read_text())
    compressed = (reference / "top.rbf.gz").read_bytes()
    assert hashlib.sha256(compressed).hexdigest() == hashes["rbf.gz"]
    raw = gzip.decompress(compressed)
    assert hashlib.sha256(raw).hexdigest() == hashes["rbf"]
    (out / "quartus.rbf").write_bytes(raw)
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "quartus.rbf"), str(out / "quartus.bt")], out / "quartus-decomp.log")
    for oracle_path in [out / "quartus.bt"]:
        oracle = fpll_settings(oracle_path.read_text())
        assert oracle, "oracle contains no FPLL settings"
        differences = {key: (oracle.get(key), settings.get(key))
                       for key in oracle.keys() | settings.keys() if oracle.get(key) != settings.get(key)}
        assert not differences, f"FPLL oracle mismatch (oracle, actual): {differences}"

    def reject(name, invalid, reasons, custom_command=None):
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run((custom_command or command) + ["--json", str(path)],
                  out / f"invalid-{name}.log", success=False)
        assert "ERROR" in log and any(reason in log for reason in reasons), log

    invalid_phases = (100, int(period_ns * 250) + 1, -int(period_ns * 250), int(period_ns * 1000))
    if output_mhz == 50:
        invalid_phases += (2501,)
    for name, changes in (
        ("phase0", {"phase_shift0": "10000 ps"}),
        *[(f"phase{i}-{value}", {f"phase_shift{i}": f"{value} ps"})
          for i in range(1, count) for value in invalid_phases],
        ("reference", {"reference_clock_frequency": "26 MHz"}),
        ("fractional", {"fractional_vco_multiplier": "true"}),
        *[(f"duty{i}", {f"duty_cycle{i}": format(25, "032b")}) for i in range(count)],
        *[(f"frequency{i}", {f"output_clock_frequency{i}": f"{50 if output_mhz == 25 else 25} MHz"}) for i in range(count)],
    ):
        invalid = copy.deepcopy(design)
        invalid["modules"]["top"]["cells"]["pll"]["parameters"].update(changes)
        reject(name, invalid, ("phase", "quadrature", "reference frequency"))
    if output_mhz in (50, 100):
        for unsupported_reference in (25, 100):
            invalid = copy.deepcopy(design)
            invalid["modules"]["top"]["cells"]["pll"]["parameters"]["reference_clock_frequency"] = f"{unsupported_reference} MHz"
            reject(f"shifted{output_mhz}-reference{unsupported_reference}", invalid, ("phase", "reference"))
    conflict_reference = out / "conflict-reference.sdc"
    conflict_reference.write_text(f"create_clock -period {2000 / reference_mhz:g} [get_ports {{FPGA_CLK1_50}}]\n")
    custom = command.copy()
    custom[custom.index("--sdc") + 1] = str(conflict_reference)
    reject("reference-constraint", design, ("reference", "constraint"), custom)
    for index in range(count):
        conflict_sdc = out / f"conflict{index}.sdc"
        conflict_sdc.write_text(sdc.read_text() +
                                f"create_clock -period {period_ns:g} [get_nets {{clocks[{index}]}}]\n")
        custom = command.copy()
        custom[custom.index("--sdc") + 1] = str(conflict_sdc)
        if phases[index]:
            reject(f"constraint{index}", design,
                   ("shifted output must use the PLL-derived phase constraint",), custom)
        else:
            aligned_report = out / f"aligned{index}.json"
            run(custom + ["--json", str(out / "synth.json"),
                          "--report", str(aligned_report)], out / f"aligned{index}.log")
            aligned = json.loads(aligned_report.read_text())
            assert len(aligned["critical_paths"]) == count
            for path in aligned["critical_paths"]:
                check_path(path)
    module = design["modules"]["top"]
    inverted = {tuple(c["connections"]["Q"]) for c in cells.values()
                if c["type"] == "MISTRAL_NOT"}
    buffers = [c for c in cells.values() if c["type"] == "MISTRAL_CLKBUF"
               and tuple(c["connections"]["A"]) in inverted]
    assert len(buffers) == count, buffers
    for index, buffer in enumerate(buffers):
        names = [name for name, net in module["netnames"].items()
                 if net["bits"] == buffer["connections"]["Q"]]
        assert names
        conflict_sdc = out / f"inverse{index}.sdc"
        conflict_sdc.write_text(sdc.read_text() +
                                f"create_clock -period {period_ns:g} [get_nets {{" + names[0] + "}]\n")
        custom = command.copy()
        custom[custom.index("--sdc") + 1] = str(conflict_sdc)
        reject(f"inverse{index}", design,
               ("explicit clock constraint cannot describe the PLL phase",), custom)
    print(f"PASS: {profile or 'quadrature'} full FPLL oracle, {count} global clocks, "
          f"{len(expected_crossings)} phase crossings and rejection checks")
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
