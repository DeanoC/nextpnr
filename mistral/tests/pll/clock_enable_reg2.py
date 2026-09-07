#!/usr/bin/env python3
"""Exercise double-register PLL clock-enable packing and bit generation."""
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
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(sdc),
               "--freq", "50", "--compress-rbf"]

    def synth(source_text, case):
        case.mkdir(parents=True, exist_ok=True)
        source = case / "top.v"
        source.write_text(source_text)
        netlist = case / "synth.json"
        run([str(args.yosys.resolve()), "-p",
             f'read_verilog "{source}"; synth_intel_alm -nobram -nolutram -nodsp -top top; '
             f'write_json "{netlist}"'], case / "yosys.log")
        design = json.loads(netlist.read_text())
        assert design["modules"]["top"]["cells"]["gate"]["parameters"]["ena_register_mode"] == "double register"
        return design

    def hg(text):
        return dict(re.findall(r"^s CMUXHG\.000\.035:(\S+) (\S+)$", text, re.M))

    def check_status_data(top, status, sample):
        drivers = {bit: cell for cell in top["cells"].values()
                   for port, bits in cell["connections"].items()
                   if cell["port_directions"][port] == "output"
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

    def route(design, case, power, mode="double register"):
        case.mkdir(parents=True, exist_ok=True)
        path = case / "synth.json"
        path.write_text(json.dumps(design))
        run(command + ["--json", str(path), "--rbf", str(case / "top.rbf"),
                       "--report", str(case / "timing.json"), "--write", str(case / "routed.json")],
            case / "route.log")
        report = json.loads((case / "timing.json").read_text())
        util = report["utilization"]
        assert util["altera_pll"]["used"] == 1
        # The sampled reference clock and the gated output each consume one
        # global clock-enable BEL; the double-register mode does not add a BEL.
        assert util["MISTRAL_CLKENA"]["used"] == 2
        assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
        for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
            assert util.get(kind, {"used": 0})["used"] == 0
        clock = report["fmax"]["gated_clock"]
        assert clock["constraint"] == 25 and clock["achieved"] >= 25, clock
        top = json.loads((case / "routed.json").read_text())["modules"]["top"]
        gate = top["cells"]["gate"]
        assert gate["type"] == "MISTRAL_CLKENA"
        assert gate["attributes"]["NEXTPNR_BEL"] == "MISTRAL_CLKENA.0.35.0"
        assert gate["connections"]["ENAOUT"] != gate["connections"]["Q"]
        status = gate["connections"]["ENAOUT"]
        consumers = [(cell, port) for cell in top["cells"].values()
                     for port, bits in cell["connections"].items()
                     if bits == status and cell["port_directions"][port] == "input"]
        assert consumers and all(port != "CLK" for _, port in consumers), consumers
        sample = [cell for name, cell in top["cells"].items()
                  if name.startswith("sampled_status_MISTRAL_FF") and cell["type"] == "MISTRAL_FF"]
        assert len(sample) == 1
        check_status_data(top, status, sample[0])
        reference = [cell for cell in top["cells"].values() if cell["type"] == "MISTRAL_CLKBUF"]
        assert len(reference) == 1
        assert sample[0]["connections"]["CLK"] == reference[0]["connections"]["Q"]
        for net in top["netnames"].values():
            if net["bits"] == status:
                assert not any("clock" in key.lower() for key in net.get("attributes", {})), net
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(case / "top.rbf"),
             str(case / "top.bt")], case / "decomp.log")
        bt = (case / "top.bt").read_text()
        expected_mode = "REG2_ENOUT" if mode == "double register" else "REG1_ENOUT"
        assert f"s CMUXHG.000.035:ENABLE_REGISTER_MODE.2 {expected_mode}" in bt
        assert re.search(r"^r CMUXHG\.000\.035\.2:SYN_EN \S+$", bt, re.M)
        assert re.search(r"^r \S+ CMUXHG\.000\.035\.2:ENABLE$", bt, re.M)
        settings = hg(bt)
        assert settings["ENABLE_REGISTER_MODE.2"] == expected_mode
        assert settings.get("ENABLE_REGISTER_POWER_UP.2", "1") == ("0" if power == "low" else "1")
        assert settings["INPUT_SEL.2"] == "16"
        assert settings["TESTSYN_ENOUT_SELECT.2"] == "PRE_SYNENB"
        return report, bt

    def oracle(case, reference, bt):
        hashes = json.loads((reference / "sha256.json").read_text())
        compressed = (reference / "top.rbf.gz").read_bytes()
        assert hashlib.sha256(compressed).hexdigest() == hashes["rbf.gz"]
        raw = gzip.decompress(compressed)
        assert hashlib.sha256(raw).hexdigest() == hashes["rbf"]
        quartus = case / "quartus.rbf"
        quartus.write_bytes(raw)
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(quartus),
             str(case / "quartus.bt")], case / "quartus-decomp.log")
        oracle_bt = (case / "quartus.bt").read_text()
        assert fpll_settings(oracle_bt) == fpll_settings(bt), "FPLL settings differ from Quartus"
        assert hg(oracle_bt) and hg(oracle_bt) == hg(bt), "CMUXHG settings differ from Quartus"

    def output_buffer(top):
        gate = top["cells"]["gate"]
        old_bit = gate["connections"]["outclk"][0]
        return next((name, cell) for name, cell in top["cells"].items()
                    if cell["type"] == "MISTRAL_CLKBUF" and cell["connections"]["A"] == [old_bit])

    def raw_gate(netlist, power, mode="double register"):
        raw = copy.deepcopy(netlist)
        top = raw["modules"]["top"]
        name, buffer = output_buffer(top)
        gate = top["cells"]["gate"]
        old_bit = gate["connections"]["outclk"][0]
        gate["type"] = "MISTRAL_CLKENA"
        gate["parameters"] = {"ena_register_mode": mode, "ena_register_power_up": power}
        gate["connections"] = {"A": gate["connections"]["inclk"], "ENA": gate["connections"]["ena"],
                                 "Q": buffer["connections"]["Q"], "ENAOUT": gate["connections"]["enaout"]}
        gate["port_directions"] = {"A": "input", "ENA": "input", "Q": "output", "ENAOUT": "output"}
        del top["cells"][name]
        for net in list(top["netnames"]):
            if top["netnames"][net]["bits"] == [old_bit]:
                del top["netnames"][net]
        return raw

    source = (fixture / "clock_enable_reg2.v").read_text()
    designs = {}
    for power in ("low", "high"):
        text = source.replace('.ena_register_power_up("low")', f'.ena_register_power_up("{power}")')
        case = out / power
        design = synth(text, case)
        designs[power] = design
        report, bt = route(design, case, power)
        reference = fixture / "fixtures" / "clock-enable-reg2"
        oracle(case, reference if power == "low" else reference / "high", bt)
        raw = raw_gate(design, power)
        _, raw_bt = route(raw, case / "raw", power)
        assert hg(raw_bt) == hg(bt), "raw packed cell differs from native cell"
        print(power, "PASS: native/raw double-register routing and Quartus oracle", report["fmax"])
        print(power, "RBF sha256", hashlib.sha256((case / "top.rbf").read_bytes()).hexdigest())

    def reject(name, mutate, expected, base):
        invalid = copy.deepcopy(base)
        mutate(invalid["modules"]["top"])
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(path)], out / f"invalid-{name}.log", success=False)
        assert expected in log, log

    low = designs["low"]
    reject("native-none", lambda top: top["cells"]["gate"]["parameters"].update({"ena_register_mode": "none"}),
           "ena_register_mode", low)
    reject("native-rising", lambda top: top["cells"]["gate"]["parameters"].update({"ena_register_mode": "rising edge"}),
           "ena_register_mode", low)
    reject("native-omitted", lambda top: top["cells"]["gate"]["parameters"].pop("ena_register_mode"),
           "ena_register_mode", low)
    raw = raw_gate(low, "low")
    reject("raw-always", lambda top: top["cells"]["gate"]["parameters"].update({"ena_register_mode": "always enabled"}),
           "no mode overrides", raw)
    reject("raw-rising", lambda top: top["cells"]["gate"]["parameters"].update({"ena_register_mode": "rising edge"}),
           "ena_register_mode", raw)
    reject("native-mode-non-string", lambda top: top["cells"]["gate"]["parameters"].update({"ena_register_mode": 1}),
           "parameter 'ena_register_mode' must be a string", low)
    falling = raw_gate(low, "low", "falling edge")
    _, falling_bt = route(falling, out / "raw-falling", "low", "falling edge")
    assert hg(falling_bt)["ENABLE_REGISTER_MODE.2"] == "REG1_ENOUT"
    print("PASS: double-register mode validation")


if __name__ == "__main__":
    main()
