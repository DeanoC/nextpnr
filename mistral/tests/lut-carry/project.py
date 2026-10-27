#!/usr/bin/env python3
"""Generate fixed Cyclone V LUT/carry characterization fixtures, never hardware loads."""
import argparse
import hashlib
import json
from pathlib import Path

BLOCKS = {"lab": (29, 77), "mlab": (15, 26)}
KINDS = ("l5", "l6-top", "l6-bottom", "carry")
CASES = tuple(f"{block}-{kind}" for block in BLOCKS for kind in KINDS)


def mask(width, index):
    value = int.from_bytes(hashlib.sha256(f"native-lut-{width}-{index}".encode()).digest()[:8], "little")
    if width == 5:
        value = (value & 0xffffffff) * 0x100000001
    assert all(any(((value >> b) ^ (value >> (b ^ (1 << p)))) & 1
                   for b in range(1 << width)) for p in range(width))
    return value


def sources(case):
    block, kind = next((b, case[len(b)+1:]) for b in BLOCKS if case.startswith(b + "-"))
    assert kind in KINDS
    x, y = BLOCKS[block]
    sites = []
    for alm in range(10):
        for lane in ((0, 1) if kind in ("l5", "carry") else (int(kind == "l6-bottom"),)):
            i = len(sites)
            width = 5 if kind == "l5" else 6
            sites.append(dict(cell=f"target_{i}", block=block.upper(), x=x, y=y, alm=alm, lane=lane,
                              location=f"{'M' if block == 'mlab' else ''}LABCELL_X{x}_Y{y}_N{6*alm+3*lane}", width=width,
                              mask=f"{0x8000800069966996 if kind == 'carry' else mask(width, i):016x}"))
    count = len(sites)
    outputs = count + int(kind == "carry")
    rtl = [f"module top(input clk, input [5:0] din, output [{outputs-1}:0] probes);",
           "wire [5:0] q;"]
    for i in range(6):
        rtl.append(f"(* preserve, dont_merge *) dffeas launch_{i} "
                   f"(.clk(clk),.d(din[{i}]),.clrn(1'b1),.prn(1'b1),.ena(1'b1),"
                   f".asdata(1'b0),.sclr(1'b0),.sload(1'b0),.aload(1'b0),.q(q[{i}]));")
    if kind == "carry":
        rtl += [f"wire [{count}:0] carry;", "assign carry[0]=1'b0;"]
    for i, s in enumerate(sites):
        rtl.append(f"wire result_{i};")
        # The two L5 halves share their four common inputs and have distinct
        # fifth inputs, so the fixture is a legal fractured ALM.
        fifth = 4+s['lane'] if kind == "l5" else 4
        data = ["q[0]", "q[1]", "q[2]", "q[3]", f"q[{fifth}]",
                "1'b0" if kind == "l5" else ("q[4]" if kind == "carry" else "q[5]")]
        if kind == "carry":
            data[4] = "1'b0"
        inputs = ",".join(f".data{p}({v})" for p, v in zip("abcdef", data))
        outputs = f".sumout(result_{i}),.cout(carry[{i+1}])" if kind == "carry" else f".combout(result_{i})"
        carry = f"carry[{i}]" if kind == "carry" else "1'b0"
        rtl.append(f"cyclonev_lcell_comb #(.lut_mask(64'h{s['mask']}),.shared_arith(\"off\"),"
                   f".extended_lut(\"off\")) {s['cell']} "
                   f"({inputs},.datag(1'b0),.cin({carry}),.sharein(1'b0),{outputs});")
        rtl.append(f"(* preserve, dont_merge *) dffeas capture_{i} "
                   f"(.clk(clk),.d(result_{i}),.clrn(1'b1),.prn(1'b1),.ena(1'b1),"
                   f".asdata(1'b0),.sclr(1'b0),.sload(1'b0),.aload(1'b0),.q(probes[{i}]));")
    if kind == "carry":
        rtl.append(f"(* preserve, dont_merge *) dffeas capture_carry "
                   f"(.clk(clk),.d(carry[{count}]),.clrn(1'b1),.prn(1'b1),.ena(1'b1),"
                   f".asdata(1'b0),.sclr(1'b0),.sload(1'b0),.aload(1'b0),.q(probes[{count}]));")
    rtl.append("endmodule")
    qsf = ["set_global_assignment -name FAMILY \"Cyclone V\"",
           "set_global_assignment -name DEVICE 5CSEBA6U23I7",
           "set_global_assignment -name TOP_LEVEL_ENTITY top",
           "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files",
           "set_global_assignment -name NUM_PARALLEL_PROCESSORS 2",
           "set_global_assignment -name SEED 1",
           "set_global_assignment -name SYSTEMVERILOG_FILE top.sv",
           "set_global_assignment -name SDC_FILE clocks.sdc",
           "set_global_assignment -name REMOVE_DUPLICATE_REGISTERS OFF",
           "set_location_assignment PIN_V11 -to clk",
           "set_instance_assignment -name IO_STANDARD \"3.3-V LVTTL\" -to clk",
           'set_instance_assignment -name VIRTUAL_PIN ON -to "din[*]"',
           'set_instance_assignment -name VIRTUAL_PIN ON -to "probes[*]"']
    for s in sites:
        qsf.append(f"set_location_assignment {s['location']} -to {s['cell']}")
    # Keep launch FFs outside the measured block. A local launch can select
    # D_F/C_F register feedback, which is a different LUT boundary and mode.
    for i, z in enumerate((1,2,4,5,7,8)):
        qsf.append(f"set_location_assignment FF_X{x+1}_Y{y}_N{z} -to launch_{i}")
    return {"top.sv": "\n".join(rtl)+"\n", "top.qsf": "\n".join(qsf)+"\n",
            "top.qpf": "PROJECT_REVISION = top\n",
            "clocks.sdc": "create_clock -name clk -period 10.000 [get_ports clk]\n"}, dict(
                schema=1, case=case, device="5CSEBA6U23I7", sites=sites, hardware=False,
                scope="Fixed primitive references; no native timing model or board qualification")


def write(case, output):
    files, spec = sources(case)
    output = Path(output)
    output.mkdir(parents=True)
    for name, text in files.items():
        (output/name).write_text(text)
    (output/"fixture.json").write_text(json.dumps(spec, indent=2)+"\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=CASES)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    write(args.case, args.output)
