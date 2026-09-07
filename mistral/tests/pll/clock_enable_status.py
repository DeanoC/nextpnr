#!/usr/bin/env python3
"""Host-only clock-enable status output routing; no characterized status clock-to-Q."""
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
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sdc = out / "clocks.sdc"
    sdc.write_text("create_clock -name FPGA_CLK1_50 -period 20 [get_ports {FPGA_CLK1_50}]\n")
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7", "--qsf", str(fixture / "diagnostic.qsf"),
               "--sdc", str(sdc), "--freq", "50", "--compress-rbf"]

    def synth(source, case):
        case.mkdir(parents=True, exist_ok=True)
        (case / "top.v").write_text(source)
        run([str(args.yosys.resolve()), "-p", f'read_verilog "{case / "top.v"}"; '
             f'synth_intel_alm -nobram -nolutram -nodsp -top top; write_json "{case / "synth.json"}"'], case / "synth.log")
        return json.loads((case / "synth.json").read_text())

    def check_status_data(top, status, sample):
        drivers = {bit: cell for cell in top["cells"].values()
                   for port, bits in cell["connections"].items() if cell["port_directions"][port] == "output"
                   for bit in bits if isinstance(bit, int)}
        pending = list(sample["connections"]["DATAIN"])
        visited = set()
        while pending:
            bit = pending.pop()
            if bit in visited:
                continue
            visited.add(bit)
            if bit == status[0]:
                break
            driver = drivers.get(bit)
            if driver and driver["type"] not in ("MISTRAL_FF", "MISTRAL_CLKENA"):
                pending.extend(bit for port, bits in driver["connections"].items()
                               if driver["port_directions"][port] == "input" for bit in bits)
        assert status[0] in visited, "ENAOUT does not reach sampling FF DATAIN"

    def route(design, case, power, branches=1):
        case.mkdir(parents=True, exist_ok=True)
        (case / "synth.json").write_text(json.dumps(design))
        run(command + ["--json", str(case / "synth.json"), "--rbf", str(case / "top.rbf"),
                       "--report", str(case / "timing.json"), "--write", str(case / "routed.json")], case / "route.log")
        report = json.loads((case / "timing.json").read_text())
        util = report["utilization"]
        assert util["altera_pll"]["used"] == util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
        assert util["MISTRAL_CLKENA"]["used"] == branches + 1
        for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
            assert util.get(kind, {"used": 0})["used"] == 0
        assert report["fmax"]["gated_clock"]["constraint"] == 25
        assert report["fmax"]["gated_clock"]["achieved"] >= 25
        assert "gate_status" not in report["fmax"]
        top = json.loads((case / "routed.json").read_text())["modules"]["top"]
        gate = top["cells"]["gate"]
        assert gate["type"] == "MISTRAL_CLKENA"
        assert gate["attributes"]["NEXTPNR_BEL"] == "MISTRAL_CLKENA.0.35.0"
        status = gate["connections"]["ENAOUT"]
        assert gate["port_directions"]["ENAOUT"] == "output"
        assert status != gate["connections"]["Q"]
        consumers = [(cell, port) for cell in top["cells"].values() for port, bits in cell["connections"].items()
                     if bits == status and cell["port_directions"][port] == "input"]
        assert consumers and all(port != "CLK" for _, port in consumers), consumers
        # Packing may insert a route-through LUT before the ordinary fabric FF.
        sample = [cell for name, cell in top["cells"].items() if name.startswith("sampled_status_MISTRAL_FF") and cell["type"] == "MISTRAL_FF"]
        assert len(sample) == 1
        check_status_data(top, status, sample[0])
        reference_buffers = [cell for cell in top["cells"].values() if cell["type"] == "MISTRAL_CLKBUF"]
        assert len(reference_buffers) == 1
        assert sample[0]["connections"]["CLK"] == reference_buffers[0]["connections"]["Q"]
        for net in top["netnames"].values():
            if net["bits"] == status:
                assert not any("clock" in key.lower() for key in net.get("attributes", {})), net
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(case / "top.rbf"), str(case / "top.bt")], case / "decomp.log")
        bt = (case / "top.bt").read_text()
        assert re.search(r"^r CMUXHG\.000\.035\.2:SYN_EN \S+$", bt, re.M)
        assert re.search(r"^r \S+ CMUXHG\.000\.035\.2:ENABLE$", bt, re.M)
        settings = hg(bt)
        assert settings["ENABLE_REGISTER_MODE.2"] == "REG1_ENOUT"
        assert settings.get("ENABLE_REGISTER_POWER_UP.2", "1") == ("0" if power == "low" else "1")
        assert settings["INPUT_SEL.2"] == "16"
        assert settings["TESTSYN_ENOUT_SELECT.2"] == "PRE_SYNENB"
        return report, bt

    def hg(text):
        return dict(re.findall(r"^s CMUXHG\.000\.035:(\S+) (\S+)$", text, re.M))

    def oracle(case, reference, bt):
        hashes = json.loads((reference / "sha256.json").read_text())
        compressed = (reference / "top.rbf.gz").read_bytes()
        assert hashlib.sha256(compressed).hexdigest() == hashes["rbf.gz"]
        raw = gzip.decompress(compressed)
        assert hashlib.sha256(raw).hexdigest() == hashes["rbf"]
        (case / "quartus.rbf").write_bytes(raw)
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(case / "quartus.rbf"), str(case / "quartus.bt")], case / "oracle.log")
        oracle_bt = (case / "quartus.bt").read_text()
        assert fpll_settings(bt) == fpll_settings(oracle_bt)
        assert hg(bt) and hg(bt) == hg(oracle_bt)
        assert re.search(r"^r CMUXHG\.000\.035\.2:SYN_EN \S+$", oracle_bt, re.M)

    for power in ("low", "high"):
        case = out / power
        source = (fixture / "clock_enable_status.v").read_text().replace('.ena_register_power_up("low")', f'.ena_register_power_up("{power}")')
        design = synth(source, case)
        report, bt = route(design, case, power)
        reference = fixture / "fixtures" / "clock-enable-status"
        oracle(case, reference if power == "low" else reference / "high", bt)
        raw = copy.deepcopy(design)
        top = raw["modules"]["top"]
        gate = top["cells"]["gate"]
        old_bit = gate["connections"]["outclk"][0]
        name, buffer = next((n, c) for n, c in top["cells"].items()
                            if c["type"] == "MISTRAL_CLKBUF" and c["connections"]["A"] == [old_bit])
        gate["type"] = "MISTRAL_CLKENA"
        gate["parameters"] = {"ena_register_power_up": power}
        gate["connections"] = {"A": gate["connections"]["inclk"], "ENA": gate["connections"]["ena"],
                               "Q": buffer["connections"]["Q"], "ENAOUT": gate["connections"]["enaout"]}
        gate["port_directions"] = {"A": "input", "ENA": "input", "Q": "output", "ENAOUT": "output"}
        del top["cells"][name]
        for name in list(top["netnames"]):
            if top["netnames"][name]["bits"] == [old_bit]:
                del top["netnames"][name]
        route(raw, case / "raw", power)
        print(power, "PASS: native/raw status routing and full Quartus oracle", report["fmax"])
        print(power, "RBF sha256", hashlib.sha256((case / "top.rbf").read_bytes()).hexdigest())


    source = (fixture / "clock_enable_status.v").read_text()
    source = source.replace("    wire [31:0] gp_out;", """    wire [31:0] gp_out;
    wire second_clock, second_status;
    reg [7:0] second_count = 0;
    reg sampled_second = 0;
    always @(posedge second_clock) second_count <= second_count + 1'b1;
    always @(posedge FPGA_CLK1_50) sampled_second <= second_status;
    cyclonev_clkena #(.clock_type("Global Clock"), .ena_register_mode("falling edge"), .ena_register_power_up("high"))
        gate_second (.inclk(pll_clock), .ena(gp_out[1]), .outclk(second_clock), .enaout(second_status));""")
    source = source.replace("22'b0, sampled_status, locked, count", "13'b0, sampled_second, second_count, sampled_status, locked, count")
    case = out / "branches"
    design = synth(source, case)
    report, bt = route(design, case, "low", branches=2)
    assert report["fmax"]["second_clock"]["constraint"] == 25
    assert report["fmax"]["second_clock"]["achieved"] >= 25
    top = json.loads((case / "routed.json").read_text())["modules"]["top"]
    gates = [top["cells"][name] for name in ("gate", "gate_second")]
    assert gates[1]["attributes"]["NEXTPNR_BEL"] == "MISTRAL_CLKENA.0.35.1"
    for port in ("ENA", "ENAOUT", "Q"):
        assert gates[0]["connections"][port] != gates[1]["connections"][port]
    assert gates[0]["connections"]["A"] == gates[1]["connections"]["A"]
    settings = hg(bt)
    assert settings["INPUT_SEL.3"] == "16"
    assert settings["ENABLE_REGISTER_MODE.3"] == "REG1_ENOUT"
    assert settings.get("ENABLE_REGISTER_POWER_UP.3", "1") == "1"
    assert re.search(r"^r CMUXHG\.000\.035\.3:SYN_EN \S+$", bt, re.M)
    assert re.search(r"^r \S+ CMUXHG\.000\.035\.3:ENABLE$", bt, re.M)
    status = gates[1]["connections"]["ENAOUT"]
    consumers = [(cell, port) for cell in top["cells"].values() for port, bits in cell["connections"].items()
                 if bits == status and cell["port_directions"][port] == "input"]
    assert consumers and all(port != "CLK" for _, port in consumers)
    sampled = [cell for name, cell in top["cells"].items()
               if name.startswith("sampled_second_MISTRAL_FF") and cell["type"] == "MISTRAL_FF"]
    assert len(sampled) == 1
    check_status_data(top, status, sampled[0])
    reference = next(cell for cell in top["cells"].values() if cell["type"] == "MISTRAL_CLKBUF")
    assert sampled[0]["connections"]["CLK"] == reference["connections"]["Q"]
    assert "second_status" not in report["fmax"]
    print("branches PASS: independent low/high status outputs and physical routes", report["fmax"])


if __name__ == "__main__":
    main()
