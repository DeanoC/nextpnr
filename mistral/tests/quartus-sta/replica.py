#!/usr/bin/env python3
"""Rebuild one nextpnr critical path as a Quartus project (#174).

replica.py <routed.json> <timing.json> <routes.tsv> <clock> <outdir>

Places the launch flip-flop and every LUT of nextpnr's worst <clock> path
at nextpnr's sites, and writes a routing constraints file (RCF) that holds
each hop to the rnodes decoded from nextpnr's bitstream (routes.tsv from
route_delay) and to the physical LUT pin nextpnr chose. Mistral rnode names
are the RCF names with other type names (GIN = LE_BUFFER, TD =
LOCAL_INTERCONNECT, GOUT = BLOCK_INPUT_MUX, H3 = R3, V4 = C4, ...). Each LUT
becomes an inverter of its path input, so other nets keep off the path. The
final hop into the capture flip-flop is replaced by a register in the last
LUT's ALM; side branches of the original nets are not rebuilt.
"""
import csv
import json
import os
import re
import sys

RCF_TYPE = {"GIN": "LE_BUFFER", "GOUT": "BLOCK_INPUT_MUX", "TD": "LOCAL_INTERCONNECT", "LD": "LOCAL_LINE",
            "H3": "R3", "H6": "R6", "H14": "R14", "V2": "C2", "V4": "C4", "V12": "C12",
            "WM": "R14_C12_INTERCONNECT_DRIVER"}
FF_N = {0: 1, 1: 2, 2: 4, 3: 5}


def bel(cell):
    kind, x, y, z = cell["attributes"]["NEXTPNR_BEL"].split(".")
    return kind, int(x), int(y), int(z)


def pinmap(cell):
    raw = cell["attributes"].get("FES_PINMAP_V1")
    return {k: v[1] for k, v in json.loads(bytes.fromhex(raw).decode())["pins"].items()} if raw else {}


def rcf_name(node):
    t, x, y, z = node.split(".")
    return f"{RCF_TYPE[t]}:X{int(x)}Y{int(y)}S0I{int(z)}"


def main():
    routed, timing, routes_tsv, clock, out = sys.argv[1:6]
    os.makedirs(out, exist_ok=True)
    mod = next(iter(json.load(open(routed))["modules"].values()))
    cells = mod["cells"]
    path = next(p for p in json.load(open(timing))["critical_paths"]
                if p["to"] == f"posedge {clock}" and clock in p["from"])["path"]
    routes = list(csv.DictReader(open(routes_tsv), delimiter="\t"))

    launch = next(s for s in path if s["type"] == "clk-to-q")["from"]["cell"]
    hops, luts = [], []
    for i, s in enumerate(path):
        if s["type"] != "logic":
            continue
        name = s["from"]["cell"]
        c = cells[name]
        kind, x, y, z = bel(c)
        alm, half = z // 6, z % 6
        assert half in (0, 1), (name, z)
        phys = pinmap(c)[s["from"]["port"]]
        route = path[i - 1]
        assert route["type"] == "routing"
        dst = f"{'MLAB' if kind == 'MISTRAL_MCOMB' else 'LAB'}.{x:03d}.{y:03d}.{alm}:{phys}"
        r = next(r for r in routes if r["dst"] == dst)
        assert abs(max(float(r["rise100"]), float(r["fall100"])) - route["delay"] * 1000) < 1.5, (dst, r, route)
        luts.append({"name": name, "kind": kind, "x": x, "y": y, "alm": alm, "half": half, "phys": phys,
                     "chain": r["chain"].split(","), "route_ps": route["delay"] * 1000, "cell_ps": s["delay"] * 1000})
    lkind, lx, ly, lz = bel(cells[launch])
    assert lkind == "MISTRAL_FF"

    v = ["module top(input clk, input din, output dout);",
         "(* preserve *) reg q0;", "always @(posedge clk) q0 <= din;"]
    qsf = ['set_global_assignment -name FAMILY "Cyclone V"', "set_global_assignment -name DEVICE 5CSEBA6U23I7",
           "set_global_assignment -name TOP_LEVEL_ENTITY top", "set_global_assignment -name VERILOG_FILE top.v",
           "set_global_assignment -name SDC_FILE top.sdc",
           "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files",
           "set_global_assignment -name GENERATE_RBF_FILE ON",
           "set_instance_assignment -name VIRTUAL_PIN ON -to din",
           "set_instance_assignment -name VIRTUAL_PIN ON -to dout",
           "set_location_assignment PIN_V11 -to clk",
           f"set_location_assignment FF_X{lx}_Y{ly}_N{(lz // 6) * 6 + FF_N[lz % 6 - 2]} -to q0"]
    prev = "q0"
    for k, l in enumerate(luts, 1):
        v.append(f"wire n{k};")
        v.append(f"cyclonev_lcell_comb #(.lut_mask(64'h5555555555555555), .shared_arith(\"off\"), "
                 f".extended_lut(\"off\")) l{k} (.dataa({prev}), .datab(1'b1), .datac(1'b1), .datad(1'b1), "
                 f".datae(1'b1), .dataf(1'b1), .datag(1'b1), .cin(1'b0), .sharein(1'b0), .combout(n{k}));")
        cell = "MLABCELL" if l["kind"] == "MISTRAL_MCOMB" else "LABCELL"
        # Quartus names a lcell_comb node after its output net.
        qsf.append(f"set_location_assignment {cell}_X{l['x']}_Y{l['y']}_N{l['alm'] * 6 + 3 * l['half']} -to n{k}")
        l["signal"], l["atom"] = prev, f"n{k}"
        prev = f"n{k}"
    last = luts[-1]
    v += ["(* preserve *) reg qc;", f"always @(posedge clk) qc <= {prev};", "assign dout = qc;", "endmodule"]
    capture = last["alm"] * 6 + (1 if last["half"] == 0 else 4)
    qsf.append(f"set_location_assignment FF_X{last['x']}_Y{last['y']}_N{capture} -to qc")
    qsf.append("set_global_assignment -name ROUTING_BACK_ANNOTATION_FILE top.rcf")
    rcf = ["section global_data {", '\trcf_written_by = "nextpnr #174 replica";', "\tdevice = 5CSEBA6U23I7;", "}", ""]
    for l in luts:
        port = {"E0": "E", "E1": "E", "F0": "F", "F1": "F"}.get(l["phys"], l["phys"])
        body = [f"\t{rcf_name(n)};" for n in l["chain"]]
        dest = f"\tdest = ( {l['atom']}, DATAA ), route_port = DATA{port};"
        rcf += [f"signal_name = {l['signal']} {{"] + body + [dest, "}"]
    open(os.path.join(out, "top.v"), "w").write("\n".join(v) + "\n")
    open(os.path.join(out, "top.qsf"), "w").write("\n".join(qsf) + "\n")
    open(os.path.join(out, "top.rcf"), "w").write("\n".join(rcf) + "\n")
    open(os.path.join(out, "top.qpf"), "w").write('PROJECT_REVISION = "top"\n')
    open(os.path.join(out, "top.sdc"), "w").write("create_clock -name clk -period 40 [get_ports clk]\n")
    json.dump({"launch": launch, "luts": luts}, open(os.path.join(out, "replica.json"), "w"), indent=1)
    for l in luts:
        print(f"{l['atom']:<4} {l['name'][:60]:<60} {l['kind']:<14} X{l['x']}Y{l['y']} alm{l['alm']} h{l['half']} "
              f"pin {l['phys']:<3} route {l['route_ps']:.0f} cell {l['cell_ps']:.0f}  "
              + "+".join(n.split(".")[0] for n in l["chain"]))


if __name__ == "__main__":
    main()
