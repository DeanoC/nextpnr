#!/usr/bin/env python3
"""Host-only PLL clock-enable routing, bitstream oracle and rejection checks."""
import argparse
import copy
import gzip
import hashlib
import json
from pathlib import Path
import re

from check import run
from triple import fpll_settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--power-up", choices=("high", "low"), default="high")
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    def synth(source, target):
        run([str(args.yosys.resolve()), "-p",
             f'read_verilog "{source}"; synth_intel_alm -nobram -nolutram -nodsp -top top; '
             f'write_json "{target}"'], target.with_suffix(".log"))
        return json.loads(target.read_text())

    source_text = (fixture / "clock_enable.v").read_text()
    source = fixture / "clock_enable.v"
    if args.power_up == "low":
        source_text = source_text.replace('.ena_register_mode("falling edge")',
                                          '.ena_register_mode("falling edge"), .ena_register_power_up("low")')
        source = out / "low.v"
        source.write_text(source_text)
    design = synth(source, out / "synth.json")
    assert design["modules"]["top"]["cells"]["gate"]["type"] == "cyclonev_clkena"
    sdc = out / "clocks.sdc"
    sdc.write_text("create_clock -name FPGA_CLK1_50 -period 20 [get_ports {FPGA_CLK1_50}]\n")
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(sdc),
               "--freq", "50", "--compress-rbf"]

    def route(case, netlist, clock_sdc=None):
        case.mkdir(parents=True, exist_ok=True)
        path = case / "synth.json"
        path.write_text(json.dumps(netlist))
        route_command = command.copy()
        if clock_sdc is not None:
            route_command[route_command.index("--sdc") + 1] = str(clock_sdc)
        run(route_command + ["--json", str(path), "--rbf", str(case / "top.rbf"),
                       "--report", str(case / "timing.json"), "--write", str(case / "routed.json")],
            case / "route.log")
        report = json.loads((case / "timing.json").read_text())
        util = report["utilization"]
        for kind in ("altera_pll", "MISTRAL_CLKENA", "cyclonev_hps_interface_mpu_general_purpose"):
            assert util[kind]["used"] == 1, util
        for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
            assert util.get(kind, {"used": 0})["used"] == 0
        clock = report["fmax"]["gated_clock"]
        assert clock["constraint"] == 25 and clock["achieved"] >= 25, clock
        routed = json.loads((case / "routed.json").read_text())["modules"]["top"]
        gates = [c for c in routed["cells"].values() if c["type"] == "MISTRAL_CLKENA"]
        assert len(gates) == 1, gates
        assert gates[0]["attributes"]["NEXTPNR_BEL"] == "MISTRAL_CLKENA.0.35.0"
        assert len(gates[0]["connections"]["ENA"]) == 1
        assert gates[0]["connections"]["ENA"][0] not in ("0", "1", "x", "z")
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
             str(case / "top.rbf"), str(case / "top.bt")], case / "decomp.log")
        bt = (case / "top.bt").read_text()
        for line in ("s CMUXHG.000.035:INPUT_SEL.2 16",
                     "s CMUXHG.000.035:ENABLE_REGISTER_MODE.2 REG1_ENOUT",
                     "s CMUXHG.000.035:TESTSYN_ENOUT_SELECT.2 PRE_SYNENB"):
            assert line in bt.splitlines(), line
        power = dict(re.findall(r"^s CMUXHG\.000\.035:(\S+) (\S+)$", bt, re.M))
        # The default high setting is omitted by the decompiler.
        assert power.get("ENABLE_REGISTER_POWER_UP.2", "1") == ("0" if args.power_up == "low" else "1"), power
        assert re.search(r"^r \S+ CMUXHG\.000\.035\.2:ENABLE$", bt, re.M), "missing routed enable"
        return report, bt

    report, bt = route(out, design)
    reference = fixture / "fixtures" / ("clock-enable-low" if args.power_up == "low" else "clock-enable")
    hashes = json.loads((reference / "sha256.json").read_text())
    compressed = (reference / "top.rbf.gz").read_bytes()
    assert hashlib.sha256(compressed).hexdigest() == hashes["rbf.gz"]
    raw = gzip.decompress(compressed)
    assert hashlib.sha256(raw).hexdigest() == hashes["rbf"]
    (out / "quartus.rbf").write_bytes(raw)
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "quartus.rbf"), str(out / "quartus.bt")], out / "quartus-decomp.log")
    oracle = (out / "quartus.bt").read_text()
    assert fpll_settings(oracle) == fpll_settings(bt), "FPLL settings differ from Quartus"
    def cmux_settings(text):
        return dict(re.findall(r"^s CMUXHG\.000\.035:(\S+) (\S+)$", text, re.M))
    assert cmux_settings(oracle) and cmux_settings(oracle) == cmux_settings(bt), "CMUXHG settings differ from Quartus"

    inverted_source = out / "inverted.v"
    inverted_source.write_text(source_text.replace(".ena(gp_out[0])", ".ena(~gp_out[0])"))
    inverted = synth(inverted_source, out / "inverted.json")
    route(out / "inverted", inverted)

    def reject(name, mutate, expected, base=None):
        invalid = copy.deepcopy(design if base is None else base)
        mutate(invalid["modules"]["top"])
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(path)], out / f"invalid-{name}.log", success=False)
        assert expected in log, log

    for key, value in (("ena_register_mode", "none"), ("ena_register_power_up", "invalid"),
                       ("disable_mode", "high"), ("test_syn", "low"), ("clock_type", "Regional Clock")):
        reject(key, lambda top, k=key, v=value: top["cells"]["gate"]["parameters"].update({k: v}), "only global clocks" if key == "clock_type" else key)
    reject("missing-ena", lambda top: top["cells"]["gate"]["connections"].pop("ena"), "ena")
    reject("undriven-ena", lambda top: top["cells"]["gate"]["connections"].update({"ena": [1000000]}), "ena")

    def observe_unknown_status(top):
        gate = top["cells"]["gate"]
        gate["connections"]["unknown_status"] = [1000000]
        gate["port_directions"]["unknown_status"] = "output"
        top["cells"]["hps_gp"]["connections"]["gp_in"][31] = 1000000
    reject("unknown-status", observe_unknown_status, "unknown_status")
    reject("non-pll-source", lambda top: top["cells"]["gate"]["connections"].update(
        {"inclk": top["cells"]["hps_gp"]["connections"]["gp_out"][:1]}), "PLL")
    reject("mode-omitted", lambda top: top["cells"]["gate"]["parameters"].pop("ena_register_mode"), "ena_register_mode")
    reject("unknown-parameter", lambda top: top["cells"]["gate"]["parameters"].update({"bogus_mode": "yes"}), "bogus_mode")

    def output_buffer(top):
        bit = top["cells"]["gate"]["connections"]["outclk"]
        return next((name, cell) for name, cell in top["cells"].items()
                    if cell["type"] == "MISTRAL_CLKBUF" and cell["connections"]["A"] == bit)
    def raw_gate(netlist):
        raw = copy.deepcopy(netlist)
        top = raw["modules"]["top"]
        name, buffer = output_buffer(top)
        gate = top["cells"]["gate"]
        old_bit = gate["connections"]["outclk"][0]
        new_bit = buffer["connections"]["Q"][0]
        gate["type"] = "MISTRAL_CLKENA"
        gate["parameters"] = {key: value for key, value in gate["parameters"].items()
                              if key == "ena_register_power_up"}
        gate["connections"] = {"A": gate["connections"]["inclk"],
                               "ENA": gate["connections"]["ena"], "Q": [new_bit]}
        gate["port_directions"] = {"A": "input", "ENA": "input", "Q": "output"}
        del top["cells"][name]
        # Drop the eliminated pre-buffer alias so the clock keeps its public name.
        for name in list(top["netnames"]):
            if top["netnames"][name]["bits"] == [old_bit]:
                del top["netnames"][name]
        return raw
    raw = raw_gate(design)
    route(out / "raw", raw)
    reject("raw-mode", lambda top: top["cells"]["gate"]["parameters"].update(
        {"ena_register_mode": "always enabled"}), "no mode overrides", raw)
    for name, netlist in (("native", design), ("raw", raw)):
        reject(name + "-power-up-non-string", lambda top: top["cells"]["gate"]["parameters"].update(
            {"ena_register_power_up": "00000000000000000000000000000001"}), "ena_register_power_up", netlist)
    reject("raw-power-up-invalid", lambda top: top["cells"]["gate"]["parameters"].update(
        {"ena_register_power_up": "invalid"}), "ena_register_power_up", raw)

    if args.power_up == "high":
        for name, netlist in (("native", design), ("raw", raw)):
            explicit = copy.deepcopy(netlist)
            explicit["modules"]["top"]["cells"]["gate"]["parameters"]["ena_register_power_up"] = "high"
            _, explicit_bt = route(out / (name + "-explicit-high"), explicit)
            default_bt = bt if name == "native" else (out / "raw" / "top.bt").read_text()
            assert cmux_settings(explicit_bt) == cmux_settings(default_bt), "explicit high differs from default"
    def raw_extra(top):
        top["cells"]["gate"]["connections"]["TEST"] = top["cells"]["gate"]["connections"]["ENA"]
        top["cells"]["gate"]["port_directions"]["TEST"] = "input"
    reject("raw-extra-port", raw_extra, "unsupported port", raw)

    # Keep the real PLL clock observed: otherwise an unrelated unused-output check
    # could mask accepting LOCKED as if it were a dedicated clock output.
    locked_source = source_text.replace(
        ".inclk(pll_clock)", ".inclk(locked)").replace(
        "    reg [7:0] count = 0;", "    reg observed = 0;\n"
        "    always @(posedge pll_clock) observed <= !observed;\n    reg [7:0] count = 0;").replace(
        "23'b0, locked, count", "22'b0, observed, locked, count")
    locked_path = out / "locked-source.v"
    locked_path.write_text(locked_source)
    locked_design = synth(locked_path, out / "locked-source.json")
    reject("locked-source", lambda top: None, "PLL clock output", locked_design)
    reject("raw-locked-source", lambda top: None, "PLL clock output", raw_gate(locked_design))
    reject("buffer-placement", lambda top: output_buffer(top)[1]["attributes"].update(
        {"BEL": "MISTRAL_CLKENA.0.35.0"}), "output buffer placement constraint")

    def direct_fanout(top):
        name, buffer = output_buffer(top)
        old_bit = buffer["connections"]["Q"][0]
        new_bit = top["cells"]["gate"]["connections"]["outclk"][0]
        del top["cells"][name]
        for cell in top["cells"].values():
            for port, bits in cell["connections"].items():
                cell["connections"][port] = [new_bit if bit == old_bit else bit for bit in bits]
        for net in top["netnames"].values():
            net["bits"] = [new_bit if bit == old_bit else bit for bit in net["bits"]]
    reject("direct-fanout", direct_fanout, "output must feed exactly one unconditional clock buffer")

    # Give the pre-fold buffer input a stable public name for SDC lookup.
    named = copy.deepcopy(design)
    named["modules"]["top"]["netnames"]["before_buffer"] = {
        "hide_name": 0, "bits": named["modules"]["top"]["cells"]["gate"]["connections"]["outclk"], "attributes": {}}
    for label, constraints in (("same-output", (("gated_clock", 40),)),
                               ("same-before", (("before_buffer", 40),)),
                               ("same-both", (("before_buffer", 40), ("gated_clock", 40)))):
        matching = out / f"{label}.sdc"
        matching.write_text(sdc.read_text() + "".join(
            f"create_clock -name {net} -period {period} [get_nets {{{net}}}]\n" for net, period in constraints))
        route(out / label, named, matching)
    named_path = out / "named.json"
    named_path.write_text(json.dumps(named))
    for label, constraints in (("before-frequency", (("before_buffer", 20),)),
                               ("fold-conflict", (("before_buffer", 20), ("gated_clock", 40)))):
        invalid_sdc = out / f"{label}.sdc"
        invalid_sdc.write_text(sdc.read_text() + "".join(
            f"create_clock -name {net} -period {period} [get_nets {{{net}}}]\n" for net, period in constraints))
        invalid_command = command.copy()
        invalid_command[invalid_command.index("--sdc") + 1] = str(invalid_sdc)
        log = run(invalid_command + ["--json", str(named_path)], out / f"invalid-{label}.log", success=False)
        assert "conflicting clock constraint" in log, log
    conflicting = out / "conflicting.sdc"
    conflicting.write_text(sdc.read_text() + "create_clock -name gated_clock -period 20 [get_nets {gated_clock}]\n")
    invalid_command = command.copy()
    invalid_command[invalid_command.index("--sdc") + 1] = str(conflicting)
    log = run(invalid_command + ["--json", str(out / "synth.json")], out / "invalid-output-clock.log", success=False)
    assert "conflicting clock constraint" in log, log
    print("PASS: PLL clock enable, inverted control, Quartus oracle and rejection checks", report["fmax"])
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
