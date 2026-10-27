#!/usr/bin/env python3
"""Fresh CPU-reference routes: recorder/empty-plan neutrality and strict feedback.

An optional untouched compiler also checks byte equivalence against the parent.
The CPU reference runs the same GPU algorithm without leasing a device.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("feedback", Path(__file__).resolve().parents[1] / "routed_placement.py")
feedback = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(feedback)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("nextpnr", "yosys"):
        parser.add_argument("--" + name, type=Path, required=True)
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--output", type=Path, help="unused artifact directory")
    output.add_argument("--output-root", type=Path, help="retain a new run directory beneath this path")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    if args.output_root:
        args.output_root.mkdir(parents=True, exist_ok=True)
        root = Path(tempfile.mkdtemp(prefix="run-", dir=args.output_root.resolve()))
    else:
        root = args.output.resolve()
        root.mkdir(parents=True, exist_ok=False)
    print("Artifacts: " + str(root), flush=True)
    (root / "top.v").write_text("""module top(input clk);
(* keep *) reg [15:0] state = 16'h1357;
always @(posedge clk) state <= {state[14:0], state[15]^state[13]^state[12]^state[10]};
endmodule
""")
    (root / "pins.qsf").write_text("set_location_assignment PIN_V11 -to clk\n")
    (root / "clocks.sdc").write_text("create_clock -name reference -period 20 [get_ports clk]\n")
    (root / "tight.sdc").write_text("create_clock -name reference -period 1 [get_ports clk]\n")
    commands = []

    def run(command, label, codes=(0,)):
        commands.append(dict(label=label, command=[str(x) for x in command]))
        with (root / (label + ".log")).open("x") as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
        assert result.returncode in codes, f"{label} exited {result.returncode}: see {root / (label + '.log')}"
        return result.returncode

    # Paths inside the Yosys command are quoted for its own parser.
    run([str(args.yosys.resolve()), "-p", f'read_verilog "{root / "top.v"}"; synth_intel_alm -top top; '
         f'write_json "{root / "synth.json"}"'], "synth")

    def route(binary, label, record=False, plan=None, preserve=False, routes=None, tight=False, group=False):
        directory = root / label
        directory.mkdir()
        command = [str(binary.resolve()), "--device", "5CSEBA6U23I7", "--json", str(root / "synth.json"),
                   "--qsf", str(root / "pins.qsf"), "--sdc", str(root / ("tight.sdc" if tight else "clocks.sdc")),
                   "--freq", "1000" if tight else "50",
                   "--seed", "7", "--placer", "heap", "--router", "gpu", "--gpu-cpu",
                   "--write", str(directory / "routed.json"), "--report", str(directory / "timing.json"),
                   "--rbf", str(directory / "core.rbf")]
        if record:
            command += ["--routed-placement-record", str(directory / "native")]
        if plan:
            command += ["--routed-placement-plan", str(plan)]
        if preserve:
            command += ["--routed-placement-preserve-routes"]
        if group:
            command += ["--routed-placement-group-moves"]
        if routes:
            command += ["--routed-placement-routes", str(routes)]
        code = run(command, label, codes=(1,) if tight else (0,))
        feedback.save(directory / "result.json", dict(exit_code=code, sha256={
            str(p.relative_to(directory)): feedback.digest(p) for p in directory.rglob("*") if p.is_file()}))
        return directory

    controls = [route(args.nextpnr, "disabled"), route(args.nextpnr, "recorded", record=True)]
    recorded = feedback.read_observation(controls[1], 0)
    origin = recorded["placement"]["origin"]
    plan = root / "empty-plan.json"
    feedback.save(plan, dict(version=1, origin=origin, parent=origin, moves=[],
                             predicted=dict(before_ps=0, after_ps=0)))
    controls.append(route(args.nextpnr, "empty-plan", record=True, plan=plan))
    feedback.read_observation(controls[-1], 0)
    if args.baseline:
        controls.append(route(args.baseline, "untouched"))
    identical = {}
    for name in ("routed.json", "timing.json", "core.rbf"):
        hashes = {directory.name: feedback.digest(directory / name) for directory in controls}
        assert len(set(hashes.values())) == 1, f"{name} changed: {hashes}"
        identical[name] = hashes

    # Reconstruct fresh placement and native preparation before importing data
    # trees. Identity replay must retain every captured tree and physical bytes.
    preserved_base = route(args.nextpnr, "preservation-baseline", record=True, preserve=True)
    preserved = feedback.read_observation(preserved_base, 0)
    preserve_plan = root / "preservation-empty-plan.json"
    origin = preserved["placement"]["origin"]
    feedback.save(preserve_plan, dict(version=1, origin=origin, parent=origin, moves=[],
                                       predicted=dict(before_ps=0, after_ps=0)))
    preserved_replay = route(args.nextpnr, "preservation-replay", record=True, preserve=True,
                            plan=preserve_plan, routes=preserved_base / "native/routes.json")
    feedback.read_observation(preserved_replay, 0)
    preservation = feedback.load(preserved_replay / "native/route-preservation.json")
    witness = feedback.load(preserved_base / "native/routes.json")
    assert preservation["preserved"] and not preservation["changed"] and not preservation["blocked"]
    assert set(preservation["preserved"]) == set(witness["nets"])
    preservation_hashes = {}
    for name in ("core.rbf", "timing.json"):
        assert feedback.digest(preserved_base / name) == feedback.digest(preserved_replay / name)
        preservation_hashes[name] = feedback.digest(preserved_base / name)

    # A setup miss enters analogue full rerouting. User-strength data trees
    # must remain protected there as well as in candidate rerouting.
    failed_base = route(args.nextpnr, "preservation-failing-baseline", record=True, preserve=True, tight=True)
    failed = feedback.read_observation(failed_base, 1)
    failed_plan = root / "preservation-failing-empty-plan.json"
    origin = failed["placement"]["origin"]
    feedback.save(failed_plan, dict(version=1, origin=origin, parent=origin, moves=[],
                                    predicted=dict(before_ps=0, after_ps=0)))
    failed_replay = route(args.nextpnr, "preservation-failing-replay", record=True, preserve=True, tight=True,
                          plan=failed_plan, routes=failed_base / "native/routes.json")
    failed_after = feedback.read_observation(failed_replay, 1)
    assert not failed_after["status"]["timing_pass"]
    assert failed["rows"] == failed_after["rows"]
    failed_preservation = feedback.load(failed_replay / "native/route-preservation.json")
    assert failed_preservation["preserved"] and not failed_preservation["changed"] and not failed_preservation["blocked"]
    assert feedback.digest(failed_base / "core.rbf") == feedback.digest(failed_replay / "core.rbf")

    # Version-2 plans carry the selected trees, including when placement is
    # unchanged. Exercise both a passing and a failing native timing gate.
    handed_checks = {}
    for tight, parent_dir, observed in ((False, preserved_base, preserved), (True, failed_base, failed)):
        saved_routes = feedback.load(parent_dir / "native/routes.json")
        request = dict(version=1, target=observed["placement"]["current"], nets={
            name:dict(source=tree["physical"]["source"], sinks=sorted(set(tree["physical"]["sinks"])),
                      wires=tree["wires"]) for name, tree in saved_routes["nets"].items()})
        label = "handoff-failing-identity" if tight else "handoff-identity"
        path = root / (label + "-plan.json")
        feedback.save(path, dict(version=2, origin=observed["placement"]["origin"],
            parent=observed["placement"]["current"], moves=[], predicted=dict(before_ps=0, after_ps=0), data_routes=request))
        result = route(args.nextpnr, label, record=True, preserve=True, plan=path,
                       routes=parent_dir / "native/routes.json", tight=tight)
        paired = feedback.read_observation(result, 1 if tight else 0)
        assert paired["status"]["proposed_routes_match"] and paired["rows"] == observed["rows"]
        assert paired["placement"]["data_routes"] == request
        assert all(feedback.digest(result / name) == feedback.digest(parent_dir / name)
                   for name in ("core.rbf", "timing.json"))
        handed_checks[label] = dict(trees=len(request["nets"]), final_trees_match=True,
                                   rows_identical=True, timing_pass=paired["status"]["timing_pass"])

    group_checks = {}
    for tight, preserve in ((False, False), (True, False), (False, True), (True, True)):
        label = "group-" + ("preserved-" if preserve else "full-") + ("failing" if tight else "passing")
        parent_dir = route(args.nextpnr, label + "-baseline", record=True, preserve=preserve, tight=tight, group=True)
        parent = feedback.read_observation(parent_dir, 1 if tight else 0)
        origin = parent["placement"]["origin"]
        group_plan = dict(version=3, origin=origin, parent=origin, moves=[], evaluation="full_route_required")
        plan_path = root / (label + "-plan.json")
        feedback.save(plan_path, group_plan)
        replay = route(args.nextpnr, label + "-replay", record=True, preserve=preserve, tight=tight, group=True,
                       plan=plan_path, routes=parent_dir / "native/routes.json" if preserve else None)
        observed = feedback.read_observation(replay, 1 if tight else 0)
        assert observed["placement"]["group_plan"] == group_plan
        assert observed["status"]["group_placement_match"]
        assert parent["rows"] == observed["rows"]
        assert all(feedback.digest(parent_dir / name) == feedback.digest(replay / name)
                   for name in ("core.rbf", "timing.json"))
        check_dir = root / (label + "-check")
        check_command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7", "--json", str(root/"synth.json"),
            "--qsf", str(root/"pins.qsf"), "--sdc", str(root/("tight.sdc" if tight else "clocks.sdc")),
            "--freq", "1000" if tight else "50", "--seed", "7", "--placer", "heap", "--router", "gpu", "--gpu-cpu",
            "--routed-placement-record", str(check_dir), "--routed-placement-group-moves",
            "--routed-placement-plan", str(plan_path), "--routed-placement-check-plan", "--no-route"]
        if preserve: check_command.append("--routed-placement-preserve-routes")
        run(check_command, label+"-check")
        proof = feedback.load(check_dir/"plan-validation.json")
        assert proof == dict(version=1, legal=True, placement_only=True, route_complete=False)
        assert not (check_dir/"completion.json").exists()
        assert feedback.load(check_dir/"placement.json")["current"] == origin
        group_checks[label] = dict(final_placement_matches=True, rows_identical=True,
                                  timing_pass=observed["status"]["timing_pass"], placement_only_check=True)

    command = [sys.executable, str(Path(feedback.__file__)), "--nextpnr", str(args.nextpnr.resolve()),
               "--json", str(root / "synth.json"), "--qsf", str(root / "pins.qsf"),
               "--sdc", str(root / "tight.sdc"), "--seed", "7", "--freq", "1000", "--gpu-cpu",
               "--output", str(root / "feedback")]
    code = run(command, "feedback", codes=(0, 1))
    best = feedback.load(root / "feedback/best.json")
    decision = feedback.load(root / "feedback/decision.json")
    assert decision.get("trials"), "fixture did not exercise native proposals and a fresh candidate route"
    assert (code == 0) == best["timing_pass"] == decision["timing_pass"]
    selected = root / "feedback" / best["bundle"]
    assert feedback.digest(selected / "result.json") == best["bundle_sha256"]
    feedback.verify_bundle(selected)

    # A single registered endpoint makes a real measured Pareto improvement
    # observable without sacrificing another endpoint. Explicit primitives keep
    # this deliberately redundant logic chain through synthesis; it is not a
    # prediction of whole-core benefit.
    (root / "one-endpoint.v").write_text("""module top(input clk);
reg state=1'b0;
(* keep *) wire [12:0] chain;
assign chain[0]=state;
for(genvar i=0;i<12;i=i+1) begin: g
MISTRAL_ALUT2 #(.LUT(4'h6)) logic_cell(.A(chain[i]),.B(1'b0),.Q(chain[i+1]));
end
always @(posedge clk) state <= chain[12];
endmodule
""")
    library = args.yosys.resolve().parent.parent / "share/yosys/intel_alm/common/alm_sim.v"
    run([str(args.yosys.resolve()), "-p", f'read_verilog -lib "{library}"; '
         f'read_verilog -sv "{root / "one-endpoint.v"}"; synth_intel_alm -top top; '
         f'write_json "{root / "one-endpoint.json"}"'], "one-endpoint-synth")
    command[command.index("--json") + 1] = str(root / "one-endpoint.json")
    command[command.index("--output") + 1] = str(root / "improvement")
    run(command, "improvement", codes=(1,))
    chosen = feedback.load(root / "improvement/best.json")
    assert chosen["qualified"] and not chosen["timing_pass"] and chosen["bundle"] != "baseline"
    base_dir = root / "improvement/baseline"
    chosen_dir = root / "improvement" / chosen["bundle"]
    original = feedback.read_observation(base_dir, 1)
    improved = feedback.read_observation(chosen_dir, 1)
    feedback.compare(original, improved)
    # Round two must reconstruct a measured first-round winner exactly.
    second = feedback.load(root / "improvement/round-2-proposals/native/proposals.json")
    first_round = [p for p in (root / "improvement").glob("round-1-candidate-*/native/placement.json")]
    assert any(feedback.load(p)["current"] == second["parent"] for p in first_round)
    gain = dict(original_score_ps=feedback.score(original), retained_score_ps=feedback.score(improved),
                decision=feedback.load(root / "improvement/decision.json"), round_two_parent_matches=True)
    feedback.save(root / "checks.json", dict(commands=commands, identical_sha256=identical,
        handoff_identity=handed_checks,
        group_identity=group_checks,
        preservation_identity=dict(preserved_count=len(preservation["preserved"]),
                                   identical_sha256=preservation_hashes, final_trees_match=True,
                                   failing_replay_preserved_count=len(failed_preservation["preserved"]),
                                   failing_replay_rows_identical=True),
        baseline_coverage_complete=True, decisions=decision, measured_improvement=gain,
        nextpnr_sha256=feedback.digest(args.nextpnr),
        untouched_sha256=feedback.digest(args.baseline) if args.baseline else None))
    print(json.dumps(dict(identical_artifacts=list(identical), decision=decision, measured_improvement=gain), indent=2))


if __name__ == "__main__":
    main()
