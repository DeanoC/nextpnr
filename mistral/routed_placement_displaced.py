#!/usr/bin/env python3
"""Retarget displaced LUTs, qualifying only complete fresh routed results."""
import argparse
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True
import routed_placement as feedback
import routed_placement_refine as refine

require, load, save, digest = feedback.require, feedback.load, feedback.save, feedback.digest
LUT_TYPES = {"MISTRAL_ALUT2", "MISTRAL_ALUT3", "MISTRAL_ALUT4", "MISTRAL_ALUT5", "MISTRAL_ALUT6", "MISTRAL_NOT"}


def validate_plan(original, source, plan):
    require(plan.get("origin") == original and plan.get("parent") == original,
            "LUT alternatives must replay from the original baseline")
    target = feedback.group_target(plan)
    require({k:v for k,v in source.items() if k != "placement"} ==
            {k:v for k,v in original.items() if k != "placement"}, "changed source logical/timing identity")
    require(source["placement"].keys() == target["placement"].keys(), "changed source placement coverage")
    changed = [name for name, value in source["placement"].items() if target["placement"][name] != value]
    require(len(changed) == 1, "a displaced LUT alternative must change exactly one cell")
    name = changed[0]; cell = original["design"]["cells"][name]
    require(cell["type"] in LUT_TYPES and not cell["cluster"], "alternative moves a register, cluster or unsupported cell")
    require(target["placement"][name][1] == source["placement"][name][1], "alternative changes binding strength")
    require(all(value[0] != target["placement"][name][0] for other, value in source["placement"].items() if other != name),
            "alternative evicts another cell")
    refine.movement_units(plan)
    return name


def search(args):
    parent_path, source_path = args.parent.resolve(), args.candidate.resolve()
    require(parent_path.parent == source_path.parent, "source bundles must share frozen inputs")
    pm, cm = feedback.verify_bundle(parent_path), feedback.verify_bundle(source_path)
    require(pm.get("inputs_sha256") == cm.get("inputs_sha256") and pm.get("inputs_sha256"), "different source worker inputs")
    parent = feedback.read_observation(parent_path, pm["exit_code"])
    source = feedback.read_observation(source_path, cm["exit_code"])
    plan = source["placement"].get("group_plan")
    require(type(plan) is dict and plan["origin"] == plan["parent"] == parent["placement"]["current"] ==
            parent["placement"]["origin"], "search requires an original-parent group candidate")
    require(feedback.group_target(plan) == source["placement"]["current"], "source group target changed")
    recipe = load(parent_path.parent/"inputs.json"); inputs = parent_path.parent/"inputs"
    require(recipe["group_moves"], "source is not an explicit group experiment")
    feedback.validate_recipe(recipe["extra_args"])
    worker_args = argparse.Namespace(output=args.output, nextpnr=args.nextpnr, json=inputs/"synth.json",
        qsf=inputs/"pins.qsf", sdc=inputs/"clocks.sdc", device=recipe["device"], seed=recipe["seed"], freq=recipe["freq"],
        extra_arg=recipe["extra_args"], preserve_routes=recipe["preserve_routes"], group_moves=True,
        gpu_cpu=recipe["cpu_reference"], rounds=0, candidates=max(1, args.routes-1), trials=args.trials)
    workers = feedback.Workers(worker_args, extra_inputs=(("search.py", Path(__file__)), ("refiner.py", Path(refine.__file__))))
    args.created_output = True
    root = workers.root
    save(root/"search.json", dict(version=1, source_parent=str(parent_path), source_candidate=str(source_path),
        parent_manifest_sha256=digest(parent_path/"result.json"), candidate_manifest_sha256=digest(source_path/"result.json"),
        route_limit=args.routes, trial_limit=args.trials))
    directory, code = workers.run("baseline"); baseline = feedback.read_observation(directory, code)
    physical = ("core.rbf", "routed.json", "timing.json", "native/endpoints.json")
    require(baseline["placement"]["current"] == parent["placement"]["current"] and
            all(digest(directory/name) == digest(parent_path/name) for name in physical), "fresh baseline physical result changed")
    best, best_plan = baseline, None
    def retain():
        save(root/"best.json", dict(bundle=str(best["directory"].relative_to(root)),
            bundle_sha256=digest(best["directory"]/"result.json"),
            plan=str(best_plan.relative_to(root)) if best_plan else None,
            plan_sha256=digest(best_plan) if best_plan else None, qualified=True, timing_pass=best["status"]["timing_pass"]), atomic=True)
    retain(); control_path = root/"control-plan.json"; save(control_path, plan)
    routes = directory/"native/routes.json" if recipe["preserve_routes"] else None
    directory, code = workers.run("control", plan=control_path, routes=routes)
    control = feedback.read_observation(directory, code)
    require(control["placement"]["group_plan"] == plan and
            all(digest(directory/name) == digest(source_path/name) for name in physical), "source control physical replay changed")
    decisions = []
    def consider(observed, path, metadata):
        nonlocal best, best_plan
        decision = dict(bundle=observed["directory"].name, score_ps=feedback.score(observed), **metadata)
        try:
            value = feedback.compare(baseline, observed); decision["eligible"] = True
            if best is baseline or value > feedback.score(best): best, best_plan = observed, path; retain()
        except feedback.Rejected as error: decision.update(eligible=False, reason=str(error))
        save(root/(observed["directory"].name+"-path-changes.json"), dict(diagnostic_only=True,
             paths=refine.path_changes(baseline, observed)))
        decisions.append(decision)
    consider(control, control_path, dict(control=True))
    proposal_count = 0; full_routes = 1
    if args.routes > 1:
        directory, code = workers.run("proposals", plan=control_path, model=control["directory"]/"native/model.json", retarget_luts=True)
        require(code == 0, "native LUT search failed")
        feedback.verify_bundle(directory, 0)
        require(not (directory/"native/completion.json").exists(), "proposal worker made a route-completion claim")
        proposal, trials = load(directory/"native/proposals.json"), load(directory/"native/displaced-lut-trials.json")
        require(proposal.get("version") == 2 and proposal.get("kind") == "displaced_luts" and
                proposal.get("parent") == control["placement"]["current"] and
                proposal.get("replay_parent") == baseline["placement"]["current"] and proposal.get("restored") is True,
                "stale displaced LUT proposals")
        require(trials.get("restored") is True and trials["attempted"] <= 128 and
                trials["legal"] == len(trials["trials"]) <= args.trials, "unbounded native LUT search")
        require(len(proposal["plans"]) <= args.routes-1, "too many full-route alternatives")
        proposal_count = len(proposal["plans"])
        for index, alternative in enumerate(proposal["plans"]):
            cell = validate_plan(baseline["placement"]["current"], control["placement"]["current"], alternative)
            trace = [row for row in trials["trials"] if row["plan_index"] == index]
            require(len(trace) == 1 and trace[0]["cell"] == cell and
                    trace[0]["evaluation"] == "full_route_required" and
                    feedback.finite(trace[0]["boundary_cost_after"]) >= 0 and
                    feedback.finite(trace[0]["boundary_cost_before"]) >= 0, "invalid native LUT trial trace")
            path = root/f"plan-{index}.json"; save(path, alternative)
            directory, code = workers.run(f"candidate-{index}", plan=path, routes=routes); full_routes += 1
            observed = feedback.read_observation(directory, code)
            require(observed["placement"].get("group_plan") == alternative and
                    observed["placement"]["current"] == feedback.group_target(alternative), "final LUT alternative changed")
            consider(observed, path, dict(control=False, cell=cell))
    save(root/"decision.json", dict(trials=decisions, proposals=proposal_count, full_routes=full_routes,
        selected=best["directory"].name, score_ps=feedback.score(best), timing_pass=best["status"]["timing_pass"]))
    print(json.dumps(dict(selected=best["directory"].name, score_ps=feedback.score(best), proposals=proposal_count,
                         full_routes=full_routes), indent=2))
    return best["status"]["timing_pass"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("parent", "candidate", "nextpnr", "output"): parser.add_argument("--"+name, type=Path, required=True)
    parser.add_argument("--routes", type=int, choices=range(1,5), default=4, help="full routes including source control")
    parser.add_argument("--trials", type=int, choices=range(1,65), default=64, help="native legal inspections")
    args = parser.parse_args()
    args.created_output = False
    try: return 0 if search(args) else 1
    except (feedback.Rejected, OSError, ValueError, KeyError, TypeError) as error:
        if args.created_output and (args.output/"best.json").exists():
            save(args.output/"stopped.json", dict(reason=str(error), retained=load(args.output/"best.json")))
        print("Displaced LUT search stopped: "+str(error), file=sys.stderr); return 1


if __name__ == "__main__": raise SystemExit(main())
