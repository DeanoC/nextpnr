#!/usr/bin/env python3
"""Declare and collect a bounded full-run placement-policy portfolio screen.

Uses the authenticated seed-racing collector, not a new PNR execution path.
Does not stop runs early, change defaults, or infer resumability from telemetry.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import time

import seed_racing


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def write_new(path, value):
    with path.open("xb") as stream:
        stream.write(canonical(value) + b"\n")


def fixture_hashes(manifest):
    """Use the collector's working-directory and child-PATH resolution rules."""
    normalized = seed_racing.validate_collection_manifest(manifest)
    environment = seed_racing._child_environment(normalized["environment"])
    binary = seed_racing._resolved_binary(normalized["command"][0], normalized["cwd"], environment)
    if binary is None:
        raise ValueError(f"cannot resolve cohort executable: {normalized['command'][0]}")
    base = Path(normalized["cwd"]) if normalized["cwd"] else Path.cwd()
    inputs = {row["role"] + ":" + str(index):
              hashlib.sha256((base / Path(row["path"])).resolve().read_bytes()).hexdigest()
              for index, row in enumerate(normalized["inputs"])}
    return hashlib.sha256(binary.read_bytes()).hexdigest(), inputs


def prepare(template, output):
    """Seal all populations and argv before the first training route."""
    binary_sha256, inputs = fixture_hashes(template)
    output.mkdir(parents=True, exist_ok=False)
    plans = []
    for phase, seeds in (("train", list(range(17, 25))), ("heldout", list(range(25, 33))),
                         ("repeat", [17])):
        # Predeclared rotation reduces phase/order bias between contiguous cohorts.
        exponents = {"train": [5, 2, 8], "heldout": [2, 8, 5], "repeat": [8, 5, 2]}[phase]
        for exponent in exponents:
            manifest = copy.deepcopy(template)
            policy = f"critexp-{exponent}"
            manifest["cohort"]["id"] = f"policy-screen-atari-{phase}-{policy}-20261006"
            manifest["seeds"] = seeds
            manifest["repeats"] = 1
            manifest["limits"] = {"per_run_seconds": 600, "total_seconds": 600 * len(seeds) + 120,
                                  "concurrency": 1}
            argv = manifest["command"]
            argv[argv.index("--placer-heap-critexp") + 1] = str(exponent)
            if argv[argv.index("--placer-heap-timingweight") + 1] != "2000":
                raise ValueError("template does not match the fixed placement-weight declaration")
            seed_racing.validate_collection_manifest(manifest)
            filename = f"{phase}-{policy}.json"
            write_new(output / filename, manifest)
            plans.append({"phase": phase, "policy": policy, "manifest": filename,
                          "manifest_sha256": hashlib.sha256(canonical(manifest)).hexdigest(),
                          "cohort_id": manifest["cohort"]["id"], "seeds": seeds})
    declaration = {"schema_version": 1, "design": template["cohort"],
                   "binary_sha256": binary_sha256,
                   "input_sha256": inputs, "policies": ["critexp-2", "critexp-5", "critexp-8"],
                   "baseline": "critexp-5", "training_seeds": list(range(17, 25)),
                   "heldout_seeds": list(range(25, 33)), "repeat_seeds": [17],
                   "selection": "training_successes_per_observed_second; ties prefer baseline then policy id",
                   "portfolio": "training-ranked policies rotate across independently shuffled seeds; no prefix ranking",
                   "scheduler_seeds": list(range(20)), "evaluation_budgets_seconds": [600, 1800, 3600],
                   "per_run_seconds": 600, "max_pnr_invocations": 51, "wall_limit_seconds": 32400,
                   "comparison": "ordinary baseline, training-selected single policy, fixed rotating portfolio",
                   "gate": "descriptive only; eight heldout seeds on one mapped family do not authorize defaults or termination",
                   "cohorts": plans}
    write_new(output / "declaration.json", declaration)
    return declaration


def load_declared(output):
    declaration = json.loads((output / "declaration.json").read_bytes())
    manifests = []
    for item in declaration["cohorts"]:
        path = output / item["manifest"]
        manifest = json.loads(path.read_bytes())
        if hashlib.sha256(canonical(manifest)).hexdigest() != item["manifest_sha256"]:
            raise ValueError("collection manifest differs from its predeclaration")
        if manifest["cohort"]["id"] != item["cohort_id"] or manifest["seeds"] != item["seeds"]:
            raise ValueError("population differs from its predeclaration")
        binary_sha256, inputs = fixture_hashes(manifest)
        if binary_sha256 != declaration["binary_sha256"]:
            raise ValueError("declared binary changed")
        if inputs != declaration["input_sha256"]:
            raise ValueError("declared fixture changed")
        manifests.append((item, manifest))
    return declaration, manifests


def run(output):
    declaration, manifests = load_declared(output)
    # Refuse simultaneous sessions of this runner. Collector also locks each cohort.
    import fcntl
    with (output / "operator.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        deadline_file = output / "deadline.json"
        if deadline_file.exists():
            deadline = json.loads(deadline_file.read_bytes())["deadline_unix"]
        else:
            deadline = time.time() + declaration["wall_limit_seconds"]
            write_new(deadline_file, {"deadline_unix": deadline})
        for item, manifest in manifests:
            if item["phase"] == "heldout":
                training = read_populations(output, [(i, m) for i, m in manifests if i["phase"] == "train"])
                ranking, statistics = ranked_training_policies(
                    training, declaration["policies"], declaration["baseline"])
                selection = {"declaration_sha256": hashlib.sha256(canonical(declaration)).hexdigest(),
                             "ranking": ranking, "statistics": statistics}
                selection_path = output / "training-selection.json"
                if selection_path.exists():
                    if json.loads(selection_path.read_bytes()) != selection:
                        raise ValueError("frozen training selection changed")
                else:
                    write_new(selection_path, selection)
            directory = output / "collections" / item["cohort_id"]
            if directory.exists():
                summaries = list(directory.glob("collection-*.json"))
                if len(summaries) == 1 and not summaries[0].name.startswith("collection-failure-"):
                    read_populations(output, [(item, manifest)])
                    print("RETAIN", item["cohort_id"], flush=True)
                    continue
                raise ValueError("incomplete prior collection: inspect evidence; do not silently rerun it")
            remaining = deadline - time.time()
            if remaining < manifest["limits"]["total_seconds"] + 120:
                raise ValueError("global wall limit cannot admit the next declared cohort")
            if shutil.disk_usage(output).free < 12 * 1024**3:
                raise ValueError("less than 12 GiB free: stop before the next cohort")
            print("START", item["phase"], item["policy"], "seeds", item["seeds"], flush=True)
            results = seed_racing.Collector(manifest, directory).run()
            print("DONE", item["phase"], item["policy"],
                  [(row["seed"], row["status"], round(row["elapsed_seconds"], 3)) for row in results], flush=True)
        print("COMPLETE", hashlib.sha256(canonical(declaration)).hexdigest(), flush=True)


def ranked_training_policies(rows, policies, baseline):
    """No heldout rows or numeric seed values may enter policy selection."""
    if any(row["phase"] != "train" for row in rows):
        raise ValueError("policy selection accepts training rows only")
    statistics = {}
    for policy in policies:
        cases = [row for row in rows if row["policy"] == policy]
        if not cases:
            raise ValueError("missing training policy")
        duration = sum(row["duration_seconds"] for row in cases)
        if duration <= 0:
            raise ValueError("training cost must be observed and positive")
        successes = sum(row["success"] for row in cases)
        statistics[policy] = {"successes": successes, "attempts": len(cases), "cost_seconds": duration,
                              "successes_per_second": successes / duration}
    ranking = sorted(policies, key=lambda policy: (-statistics[policy]["successes_per_second"],
                                                  policy != baseline, policy))
    return ranking, statistics


def schedule(seeds, policies, scheduler_seed, portfolio):
    """Candidate order sees only declaration metadata, never labels or costs."""
    order = sorted(seeds, key=lambda seed: hashlib.sha256(
        canonical(["portfolio-seed-order-v1", scheduler_seed, str(seed)])).hexdigest())
    if not portfolio:
        return [(policies[0], seed) for seed in order]
    return [(policies[(index + lap) % len(policies)], seed)
            for lap in range(len(policies)) for index, seed in enumerate(order)]


def replay(order, cases, budget):
    """Observed full-run serial simulation; pay timeout/restart costs in full."""
    consumed = 0.0
    completed = []
    for key in order:
        case = cases[key]
        cost = case["duration_seconds"]
        if consumed + cost > budget:
            return {"success": False, "time_to_first_success_seconds": None,
                    "consumed_seconds": budget, "completed": completed, "censored_next": list(key)}
        consumed += cost
        completed.append(list(key))
        if case["success"]:
            return {"success": True, "time_to_first_success_seconds": consumed,
                    "consumed_seconds": consumed, "completed": completed, "censored_next": None,
                    "final_multi_clock_margin_ns": case.get("final_multi_clock_margin_ns")}
    return {"success": False, "time_to_first_success_seconds": None,
            "consumed_seconds": consumed, "completed": completed, "censored_next": None}


def read_populations(output, manifests):
    declaration = json.loads((output / "declaration.json").read_bytes())
    summaries = []
    cohorts = {}
    artifacts = {}
    execution_identities = []
    for item, manifest in manifests:
        paths = list((output / "collections" / item["cohort_id"]).glob("collection-*.json"))
        if len(paths) != 1 or paths[0].name.startswith("collection-failure-"):
            raise ValueError("evaluation requires all declared collections; omissions are not failures")
        summary = json.loads(paths[0].read_bytes())
        bound = summary.get("cohort_identity", {}).get("manifest", {})
        if bound.get("binary", {}).get("sha256") != declaration["binary_sha256"]:
            raise ValueError("collected binary differs from declaration")
        inputs = {row["role"] + ":" + str(index): row["sha256"]
                  for index, row in enumerate(bound.get("inputs", []))}
        if inputs != declaration["input_sha256"]:
            raise ValueError("collected input differs from declaration")
        execution_identities.append(bound.get("execution_identity"))
        for field in ("cohort", "architecture", "artifacts", "command", "required_clocks",
                      "seeds", "repeats", "environment", "limits"):
            actual = bound.get(field)
            if field == "environment":
                # Collector includes deterministic platform-minimum environment.
                if not all(actual.get(k) == v for k, v in manifest[field].items()):
                    raise ValueError("collection environment differs from declaration")
            elif actual != manifest[field]:
                raise ValueError(f"collection {field} differs from declaration")
        cohorts[item["cohort_id"]] = item
        for result in summary["results"]:
            artifacts[result["run_id"]] = result.get("artifacts", {})
        summaries.append(paths[0])
    if not execution_identities or any(identity != execution_identities[0]
                                       for identity in execution_identities):
        raise ValueError("backend/runtime identity changed between declared populations")
    document = seed_racing.assemble_dataset(summaries)
    if document.get("excluded_runs"):
        raise ValueError("artifactless excluded attempts prevent a complete comparison")
    rows = seed_racing.validate_dataset(document)
    by_population = {}
    for row in rows:
        if row.get("process_started") is not True:
            raise ValueError("unstarted candidates cannot supply observed full-run costs")
        item = cohorts[row["cohort_id"]]
        row.update(phase=item["phase"], policy=item["policy"])
        row["artifact_records"] = artifacts[row["run_id"]]
        by_population.setdefault((item["phase"], item["policy"]), []).append(row)
    for item, _ in manifests:
        population = by_population.get((item["phase"], item["policy"]), [])
        if sorted(row["seed"] for row in population) != sorted(item["seeds"]):
            raise ValueError("missing or duplicated declared candidate")
    return rows


def repeat_checks(rows, policies, seeds):
    checks = []
    for policy in policies:
        for seed in seeds:
            pair = [next(row for row in rows if row["phase"] == phase and
                         row["policy"] == policy and row["seed"] == seed)
                    for phase in ("train", "repeat")]
            check = {"policy": policy, "seed": seed, "statuses": [row["status"] for row in pair],
                     "classification": "inconclusive: at least one anchor did not complete"}
            if all(row["status"] in ("completed", "analogue_timing_failure") for row in pair):
                same = {}
                for name in ("final_report", "bitstream"):
                    digests = []
                    for row in pair:
                        record = row["artifact_records"].get(name, {})
                        if record.get("available") is not True:
                            raise ValueError("completed repeat anchor lacks " + name)
                        digest = hashlib.sha256(Path(record["path"]).read_bytes()).hexdigest()
                        if digest != record["sha256"]:
                            raise ValueError("repeat anchor artifact changed")
                        digests.append(digest)
                    same[name + "_byte_identical"] = len(set(digests)) == 1
                check.update(same)
                check["classification"] = "repeatable" if all(same.values()) else "mismatch: investigate"
            checks.append(check)
    return checks


def evaluate(output, report_path=None):
    declaration, manifests = load_declared(output)
    rows = read_populations(output, manifests)
    train = [row for row in rows if row["phase"] == "train"]
    ranking, training_statistics = ranked_training_policies(train, declaration["policies"], declaration["baseline"])
    selection = json.loads((output / "training-selection.json").read_bytes())
    if selection != {"declaration_sha256": hashlib.sha256(canonical(declaration)).hexdigest(),
                     "ranking": ranking, "statistics": training_statistics}:
        raise ValueError("evaluation differs from the selection frozen before heldout collection")
    held = {(row["policy"], row["seed"]): row for row in rows if row["phase"] == "heldout"}
    schedules = []
    for scheduler_seed in declaration["scheduler_seeds"]:
        orders = {"baseline": schedule(declaration["heldout_seeds"], [declaration["baseline"]], scheduler_seed, False),
                  "selected_single": schedule(declaration["heldout_seeds"], [ranking[0]], scheduler_seed, False),
                  "rotating_portfolio": schedule(declaration["heldout_seeds"], ranking, scheduler_seed, True)}
        for budget in declaration["evaluation_budgets_seconds"]:
            schedules.append({"scheduler_seed": scheduler_seed, "budget_seconds": budget,
                              "policies": {name: replay(order, held, budget) for name, order in orders.items()}})
    result = {"declaration_sha256": hashlib.sha256(canonical(declaration)).hexdigest(),
              "classification": "offline serial full-run simulation; not observed online execution",
              "training_ranking": ranking, "training_statistics": training_statistics,
              "repeat_checks": repeat_checks(rows, declaration["policies"], declaration["repeat_seeds"]),
              "training_cost_seconds": sum(row["duration_seconds"] for row in train),
              "repeat_check_cost_seconds": sum(row["duration_seconds"] for row in rows if row["phase"] == "repeat"),
              "total_collection_cost_seconds": sum(row["duration_seconds"] for row in rows),
              "heldout_cases": [{k: row[k] for k in ("policy", "seed", "status", "success", "duration_seconds",
                                                      "final_multi_clock_margin_ns")} for row in held.values()],
              "schedule_results": schedules,
              "limitations": ["Single mapped family; heldout seeds are not cross-design generalization.",
                               "20 schedule permutations are not 20 independent hardware populations.",
                               "Time-bounded failure is not proof of permanent failure.",
                               "Training and repeat costs are separate and must be charged for first deployment.",
                               "No checkpoint/resume, learned predictor, live termination or default-policy change."]}
    write_new(report_path or output / "evaluation.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("prepare", "run", "evaluate"))
    parser.add_argument("--template", type=Path)
    parser.add_argument("--report", type=Path, help="new evaluation file; existing evidence is never overwritten")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.operation == "prepare":
        prepare(json.loads(args.template.read_bytes()), args.output.resolve())
    elif args.operation == "run":
        run(args.output.resolve())
    else:
        print(json.dumps(evaluate(args.output.resolve(), args.report), indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
