#!/usr/bin/env python3
"""Exercise real FES cart merge on a shell with independent clock domains."""

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
    shell = output / "shell.v"
    shell.write_text('''module top(input clk_a, input clk_b, input address,
                                 output qa, output qb, output data);
    (* keep *) wire plug_addr;
    (* keep *) wire plug_rdata_d = 1'b0;
    (* BEL = "MISTRAL_FF.24.1.2" *) MISTRAL_FF plug_addr_ff_0 (.CLK(clk_a), .DATAIN(address), .Q(plug_addr),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    MISTRAL_FF second_domain (.CLK(clk_b), .DATAIN(address), .Q(qb),
        .ACLR(1'b1), .ENA(~address), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    (* BEL = "MISTRAL_FF.28.1.2" *) MISTRAL_FF plug_rdata_ff_0 (.CLK(clk_a), .DATAIN(plug_rdata_d), .Q(data),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    wire [9:0] shell_data;
    MISTRAL_M10K #(.CFG_ABITS(10), .CFG_DBITS(10), .CFG_ASYNC_READ(1)) shell_memory (
        .CLK1(clk_a), .A1ADDR(10'b0), .A1DATA(10'b0), .A1EN(address),
        .B1ADDR(10'b0), .B1DATA(shell_data), .B1EN(1'b1),
        .ACLR0(1'b0), .ACLR1(1'b0));
    reg [7:0] arithmetic = 8'd0;
    always @(posedge clk_a) arithmetic <= arithmetic + 8'd1;
    wire lut6_result;
    (* keep *) MISTRAL_ALUT6 #(.LUT(64'h6996966996696996)) six_input (
        .A(arithmetic[0]), .B(arithmetic[1]), .C(arithmetic[2]),
        .D(arithmetic[3]), .E(arithmetic[4]), .F(arithmetic[5]), .Q(lut6_result));
    assign qa = plug_addr ^ shell_data[0] ^ arithmetic[7] ^ lut6_result;
endmodule
''')
    cart = output / "cart.v"
    cart.write_text('''module cart(input FPGA_CLK1_50, input plug_addr,
                                  output plug_rdata);
    wire [9:0] data;
    MISTRAL_M10K #(.CFG_ABITS(10), .CFG_DBITS(10), .CFG_ASYNC_READ(1)) memory (
        .CLK1(FPGA_CLK1_50), .A1ADDR(10'b0), .A1DATA(10'b0), .A1EN(1'b0),
        .B1ADDR({9'b0, plug_addr}), .B1DATA(data), .ACLR0(1'b0), .ACLR1(1'b0));
    wire [9:0] a_data, b_data;
    wire state;
    MISTRAL_FF state_ff (.CLK(FPGA_CLK1_50), .DATAIN(plug_addr), .Q(state),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    MISTRAL_M10K_TDP #(.CFG_ABITS(10), .CFG_DBITS(10), .CFG_ASYNC_READ(1)) writable (
        .CLK1(FPGA_CLK1_50), .CLK2(FPGA_CLK1_50),
        .A1ADDR({9'b0, plug_addr}), .B1ADDR(10'b0),
        .A1DATA({9'b0, state}), .B1DATA(10'b0), .A1Q(a_data), .B1Q(b_data),
        .A1EN(1'b1), .B1EN(1'b1), .A1WE(plug_addr), .B1WE(1'b0),
        .A1BE(2'b11), .B1BE(2'b11), .ACLR0(1'b0), .ACLR1(1'b0));
    assign plug_rdata = data[0] ^ a_data[0] ^ b_data[0] ^ state;
endmodule
''')
    for name, top in (("shell", "top"), ("cart", "cart")):
        command = [str(args.yosys.resolve()), "-p",
                   f"read_verilog {output / (name + '.v')}; "
                   f"synth_intel_alm -nolutram -nodsp -top {top}; "
                   f"write_json {output / (name + '.json')}"]
        with (output / (name + "-synth.log")).open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    # A real scaffold was packed before being frozen; packing also creates
    # constant nets and the physical clock buffers used by cart merge.
    raw = output / "shell-unpacked.json"
    (output / "shell.json").rename(raw)
    qsf = output / "shell.qsf"
    qsf.write_text("\n".join(f"set_location_assignment PIN_{pin} -to {port}"
                             for pin, port in (("V11", "clk_a"), ("U10", "clk_b"),
                                               ("W8", "address"), ("AG5", "qa"),
                                               ("AD19", "qb"), ("AD12", "data"))) + "\n")
    with (output / "shell-pack.log").open("w") as log:
        subprocess.run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                        "--qsf", str(qsf), "--json", str(raw), "--pack-only",
                        "--write", str(output / "shell.json")],
                       stdout=log, stderr=subprocess.STDOUT, check=True)

    def merge(name, clock=None, expected=None, shell_name="shell", cart_name="cart"):
        command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                   "--json", str(output / (shell_name + ".json")),
                   "--fes-cart", str(output / (cart_name + ".json")),
                   "--no-pack", "--no-place", "--no-route",
                   "--write", str(output / (name + ".json"))]
        if clock is not None:
            command += ["--fes-slot-clock", clock]
        result = subprocess.run(command, capture_output=True, text=True)
        (output / (name + ".log")).write_text(result.stdout + result.stderr)
        if expected:
            assert result.returncode != 0 and expected in result.stdout + result.stderr, result
            return
        assert result.returncode == 0, result.stdout + result.stderr
        return json.loads((output / (name + ".json")).read_text())["modules"]["top"]

    merge("ambiguous", expected="multiple clocks")
    merge("missing", "missing_clock", expected="not a driven shell net")
    # Use the actual FF clock aliases after synthesis (before global promotion).
    source = json.loads((output / "shell.json").read_text())
    design = source["modules"]["top"]
    for clock_cell in ("plug_addr_ff_0", "second_domain"):
        bit = design["cells"][clock_cell]["connections"]["CLK"]
        name = next(name for name, net in design["netnames"].items() if net["bits"] == bit)
        merged = merge("selected-" + clock_cell, name)
        cells = merged["cells"]
        assert cells["fes_cart$memory"]["connections"]["CLK1"] == cells[clock_cell]["connections"]["CLK"]
        for cell, port in (("writable", "CLK1"), ("writable", "CLK2"), ("state_ff", "CLK")):
            assert cells["fes_cart$" + cell]["connections"][port] == cells[clock_cell]["connections"]["CLK"]
        ground = cells["$PACKER_GND_DRV"]["connections"]["Q"]
        supply = cells["$PACKER_VCC_DRV"]["connections"]["Q"]
        assert cells["fes_cart$memory"]["connections"]["A1EN"] == ground
        assert cells["fes_cart$writable"]["connections"]["B1WE"] == ground
        assert cells["fes_cart$writable"]["connections"]["B1EN"] == supply
        assert cells["fes_cart$writable"]["connections"]["A1BE"] == supply * 2
    cart_source = json.loads((output / "cart.json").read_text())
    cart_cells = cart_source["modules"]["cart"]["cells"]
    # An unsupported divided clock must fail, not become a full-rate clock.
    cart_cells["writable"]["connections"]["CLK2"] = cart_cells["state_ff"]["connections"]["Q"]
    (output / "derived-cart.json").write_text(json.dumps(cart_source))
    merge("derived", name, expected="does not use the declared socket clock", cart_name="derived-cart")
    cart_cells["writable"]["connections"]["CLK2"] = ["0"]
    (output / "constant-clock-cart.json").write_text(json.dumps(cart_source))
    merge("constant-clock", name, expected="does not use the declared socket clock", cart_name="constant-clock-cart")
    del cart_cells["writable"]["connections"]["CLK2"]
    (output / "missing-clock-cart.json").write_text(json.dumps(cart_source))
    merge("missing-cart-clock", name, expected="requires one declared socket clock input", cart_name="missing-clock-cart")
    # A routed shell carries packed physical pins, unlike the pack-only
    # fixture above. Re-entering normal RAM setup would reinterpret cleared
    # hard pins, while logical sink traversal used to throw dict::at().
    with (output / "shell-route.log").open("w") as log:
        subprocess.run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                        "--qsf", str(qsf), "--json", str(raw), "--router", "router2",
                        "--write", str(output / "routed-shell.json"),
                        "--rbf", str(output / "shell.rbf"), "--compress-rbf"],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    routed = json.loads((output / "routed-shell.json").read_text())["modules"]["top"]
    assert routed['cells']['six_input']['type'] == 'MISTRAL_ALUT6'
    inverted = json.loads(bytes.fromhex(routed['cells']['second_domain']['attributes']['FES_PINMAP_V1']).decode())['pins']
    assert inverted['ENA'][0] == 3, inverted
    with (output / 'shell-restore.log').open('w') as log:
        subprocess.run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7',
                        '--json', str(output / 'routed-shell.json'), '--fes-scaffold',
                        '--fes-cram-region', '0,0,1,1',
                        '--no-pack', '--no-place', '--router', 'router2',
                        '--rbf', str(output / 'shell-restored.rbf'), '--compress-rbf'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    assert 'ERROR:' not in (output / 'shell-restore.log').read_text()
    assert (output / 'shell.rbf').read_bytes() == (output / 'shell-restored.rbf').read_bytes(), 'restored shell RBF changed'
    for index, region in enumerate(('1,0,1,1', '0,0,999999,1', '0,0,1,1,2', '-1,0,1,1')):
        result = subprocess.run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7',
                                 '--json', str(output / 'routed-shell.json'),
                                 '--fes-cram-region', region, '--no-pack', '--no-place', '--no-route'],
                                capture_output=True, text=True)
        (output / f'invalid-cram-region-{index}.log').write_text(result.stdout + result.stderr)
        assert result.returncode != 0 and 'Invalid FES CRAM region' in result.stdout + result.stderr
    result = subprocess.run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7',
                             '--json', str(output / 'shell.json'), '--fes-cram-region', '0,0,1,1',
                             '--no-pack', '--no-place', '--no-route'], capture_output=True, text=True)
    assert result.returncode != 0 and 'requires an already routed scaffold' in result.stdout + result.stderr
    bit = routed["cells"]["plug_addr_ff_0"]["connections"]["CLK"]
    clock = next(key for key, net in routed["netnames"].items() if net["bits"] == bit)
    assert routed["cells"]["plug_addr_ff_0"]["attributes"]["NEXTPNR_BEL"] == "MISTRAL_FF.24.1.2"
    assert routed["cells"]["plug_rdata_ff_0"]["attributes"]["NEXTPNR_BEL"] == "MISTRAL_FF.28.1.2"
    reserved_qsf = output / "cart-reserved.qsf"
    reserved_qsf.write_text(qsf.read_text() + 'set_global_assignment -name FES_RESERVED_RECT "24 1 28 11"\n')
    with (output / "reserved-place.log").open("w") as log:
        result = subprocess.run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                                 "--json", str(output / "routed-shell.json"),
                                 "--qsf", str(reserved_qsf),
                                 "--fes-cart", str(output / "cart.json"),
                                 "--fes-slot-clock", clock, "--fes-scaffold", "--no-pack", "--no-route",
                                 "--seed", "2", "--write", str(output / "reserved-placed.json")],
                                stdout=log, stderr=subprocess.STDOUT)
    assert result.returncode == 0, (output / "reserved-place.log").read_text()
    reserved_placed = json.loads((output / "reserved-placed.json").read_text())["modules"]["top"]["cells"]
    for name in ("plug_addr_ff_0", "plug_rdata_ff_0"):
        assert reserved_placed[name]["attributes"]["NEXTPNR_BEL"] == routed["cells"][name]["attributes"]["NEXTPNR_BEL"]
    reserved_module = json.loads((output / "reserved-placed.json").read_text())["modules"]["top"]
    def routed_pips(encoded):
        if not encoded or not encoded.strip():
            return set()
        fields = encoded.split(";")
        assert len(fields) % 3 == 0, encoded
        return {(fields[i], fields[i + 1]) for i in range(0, len(fields), 3)}
    for name, net in routed["netnames"].items():
        # Cart merge disconnects the placeholder ground sink, so its old
        # constant-net route is the one deliberate exception.
        if name != "$PACKER_GND_NET" and "ROUTING" in net.get("attributes", {}):
            assert routed_pips(reserved_module["netnames"][name]["attributes"]["ROUTING"]) == \
                routed_pips(net["attributes"]["ROUTING"]), name
    for name, cell in reserved_placed.items():
        if not name.startswith("fes_cart$"):
            continue
        bel = cell.get("attributes", {}).get("NEXTPNR_BEL", "")
        if bel:
            _, x, y, _ = bel.split(".")
            assert 24 <= int(x) <= 28 and 1 <= int(y) <= 11, (name, bel)
    # A newly introduced, unbound shell FF cannot take a reserved BEL.
    # Reserve the whole chip in this rejection case so no legal site exists.
    unconstrained = json.loads((output / "routed-shell.json").read_text())
    new_cell = copy.deepcopy(unconstrained["modules"]["top"]["cells"]["plug_addr_ff_0"])
    new_cell["attributes"] = {}
    new_cell["connections"]["Q"] = [999999]
    unconstrained["modules"]["top"]["cells"]["new_unconstrained_ff"] = new_cell
    (output / "unconstrained-shell.json").write_text(json.dumps(unconstrained))
    all_reserved_qsf = output / "all-reserved.qsf"
    all_reserved_qsf.write_text(qsf.read_text() + 'set_global_assignment -name FES_RESERVED_RECT "0 0 100 100"\n')
    result = subprocess.run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                             "--json", str(output / "unconstrained-shell.json"),
                             "--qsf", str(all_reserved_qsf), "--fes-scaffold", "--no-pack", "--no-route"],
                            capture_output=True, text=True)
    assert result.returncode != 0 and "Unable to find legal placement for cell 'new_unconstrained_ff'" in result.stdout + result.stderr
    outside_cart = json.loads((output / "cart.json").read_text())
    outside_cart["modules"]["cart"]["cells"]["state_ff"]["attributes"]["BEL"] = "MISTRAL_FF.29.12.2"
    (output / "cart-outside.json").write_text(json.dumps(outside_cart))
    result = subprocess.run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                             "--json", str(output / "routed-shell.json"),
                             "--qsf", str(reserved_qsf),
                             "--fes-cart", str(output / "cart-outside.json"),
                             "--fes-slot-clock", clock, "--fes-scaffold", "--no-pack", "--no-route"],
                            capture_output=True, text=True)
    assert result.returncode != 0 and "must stay in its own reserved region" in result.stdout + result.stderr
    merged = merge("routed-merge", clock, shell_name="routed-shell")
    def same_connections(cell, ports=None):
        before, after = routed["cells"][cell], merged["cells"][cell]
        for port, bits in before["connections"].items():
            if not bits or (ports is not None and port not in ports):
                continue
            assert len(bits) == len(after["connections"][port]), (cell, port)
            for index, bit in enumerate(bits):
                aliases = [(key, position) for key, net in routed["netnames"].items()
                           for position, value in enumerate(net["bits"]) if value == bit]
                assert any(key in merged["netnames"] and
                           merged["netnames"][key]["bits"][position] == after["connections"][port][index]
                           for key, position in aliases), (cell, port, index)
    for cell in ("shell_memory", "second_domain", "plug_addr_ff_0"):
        assert merged["cells"][cell]["parameters"] == routed["cells"][cell]["parameters"], cell
        assert merged["cells"][cell]["attributes"] == routed["cells"][cell]["attributes"], cell
        same_connections(cell)
    same_connections("plug_rdata_ff_0", {"SCLR", "ACLR", "ENA"})
    for constant in ("$PACKER_GND_NET", "$PACKER_VCC_NET"):
        assert merged["netnames"][constant]["attributes"]["ROUTING"] == routed["netnames"][constant]["attributes"]["ROUTING"]
    # Exercise actual scaffold locking, placement and final router consistency.
    # Arithmetic SO shares a physical wire with a WA4 input alias: restoring
    # both as output pins previously made getNetinfoSourceWire assert.
    assert any(c['type'] == 'MISTRAL_ALUT_ARITH' for c in routed['cells'].values())
    with (output / 'scaffold-place.log').open('w') as log:
        subprocess.run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7',
                        '--json', str(output / 'routed-shell.json'),
                        '--fes-cart', str(output / 'cart.json'),
                        '--fes-slot-clock', clock, '--fes-scaffold', '--no-pack', '--router', 'router2', '--seed', '2',
                        '--fes-cram-region', '0,0,7605,7024',
                        '--write', str(output / 'scaffold-placed.json'),
                        '--rbf', str(output / 'scaffold-composed.rbf'), '--compress-rbf'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    routing_log = (output / 'scaffold-place.log').read_text()
    assert 'ERROR:' not in routing_log, routing_log
    assert 'Routing complete.' in routing_log and 'overused=0 overuse=0 archfail=0' in routing_log, routing_log
    assert 'Program finished normally.' in routing_log, routing_log
    assert (output / 'scaffold-composed.rbf').stat().st_size > 40408
    assert (output / 'scaffold-placed.json').is_file()
    placed = json.loads((output / 'scaffold-placed.json').read_text())['modules']['top']
    route_through = placed['cells']['fes_cart$state_ff$ROUTETHRU']
    # merge_fes_cart tags every cart cell with its target region name (the
    # default region when --fes-cart-region is omitted, matching the legacy
    # boolean FES_SLOT=1 cart marker written by cart-authoring recipes).
    assert route_through['attributes']['FES_SLOT'] == 'cart', route_through
    for constant, value in (('GND', 0), ('VCC', 1)):
        cell = placed['cells'][f'fes_cart$local_{constant}_DRV']
        assert cell['type'] == 'MISTRAL_CONST' and int(cell['parameters']['LUT'], 2) == value
        assert cell['attributes']['FES_SLOT'] == 'cart'
        local_bits = placed['netnames'][f'fes_cart$local_{constant}_NET']['bits']
        assert cell['connections']['Q'] == local_bits
        for name, other in placed['cells'].items():
            if not name.startswith('fes_cart$'):
                assert all(not set(bits).intersection(local_bits) for bits in other['connections'].values()), name
    for cell, before in routed['cells'].items():
        if 'NEXTPNR_BEL' in before.get('attributes', {}):
            assert placed['cells'][cell]['attributes']['NEXTPNR_BEL'] == before['attributes']['NEXTPNR_BEL'], cell
    def lab_location(cell):
        bel = cell.get('attributes', {}).get('NEXTPNR_BEL', '')
        if bel.startswith(('MISTRAL_COMB.', 'MISTRAL_MCOMB.', 'MISTRAL_FF.')):
            return tuple(bel.split('.')[1:3])
        return None
    frozen_labs = {lab_location(c) for c in routed['cells'].values()} - {None}
    cart_labs = {lab_location(c) for name, c in placed['cells'].items()
                 if name.startswith('fes_cart$')} - {None}
    assert frozen_labs and cart_labs and not frozen_labs.intersection(cart_labs), (frozen_labs, cart_labs)
    def lab_states(module):
        payload = json.loads(bytes.fromhex(module['attributes']['FES_LABSTATE_V1']).decode())
        return {(str(row[0]), str(row[1])): row for row in payload['labs']}
    before_states, after_states = lab_states(routed), lab_states(placed)
    for location in frozen_labs:
        assert before_states[location] == after_states[location], location

    # Saved maps are part of the frozen-route contract, including physical
    # padding lanes and folded signal states which routing alone cannot infer.
    folded_states = set()
    for cell, before in routed['cells'].items():
        if 'NEXTPNR_BEL' not in before.get('attributes', {}):
            continue
        saved = before['attributes']['FES_PINMAP_V1']
        payload = json.loads(bytes.fromhex(saved).decode())
        assert payload['count'] == len(payload['pins']), cell
        folded_states.update(v[0] for v in payload['pins'].values() if len(v) == 1)
        assert placed['cells'][cell]['attributes']['FES_PINMAP_V1'] == saved, cell
    assert {1, 2}.issubset(folded_states), folded_states
    ram_map = json.loads(bytes.fromhex(routed['cells']['shell_memory']['attributes']['FES_PINMAP_V1']).decode())['pins']
    assert ram_map['B1DATA[9]'][1:], ram_map['B1DATA[9]']
    unused = routed['cells']['shell_memory']['connections']['B1DATA'][9]
    assert not any(unused in bits for c in routed['cells'].values()
                   for port, bits in c['connections'].items()
                   if c['port_directions'][port] == 'input'), unused
    arithmetic = next(name for name, cell in routed['cells'].items()
                      if cell['type'] == 'MISTRAL_ALUT_ARITH' and cell['connections'].get('SO'))
    for corruption in ('malformed', 'missing-output', 'wrong-direction', 'duplicate-pin', 'unsupported-version', 'invalid-hex', 'missing-folded', 'empty-inverted'):
        bad = copy.deepcopy(json.loads((output / 'routed-shell.json').read_text()))
        attrs = bad['modules']['top']['cells'][arithmetic]['attributes']
        payload = json.loads(bytes.fromhex(attrs['FES_PINMAP_V1']).decode())
        pinmap = payload['pins']
        if corruption == 'malformed':
            attrs['FES_PINMAP_V1'] = b'{'.hex()
        elif corruption == 'invalid-hex':
            attrs['FES_PINMAP_V1'] = 'xx'
        elif corruption == 'unsupported-version':
            attrs['FES_PINMAP_V2'] = attrs.pop('FES_PINMAP_V1')
        else:
            assert pinmap['SO'][1:]
            if corruption == 'missing-output':
                del pinmap['SO']
                payload['count'] = len(pinmap)  # Exercise logical completeness, not just count.
            elif corruption == 'missing-folded':
                folded = next(k for k,v in pinmap.items() if len(v) == 1 and v[0] != 0)
                del pinmap[folded]  # Retain count to detect truncated folded state.
            elif corruption == 'empty-inverted':
                pinmap['SO'] = [3]
            elif corruption == 'wrong-direction':
                pinmap['SO'] = [0, 'WA4']
            else:
                pinmap['SO'].append(pinmap['SO'][1])
            attrs['FES_PINMAP_V1'] = json.dumps(payload, sort_keys=True).encode().hex()
        path = output / (corruption + '-scaffold.json')
        path.write_text(json.dumps(bad))
        result = subprocess.run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7',
                                 '--json', str(path), '--fes-scaffold', '--no-pack', '--no-place', '--no-route'],
                                capture_output=True, text=True)
        text = result.stdout + result.stderr
        (output / (corruption + '-scaffold.log')).write_text(text)
        assert result.returncode != 0 and 'frozen' in text.lower() and 'pin' in text.lower(), (corruption, text)
    for corruption in ('missing', 'malformed', 'geometry', 'selector', 'version', 'device', 'truncated'):
        bad = copy.deepcopy(json.loads((output / 'routed-shell.json').read_text()))
        attrs = bad['modules']['top']['attributes']
        payload = json.loads(bytes.fromhex(attrs['FES_LABSTATE_V1']).decode())
        if corruption == 'missing':
            del attrs['FES_LABSTATE_V1']
        elif corruption == 'malformed':
            attrs['FES_LABSTATE_V1'] = b'{'.hex()
        elif corruption == 'version':
            attrs['FES_LABSTATE_V2'] = attrs.pop('FES_LABSTATE_V1')
        else:
            if corruption == 'geometry':
                payload['labs'][0][0] += 1
            elif corruption == 'selector':
                payload['labs'][0][3][4] = 3
            elif corruption == 'device':
                payload['device'] = 'wrong-device'
            else:
                payload['labs'][0][3].pop()
            attrs['FES_LABSTATE_V1'] = json.dumps(payload, sort_keys=True).encode().hex()
        path = output / ('lab-' + corruption + '.json')
        path.write_text(json.dumps(bad))
        result = subprocess.run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7',
                                 '--json', str(path), '--fes-scaffold', '--no-pack', '--no-place', '--no-route'],
                                capture_output=True, text=True)
        text = result.stdout + result.stderr
        (output / ('lab-' + corruption + '.log')).write_text(text)
        assert result.returncode != 0 and 'frozen' in text.lower() and 'lab' in text.lower(), (corruption, text)
    # The original one-clock diagnostic retains implicit clock selection.
    design["cells"]["second_domain"]["connections"]["CLK"] = design["cells"]["plug_addr_ff_0"]["connections"]["CLK"]
    (output / "single.json").write_text(json.dumps(source))
    merge("single", shell_name="single")
    print("FES clock selection, inverted-control byte-identical replay, full RAM cart route/RBF, exact frozen LAB/maps and fifteen metadata rejection cases: PASS")


if __name__ == "__main__":
    main()
