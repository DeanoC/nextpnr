#!/usr/bin/env python3
"""Verify that constant-driven FES cart outputs route to the socket boundary."""

import argparse
import copy
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    yosys = str(args.yosys.resolve())
    nextpnr = str(args.nextpnr.resolve())

    shell_v = output / "shell.v"
    shell_v.write_text("""module top(input clk, input address, output [2:0] data);
    (* keep *) wire plug_addr;
    (* keep *) wire [2:0] plug_rdata_d = 3'b000;
    (* BEL = "MISTRAL_FF.24.1.2" *) MISTRAL_FF plug_addr_ff_0 (
        .CLK(clk), .DATAIN(address), .Q(plug_addr),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    (* BEL = "MISTRAL_FF.28.1.2" *) MISTRAL_FF plug_rdata_ff_0 (
        .CLK(clk), .DATAIN(plug_rdata_d[0]), .Q(data[0]),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    (* BEL = "MISTRAL_FF.28.1.8" *) MISTRAL_FF plug_rdata_ff_1 (
        .CLK(clk), .DATAIN(plug_rdata_d[1]), .Q(data[1]),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    (* BEL = "MISTRAL_FF.28.1.14" *) MISTRAL_FF plug_rdata_ff_2 (
        .CLK(clk), .DATAIN(plug_rdata_d[2]), .Q(data[2]),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
endmodule
""")

    cart_v = output / "cart.v"
    cart_v.write_text("""module cart(input FPGA_CLK1_50, input plug_addr, output [2:0] plug_rdata);
    reg q;
    always @(posedge FPGA_CLK1_50) q <= plug_addr;
    assign plug_rdata = {1'b0, 1'b1, q};
endmodule
""")

    qsf = output / "shell.qsf"
    qsf.write_text(
        "set_location_assignment PIN_V11 -to clk\n"
        "set_location_assignment PIN_W8 -to address\n"
        "set_location_assignment PIN_AD12 -to data[0]\n"
        "set_location_assignment PIN_AE12 -to data[1]\n"
        "set_location_assignment PIN_AF10 -to data[2]\n"
        'set_global_assignment -name FES_RESERVED_RECT "24 2 28 8"\n'
    )

    for name, top in (("shell", "top"), ("cart", "cart")):
        with (output / f"{name}-synth.log").open("w") as log:
            subprocess.run(
                [yosys, "-p",
                 f"read_verilog {output / (name + '.v')}; "
                 f"synth_intel_alm -nolutram -nodsp -top {top}; "
                 f"write_json {output / (name + '.json')}"],
                stdout=log, stderr=subprocess.STDOUT, check=True,
            )

    # Confirm Yosys emitted constant-input MISTRAL_OB pads for plug_rdata[1] and plug_rdata[2].
    cart_json = json.loads((output / "cart.json").read_text())
    cart_mod = cart_json["modules"]["cart"]
    ob_inputs = {}
    rdata_bits = cart_mod["ports"]["plug_rdata"]["bits"]
    for cell in cart_mod["cells"].values():
        if cell["type"] == "MISTRAL_OB":
            pad = cell["connections"]["PAD"][0]
            if pad in rdata_bits:
                ob_inputs[rdata_bits.index(pad)] = cell["connections"]["I"][0]
    assert ob_inputs[1] == "1" and ob_inputs[2] == "0", ob_inputs

    with (output / "shell-route.log").open("w") as log:
        subprocess.run(
            [nextpnr, "--device", "5CSEBA6U23I7", "--qsf", str(qsf),
             "--json", str(output / "shell.json"), "--router", "router2",
             "--write", str(output / "routed-shell.json"),
             "--rbf", str(output / "shell.rbf"), "--compress-rbf"],
            stdout=log, stderr=subprocess.STDOUT, check=True,
        )

    # Full scaffold cart merge + placement + routing.
    res = subprocess.run(
        [nextpnr, "--device", "5CSEBA6U23I7", "--qsf", str(qsf),
         "--json", str(output / "routed-shell.json"),
         "--fes-scaffold", "--fes-cart", str(output / "cart.json"),
         "--no-pack", "--router", "router2", "--seed", "1",
         "--write", str(output / "merged-routed.json"),
         "--rbf", str(output / "merged.rbf"), "--compress-rbf"],
        capture_output=True, text=True,
    )
    merge_log = res.stdout + res.stderr
    (output / "merge-route.log").write_text(merge_log)
    assert res.returncode == 0, merge_log
    assert "(ib=2 ob=3 missed=0)" in merge_log, merge_log
    assert "overused=0 overuse=0 archfail=0" in merge_log, merge_log

    merged = json.loads((output / "merged-routed.json").read_text())["modules"]["top"]
    cells = merged["cells"]
    netnames = merged["netnames"]
    gnd_bits = netnames["$PACKER_GND_NET"]["bits"]
    for bit_index, tag, expected_lut in ((1, "VCC", 1), (2, "GND", 0)):
        drv_name = f"fes_cart$const_ob_{tag}_DRV"
        net_name = f"fes_cart$const_ob_{tag}_NET"
        assert drv_name in cells, sorted(cells)
        drv = cells[drv_name]
        assert drv["type"] == "MISTRAL_CONST", drv
        assert int(drv["parameters"]["LUT"], 2) == expected_lut, drv
        assert drv["attributes"]["FES_SLOT"] == "cart", drv
        bel = drv["attributes"]["NEXTPNR_BEL"]
        _, x, y, _ = bel.split(".")
        assert 24 <= int(x) <= 28 and 2 <= int(y) <= 8, (drv_name, bel)
        const_bits = netnames[net_name]["bits"]
        assert drv["connections"]["Q"] == const_bits, drv
        ff = cells[f"plug_rdata_ff_{bit_index}"]
        assert ff["connections"]["DATAIN"] == const_bits, (bit_index, ff)
        assert ff["connections"]["DATAIN"] != gnd_bits, (bit_index, ff)

    # Direct constant bit on the top-level plug_rdata port (without MISTRAL_OB).
    direct_cart = copy.deepcopy(cart_json)
    dmod = direct_cart["modules"]["cart"]
    pad_bit_1 = dmod["ports"]["plug_rdata"]["bits"][1]
    dmod["ports"]["plug_rdata"]["bits"][1] = "1"
    for name in list(dmod["cells"]):
        cell = dmod["cells"][name]
        if cell["type"] == "MISTRAL_OB" and cell["connections"]["PAD"] == [pad_bit_1]:
            del dmod["cells"][name]
    direct_path = output / "cart-direct-const.json"
    direct_path.write_text(json.dumps(direct_cart))
    direct_res = subprocess.run(
        [nextpnr, "--device", "5CSEBA6U23I7", "--qsf", str(qsf),
         "--json", str(output / "routed-shell.json"),
         "--fes-scaffold", "--fes-cart", str(direct_path),
         "--no-pack", "--no-place", "--no-route",
         "--write", str(output / "direct-merged.json")],
        capture_output=True, text=True,
    )
    direct_log = direct_res.stdout + direct_res.stderr
    (output / "direct-merge.log").write_text(direct_log)
    assert direct_res.returncode == 0, direct_log
    assert "(ib=2 ob=3 missed=0)" in direct_log, direct_log
    direct_merged = json.loads((output / "direct-merged.json").read_text())["modules"]["top"]
    assert direct_merged["cells"]["plug_rdata_ff_1"]["connections"]["DATAIN"] == \
        direct_merged["netnames"]["fes_cart$const_ob_VCC_NET"]["bits"]

    # Unsupported high-impedance ('z') MISTRAL_OB input must fail closed.
    bad_cart = copy.deepcopy(cart_json)
    bmod = bad_cart["modules"]["cart"]
    pad_bit_1 = bmod["ports"]["plug_rdata"]["bits"][1]
    for cell in bmod["cells"].values():
        if cell["type"] == "MISTRAL_OB" and cell["connections"]["PAD"] == [pad_bit_1]:
            cell["connections"]["I"] = ["z"]
    bad_path = output / "cart-z-const.json"
    bad_path.write_text(json.dumps(bad_cart))
    bad_res = subprocess.run(
        [nextpnr, "--device", "5CSEBA6U23I7", "--qsf", str(qsf),
         "--json", str(output / "routed-shell.json"),
         "--fes-scaffold", "--fes-cart", str(bad_path),
         "--no-pack", "--no-place", "--no-route"],
        capture_output=True, text=True,
    )
    bad_log = bad_res.stdout + bad_res.stderr
    (output / "bad-merge.log").write_text(bad_log)
    assert bad_res.returncode != 0 and "plug_rdata[1]" in bad_log, bad_log

    print("PASS: constant-driven FES cart outputs route from cart MISTRAL_CONST drivers and reject unsupported 'z'")


if __name__ == "__main__":
    main()
