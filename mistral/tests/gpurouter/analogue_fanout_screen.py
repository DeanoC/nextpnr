#!/usr/bin/env python3
"""Bounded eight-attempt Atari fanout comparison, using the existing collector."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

SOURCE = "e35e008864792f640f3776f99e11e3d054c288df"
NET = "video.configured_MISTRAL_ALUT3_A_C_MISTRAL_ALUT5_A_Q_MISTRAL_ALUT4_D_Q_MISTRAL_ALUT5_E_Q"
CASES = [(2, 34, "failure"), (8, 35, "failure"), (2, 35, "control"), (8, 34, "control")]


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def sha(value):
    return hashlib.sha256(value).hexdigest()


def write_new(path, value):
    with path.open("xb") as stream:
        stream.write(canonical(value) + b"\n")


def prepare(repo, template_path, output):
    import seed_racing
    if subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip() != SOURCE or \
            subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=repo):
        raise ValueError("preparation requires clean selected source revision")
    template = json.loads(template_path.read_bytes())
    output.mkdir(parents=True, exist_ok=False)
    binary = (repo.parent / "build-mistral-cuda/nextpnr-mistral").resolve()
    cases = []
    for fanout in (64, 96):
        for exponent, seed, role in CASES:
            name = f"fanout-{fanout}-exp-{exponent}-seed-{seed}"
            manifest = copy.deepcopy(template)
            manifest["cohort"]["id"] = "atari-" + name + "-20261008"
            manifest["cwd"] = str(repo)
            manifest["seeds"] = [seed]
            manifest["limits"] = dict(per_run_seconds=600, total_seconds=720, concurrency=1)
            manifest["provenance"] = dict(source_revision=SOURCE, dirty=False, runtime_environment_id="auto")
            argv = manifest["command"]
            argv[0] = str(binary)
            argv[argv.index("--placer-heap-critexp") + 1] = str(exponent)
            # Baseline has no override; treatment changes this option only.
            if fanout == 96:
                argv.extend(["--gpu-opt", "analogueCandidateFanout=96"])
            seed_racing.validate_collection_manifest(manifest)
            write_new(output / (name + ".json"), manifest)
            cases.append(dict(name=name, exponent=exponent, seed=seed, role=role, fanout=fanout,
                              manifest_sha256=sha(canonical(manifest))))
    plan = dict(source_revision=SOURCE, binary_sha256=sha(binary.read_bytes()),
                inputs=[dict(item, sha256=sha(Path(item["path"]).read_bytes())) for item in template["inputs"]],
                required_clocks=template["required_clocks"], max_invocations=8, per_run_seconds=600,
                wall_seconds=5400, target_net=NET, cases=cases,
                gate="both historical failures must remain legal, miss pixel setup and show target net with >64 distinct sinks on same-domain critical path; controls must pass",
                classification="retrospectively selected diagnostic cases, not independent confirmation or default-policy evidence")
    write_new(output / "plan.json", plan)
    return plan


def authenticated(directory):
    import seed_racing
    summaries = list(directory.glob("collection-*.json"))
    if len(summaries) != 1:
        raise ValueError("missing or ambiguous collection summary")
    rows = seed_racing.validate_dataset(seed_racing.assemble_dataset(summaries))
    if len(rows) != 1:
        raise ValueError("expected one attempted case")
    summary = json.loads(summaries[0].read_bytes())
    result = summary["results"][0]
    record = result["artifacts"].get("final_report", {})
    report = None
    if record.get("available"):
        raw = Path(record["path"]).read_bytes()
        if sha(raw) != record["sha256"]:
            raise ValueError("report hash mismatch")
        report = json.loads(raw)
    return rows[0], report, summary


def reproduce_gate(row, report):
    if row["status"] != "analogue_timing_failure" or row["legal_route"] is not True or report is None:
        return False
    clocks = report["timing_summary"]["clocks"]
    if clocks["pixel_clk"]["setup_wns_ns"] > 0:
        return False
    if any(v["hold_wns_ns"] <= 0 or (k != "pixel_clk" and v["setup_wns_ns"] <= 0) for k, v in clocks.items()):
        return False
    on_path = any(p["from"] == p["to"] == "posedge pixel_clk" and
                  any(s.get("net") == NET for s in p["path"]) for p in report["critical_paths"])
    sinks = next((len({(e["cell"], e["port"]) for e in n["endpoints"]})
                  for n in report["detailed_net_timings"] if n["net"] == NET), 0)
    return on_path and sinks > 64


def run(output, dry_run=False):
    import fcntl
    import seed_racing
    plan = json.loads((output / "plan.json").read_bytes())
    manifests = []
    for case in plan["cases"]:
        manifest = json.loads((output / (case["name"] + ".json")).read_bytes())
        if sha(canonical(manifest)) != case["manifest_sha256"]:
            raise ValueError("declaration changed")
        if sha(Path(manifest["command"][0]).read_bytes()) != plan["binary_sha256"]:
            raise ValueError("binary changed")
        manifests.append(manifest)
    for item in plan["inputs"]:
        if sha(Path(item["path"]).read_bytes()) != item["sha256"]:
            raise ValueError("input changed")
    if dry_run:
        total = sum(len(seed_racing.Collector(m, output / "dry" / c["name"]).run(dry_run=True))
                    for c, m in zip(plan["cases"], manifests))
        if total != 8:
            raise ValueError("invocation bound differs")
        print("DRY_RUN_INVOCATIONS", total)
        return
    with (output / "operator.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if (output / "decision.json").exists():
            print("ALREADY_FINALIZED; inspect immutable decision")
            return
        deadline_path = output / "deadline.json"
        if not deadline_path.exists():
            write_new(deadline_path, dict(deadline_unix=time.time() + plan["wall_seconds"]))
        deadline = json.loads(deadline_path.read_bytes())["deadline_unix"]
        observed, identities = [], []
        gate_failed = False
        for index, (case, manifest) in enumerate(zip(plan["cases"], manifests)):
            if index == 2 and gate_failed:
                write_new(output / "decision.json", dict(classification="STOP: historical bottleneck not reproduced", observed=observed))
                print("STOP_REPRODUCTION", flush=True)
                return
            if index == 4 and any(not r["success"] for r in observed if r["role"] == "control"):
                write_new(output / "decision.json", dict(classification="STOP: passing controls did not reproduce", observed=observed))
                print("STOP_CONTROLS", flush=True)
                return
            directory = output / "collections" / case["name"]
            if not directory.exists():
                if time.time() + 720 > deadline or shutil.disk_usage(output).free < 12 * 1024**3:
                    raise ValueError("deadline/free-space admission guard")
                print("START", case["name"], flush=True)
                seed_racing.Collector(manifest, directory).run()
            row, report, summary = authenticated(directory)
            bound = summary["cohort_identity"]["manifest"]
            # Collector seals cohorts; additionally bind them to the declared comparison.
            if bound["binary"]["sha256"] != plan["binary_sha256"] or bound["command"] != manifest["command"]:
                raise ValueError("collected execution differs from declaration")
            if [dict(role=i["role"], sha256=i["sha256"]) for i in bound["inputs"]] != \
                    [dict(role=i["role"], sha256=i["sha256"]) for i in plan["inputs"]]:
                raise ValueError("collected inputs differ from declaration")
            for key in ("seeds", "required_clocks", "limits", "artifacts", "cohort", "repeats"):
                if bound[key] != manifest[key]:
                    raise ValueError("collected configuration differs: " + key)
            identities.append(bound["execution_identity"])
            if any(identity != identities[0] for identity in identities):
                raise ValueError("runtime/backend changed between cases")
            record = dict(case, status=row["status"], success=row["success"], seconds=row["duration_seconds"],
                          margin_ns=row["final_multi_clock_margin_ns"],
                          clocks=report["timing_summary"]["clocks"] if report else None)
            if index < 2:
                record["reproduction_gate"] = reproduce_gate(row, report)
                gate_failed |= not record["reproduction_gate"]
            observed.append(record)
            print("DONE", json.dumps(record, sort_keys=True), flush=True)
        write_new(output / "decision.json", dict(classification="completed diagnostic pairs; not confirmation", observed=observed,
                                                  process_seconds=sum(r["seconds"] for r in observed)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=["prepare", "dry-run", "run"])
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--template", type=Path)
    args = parser.parse_args()
    sys.path.insert(0, str(args.repo.resolve() / "python"))
    if args.operation == "prepare":
        print(sha(canonical(prepare(args.repo.resolve(), args.template.resolve(), args.output.resolve()))))
    else:
        run(args.output.resolve(), args.operation == "dry-run")
