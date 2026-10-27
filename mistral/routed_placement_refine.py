#!/usr/bin/env python3
"""Bounded ablation of a sealed group move; full routed guards select artifacts."""
import argparse
import copy
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
import routed_placement as feedback

require, load, save, digest = feedback.require, feedback.load, feedback.save, feedback.digest


def movement_units(plan):
    """Close carry membership and BEL exchange cycles without inventing moves."""
    target = feedback.group_target(plan)
    cells = plan["origin"]["design"]["cells"]
    require(type(cells) is dict, "group refinement needs the complete native cell graph")
    moves = {row["cell"]: row for row in plan["moves"]}
    require(len(moves) == len(plan["moves"]) and moves, "empty or duplicate refinement moves")
    require(len({row["to_bel"] for row in moves.values()}) == len(moves) and
            all(row["from_bel"] != row["to_bel"] for row in moves.values()), "duplicate target or unchanged move")
    groups = {}
    for name in sorted(moves):
        require(name in cells, "unknown refinement cell")
        cluster = cells[name]["cluster"]
        key = ("cluster", cluster) if cluster else ("cell", name)
        groups.setdefault(key, set()).add(name)
    for (kind, key), members in groups.items():
        if kind == "cluster":
            require(members == {name for name, cell in cells.items() if cell["cluster"] == key},
                    "refinement cannot split a carry cluster")
    atoms = list(groups.values())
    owner = {name: i for i, members in enumerate(atoms) for name in members}
    occupied = {value[0]: name for name, value in plan["origin"]["placement"].items() if value[0]}
    dependencies = [set() for _ in atoms]
    for name, move in moves.items():
        other = occupied.get(move["to_bel"])
        if other and other != name:
            require(other in moves, "refinement target has an unmoved occupant")
            if owner[name] != owner[other]: dependencies[owner[name]].add(owner[other])
    def reach(index):
        found = {index}; pending = [index]
        while pending:
            for child in dependencies[pending.pop()]:
                if child not in found: found.add(child); pending.append(child)
        return found
    reachable = [reach(i) for i in range(len(atoms))]
    remaining = set(range(len(atoms))); components = []
    while remaining:
        first = min(remaining)
        component = {i for i in remaining if i in reachable[first] and first in reachable[i]}
        components.append(component); remaining -= component
    mapped = {old: i for i, component in enumerate(components) for old in component}
    result = []
    for index, component in enumerate(components):
        members = sorted(set().union(*(atoms[i] for i in component)))
        deps = {mapped[j] for i in component for j in dependencies[i]} - {index}
        result.append(dict(id=index, cells=members, dependencies=sorted(deps),
                           registers=sum(cells[name]["type"] == "MISTRAL_FF" for name in members),
                           carry=any(cells[name]["cluster"] for name in members)))
    require(target["placement"] != plan["origin"]["placement"], "refinement move changes no placement")
    return result


def keep_closure(units, keep):
    result = set(keep); pending = list(result)
    while pending:
        for dependency in units[pending.pop()]["dependencies"]:
            if dependency not in result: result.add(dependency); pending.append(dependency)
    return frozenset(result)


def remove_closure(units, removed):
    removed = set(removed)
    while True:
        extra = {u["id"] for u in units if set(u["dependencies"]) & removed} - removed
        if not extra: break
        removed |= extra
    return frozenset(set(range(len(units))) - removed)


def subset_plan(plan, units, keep):
    require(keep_closure(units, keep) == frozenset(keep), "incomplete movement dependency closure")
    members = {name for i in keep for name in units[i]["cells"]}
    result = copy.deepcopy(plan)
    result["moves"] = [row for row in result["moves"] if row["cell"] in members]
    feedback.group_target(result)
    return result


def redirect_register_plan(plan, restored, replacement=None):
    """Restore registers and contract incoming moves through their old targets."""
    cells = plan["origin"]["design"]["cells"]
    original = {move["cell"]:move for move in plan["moves"]}
    occupied = {value[0]:name for name, value in plan["origin"]["placement"].items() if value[0]}
    restored = set(restored)
    require(restored and restored <= original.keys(), "unknown restored register")
    require(all(cells[name]["type"] == "MISTRAL_FF" and not cells[name]["cluster"] for name in restored),
            "register redirection cannot split a cluster or restore a non-register")
    if replacement is not None:
        require(replacement[0] in original and replacement[0] not in restored and
                replacement[1] in {move["from_bel"] for name, move in original.items()
                                   if cells[name]["type"] == "MISTRAL_FF" and not cells[name]["cluster"]},
                "register replacement must use a native register origin site")
    result = copy.deepcopy(plan)
    moves = []; redirected = []
    for move in result["moves"]:
        name = move["cell"]
        if name in restored: continue
        target = move["to_bel"]; visited = set()
        while occupied.get(target) in restored:
            other = occupied[target]
            require(other not in visited, "cyclic restored-register chain")
            visited.add(other); target = original[other]["to_bel"]
        if visited:
            require(cells[name]["type"] == "MISTRAL_FF" and not cells[name]["cluster"],
                    "register redirection would move a non-register or cluster")
            if replacement is not None and replacement[0] == name: target = replacement[1]
        if target == move["from_bel"]: continue
        if target != move["to_bel"]:
            redirected.append(dict(cell=name, before_bel=move["to_bel"], after_bel=target))
            move["to_bel"] = target
        moves.append(move)
    # Pure deletions already belong to the literal-subset lane.
    if not moves or not redirected: return None
    result["moves"] = moves
    movement_units(result)
    return dict(plan=result, moved_cells=len(moves), redirected=redirected,
                restored=sorted(original.keys() - {move["cell"] for move in moves}))


def register_candidates(plan, baseline, control):
    """Try chain shortcuts and rotations using only native sites in the plan."""
    movement_units(plan)
    cells = plan["origin"]["design"]["cells"]
    moves = {move["cell"]:move for move in plan["moves"]}
    occupied = {value[0]:name for name, value in plan["origin"]["placement"].items() if value[0]}
    registers = {name for name in moves if cells[name]["type"] == "MISTRAL_FF" and not cells[name]["cluster"]}
    predecessors = {occupied[move["to_bel"]]:name for name, move in moves.items()
                    if name in registers and occupied.get(move["to_bel"]) in registers}
    require(baseline["rows"].keys() == control["rows"].keys(), "changed register-search timing coverage")
    regressions = []
    for key, old in baseline["rows"].items():
        if key[0] in registers and old["setup_timed"]:
            delta = control["rows"][key]["setup_margin_ps"] - old["setup_margin_ps"]
            if delta < 0: regressions.append((old["setup_margin_ps"] >= 0, delta, key[0]))
    anchors = list(dict.fromkeys(name for _, _, name in sorted(regressions)))
    proposals = []; seen = set()
    def add(restored, family, replacement=None):
        candidate = redirect_register_plan(plan, restored, replacement)
        if candidate is None: return
        token = json.dumps(candidate["plan"]["moves"], sort_keys=True)
        if token in seen: return
        seen.add(token); proposals.append(dict(family=family, **candidate))
    for anchor in anchors:
        failing = any(not passing and name == anchor for passing, _, name in regressions)
        add({anchor}, "restore_failing_register" if failing else "restore_passing_register")
    for anchor in anchors:
        previous = predecessors.get(anchor)
        if previous is None: continue
        head = previous; visited = {anchor}
        while head in predecessors and head not in visited:
            visited.add(head); head = predecessors[head]
        if head in visited: continue  # Closed exchanges have no vacated head site.
        add({anchor}, "close_register_chain", (previous, moves[head]["from_bel"]))
    # Restore all regressed registers along each connected displacement chain.
    for anchor in anchors:
        connected = {anchor}; pending = [anchor]
        while pending:
            name = pending.pop()
            adjacent = {predecessors.get(name), occupied.get(moves[name]["to_bel"])} & registers
            for other in adjacent - connected:
                connected.add(other); pending.append(other)
        restored = connected & set(anchors)
        if len(restored) > 1: add(restored, "restore_register_chain")
    ordered = []; families = set()
    for candidate in proposals:
        if candidate["family"] not in families: ordered.append(candidate); families.add(candidate["family"])
    ordered += [candidate for candidate in proposals if candidate not in ordered]
    return ordered


def path_inventory(observation):
    table = load(observation["directory"] / "native/endpoint-paths.json")
    require(table.get("version") == 1 and table.get("diagnostic_only") is True and
            table.get("clock_skew") is True and table.get("final_analogue_model") is True,
            "missing native setup path diagnostics")
    paths = {}
    kinds = {"clk-to-clk", "clk-skew", "clk-to-q", "source", "logic", "routing", "setup", "hold"}
    for path in table["paths"]:
        key = (path["cell"], path["port"], *path["launch"], *path["capture"])
        require(key not in paths and key in observation["rows"] and observation["rows"][key]["setup_timed"],
                "unexpected or duplicate native setup path")
        require(path["setup_margin_ps"] == observation["rows"][key]["setup_margin_ps"] and
                type(path["segments"]) is list and path["segments"], "incomplete native path")
        require(all(s["type"] in kinds and type(s["delay_ps"]) is int for s in path["segments"]),
                "invalid native path segment")
        require(sum(s["delay_ps"] for s in path["segments"]) == path["path_sum_ps"], "native path sum mismatch")
        paths[key] = path
    require(paths.keys() == {key for key, row in observation["rows"].items() if row["setup_timed"]},
            "incomplete setup-path inventory")
    return paths


def path_changes(parent, candidate):
    old, new = path_inventory(parent), path_inventory(candidate)
    require(old.keys() == new.keys(), "changed diagnostic path coverage")
    entries = []
    for key in sorted(old):
        delta = new[key]["setup_margin_ps"] - old[key]["setup_margin_ps"]
        if not delta: continue
        def components(path):
            value = {}
            for s in path["segments"]: value[s["type"]] = value.get(s["type"], 0) + s["delay_ps"]
            return value
        a, b = components(old[key]), components(new[key])
        entries.append(dict(key=list(key), before_ps=old[key]["setup_margin_ps"], after_ps=new[key]["setup_margin_ps"],
            change_ps=delta, component_change_ps={kind:b.get(kind, 0)-a.get(kind, 0) for kind in sorted(a.keys() | b.keys())},
            before_path=old[key], after_path=new[key]))
    return entries


def candidates(plan, units, baseline, control, active=None):
    """Prioritize regressed moved endpoints, then coarse register/core ablations."""
    all_units = frozenset(range(len(units)))
    active = all_units if active is None else frozenset(active)
    owner = {cell:u["id"] for u in units if u["id"] in active for cell in u["cells"]}
    paths = path_inventory(control)
    regressions = []
    for key, old in baseline["rows"].items():
        if old["setup_timed"]:
            change = control["rows"][key]["setup_margin_ps"] - old["setup_margin_ps"]
            if change < 0: regressions.append((old["setup_margin_ps"] >= 0, change, key))
    proposals = []; seen = set()
    def add(keep, family):
        keep = keep_closure(units, keep) & active
        if not keep or keep == active or keep in seen: return
        seen.add(keep)
        proposals.append(dict(keep=sorted(keep), removed=sorted(all_units-keep), family=family,
                              moved_cells=sum(len(units[i]["cells"]) for i in keep)))
    # An already-failing moved register gets priority over passing side paths.
    for passing, _, key in sorted(regressions):
        if key[0] in owner:
            add(remove_closure(units, {owner[key[0]]}),
                "regressed_passing_endpoint" if passing else "regressed_failing_endpoint")
    worst = min(row["setup_margin_ps"] for row in baseline["rows"].values() if row["setup_timed"])
    core = set()
    for key, row in baseline["rows"].items():
        if row["setup_timed"] and row["setup_margin_ps"] == worst:
            if key[0] in owner: core.add(owner[key[0]])
            net_name = plan["origin"]["design"]["cells"][key[0]]["ports"][key[1]][1]
            driver = plan["origin"]["design"]["nets"][net_name]["driver"][0]
            if driver in owner: core.add(owner[driver])
    essential = keep_closure(units, core)
    registers = {u["id"] for u in units if u["registers"] and u["id"] not in essential}
    add(remove_closure(units, registers), "optional_registers")
    add(essential | {u["id"] for u in units if u["carry"]}, "carry_and_endpoint")
    add(essential, "endpoint_core")
    impact = {u["id"]:0 for u in units}
    for _, change, key in regressions:
        touched = {owner[name] for s in paths[key]["segments"] for name in (s["from"][0], s["to"][0]) if name in owner}
        for unit in touched: impact[unit] += -change
    for unit in sorted(impact, key=lambda u:(-impact[u], u)):
        add(remove_closure(units, {unit}), "single_unit")
    # Try each family before spending the full-route quota on similar drops.
    ordered = []; families = set()
    for proposal in proposals:
        if proposal["family"] not in families: ordered.append(proposal); families.add(proposal["family"])
    ordered += [p for p in proposals if p not in ordered]
    return ordered


def diagnostic_rank(baseline, candidate, moved_cells):
    """Search guidance only; this never authorizes an artifact selection."""
    new_failures = failing_regressions = decreases = damage = bad_hold = 0
    for key, old in baseline["rows"].items():
        row = candidate["rows"][key]
        if old["setup_timed"]:
            delta = row["setup_margin_ps"] - old["setup_margin_ps"]
            new_failures += old["setup_margin_ps"] >= 0 and row["setup_margin_ps"] < 0
            failing_regressions += delta < 0 and old["setup_margin_ps"] < 0
            decreases += delta < 0
            damage += max(0, -delta)
        if old["hold_related"]:
            bad_hold += (row["hold_margin_ps"] <= 0 if old["hold_margin_ps"] > 0
                         else row["hold_margin_ps"] < old["hold_margin_ps"])
    gain = feedback.score(candidate)[0] - feedback.score(baseline)[0]
    return new_failures, bad_hold, failing_regressions, decreases, damage, -gain, moved_cells


def refine(args):
    parent_path, candidate_path = args.parent.resolve(), args.candidate.resolve()
    require(parent_path.parent == candidate_path.parent, "source bundles must share frozen inputs")
    pm, cm = feedback.verify_bundle(parent_path), feedback.verify_bundle(candidate_path)
    require(pm.get("inputs_sha256") == cm.get("inputs_sha256") and pm.get("inputs_sha256"), "different frozen source inputs")
    parent = feedback.read_observation(parent_path, pm["exit_code"])
    source = feedback.read_observation(candidate_path, cm["exit_code"])
    plan = source["placement"].get("group_plan")
    require(type(plan) is dict and plan["parent"] == parent["placement"]["current"] and
            plan["origin"] == parent["placement"]["origin"] == parent["placement"]["current"],
            "refinement requires an immediate-parent group move from the original baseline")
    require(feedback.group_target(plan) == source["placement"]["current"], "source group target changed")
    units = movement_units(plan)
    recipe = load(parent_path.parent / "inputs.json"); frozen = parent_path.parent / "inputs"
    feedback.validate_recipe(recipe["extra_args"])
    worker_args = argparse.Namespace(output=args.output, nextpnr=args.nextpnr, json=frozen/"synth.json", qsf=frozen/"pins.qsf",
        sdc=frozen/"clocks.sdc", device=recipe["device"], seed=recipe["seed"], freq=recipe["freq"],
        extra_arg=recipe["extra_args"], preserve_routes=recipe["preserve_routes"], group_moves=True,
        gpu_cpu=recipe["cpu_reference"], rounds=0, candidates=args.routes, trials=args.checks)
    workers = feedback.Workers(worker_args, extra_inputs=(("refiner.py", Path(__file__)),))
    root = workers.root
    save(root/"refinement.json", dict(version=1, source_parent=str(parent_path), source_candidate=str(candidate_path),
        source_parent_manifest_sha256=digest(parent_path/"result.json"), source_candidate_manifest_sha256=digest(candidate_path/"result.json"),
        units=units, route_limit=args.routes, check_limit=args.checks,
        mode="register_destinations" if args.redirect_registers else "literal_subsets"))
    baseline_dir, code = workers.run("baseline")
    baseline = feedback.read_observation(baseline_dir, code)
    require(baseline["placement"]["current"] == parent["placement"]["current"], "fresh baseline identity changed")
    require(all(digest(baseline_dir/name) == digest(parent_path/name) for name in
                ("core.rbf", "routed.json", "timing.json", "native/endpoints.json")), "fresh baseline physical result changed")
    best = baseline; best_plan = None
    def retain():
        save(root/"best.json", dict(bundle=str(best["directory"].relative_to(root)),
            bundle_sha256=digest(best["directory"]/"result.json"), plan=str(best_plan.relative_to(root)) if best_plan else None,
            plan_sha256=digest(best_plan) if best_plan else None, qualified=True, timing_pass=best["status"]["timing_pass"]), atomic=True)
    retain()
    control_plan = root/"control-plan.json"; save(control_plan, plan)
    routes = baseline_dir/"native/routes.json" if worker_args.preserve_routes else None
    control_dir, code = workers.run("control", plan=control_plan, routes=routes)
    control = feedback.read_observation(control_dir, code)
    require(control["placement"]["group_plan"] == plan, "control replay changed its plan")
    require(all(digest(control_dir/name) == digest(candidate_path/name) for name in
                ("core.rbf", "routed.json", "timing.json", "native/endpoints.json")), "source candidate replay changed physical results")
    changes = path_changes(baseline, control)
    save(root/"control-path-changes.json", dict(diagnostic_only=True, paths=changes))
    decisions = []
    def consider(observed, path, metadata):
        nonlocal best, best_plan
        decision = dict(bundle=observed["directory"].name, score_ps=feedback.score(observed), **metadata)
        try:
            value = feedback.compare(baseline, observed)
            decision["eligible"] = True
            if best is baseline or value > feedback.score(best): best, best_plan = observed, path; retain()
        except feedback.Rejected as error: decision.update(eligible=False, reason=str(error))
        decisions.append(decision)
    consider(control, control_plan, dict(control=True, moved_cells=len(plan["moves"])))
    checked = []; full_routes = 1
    active = frozenset(range(len(units))); diagnostic = control
    rank = diagnostic_rank(baseline, control, len(plan["moves"]))
    redirects = register_candidates(plan, baseline, control) if args.redirect_registers else None
    attempted = set()
    while full_routes < args.routes and len(checked) < args.checks:
        proposals = redirects if redirects is not None else candidates(plan, units, baseline, diagnostic, active)
        def proposal_plan(proposal):
            return proposal["plan"] if redirects is not None else subset_plan(plan, units, proposal["keep"])
        def token(proposal): return json.dumps(proposal_plan(proposal)["moves"], sort_keys=True)
        proposals = [p for p in proposals if token(p) not in attempted]
        if not proposals: break
        proposal = proposals[0]; attempted.add(token(proposal))
        index = len(checked)
        reduced = proposal_plan(proposal)
        metadata = {key:value for key, value in proposal.items() if key != "plan"}
        path = root/f"{'redirect' if redirects is not None else 'subset'}-{index}.json"; save(path, reduced)
        directory, code = workers.run(f"check-{index}", plan=path, check_only=True)
        require(code == 0, "native plan validation failed unexpectedly")
        feedback.verify_bundle(directory, 0)
        proof = load(directory/"native/plan-validation.json"); placement = load(directory/"native/placement.json")
        require(proof.get("version") == 1 and type(proof.get("legal")) is bool and proof.get("placement_only") is True and
                proof.get("route_complete") is False and not (directory/"native/completion.json").exists(), "invalid legality-only proof")
        require(placement["origin"] == baseline["placement"]["origin"] and placement.get("group_plan") == reduced,
                "native legality check changed its request")
        require(placement["current"] == (feedback.group_target(reduced) if proof["legal"] else placement["origin"]),
                "native legality check did not apply or restore the exact placement")
        checked.append(dict(index=index, legal=proof["legal"], **metadata))
        if not proof["legal"]: continue
        trial_dir, code = workers.run(f"trial-{index}", plan=path, routes=routes); full_routes += 1
        observed = feedback.read_observation(trial_dir, code)
        require(observed["placement"].get("group_plan") == reduced and observed["placement"]["current"] == feedback.group_target(reduced),
                "full route changed its subset placement")
        save(root/f"trial-{index}-path-changes.json", dict(diagnostic_only=True, paths=path_changes(baseline, observed)))
        consider(observed, path, dict(control=False, **metadata))
        next_rank = diagnostic_rank(baseline, observed, len(reduced["moves"]))
        a, b = feedback.score(baseline), feedback.score(observed)
        if redirects is None and (b[0] >= a[0] + 20 or (b[0] == a[0] and b[1] >= a[1] + 20)) and next_rank < rank:
            # A rejected result may guide a smaller diagnostic. Every route
            # still uses the original baseline, and retain() still needs compare().
            active, diagnostic, rank = frozenset(proposal["keep"]), observed, next_rank
    save(root/"decision.json", dict(trials=decisions, checks=checked, full_routes=full_routes,
        selected=best["directory"].name, score_ps=feedback.score(best), timing_pass=best["status"]["timing_pass"]))
    print(json.dumps(dict(selected=best["directory"].name, score_ps=feedback.score(best), full_routes=full_routes), indent=2))
    return best["status"]["timing_pass"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("parent", "candidate", "nextpnr", "output"): parser.add_argument("--"+name, type=Path, required=True)
    parser.add_argument("--routes", type=int, choices=range(1, 5), default=4, help="full routes including the control, 1..4")
    parser.add_argument("--checks", type=int, choices=range(1, 65), default=16, help="native legality checks, 1..64")
    parser.add_argument("--redirect-registers", action="store_true", help="try register chain shortcuts/rotations instead of literal subsets")
    args = parser.parse_args()
    try: return 0 if refine(args) else 1
    except (feedback.Rejected, OSError, ValueError, KeyError, TypeError) as error:
        if args.output.is_dir() and (args.output/"best.json").exists() and not (args.output/"stopped.json").exists():
            save(args.output/"stopped.json", dict(reason=str(error), retained=load(args.output/"best.json")))
        print("Group refinement stopped: "+str(error), file=sys.stderr); return 1


if __name__ == "__main__": raise SystemExit(main())
