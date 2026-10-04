#!/usr/bin/env python3
"""Bounded seed-race collection and prefix-only offline evaluation.

The module intentionally uses only the Python standard library.  Collection is
driven by a JSON manifest; evaluation consumes fully observed synthetic or real
run records, but hides observations after each policy decision point.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import random
import shutil
import signal
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple


SCHEMA_VERSION = 1
TERMINAL_STATUSES = {
    "completed",
    "process_failure",
    "timeout",
    "cancelled",
    "launch_error",
}
PREFIX_FEATURES = {
    "elapsed_seconds", "phase", "attempt", "round", "work", "searches", "node_expansions", "traversals",
    "backend_retries", "unrouted_connections", "overused_wires", "total_excess_occupancy", "wire_count",
    "affected_nets", "binding_failures", "architecture_failures", "plateau_length", "recent_progress",
    "improvement", "congestion_weight", "bounding_box_expansion", "retry_count", "fallback_count",
    "table_wns_ns", "table_tns_ns", "table_failing_endpoints", "repaired_connections",
    "displaced_connections", "frozen_connections", "repair_improvement", "timing_model", "analogue_wns_ns",
    "analogue_tns_ns", "analogue_clocks",
}


def _positive_number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise ValueError(f"{field} must be a positive finite number")
    return float(value)


def _positive_int(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{field} must be a positive integer")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _json_dump(path: Path, value: Any) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    temporary.replace(path)


def _safe_component(value: Any) -> str:
    text = str(value)
    safe = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in text)
    if not safe or safe in {".", ".."}:
        raise ValueError(f"unsafe path component: {text!r}")
    return safe[:80]


def _require_keys(mapping: Mapping[str, Any], keys: Iterable[str], context: str) -> None:
    missing = sorted(set(keys) - set(mapping))
    if missing:
        raise ValueError(f"{context} missing required fields: {', '.join(missing)}")


def validate_collection_manifest(document: Mapping[str, Any]) -> Dict[str, Any]:
    """Validate and normalize a collection manifest without touching the filesystem."""
    _require_keys(document, ("cohort", "command", "seeds", "repeats", "limits"), "manifest")
    if document.get("schema_version", SCHEMA_VERSION) != SCHEMA_VERSION:
        raise ValueError("unsupported collection manifest schema_version")
    cohort = document["cohort"]
    if not isinstance(cohort, dict):
        raise ValueError("cohort must be an object")
    _require_keys(cohort, ("id", "design_id", "mapped_design_id", "constraint_family"), "cohort")
    command = document["command"]
    if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
        raise ValueError("command must be a non-empty argv string array")
    seeds = document["seeds"]
    if not isinstance(seeds, list) or not seeds or any(isinstance(seed, (dict, list, bool)) for seed in seeds):
        raise ValueError("seeds must be a non-empty scalar array")
    if len({str(seed) for seed in seeds}) != len(seeds):
        raise ValueError("seeds must be unique")
    repeats = _positive_int(document["repeats"], "repeats")
    limits = document["limits"]
    if not isinstance(limits, dict):
        raise ValueError("limits must be an object")
    _require_keys(limits, ("per_run_seconds", "total_seconds", "concurrency"), "limits")
    per_run = _positive_number(limits["per_run_seconds"], "limits.per_run_seconds")
    total = _positive_number(limits["total_seconds"], "limits.total_seconds")
    concurrency = _positive_int(limits["concurrency"], "limits.concurrency")
    inputs = document.get("inputs", [])
    if not isinstance(inputs, list):
        raise ValueError("inputs must be an array")
    for item in inputs:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str):
            raise ValueError("each input needs a string path")
    artifacts = document.get("artifacts", {})
    if not isinstance(artifacts, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in artifacts.items()):
        raise ValueError("artifacts must map names to relative paths")
    for relative in artifacts.values():
        if Path(relative).is_absolute() or ".." in Path(relative).parts:
            raise ValueError("artifact paths must stay within a run directory")
    environment = document.get("environment", {})
    if not isinstance(environment, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in environment.items()):
        raise ValueError("environment must be an explicit string map")
    return {
        "schema_version": SCHEMA_VERSION,
        "cohort": dict(cohort),
        "command": list(command),
        "seeds": list(seeds),
        "repeats": repeats,
        "limits": {"per_run_seconds": per_run, "total_seconds": total, "concurrency": concurrency},
        "inputs": [dict(item) for item in inputs],
        "artifacts": dict(artifacts),
        "environment": dict(environment),
        "cwd": document.get("cwd"),
        "provenance": dict(document.get("provenance", {})),
    }


def _expand_argv(template: Sequence[str], fields: Mapping[str, str]) -> List[str]:
    result = []
    for argument in template:
        try:
            result.append(argument.format_map(fields))
        except KeyError as error:
            raise ValueError(f"unknown command placeholder: {error.args[0]}") from error
    return result


def _resolved_binary(argv0: str, cwd: Optional[str], environment: Mapping[str, str]) -> Optional[Path]:
    candidate = Path(argv0)
    if candidate.is_absolute() or candidate.parent != Path("."):
        if not candidate.is_absolute() and cwd:
            candidate = Path(cwd) / candidate
        return candidate.resolve() if candidate.is_file() else None
    found = shutil.which(argv0, path=environment.get("PATH", os.environ.get("PATH")))
    return Path(found).resolve() if found else None


def _terminate_owned_child(process: subprocess.Popen[Any]) -> None:
    """Terminate only the fresh process group created for this child."""
    if process.poll() is not None:
        return
    if os.name == "posix":
        os.killpg(process.pid, signal.SIGTERM)
    else:  # pragma: no cover - exercised on Windows only
        process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
        else:  # pragma: no cover
            process.kill()


@dataclass(frozen=True)
class RunSpec:
    run_id: str
    seed: Any
    repeat: int
    directory: Path
    argv: Tuple[str, ...]


class Collector:
    def __init__(self, manifest: Mapping[str, Any], output_root: Path):
        self.manifest = validate_collection_manifest(manifest)
        self.output_root = output_root.resolve()
        self._active: Dict[str, subprocess.Popen[Any]] = {}
        self._lock = threading.Lock()

    def plan(self) -> List[RunSpec]:
        cohort_id = _safe_component(self.manifest["cohort"]["id"])
        specs = []
        for seed in self.manifest["seeds"]:
            for repeat in range(1, self.manifest["repeats"] + 1):
                base = f"seed-{_safe_component(seed)}-repeat-{repeat}"
                # UUIDs make independent collectors non-overwriting without relying on a race-prone scan.
                run_id = f"{base}-{uuid.uuid4().hex[:12]}"
                directory = self.output_root / cohort_id / run_id
                fields = {
                    "seed": str(seed),
                    "repeat": str(repeat),
                    "run_id": run_id,
                    "run_dir": str(directory),
                    "telemetry": str(directory / "telemetry.jsonl"),
                    "report": str(directory / "report.json"),
                }
                specs.append(RunSpec(run_id, seed, repeat, directory, tuple(_expand_argv(self.manifest["command"], fields))))
        return specs

    def _input_records(self) -> List[Dict[str, Any]]:
        records = []
        base = Path(self.manifest["cwd"] or os.getcwd())
        for item in self.manifest["inputs"]:
            path = Path(item["path"])
            resolved = (base / path).resolve() if not path.is_absolute() else path.resolve()
            record = dict(item)
            record.update({"resolved_path": str(resolved), "sha256": sha256_file(resolved) if resolved.is_file() else None})
            records.append(record)
        return records

    def _run_one(self, spec: RunSpec, deadline: float) -> Dict[str, Any]:
        now = time.monotonic()
        if now >= deadline:
            return {"run_id": spec.run_id, "seed": spec.seed, "repeat": spec.repeat, "status": "not_started_total_budget"}
        spec.directory.mkdir(parents=True, exist_ok=False)
        environment = os.environ.copy()
        environment.update(self.manifest["environment"])
        binary = _resolved_binary(spec.argv[0], self.manifest["cwd"], environment)
        started_wall = time.time()
        immutable = {
            "schema_version": SCHEMA_VERSION,
            "run_id": spec.run_id,
            "cohort": self.manifest["cohort"],
            "seed": spec.seed,
            "replicate_index": spec.repeat,
            "argv": list(spec.argv),
            "cwd": str(Path(self.manifest["cwd"] or os.getcwd()).resolve()),
            "environment": self.manifest["environment"],
            "limits": self.manifest["limits"],
            "provenance": self.manifest["provenance"],
            "inputs": self._input_records(),
            "binary": {"path": str(binary) if binary else None, "sha256": sha256_file(binary) if binary else None},
            "artifacts": self.manifest["artifacts"],
            "started_unix_seconds": started_wall,
        }
        _json_dump(spec.directory / "manifest.json", immutable)
        result: Dict[str, Any] = {"run_id": spec.run_id, "seed": spec.seed, "repeat": spec.repeat}
        stdout_path, stderr_path = spec.directory / "stdout.log", spec.directory / "stderr.log"
        started = time.monotonic()
        process: Optional[subprocess.Popen[Any]] = None
        try:
            with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
                process = subprocess.Popen(
                    list(spec.argv), cwd=self.manifest["cwd"], env=environment, stdout=stdout, stderr=stderr,
                    start_new_session=(os.name == "posix"),
                )
                with self._lock:
                    self._active[spec.run_id] = process
                timeout = min(self.manifest["limits"]["per_run_seconds"], max(0.001, deadline - time.monotonic()))
                try:
                    return_code = process.wait(timeout=timeout)
                    result.update({"status": "completed" if return_code == 0 else "process_failure", "return_code": return_code, "signal": -return_code if return_code < 0 else None})
                except subprocess.TimeoutExpired:
                    _terminate_owned_child(process)
                    reason = "total_budget" if time.monotonic() >= deadline else "per_run_timeout"
                    result.update({"status": "timeout", "termination_reason": reason, "return_code": process.returncode, "signal": -process.returncode if process.returncode and process.returncode < 0 else None})
        except OSError as error:
            result.update({"status": "launch_error", "error": f"{type(error).__name__}: {error}"})
        finally:
            with self._lock:
                self._active.pop(spec.run_id, None)
        result["elapsed_seconds"] = time.monotonic() - started
        artifacts = {
            "stdout": {"path": str(stdout_path), "sha256": sha256_file(stdout_path)},
            "stderr": {"path": str(stderr_path), "sha256": sha256_file(stderr_path)},
        }
        for name, relative in self.manifest["artifacts"].items():
            path = spec.directory / relative
            artifacts[name] = {"path": str(path), "sha256": sha256_file(path) if path.is_file() else None, "available": path.is_file()}
        result["artifacts"] = artifacts
        _json_dump(spec.directory / "result.json", result)
        return result

    def cancel(self) -> None:
        with self._lock:
            processes = list(self._active.values())
        for process in processes:
            _terminate_owned_child(process)

    def run(self, dry_run: bool = False) -> List[Dict[str, Any]]:
        specs = self.plan()
        if dry_run:
            return [{"run_id": s.run_id, "seed": s.seed, "repeat": s.repeat, "directory": str(s.directory), "argv": list(s.argv), "status": "dry_run"} for s in specs]
        self.output_root.mkdir(parents=True, exist_ok=True)
        deadline = time.monotonic() + self.manifest["limits"]["total_seconds"]
        results = []
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=self.manifest["limits"]["concurrency"]) as executor:
                future_specs = [(executor.submit(self._run_one, spec, deadline), spec) for spec in specs]
                for future, spec in future_specs:
                    try:
                        results.append(future.result())
                    except Exception as error:  # preserve other completed runs
                        results.append({"run_id": spec.run_id, "seed": spec.seed, "repeat": spec.repeat, "status": "runner_error", "error": f"{type(error).__name__}: {error}"})
        except KeyboardInterrupt:
            self.cancel()
            raise
        summary = {"schema_version": SCHEMA_VERSION, "cohort": self.manifest["cohort"], "results": results}
        _json_dump(self.output_root / f"collection-{uuid.uuid4().hex}.json", summary)
        return results


def load_jsonl(path: Path) -> Tuple[List[Dict[str, Any]], bool]:
    """Load a valid telemetry prefix and flag truncation or schema/order errors."""
    records, truncated = [], False
    expected_sequence = 0
    prior_elapsed = -math.inf
    run_id = None
    terminal_seen = False
    phase_stack: List[Tuple[str, int]] = []
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            try:
                value = json.loads(line)
                if (not isinstance(value, dict) or value.get("schema_version") != SCHEMA_VERSION or
                        value.get("sequence") != expected_sequence or not isinstance(value.get("run_id"), str) or
                        not isinstance(value.get("event"), str) or terminal_seen):
                    raise ValueError
                elapsed = value.get("elapsed_s")
                if (isinstance(elapsed, bool) or not isinstance(elapsed, (int, float)) or
                        not math.isfinite(elapsed) or elapsed < prior_elapsed):
                    raise ValueError
                if run_id is None:
                    run_id = value["run_id"]
                elif value["run_id"] != run_id:
                    raise ValueError
                event, phase, attempt = value["event"], value.get("phase"), value.get("attempt")
                if expected_sequence == 0 and event != "run_start":
                    raise ValueError
                if event == "phase_start":
                    if not isinstance(phase, str) or isinstance(attempt, bool) or not isinstance(attempt, int):
                        raise ValueError
                    phase_stack.append((phase, attempt))
                elif event == "phase_end":
                    if not phase_stack or phase_stack[-1] != (phase, attempt):
                        raise ValueError
                    phase_stack.pop()
                elif event == "run_end":
                    if phase_stack:
                        raise ValueError
                elif phase is not None and (phase, attempt) not in phase_stack:
                    raise ValueError
                records.append(value)
                expected_sequence += 1
                prior_elapsed = float(elapsed)
                terminal_seen = value["event"] == "run_end"
            except (json.JSONDecodeError, ValueError):
                truncated = True
                break
    return records, truncated


def validate_dataset(document: Mapping[str, Any]) -> List[Dict[str, Any]]:
    if document.get("schema_version", SCHEMA_VERSION) != SCHEMA_VERSION or not isinstance(document.get("runs"), list):
        raise ValueError("unsupported evaluator dataset")
    normalized = []
    for run in document["runs"]:
        _require_keys(run, ("run_id", "cohort_id", "mapped_design_id", "constraint_family", "seed", "status", "duration_seconds", "observations", "outcome"), "run")
        duration = _positive_number(run["duration_seconds"], "duration_seconds")
        observations = run["observations"]
        if not isinstance(observations, list):
            raise ValueError("observations must be an array")
        prior = -math.inf
        clean = []
        for observation in observations:
            if not isinstance(observation, dict) or "elapsed_seconds" not in observation:
                raise ValueError("each observation needs elapsed_seconds")
            elapsed = float(observation["elapsed_seconds"])
            if not math.isfinite(elapsed) or elapsed < prior or elapsed < 0 or elapsed > duration:
                raise ValueError("observation times must be finite, monotonic, and within duration")
            prior = elapsed
            clean.append(dict(observation))
        outcome = run["outcome"]
        if not isinstance(outcome, dict):
            raise ValueError("outcome must be an object")
        legal = outcome.get("legal_route") is True
        required_clocks = outcome.get("required_clocks")
        if (not isinstance(required_clocks, list) or not required_clocks or
                not all(isinstance(name, str) and name for name in required_clocks) or
                len(set(required_clocks)) != len(required_clocks)):
            raise ValueError("outcome.required_clocks must be a non-empty unique string array")
        clocks = outcome.get("analogue_clocks")
        clock_by_name = {}
        if isinstance(clocks, list):
            for clock in clocks:
                if not isinstance(clock, dict) or not isinstance(clock.get("name"), str):
                    raise ValueError("each analogue clock needs a string name")
                if clock["name"] in clock_by_name:
                    raise ValueError("analogue clock names must be unique")
                clock_by_name[clock["name"]] = clock
        missing_required = [name for name in required_clocks if name not in clock_by_name]
        required_records = [clock_by_name[name] for name in required_clocks if name in clock_by_name]
        def finite_metric(value: Any) -> bool:
            return (not isinstance(value, bool) and isinstance(value, (int, float)) and
                    math.isfinite(float(value)))
        timing_available = (not missing_required and len(required_records) == len(required_clocks) and
                            all(clock.get("available") is True and finite_metric(clock.get("setup_wns_ns")) and
                                finite_metric(clock.get("hold_wns_ns")) for clock in required_records))
        if not timing_available:
            analogue_pass = False
            final_margin = None
        else:
            margins = [min(float(clock["setup_wns_ns"]), float(clock["hold_wns_ns"]))
                       for clock in required_records]
            analogue_pass = all(margin >= 0 for margin in margins)
            final_margin = min(margins)
        copy = dict(run)
        copy.update({"duration_seconds": duration, "observations": clean,
                     "success": legal and analogue_pass and run["status"] == "completed",
                     "legal_route": legal, "analogue_timing_pass": analogue_pass,
                     "timing_available": timing_available, "missing_required_clocks": missing_required,
                     "final_multi_clock_margin_ns": final_margin})
        normalized.append(copy)
    return normalized


def observation_at(run: Mapping[str, Any], checkpoint: float) -> Optional[Dict[str, Any]]:
    """Return a defensive copy of the latest prefix observation, never final data."""
    visible = [obs for obs in run["observations"] if obs["elapsed_seconds"] <= checkpoint]
    if not visible:
        return None
    return {key: value for key, value in visible[-1].items() if key in PREFIX_FEATURES}


def heuristic_score(observation: Optional[Mapping[str, Any]]) -> Tuple[float, ...]:
    """A transparent prefix-only score; larger tuples rank better."""
    if observation is None:
        return (-math.inf,)
    overuse = observation.get("total_excess_occupancy")
    if overuse is None:
        overuse = observation.get("overused_wires")
    unrouted = observation.get("unrouted_connections")
    progress = observation.get("recent_progress")
    table_wns = observation.get("table_wns_ns")
    return (
        -float(overuse) if isinstance(overuse, (int, float)) else -math.inf,
        -float(unrouted) if isinstance(unrouted, (int, float)) else -math.inf,
        float(progress) if isinstance(progress, (int, float)) else -math.inf,
        float(table_wns) if isinstance(table_wns, (int, float)) else -math.inf,
    )


def _metrics(retained: Sequence[Mapping[str, Any]], population: Sequence[Mapping[str, Any]], aggregate: float, makespan: float, label: str) -> Dict[str, Any]:
    successes = [run for run in population if run["success"]]
    retained_successes = [run for run in retained if run["success"]]
    recall = len(retained_successes) / len(successes) if successes else None
    margins = [run["final_multi_clock_margin_ns"] for run in retained_successes if run["final_multi_clock_margin_ns"] is not None]
    return {
        "cost_model": label,
        "population_runs": len(population),
        "population_eventual_successes": len(successes),
        "retained_runs": len(retained),
        "retained_eventual_successes": len(retained_successes),
        "eventual_success_recall": recall,
        "false_rejections": len(successes) - len(retained_successes),
        "at_least_one_success": bool(retained_successes),
        "aggregate_compute_seconds": aggregate,
        "serial_wall_clock_seconds": makespan,
        "final_multi_clock_margin_ns": max(margins) if margins else None,
    }


def successive_halving(
    runs: Sequence[Mapping[str, Any]], checkpoints: Sequence[float], quotas: Sequence[int],
    exploratory_survivors: int, scheduler_seed: int, restart: bool,
    score_overhead_seconds: float = 0.0,
) -> Dict[str, Any]:
    if not checkpoints or len(checkpoints) != len(quotas):
        raise ValueError("checkpoints and quotas must be non-empty and equal length")
    if any(b <= a for a, b in zip(checkpoints, checkpoints[1:])) or any(value <= 0 for value in checkpoints):
        raise ValueError("checkpoints must be fixed, positive, increasing decision points")
    if any(isinstance(q, bool) or not isinstance(q, int) or q <= 0 for q in quotas):
        raise ValueError("quotas must be positive integers")
    if exploratory_survivors < 0:
        raise ValueError("exploratory_survivors cannot be negative")
    rng = random.Random(scheduler_seed)
    survivors = list(runs)
    stage_records, aggregate = [], 0.0
    prior_checkpoint = 0.0
    for checkpoint, quota in zip(checkpoints, quotas):
        evaluated = list(survivors)
        if restart:
            aggregate += sum(min(run["duration_seconds"], checkpoint) for run in evaluated)
        else:
            aggregate += sum(max(0.0, min(run["duration_seconds"], checkpoint) - min(run["duration_seconds"], prior_checkpoint)) for run in evaluated)
        aggregate += score_overhead_seconds * len(evaluated)
        # An independent scheduler RNG breaks score ties. Neither numeric seed nor
        # seed-bearing run IDs are visible to ranking.
        scored = [(heuristic_score(observation_at(run, checkpoint)), rng.random(), run) for run in evaluated]
        scored.sort(key=lambda item: (item[0], item[1]), reverse=True)
        keep = min(quota, len(scored))
        explore = min(exploratory_survivors, keep)
        ranked_count = keep - explore
        ranked = [run for _, _, run in scored[:ranked_count]]
        remaining = [run for _, _, run in scored[ranked_count:] if run not in ranked]
        explored = rng.sample(remaining, min(explore, len(remaining)))
        survivors = ranked + explored
        stage_records.append({"checkpoint": checkpoint, "evaluated": [run["run_id"] for run in evaluated], "promoted": [run["run_id"] for run in survivors], "exploratory": [run["run_id"] for run in explored]})
        prior_checkpoint = checkpoint
    if restart:
        aggregate += sum(run["duration_seconds"] for run in survivors)
    else:
        aggregate += sum(max(0.0, run["duration_seconds"] - min(run["duration_seconds"], prior_checkpoint)) for run in survivors)
    metrics = _metrics(survivors, runs, aggregate, aggregate, "restart_execution" if restart else "ideal_resumable_simulation")
    metrics.update({"policy": "successive_halving", "scheduler_seed": scheduler_seed, "stages": stage_records, "retained": [run["run_id"] for run in survivors]})
    return metrics


def random_full_run_baseline(runs: Sequence[Mapping[str, Any]], budget_seconds: float, scheduler_seed: int) -> Dict[str, Any]:
    _positive_number(budget_seconds, "budget_seconds")
    order = list(runs)
    random.Random(scheduler_seed).shuffle(order)
    completed, consumed, first_success = [], 0.0, None
    for run in order:
        if consumed + run["duration_seconds"] > budget_seconds:
            consumed = budget_seconds  # final observation is censored and cannot produce a success label
            break
        consumed += run["duration_seconds"]
        completed.append(run)
        if first_success is None and run["success"]:
            first_success = consumed
    metrics = _metrics(completed, runs, consumed, consumed, "observed_full_run_serial")
    metrics.update({"policy": "random_full_run", "scheduler_seed": scheduler_seed, "order": [run["run_id"] for run in order], "completed": [run["run_id"] for run in completed], "time_to_first_success_seconds": first_success})
    return metrics


def evaluate(document: Mapping[str, Any], checkpoints: Sequence[float], quotas: Sequence[int], exploratory: int, scheduler_seeds: Sequence[int], budget: float) -> Dict[str, Any]:
    runs = validate_dataset(document)
    # Replicates/prefixes stay together because the unit of scheduling is the complete run.
    return {
        "schema_version": SCHEMA_VERSION,
        "evaluation_population": {"runs": len(runs), "cohorts": sorted({run["cohort_id"] for run in runs}), "mapped_design_constraint_families": sorted({f'{run["mapped_design_id"]}:{run["constraint_family"]}' for run in runs}), "scope": "single-design" if len({(run["mapped_design_id"], run["constraint_family"]) for run in runs}) == 1 else "cross-design-descriptive"},
        "random_full_run": [random_full_run_baseline(runs, budget, seed) for seed in scheduler_seeds],
        "successive_halving_ideal": [successive_halving(runs, checkpoints, quotas, exploratory, seed, False) for seed in scheduler_seeds],
        "successive_halving_restart": [successive_halving(runs, checkpoints, quotas, exploratory, seed, True) for seed in scheduler_seeds],
    }


def _comma_numbers(value: str, integer: bool = False) -> List[Any]:
    converter = int if integer else float
    return [converter(item) for item in value.split(",") if item]


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)
    collect = subparsers.add_parser("collect", help="execute a bounded collection manifest")
    collect.add_argument("manifest", type=Path)
    collect.add_argument("--output", required=True, type=Path)
    collect.add_argument("--dry-run", action="store_true")
    replay = subparsers.add_parser("evaluate", help="replay prefix-only policies offline")
    replay.add_argument("dataset", type=Path)
    replay.add_argument("--checkpoints", required=True)
    replay.add_argument("--quotas", required=True)
    replay.add_argument("--exploratory-survivors", type=int, default=1)
    replay.add_argument("--scheduler-seeds", default="0,1,2,3,4")
    replay.add_argument("--budget-seconds", required=True, type=float)
    replay.add_argument("--output", type=Path)
    arguments = parser.parse_args(argv)
    try:
        with arguments.manifest.open(encoding="utf-8") if arguments.operation == "collect" else arguments.dataset.open(encoding="utf-8") as stream:
            document = json.load(stream)
        if arguments.operation == "collect":
            result = Collector(document, arguments.output).run(arguments.dry_run)
        else:
            result = evaluate(document, _comma_numbers(arguments.checkpoints), _comma_numbers(arguments.quotas, True), arguments.exploratory_survivors, _comma_numbers(arguments.scheduler_seeds, True), arguments.budget_seconds)
        if getattr(arguments, "output", None) and arguments.operation == "evaluate":
            _json_dump(arguments.output, result)
        else:
            json.dump(result, sys.stdout, indent=2, sort_keys=True, allow_nan=False)
            sys.stdout.write("\n")
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
