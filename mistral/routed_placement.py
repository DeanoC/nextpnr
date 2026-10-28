#!/usr/bin/env python3
"""Bounded placement feedback using complete native routed observations.

Every worker starts from immutable synthesis and constraints. No timing failure
override or GPU-job timeout is used. A selected failing design remains a failure.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys


class Rejected(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise Rejected(message)


def load(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key: " + key)
            result[key] = value
        return result
    def constant(value):
        raise Rejected("non-finite JSON value: " + value)
    return json.loads(Path(path).read_text(), object_pairs_hook=unique, parse_constant=constant)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value, atomic=False):
    path = Path(path)
    target = path.with_name(path.name + ".next") if atomic else path
    with target.open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")
    if atomic:
        os.replace(target, path)


def integer(value):
    require(type(value) is int, "native picosecond value must be an integer")
    return value


def finite(value):
    require(type(value) in (int, float) and math.isfinite(value), "invalid numeric observation")
    return value


def keyed(entries, fields):
    require(type(entries) is list, "missing native inventory")
    result = {}
    for entry in entries:
        require(type(entry) is dict and all(k in entry for k in fields), "invalid inventory entry")
        key = tuple(entry[k] for k in fields)
        require(key not in result, "duplicate native inventory key")
        result[key] = entry
    return result


def verify_bundle(directory, exit_code=None):
    directory = Path(directory)
    manifest = load(directory / "result.json")
    require(type(manifest.get("exit_code")) is int and type(manifest.get("sha256")) is dict,
            "invalid worker result")
    if exit_code is not None:
        require(manifest["exit_code"] == exit_code, "worker exit result mismatch")
    paths = {str(p.relative_to(directory)): p for p in directory.rglob("*")
             if p.is_file() and p != directory / "result.json"}
    require(paths.keys() == manifest["sha256"].keys(), "worker artifact inventory changed")
    require(all(not p.is_symlink() and digest(p) == manifest["sha256"][name] for name, p in paths.items()),
            "worker artifact changed")
    for name, expected in manifest.get("dependencies_sha256", {}).items():
        require(Path(name).is_file() and digest(name) == expected, "worker plan/model dependency changed")
    for name, expected in manifest.get("inputs_sha256", {}).items():
        require(Path(name).name == name, "invalid frozen input name")
        path = directory.parent / "inputs" / name
        require(path.is_file() and digest(path) == expected, "frozen worker input changed")
    return manifest


def read_observation(directory, exit_code):
    directory = Path(directory)
    status = load(directory / "native/completion.json")
    for key in ("process_complete", "route_complete", "analysis_complete", "graph_matches", "coverage_complete"):
        require(status.get(key) is True, "native " + key + " is not complete")
    verify_bundle(directory, exit_code)
    inventory = load(directory / "native/endpoints.json")
    placement = load(directory / "native/placement.json")
    timing = load(directory / "timing.json")
    model = load(directory / "native/model.json")
    observed = load(directory / "native/routed-identity.json")
    require(type(load(directory / "routed.json")) is dict, "incomplete routed JSON artifact")
    require(integer(status.get("rbf_bytes")) == (directory / "core.rbf").stat().st_size > 0,
            "incomplete RBF artifact")
    require(status.get("version") == 2 and inventory.get("version") == 2 and placement.get("version") == 1,
            "unsupported observation format")
    require(inventory.get("analysis_complete") is True and inventory.get("final_analogue_model") is True and
            inventory.get("complete") is True and inventory.get("unsupported") == [],
            "incomplete native endpoint coverage")
    require(timing.get("timing_summary", {}).get("final_analogue_model") is True, "table-only timing report")
    calibrated = model.get("cohort_route_model", {})
    require(model.get("timing_summary", {}).get("final_analogue_model") is True and
            calibrated.get("version") == 1 and type(calibrated.get("identity")) is dict and calibrated["identity"] and
            type(calibrated.get("arcs")) is list and calibrated["arcs"] and model.get("fmax") == timing.get("fmax"),
            "incomplete measured calibration artifact")
    require(type(status.get("timing_pass")) is bool, "missing native timing result")
    before = integer(status.get("errors_before_signoff"))
    after = integer(status.get("errors_after_signoff"))
    finished = integer(status.get("errors_at_completion"))
    require(before == 0 and finished == after, "worker errors outside final timing")
    require((exit_code == 0 and status["timing_pass"] and after == 0) or
            (exit_code == 1 and not status["timing_pass"] and after > 0), "unclassified worker termination")
    groups = {}
    all_keys = set()
    for name in ("endpoints", "static_endpoints", "inactive_endpoints", "clock_inputs"):
        groups[name] = keyed(inventory.get(name), ("cell", "port"))
        require(not all_keys.intersection(groups[name]), "overlapping endpoint classifications")
        all_keys.update(groups[name])
    require(integer(inventory.get("expected_endpoint_count")) == len(all_keys), "incomplete graph inventory")
    require(groups["endpoints"], "no registered endpoint coverage")
    rows = {}
    for endpoint, entry in groups["endpoints"].items():
        require(type(entry.get("rows")) is list and entry["rows"], "missing endpoint rows")
        for row in entry["rows"]:
            require(type(row.get("launch")) is list and len(row["launch"]) == 2 and
                    type(row.get("capture")) is list and len(row["capture"]) == 2, "invalid clock identity")
            for clock in (row["launch"], row["capture"]):
                require(type(clock[0]) is str and clock[0] and type(clock[1]) is int and clock[1] in (0, 1),
                        "invalid native clock/edge")
            key = (*endpoint, *row["launch"], *row["capture"])
            require(key not in rows, "duplicate endpoint clock pair")
            require(type(row.get("setup_timed")) is bool and type(row.get("hold_related")) is bool,
                    "missing timing classification")
            for name in ("max_path_ps", "min_path_ps"):
                integer(row.get(name))
            if row["setup_timed"]:
                require(integer(row.get("setup_window_ps")) > 0, "invalid setup window")
                integer(row.get("setup_margin_ps"))
            else:
                require(row.get("setup_window_ps") is None and row.get("setup_margin_ps") is None,
                        "invented unrelated setup margin")
            if row["hold_related"]:
                integer(row.get("hold_margin_ps"))
            else:
                require(row.get("hold_margin_ps") is None, "invented unrelated hold margin")
            rows[key] = row
    for name in ("static_endpoints", "inactive_endpoints", "clock_inputs"):
        require(all(type(e.get("proof")) is list and e["proof"] and "rows" not in e
                    for e in groups[name].values()), "missing native structural proof")
    require(any(row["setup_timed"] for row in rows.values()), "no timed setup rows")
    current = placement.get("current", {})
    require(type(current.get("design", {}).get("ports")) is dict, "missing native top-level port contract")
    if current.get("architecture", {}).get("group_moves") is True:
        require(status.get("group_placement_match") is True and observed.get("placement") == current.get("placement"),
                "group placement changed or omitted its final binding proof")
        if "group_plan" in placement:
            require(type(placement["group_plan"]) is dict and
                    placement["group_plan"].get("origin") == placement["origin"] and
                    group_target(placement["group_plan"]) == current, "group plan does not match its native placement")
        else:
            require(current == placement["origin"], "group placement omitted its request")
    else:
        require("group_placement_match" not in status and "group_plan" not in placement,
                "unexpected group placement proof")
    if current.get("architecture", {}).get("preserve_routes") is True:
        routes = load(directory / "native/routes.json")
        require(status.get("preserved_routes_match") is True and routes.get("version") == 1 and
                routes.get("parent") == current and type(routes.get("nets")) is dict,
                "missing or mismatched preserved-route observation")
        costs = calibrated.get("route_costs", {})
        require(costs.get("version") == 3 and costs.get("routes") == routes and
                type(costs.get("background")) is dict and
                type(costs.get("reservations")) is dict and
                type(costs.get("calibrated")) is bool and finite(costs.get("prior")) > 0 and
                type(costs.get("observed")) is dict and type(costs.get("types")) is list and
                len(costs["types"]) == 256,
                "missing or mismatched native routing-cost calibration")
    requested = placement.get("data_routes")
    if requested is not None:
        require(type(requested) is dict, "invalid proposed-route request")
        require(current.get("architecture", {}).get("preserve_routes") is True and
                requested.get("version") == 1 and requested.get("target") == current and
                type(requested.get("nets")) is dict and status.get("proposed_routes_match") is True,
                "missing or stale proposed-route handoff")
        handed = load(directory / "native/route-handoff.json")
        require(handed.get("version") == 1 and handed.get("target") == current and
                type(handed.get("nets")) is dict and handed["nets"].keys() == requested["nets"].keys(),
                "incomplete proposed-route witness")
        def tree(value):
            require(type(value) is list and all(type(row) is list and len(row) == 2 and
                    all(type(item) is str for item in row) for row in value), "invalid proposed wire tree")
            result = dict(value)
            require(len(result) == len(value), "duplicate proposed wire")
            return result
        for name, expected in requested["nets"].items():
            actual = routes["nets"].get(name, {})
            proof = handed["nets"][name]
            require(type(expected) is dict and type(actual) is dict and type(proof) is dict,
                    "invalid proposed-route record")
            physical = actual.get("physical", {})
            require(physical == proof.get("physical") and physical.get("source") == expected.get("source") and
                    type(expected.get("sinks")) is list and all(type(sink) is str for sink in expected["sinks"]) and
                    set(physical.get("sinks", [])) == set(expected["sinks"]) and
                    tree(actual.get("wires")) == tree(proof.get("wires")) == tree(expected.get("wires")),
                    "proposed route changed after handoff")
    else:
        require("proposed_routes_match" not in status, "unexpected proposed-route completion")
    command_path = directory / "command.json"
    command = load(command_path) if command_path.exists() else []
    branches_requested = "--routed-placement-preserve-branches" in command
    if branches_requested:
        require(current.get("architecture", {}).get("preserve_routes") is True and
                current.get("architecture", {}).get("group_moves") is True and
                status.get("preserved_branches_match") is True, "missing branch-preservation completion")
        proof = load(directory / "native/route-branches.json")
        parent = placement.get("group_plan", {}).get("parent", current)
        require(proof.get("version") == 1 and proof.get("parent") == parent and proof.get("target") == current and
                type(proof.get("nets")) is dict, "stale branch-preservation contract")
        imported = None
        if "--routed-placement-routes" in command:
            source_path = command[command.index("--routed-placement-routes") + 1]
            dependencies = load(directory / "result.json").get("dependencies_sha256", {})
            require(source_path in dependencies and digest(source_path) == dependencies[source_path],
                    "unsealed branch route source")
            imported = load(source_path)
            require(imported.get("parent") == parent, "changed branch source parent")
        else:
            require(not proof["nets"], "branches without a parent route source")
        for name, branch in proof["nets"].items():
            source = imported["nets"].get(name)
            actual = routes["nets"].get(name, {})
            require(type(branch) is dict and branch.get("source") == source and type(source) is dict and
                    branch.get("physical") == actual.get("physical"), "branch physical/source contract changed")
            def bindings(value):
                require(type(value) is list and all(type(row) is list and len(row) == 2 and
                        all(type(item) is str for item in row) for row in value), "invalid branch wire tree")
                result = dict(value)
                require(len(result) == len(value), "duplicate branch wire")
                return result
            saved, final, prefix = bindings(source["wires"]), bindings(actual.get("wires")), bindings(branch.get("wires"))
            require(branch["physical"].get("source") in final and
                    all(sink in final for sink in branch["physical"].get("sinks", [])),
                    "incomplete extended branch route")
            require(prefix and all(saved.get(wire) == pip and final.get(wire) == pip for wire, pip in prefix.items()),
                    "protected branch edge changed")
            require(type(branch.get("sinks")) is list and branch["sinks"] and
                    len(set(branch["sinks"])) == len(branch["sinks"]) and
                    all(sink in prefix and sink in branch["physical"]["sinks"] for sink in branch["sinks"]),
                    "missing preserved branch sink")
    else:
        require("preserved_branches_match" not in status and not (directory / "native/route-branches.json").exists(),
                "unexpected branch-preservation proof")
    require(current.get("design") == observed.get("design") and current.get("clocks") == observed.get("clocks"),
            "routed logical contract mismatch")
    require(current.get("design") and placement.get("origin", {}).get("design"), "empty logical identity")
    require(timing.get("fmax"), "missing clock Fmax inventory")
    for clock in timing["fmax"].values():
        require(finite(clock.get("constraint")) > 0 and finite(clock.get("achieved")) > 0, "invalid Fmax entry")
    primitive = keyed(inventory.get("primitive_clocks"), ("cell", "port"))
    for row in primitive.values():
        require(type(row.get("clock")) is str and row["clock"], "missing primitive clock")
        for name in ("actual_ps", "required_ps"):
            require(type(row.get(name)) is list and len(row[name]) == 3, "invalid primitive waveform")
            for value in row[name]:finite(value)
        for actual, minimum in zip(row["actual_ps"], row["required_ps"]):
            require(minimum >= 0 and (minimum == 0 or actual >= minimum), "invalid primitive clock margin")
    return {"directory": directory, "status": status, "inventory": inventory, "placement": placement,
            "timing": timing, "groups": groups, "rows": rows, "primitive": primitive}


def score(observation):
    margins = [integer(r["setup_margin_ps"]) for r in observation["rows"].values() if r["setup_timed"]]
    require(margins, "no timed setup rows")
    return min(margins), sum(min(0, value) for value in margins)


def group_target(plan):
    require(type(plan) is dict and set(plan) == {"version", "origin", "parent", "moves", "evaluation"} and
            plan["version"] == 3 and plan["evaluation"] == "full_route_required" and
            type(plan["origin"]) is dict and type(plan["moves"]) is list and len(plan["moves"]) <= 256,
            "invalid full-route group plan")
    origin = plan["origin"]
    require(origin.get("architecture", {}).get("group_moves") is True and
            type(origin.get("placement")) is dict, "group plan lacks its explicit recipe")
    target = {**origin, "placement": dict(origin["placement"])}
    seen = set()
    for row in plan["moves"]:
        require(type(row) is dict and set(row) == {"cell", "from_bel", "to_bel", "strength"} and
                type(row["cell"]) is str and row["cell"] not in seen and
                type(row["from_bel"]) is str and type(row["to_bel"]) is str and row["to_bel"],
                "invalid group plan move")
        seen.add(row["cell"])
        old = origin["placement"].get(row["cell"])
        require(type(old) is list and len(old) == 2 and old == [row["from_bel"], integer(row["strength"])],
                "stale group plan binding")
        target["placement"][row["cell"]] = [row["to_bel"], row["strength"]]
    return target


def compare(parent, candidate):
    require(parent["placement"]["origin"] == candidate["placement"]["origin"], "different fresh placement/recipe")
    for key in ("design", "clocks", "timing_settings", "architecture", "ordering", "rng"):
        require(parent["placement"]["current"][key] == candidate["placement"]["current"][key],
                "changed logical/timing contract: " + key)
    require(parent["rows"].keys() == candidate["rows"].keys(), "changed endpoint/clock-pair coverage")
    for name in ("static_endpoints", "inactive_endpoints", "clock_inputs"):
        require(parent["groups"][name] == candidate["groups"][name], "changed native proof inventory")
    for name in ("endpoints",):
        for key, entry in parent["groups"][name].items():
            require(candidate["groups"][name].get(key, {}).get("class") == entry.get("class"),
                    "changed endpoint classification")
    for key, old in parent["rows"].items():
        new = candidate["rows"][key]
        for field in ("launch", "capture", "setup_timed", "hold_related", "setup_window_ps"):
            require(old[field] == new[field], "changed clock relationship")
        if old["setup_timed"]:
            require(new["setup_margin_ps"] >= old["setup_margin_ps"], "setup endpoint regression")
        else:
            require(new["max_path_ps"] <= old["max_path_ps"] and new["min_path_ps"] >= old["min_path_ps"],
                    "unrelated path extrema regression")
        if old["hold_related"]:
            floor = old["hold_margin_ps"]
            require(new["hold_margin_ps"] > 0 if floor > 0 else new["hold_margin_ps"] >= floor,
                    "new or worse related hold violation")
    require(parent["primitive"].keys() == candidate["primitive"].keys(), "changed primitive clock inventory")
    for key, old in parent["primitive"].items():
        new = candidate["primitive"][key]
        for field in ("clock", "period_ps", "high_ps", "low_ps", "required_ps"):
            require(old[field] == new[field], "changed primitive clock requirement")
        for i, minimum in enumerate(old["required_ps"]):
            if minimum:
                require(new["actual_ps"][i] >= old["actual_ps"][i], "primitive clock margin regression")
    require(parent["timing"]["fmax"].keys() == candidate["timing"]["fmax"].keys(), "changed Fmax clock set")
    for key, old in parent["timing"]["fmax"].items():
        new = candidate["timing"]["fmax"][key]
        require(new["constraint"] == old["constraint"] and new["achieved"] >= old["achieved"],
                "clock Fmax regression")
    a, b = score(parent), score(candidate)
    require(b[0] >= a[0] + 20 or (b[0] == a[0] and b[1] >= a[1] + 20), "no measured 20 ps setup gain")
    return b


def validate_recipe(extra):
    # No file-bearing options, callbacks or second transformation pass: their
    # mutable inputs would escape the frozen worker input bundle.
    flags = {"--routed-placement-preserve-branches", "--arrival-pin-assignment", "--isolated-lut-placement", "--mistral-ff4", "--mistral-clkb",
             "--mistral-dsp-signed-lanes", "--compress-rbf", "--detailed-timing-report", "--gpu-perf",
             "--gpu-required", "--placer-heap-no-ctrl-set"}
    numbers = {"--placer-heap-alpha", "--placer-heap-beta", "--placer-heap-critexp", "--placer-heap-timingweight",
               "--placer-heap-cell-placement-timeout", "--gpu-batches", "--timing-report-paths"}
    tokens = iter(extra)
    for arg in tokens:
        name, equal, value = arg.partition("=")
        if name in flags:
            require(not equal, "flag has an unexpected value: " + name)
            continue
        require(name in numbers or name == "--gpu-opt", "unsupported recipe override: " + name)
        if not equal:
            value = next(tokens, "")
        if name == "--gpu-opt":
            key, equal, value = value.partition("=")
            require(equal and key and key not in ("device", "cpu", "required") and "telemetry" not in key.lower(),
                    "unsupported GPU tuning override")
        try:
            finite(float(value))
        except ValueError:
            raise Rejected("recipe tuning values must be finite numbers")


class Workers:
    def __init__(self, args, extra_inputs=()):
        self.args = args
        self.root = args.output.resolve()
        self.root.mkdir(parents=True, exist_ok=False)
        inputs = self.root / "inputs"
        inputs.mkdir()
        self.files = {}
        for name, source in (("synth.json", args.json), ("pins.qsf", args.qsf), ("clocks.sdc", args.sdc),
                             ("nextpnr-mistral", args.nextpnr), ("controller.py", Path(__file__)), *extra_inputs):
            require(Path(name).name == name and name not in self.files, "duplicate or invalid frozen input name")
            target = inputs / name
            shutil.copy2(source.resolve(), target)
            self.files[name] = target
        design = load(self.files["synth.json"])
        for module in design.get("modules", {}).values():
            require(not module.get("attributes", {}).get("step"), "input is a placement/routing checkpoint")
            for key, value in module.get("settings", {}).items():
                if key in ("timing/allowFail", "timing/ignoreLoops", "timing/ignoreRelClk"):
                    require(value in (False, 0, "0", "false"), "saved timing failure override")
        self.hashes = {name: digest(path) for name, path in self.files.items()}
        save(self.root / "inputs.json", {"sha256": self.hashes, "device": args.device, "seed": args.seed,
                                        "freq": args.freq, "extra_args": args.extra_arg,
                                        "cpu_reference": args.gpu_cpu, "rounds": args.rounds,
                                        "preserve_routes": args.preserve_routes,
                                        "candidates": args.candidates, "trials": args.trials,
                                        "group_moves": args.group_moves})
        self.base = [str(self.files["nextpnr-mistral"]), "--device", args.device, "--json", str(self.files["synth.json"]),
                     "--qsf", str(self.files["pins.qsf"]), "--sdc", str(self.files["clocks.sdc"]), "--freq", str(args.freq),
                     "--seed", str(args.seed), "--placer", "heap", "--router", "gpu", *args.extra_arg]
        if args.preserve_routes:
            self.base.append("--routed-placement-preserve-routes")
        if args.group_moves:
            self.base.append("--routed-placement-group-moves")
        if args.gpu_cpu:
            self.base.append("--gpu-cpu")
        else:
            require(shutil.which("gpu-run"), "gpu-run is required for device routing")

    def run(self, label, plan=None, model=None, routes=None, check_only=False, retarget_luts=False):
        require(self.hashes == {name: digest(path) for name, path in self.files.items()}, "immutable input changed")
        directory = self.root / label
        directory.mkdir(exist_ok=False)
        require(routes is None or (self.args.preserve_routes and plan is not None and model is None),
                "route preservation requires a full candidate route")
        require(not check_only or (self.args.group_moves and plan is not None and model is None and routes is None),
                "plan checking requires only an explicit group plan")
        require(not retarget_luts or (self.args.group_moves and plan is not None and model is not None and not check_only),
                "LUT retargeting requires an explicit group plan and matching model")
        dependencies = {str(p): digest(p) for p in (plan, model, routes) if p is not None}
        command = self.base + ["--routed-placement-record", str(directory / "native")]
        if plan is not None:
            command += ["--routed-placement-plan", str(plan)]
        if routes is not None:
            command += ["--routed-placement-routes", str(routes)]
        if check_only:
            command += ["--no-route", "--routed-placement-check-plan"]
        elif model is not None:
            command += ["--no-route", "--routed-placement-propose", str(model), "--routed-placement-trials",
                        str(self.args.trials), "--routed-placement-candidates", str(self.args.candidates)]
            if retarget_luts: command.append("--routed-placement-retarget-luts")
        else:
            command += ["--rbf", str(directory / "core.rbf"), "--write", str(directory / "routed.json"),
                        "--report", str(directory / "timing.json")]
            if not self.args.gpu_cpu:
                command = ["gpu-run", "--phase-lease", "--", *command]
        save(directory / "command.json", command)
        print(label, flush=True)
        with (directory / "run.log").open("x") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        require(self.hashes == {name: digest(path) for name, path in self.files.items()}, "immutable input changed")
        require(dependencies == {name: digest(name) for name in dependencies}, "worker dependency changed")
        artifacts = {str(p.relative_to(directory)): digest(p) for p in directory.rglob("*") if p.is_file()}
        save(directory / "result.json", {"exit_code": result.returncode, "sha256": artifacts,
                                          "inputs_sha256": self.hashes, "dependencies_sha256": dependencies})
        return directory, result.returncode


def optimize(workers, rounds, candidates, preserve_routes=False, group_moves=False):
    root = workers.root
    directory, code = workers.run("baseline")
    best_plan = None
    best = None
    decisions = []

    def retain(directory, observation, plan):
        verify_bundle(directory)
        save(root / "best.json", {"bundle": str(directory.relative_to(root)),
                                 "bundle_sha256": digest(directory / "result.json"),
                                 "plan": str(plan.relative_to(root)) if plan else None,
                                 "plan_sha256": digest(plan) if plan else None,
                                 "qualified": observation is not None,
                                 "timing_pass": observation["status"]["timing_pass"] if observation else False}, atomic=True)

    retain(directory, None, None)
    try:
        best = read_observation(directory, code)
        retain(directory, best, None)
    except (Rejected, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        save(root / "decision.json", {"stopped": "baseline cannot authorize refinement", "reason": str(error)})
        print("Baseline retained without refinement: " + str(error), flush=True)
        return False
    for round_index in range(1, rounds + 1):
        if best["status"]["timing_pass"]:
            break
        verify_bundle(best["directory"])
        proposal_dir, code = workers.run(f"round-{round_index}-proposals", best_plan,
                                         best["directory"] / "native/model.json")
        if code != 0:
            decisions.append({"round": round_index, "stopped": "proposal worker failed", "exit_code": code})
            break
        verify_bundle(proposal_dir, code)
        proposal = load(proposal_dir / "native/proposals.json")
        require(proposal.get("version") == (2 if group_moves else 1) and proposal.get("restored") is True and
                proposal.get("parent") == best["placement"]["current"] and type(proposal.get("plans")) is list and
                len(proposal["plans"]) <= candidates, "invalid or stale proposal set")
        if group_moves:
            require(proposal.get("kind") == "group_moves", "missing explicit group proposal family")
        winner = None
        for index, plan in enumerate(proposal["plans"]):
            require(plan.get("origin") == best["placement"]["origin"] and
                    plan.get("parent") == best["placement"]["current"], "proposal parent identity mismatch")
            if group_moves:
                expected_group_target = group_target(plan)
            elif preserve_routes:
                require(plan.get("version") == 2 and type(plan.get("data_routes")) is dict,
                        "preserved proposal dropped its selected trees")
            plan_path = root / f"round-{round_index}-plan-{index}.json"
            save(plan_path, plan)
            routes = best["directory"] / "native/routes.json" if preserve_routes else None
            trial_dir, code = workers.run(f"round-{round_index}-candidate-{index}", plan_path, routes=routes)
            decision = {"round": round_index, "candidate": index, "bundle": str(trial_dir.relative_to(root))}
            try:
                candidate = read_observation(trial_dir, code)
                if group_moves:
                    require(candidate["placement"].get("group_plan") == plan and
                            candidate["placement"]["current"] == expected_group_target,
                            "candidate omitted or changed its group plan")
                elif preserve_routes:
                    require(candidate["placement"].get("data_routes") == plan["data_routes"],
                            "candidate omitted or changed the proposed route request")
            except (Rejected, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
                decisions.append({**decision, "eligible": False, "stopped": "incomplete or unexpected worker result",
                                  "reason": str(error)})
                winner = None
                break
            try:
                value = compare(best, candidate)
                decision.update(eligible=True, score=value)
                if winner is None or value > winner[0]:
                    winner = value, candidate, plan_path
            except (Rejected, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
                decision.update(eligible=False, reason=str(error))
            decisions.append(decision)
        if winner is None:
            break
        _, best, best_plan = winner
        retain(best["directory"], best, best_plan)
    save(root / "decision.json", {"trials": decisions, "selected": str(best["directory"].relative_to(root)),
                                 "score_ps": score(best), "timing_pass": best["status"]["timing_pass"]})
    print(f"Selected {best['directory'].name}: setup WNS {score(best)[0]} ps; "
          f"timing {'passes' if best['status']['timing_pass'] else 'fails'}", flush=True)
    return best["status"]["timing_pass"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("nextpnr", "json", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--device", default="5CSEBA6U23I7")
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--freq", type=float, required=True)
    parser.add_argument("--rounds", type=int, choices=range(3), default=2)
    parser.add_argument("--candidates", type=int, choices=range(1, 5), default=4)
    parser.add_argument("--trials", type=int, choices=range(1, 65), default=64)
    parser.add_argument("--extra-arg", action="append", default=[])
    parser.add_argument("--gpu-cpu", action="store_true", help="explicitly use the GPU algorithm's CPU reference")
    parser.add_argument("--preserve-routes", action="store_true", help="preserve validated unchanged data trees (opt-in)")
    parser.add_argument("--group-moves", action="store_true", help="evaluate legal logic/register groups by full routing (opt-in)")
    args = parser.parse_args()
    try:
        require(args.seed >= 0 and math.isfinite(args.freq) and args.freq > 0, "invalid seed or frequency")
        validate_recipe(args.extra_arg)
        return 0 if optimize(Workers(args), args.rounds, args.candidates, args.preserve_routes, args.group_moves) else 1
    except (Rejected, OSError, ValueError, KeyError, TypeError) as error:
        print("Routed placement stopped: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
