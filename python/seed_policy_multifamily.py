#!/usr/bin/env python3
"""Bounded cross-family, full-run policy screen; no default or live-stop changes."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import time

import seed_racing
import seed_policy_portfolio as portfolio

POLICIES = ["critexp-2", "critexp-5", "critexp-7", "critexp-8"]


def digest(value):
    return hashlib.sha256(portfolio.canonical(value)).hexdigest()


def set_option(argv, option, value):
    if argv.count(option) > 1:
        raise ValueError("duplicate option: " + option)
    if option in argv:
        argv[argv.index(option) + 1] = str(value)
    else:
        argv.extend([option, str(value)])


def validate_split(families):
    if len(families) != 5 or sum(f["phase"] == "train" for f in families) != 2 or \
            sum(f["phase"] == "heldout" for f in families) != 3:
        raise ValueError("screen requires exactly two training and three held-out families")
    names = [f["name"] for f in families]
    if len(set(names)) != len(names) or any(not n or not all(c.isalnum() or c == "-" for c in n) for n in names):
        raise ValueError("family names must be unique safe path components")
    mapped = [f["mapped_sha256"] for f in families]
    if len(set(mapped)) != len(mapped):
        raise ValueError("mapped netlist bytes cannot occur in multiple family groups")


def prepare(config, output):
    families = []
    manifests = {}
    for entry in config["families"]:
        manifest = seed_racing.validate_collection_manifest(json.loads(Path(entry["template"]).read_bytes()))
        manifest["command"][0] = config["binary"]
        manifest["provenance"] = dict(source_revision=config["binary_source_revision"], dirty=False,
                                      runtime_environment_id="auto")
        baseline = entry["baseline"]
        if baseline not in POLICIES:
            raise ValueError("baseline is outside the predeclared policy vocabulary")
        argv = manifest["command"]
        old_exp = argv[argv.index("--placer-heap-critexp") + 1] if "--placer-heap-critexp" in argv else "7"
        old_weight = argv[argv.index("--placer-heap-timingweight") + 1] if "--placer-heap-timingweight" in argv else "10"
        if baseline != "critexp-" + old_exp or str(entry["weight"]) != old_weight:
            raise ValueError("baseline/weight must match the retained template (including native defaults)")
        binary_sha, inputs = portfolio.fixture_hashes(manifest)
        mapped = [value for key, value in inputs.items() if key.startswith("mapped_netlist:")]
        if len(mapped) != 1 or manifest["cohort"]["mapped_design_id"] != "sha256:" + mapped[0]:
            raise ValueError("mapped family must be bound to its actual netlist bytes")
        if manifest["environment"] != {"CUDA_CACHE_DISABLE": "1"}:
            raise ValueError("screen requires the retained CUDA environment")
        family = dict(name=entry["name"], phase=entry["phase"], baseline=baseline, weight=entry["weight"],
                      mapped_sha256=mapped[0], binary_sha256=binary_sha, input_sha256=inputs,
                      required_clocks=manifest["required_clocks"], design=manifest["cohort"])
        families.append(family)
        manifests[entry["name"]] = manifest
    validate_split(families)
    if len({f["binary_sha256"] for f in families}) != 1:
        raise ValueError("all families must use the same frozen binary")
    output.mkdir(parents=True, exist_ok=False)
    for family in families:
        directory = output / family["name"]
        directory.mkdir()
        seeds = list(range(33, 37 if family["phase"] == "train" else 39))
        cohorts = []
        # Freeze different rotations across families without seeing any outcomes.
        index = families.index(family)
        order = POLICIES[index % 4:] + POLICIES[:index % 4]
        for policy in order:
            manifest = copy.deepcopy(manifests[family["name"]])
            manifest["cohort"]["id"] = f"multifamily-{family['name']}-{policy}-20261007"
            manifest["seeds"], manifest["repeats"] = seeds, 1
            manifest["limits"] = dict(per_run_seconds=600, total_seconds=600 * len(seeds) + 120, concurrency=1)
            set_option(manifest["command"], "--placer-heap-timingweight", family["weight"])
            set_option(manifest["command"], "--placer-heap-critexp", policy.split("-")[1])
            if "--timing-allow-fail" not in manifest["command"]:
                manifest["command"].append("--timing-allow-fail")
            manifest = seed_racing.validate_collection_manifest(manifest)
            filename = policy + ".json"
            portfolio.write_new(directory / filename, manifest)
            cohorts.append(dict(phase=family["phase"], policy=policy, manifest=filename,
                                manifest_sha256=digest(manifest), cohort_id=manifest["cohort"]["id"], seeds=seeds))
        declaration = dict(binary_sha256=family["binary_sha256"], input_sha256=family["input_sha256"], cohorts=cohorts)
        portfolio.write_new(directory / "declaration.json", declaration)
        family["declaration_sha256"] = digest(declaration)
        family["seeds"] = seeds
    plan = dict(schema_version=1, families=families, policies=POLICIES, wall_limit_seconds=72000,
                max_pnr_invocations=104, per_run_seconds=600, scheduler_seeds=list(range(20)),
                budgets_seconds=[600, 1800, 3600], amortization_uses=[1, 10, 100],
                selection="equal-family mean efficiency normalized by each training family's best efficiency; ties prefer critexp-5 then policy id",
                gate="descriptive only; known historical families, fresh seed/policy observations; no defaults or live termination")
    portfolio.write_new(output / "plan.json", plan)
    return plan


def load_plan(output):
    plan = json.loads((output / "plan.json").read_bytes())
    validate_split(plan["families"])
    groups = {}
    for family in plan["families"]:
        declaration, manifests = portfolio.load_declared(output / family["name"])
        if digest(declaration) != family["declaration_sha256"] or \
                declaration["binary_sha256"] != family["binary_sha256"] or \
                declaration["input_sha256"] != family["input_sha256"]:
            raise ValueError("family declaration differs from the frozen screen")
        if {i["policy"] for i, _ in manifests} != set(plan["policies"]) or len(manifests) != len(plan["policies"]):
            raise ValueError("family policy population differs from the frozen screen")
        for item, manifest in manifests:
            if item["phase"] != family["phase"] or item["seeds"] != family["seeds"] or manifest["repeats"] != 1:
                raise ValueError("family phase/population changed")
        groups[family["name"]] = manifests
    return plan, groups


def validate_runtimes(records, amendment=None):
    identities = [r["execution_identity"] for r in records]
    if amendment is None:
        if any(identity != identities[0] for identity in identities):
            raise ValueError("backend/runtime changed across families")
        return
    normalized = []
    for record in records:
        identity = record["execution_identity"]
        manifest = record["runtime_manifest"]
        runtime_id = "sha256:" + digest(manifest)
        kernel = manifest["platform"]["release"]
        if identity["runtime_environment_id"] != runtime_id or \
                amendment["authorized_runtimes"].get(runtime_id) != kernel:
            raise ValueError("runtime is outside the explicit kernel amendment")
        value = copy.deepcopy(manifest)
        del value["platform"]["release"]
        normalized.append(dict(backend=identity["backend"], manifest=value))
    if any(value != normalized[0] for value in normalized):
        raise ValueError("kernel amendment cannot authorize binary/library/CPU/backend changes")


def load_amendment(output, plan):
    path = output / "kernel-amendment.json"
    if not path.exists():
        return None
    amendment = json.loads(path.read_bytes())
    if amendment.get("plan_sha256") != digest(plan) or amendment.get("kind") != "kernel-only-transition":
        raise ValueError("kernel amendment is not bound to this experiment")
    return amendment


def read_rows(output, families, groups):
    rows = []
    records = []
    plan = json.loads((output / "plan.json").read_bytes())
    for family in families:
        for item, manifest in groups[family["name"]]:
            # Authenticate each cohort unchanged; only this screen layer handles
            # explicitly declared inter-cohort kernel strata.
            population = portfolio.read_populations(output / family["name"], [(item, manifest)])
            path = next((output / family["name"] / "collections" / item["cohort_id"]).glob("collection-*.json"))
            bound = json.loads(path.read_bytes())["cohort_identity"]["manifest"]
            runtime = bound["binary"]["runtime_environment"]["manifest"]
            records.append(dict(execution_identity=bound["execution_identity"], runtime_manifest=runtime))
            for row in population:
                row.update(family=family["name"], kernel_release=runtime["platform"]["release"],
                           runtime_environment_id=bound["execution_identity"]["runtime_environment_id"])
            rows.extend(population)
    validate_runtimes(records, load_amendment(output, plan))
    return rows


def select(rows, families, policies):
    if any(row["phase"] != "train" or row["family"] not in families for row in rows):
        raise ValueError("held-out families may not enter training selection")
    statistics = {}
    scores = {policy: 0.0 for policy in policies}
    for family in families:
        _, stats = portfolio.ranked_training_policies(
            [r for r in rows if r["family"] == family], policies, "critexp-5")
        best = max(s["successes_per_second"] for s in stats.values())
        for policy in policies:
            scores[policy] += (stats[policy]["successes_per_second"] / best if best else 0.0) / len(families)
        statistics[family] = stats
    ranking = sorted(policies, key=lambda p: (-scores[p], p != "critexp-5", p))
    return dict(ranking=ranking, scores=scores, statistics=statistics,
                training_cost_seconds=sum(r["duration_seconds"] for r in rows))


def frozen_selection(output, plan, groups):
    families = [f for f in plan["families"] if f["phase"] == "train"]
    selection = select(read_rows(output, families, groups), [f["name"] for f in families], plan["policies"])
    selection["plan_sha256"] = digest(plan)
    path = output / "training-selection.json"
    if path.exists():
        if json.loads(path.read_bytes()) != selection:
            raise ValueError("frozen training selection changed")
    else:
        if any((output / f["name"] / "collections").exists()
               for f in plan["families"] if f["phase"] == "heldout"):
            raise ValueError("cannot create training selection retrospectively after held-out collection")
        portfolio.write_new(path, selection)
    return selection


def charged_replay(order, cases, budget, training_charge):
    if training_charge >= budget:
        return dict(success=False, consumed_seconds=budget, time_to_first_success_seconds=None,
                    training_charge_seconds=training_charge, completed=[], censored_next=None)
    result = portfolio.replay(order, cases, budget - training_charge)
    result["training_charge_seconds"] = training_charge
    result["consumed_seconds"] += training_charge
    if result["success"]:
        result["time_to_first_success_seconds"] += training_charge
    return result


def evaluate(output):
    plan, groups = load_plan(output)
    # Never create a missing training selection retrospectively after held-out outcomes.
    if not (output / "training-selection.json").is_file():
        raise ValueError("missing selection frozen before held-out collection")
    selection = frozen_selection(output, plan, groups)
    rows = read_rows(output, plan["families"], groups)
    reports = []
    for family in plan["families"]:
        if family["phase"] != "heldout":
            continue
        cases = {(r["policy"], r["seed"]): r for r in rows if r["family"] == family["name"]}
        schedules = []
        for scheduler_seed in plan["scheduler_seeds"]:
            orders = dict(baseline=portfolio.schedule(family["seeds"], [family["baseline"]], scheduler_seed, False),
                          selected_single=portfolio.schedule(family["seeds"], [selection["ranking"][0]], scheduler_seed, False),
                          rotating_portfolio=portfolio.schedule(family["seeds"], selection["ranking"], scheduler_seed, True))
            for budget in plan["budgets_seconds"]:
                schedules.append(dict(scheduler_seed=scheduler_seed, budget_seconds=budget,
                                      policies={name: portfolio.replay(order, cases, budget) for name, order in orders.items()},
                                      hypothetical_training_charged={str(n): {
                                          name: charged_replay(order, cases, budget,
                                                              0 if name == "baseline" else selection["training_cost_seconds"] / n)
                                          for name, order in orders.items()} for n in plan["amortization_uses"]}))
        stats = {policy: dict(successes=sum(r["success"] for r in cases.values() if r["policy"] == policy),
                              cost_seconds=sum(r["duration_seconds"] for r in cases.values() if r["policy"] == policy))
                 for policy in plan["policies"]}
        kernels = sorted({r["kernel_release"] for r in cases.values()})
        reports.append(dict(family=family["name"], baseline=family["baseline"], observed_statistics=stats,
                            kernel_releases=kernels, cross_kernel_cost_comparison=len(kernels) > 1,
                            policy_kernel_strata={p: sorted({r["kernel_release"] for r in cases.values()
                                                            if r["policy"] == p}) for p in plan["policies"]},
                            schedules=schedules))
    result = dict(plan_sha256=digest(plan), classification="offline full-run serial counterfactuals, not online execution",
                  kernel_amendment=load_amendment(output, plan),
                  training_selection=selection, heldout_families=reports,
                  total_process_cost_seconds=sum(r["duration_seconds"] for r in rows),
                  training_charge_per_use_seconds={str(n): selection["training_cost_seconds"] / n
                                                  for n in plan["amortization_uses"]},
                  limitations=["Historical fixture results were known; fresh combinations are held out, not pristine designs.",
                               "Three held-out families and six seeds each are descriptive, not broad generalization.",
                               "20 permutations reuse the same cases; no independent-population confidence claim.",
                               "No observed amortized deployment; add training charge to search cost, not to baseline.",
                               "Process times include restart/startup/cleanup; cohort freezing and evaluation overhead are separate."])
    if result["kernel_amendment"]:
        result["limitations"].append("Explicit kernel transition: mixed RAM-test runtime costs are descriptive/confounded; no small-speedup claim.")
    portfolio.write_new(output / "evaluation.json", result)
    return result


def run(output):
    import fcntl
    plan, groups = load_plan(output)
    with (output / "operator.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        deadline_path = output / "deadline.json"
        if not deadline_path.exists():
            portfolio.write_new(deadline_path, dict(start_unix=time.time(), deadline_unix=time.time() + plan["wall_limit_seconds"]))
        deadline = json.loads(deadline_path.read_bytes())["deadline_unix"]
        for phase in ("train", "heldout"):
            if phase == "heldout":
                selection = frozen_selection(output, plan, groups)
                print("FROZEN_SELECTION", selection["ranking"], flush=True)
            for family in [f for f in plan["families"] if f["phase"] == phase]:
                for item, manifest in groups[family["name"]]:
                    directory = output / family["name"] / "collections" / item["cohort_id"]
                    if directory.exists():
                        portfolio.read_populations(output / family["name"], [(item, manifest)])
                        print("RETAIN", item["cohort_id"], flush=True)
                        continue
                    if deadline - time.time() < manifest["limits"]["total_seconds"] + 120:
                        raise ValueError("global deadline cannot admit another declared cohort")
                    if shutil.disk_usage(output).free < 12 * 1024**3:
                        raise ValueError("less than 12 GiB free; stop before the next cohort")
                    print("START", phase, family["name"], item["policy"], flush=True)
                    results = seed_racing.Collector(manifest, directory).run()
                    print("DONE", phase, family["name"], item["policy"],
                          [(r["seed"], r["status"], round(r["elapsed_seconds"], 3)) for r in results], flush=True)
        evaluate(output)
        portfolio.write_new(output / "completion.json", dict(completed_unix=time.time(), plan_sha256=digest(plan)))
        print("COMPLETE", digest(plan), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=["prepare", "run", "evaluate", "dry-run"])
    parser.add_argument("--config", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if args.operation == "prepare":
        if args.config is None:
            parser.error("prepare requires --config")
        print(digest(prepare(json.loads(args.config.read_bytes()), output)))
    elif args.operation == "run":
        run(output)
    elif args.operation == "evaluate":
        evaluate(output)
    else:
        plan, groups = load_plan(output)
        count = sum(len(seed_racing.Collector(manifest, output / "dry-run" / item["cohort_id"]).run(dry_run=True))
                    for group in groups.values() for item, manifest in group)
        if count != plan["max_pnr_invocations"]:
            raise ValueError("dry-run differs from the declared invocation bound")
        print("DRY_RUN_INVOCATIONS", count, "PLAN", digest(plan))


if __name__ == "__main__":
    main()
