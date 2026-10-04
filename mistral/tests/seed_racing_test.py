#!/usr/bin/env python3
import json
import os
import signal
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

import seed_racing


FIXTURE = ROOT / "mistral" / "tests" / "seed_racing" / "synthetic.json"


class DatasetTests(unittest.TestCase):
    def setUp(self):
        self.document = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.runs = seed_racing.validate_dataset(self.document)
        self.by_id = {run["run_id"]: run for run in self.runs}

    def test_legality_and_final_analogue_all_clock_timing_are_separate(self):
        self.assertTrue(self.by_id["legal-analogue-failure"]["legal_route"])
        self.assertFalse(self.by_id["legal-analogue-failure"]["success"])
        self.assertFalse(self.by_id["table-analogue-disagreement"]["success"])
        self.assertFalse(self.by_id["multi-clock-failure"]["success"])
        self.assertFalse(self.by_id["no-timing-data"]["timing_available"])
        self.assertTrue(self.by_id["late-winner"]["success"])

    def test_omitted_required_clock_cannot_be_a_success(self):
        document = json.loads(FIXTURE.read_text(encoding="utf-8"))
        winner = next(run for run in document["runs"] if run["run_id"] == "multi-clock-failure")
        winner["outcome"]["required_clocks"] = ["fast"]
        with self.assertRaisesRegex(ValueError, "does not match dataset declaration"):
            seed_racing.validate_dataset(document)

    def test_prefix_hides_seed_final_and_future_observations(self):
        run = dict(self.by_id["late-winner"])
        run["observations"] = [dict(item) for item in run["observations"]]
        run["observations"][0].update({"seed": -999999, "router_seed": 99, "duration_seconds": 1, "final": {"success": True}, "surprise_outcome": "winner"})
        prefix = seed_racing.observation_at(run, 5)
        self.assertEqual(set(prefix), set(run["observations"][0]).intersection(seed_racing.PREFIX_FEATURES))
        score = seed_racing.heuristic_score(prefix)
        run["observations"][1]["overused_wires"] = -1000000
        self.assertEqual(score, seed_racing.heuristic_score(seed_racing.observation_at(run, 5)))

    def test_restart_and_ideal_cost_charge_repeated_work_honestly(self):
        pair = [self.by_id["early-leader-late-failure"], self.by_id["late-winner"]]
        ideal = seed_racing.successive_halving(pair, [5], [1], 0, 7, restart=False)
        restart = seed_racing.successive_halving(pair, [5], [1], 0, 7, restart=True)
        self.assertEqual(ideal["aggregate_compute_seconds"], 25)
        self.assertEqual(restart["aggregate_compute_seconds"], 30)
        self.assertEqual(ideal["cost_model"], "ideal_resumable_simulation")
        self.assertEqual(restart["cost_model"], "restart_execution")

    def test_halving_records_terminal_successes_and_drops_terminal_failures(self):
        terminal_success = dict(self.by_id["late-winner"], run_id="terminal-success",
                                duration_seconds=2, outcome_observed_seconds=2)
        terminal_failure = dict(self.by_id["early-leader-late-failure"],
                                run_id="terminal-failure", duration_seconds=3,
                                outcome_observed_seconds=3)
        active = dict(self.by_id["late-winner"], run_id="active", duration_seconds=10)
        result = seed_racing.successive_halving(
            [terminal_success, terminal_failure, active], [5], [1], 0, 9, restart=False)
        self.assertTrue(result["at_least_one_success"])
        self.assertIn("terminal-success", result["retained"])
        self.assertNotIn("terminal-failure", result["retained"])
        self.assertEqual(result["stages"][0]["terminal_successes"], ["terminal-success"])
        self.assertEqual(result["stages"][0]["terminal_failures"], ["terminal-failure"])
        self.assertEqual(result["stages"][0]["promoted"], ["active"])

    def test_halving_never_exposes_outcome_before_observed_timestamp(self):
        run = dict(self.by_id["late-winner"], duration_seconds=5,
                   outcome_observed_seconds=5)
        result = seed_racing.successive_halving([run], [4], [1], 0, 9, restart=False)
        self.assertEqual(result["stages"][0]["terminal_successes"], [])
        self.assertEqual(result["stages"][0]["promoted"], [run["run_id"]])

    def test_halving_respects_shared_fixed_budget(self):
        result = seed_racing.successive_halving(
            self.runs, [5], [3], 0, 4, restart=True, budget_seconds=20)
        self.assertLessEqual(result["aggregate_compute_seconds"], 20)
        self.assertTrue(result["budget_exhausted"])
        self.assertEqual(result["retained"], [])

    def test_exploration_survivor_and_scheduler_are_reproducible(self):
        first = seed_racing.successive_halving(self.runs, [5], [3], 1, 1234, restart=False)
        second = seed_racing.successive_halving(self.runs, [5], [3], 1, 1234, restart=False)
        self.assertEqual(first["stages"], second["stages"])
        self.assertEqual(len(first["stages"][0]["exploratory"]), 1)
        self.assertIn(first["stages"][0]["exploratory"][0], first["retained"])

    def test_numeric_seed_is_not_a_tie_breaker_or_feature(self):
        def tied_runs(ids_and_seeds):
            return [{
                "run_id": run_id, "seed": numeric_seed, "duration_seconds": 10,
                "observations": [{"elapsed_seconds": 5, "overused_wires": 1}],
                "success": False, "final_multi_clock_margin_ns": None,
            } for run_id, numeric_seed in ids_and_seeds]
        first = seed_racing.successive_halving(tied_runs((("seed-999999", 999999), ("seed-1", -100))), [5], [1], 0, 81, restart=False)
        second = seed_racing.successive_halving(tied_runs((("seed-2", -8), ("seed-777777", 777777))), [5], [1], 0, 81, restart=False)
        first_position = ["seed-999999", "seed-1"].index(first["retained"][0])
        second_position = ["seed-2", "seed-777777"].index(second["retained"][0])
        self.assertEqual(first_position, second_position)

    def test_censored_and_all_failure_populations_are_honest(self):
        failures = [run for run in self.runs if run["run_id"] != "late-winner"]
        result = seed_racing.random_full_run_baseline(failures, 10_000, 2)
        self.assertEqual(result["population_eventual_successes"], 0)
        self.assertIsNone(result["eventual_success_recall"])
        self.assertFalse(result["at_least_one_success"])
        censored = seed_racing.random_full_run_baseline(self.runs, 1, 2)
        self.assertEqual(censored["completed"], [])
        self.assertFalse(censored["at_least_one_success"])

    def test_dataset_rejects_non_numeric_observation_time(self):
        self.assertIsInstance(self.runs[0]["observations"][0]["elapsed_seconds"], float)
        for invalid in ("5", 10 ** 10000):
            document = json.loads(FIXTURE.read_text(encoding="utf-8"))
            document["runs"][0]["observations"][0]["elapsed_seconds"] = invalid
            with self.assertRaisesRegex(ValueError, "finite numeric"):
                seed_racing.validate_dataset(document)

    def test_oversized_numeric_fields_raise_validation_errors(self):
        enormous = 10 ** 10000
        for field in ("duration_seconds", "outcome_observed_seconds"):
            document = json.loads(FIXTURE.read_text(encoding="utf-8"))
            document["runs"][0][field] = enormous
            with self.assertRaisesRegex(ValueError, "positive finite"):
                seed_racing.validate_dataset(document)
        with self.assertRaisesRegex(ValueError, "positive finite"):
            seed_racing.successive_halving(
                self.runs, [5], [1], 0, 1, restart=False, budget_seconds=enormous)

    def test_dataset_accepts_timing_constraint_failure(self):
        document = json.loads(FIXTURE.read_text(encoding="utf-8"))
        document["runs"][0]["status"] = "timing_constraint_failure"
        seed_racing.validate_dataset(document)

    def test_dataset_binds_gpu_runs_to_the_cohort_execution_backend(self):
        document = json.loads(FIXTURE.read_text(encoding="utf-8"))
        identity = document["cohort_identities"]["synthetic"]
        identity["manifest"]["command"] = ["nextpnr-mistral", "--router", "gpu"]
        runtime_manifest = {"runtime_binary": "/test/nextpnr", "files": []}
        runtime_encoded = json.dumps(
            runtime_manifest, sort_keys=True, separators=(",", ":")).encode()
        runtime_id = "sha256:" + seed_racing.hashlib.sha256(runtime_encoded).hexdigest()
        identity["manifest"]["execution_identity"] = {
            "backend": "cuda:" + "a" * 32 + ":0000:01:00.0:GPU",
            "runtime_environment_id": runtime_id
        }
        identity["manifest"]["provenance"] = {"runtime_environment_id": runtime_id}
        identity["manifest"]["binary"] = {
            "runtime_environment": {"runtime_environment_id": runtime_id,
                                    "manifest": runtime_manifest}}
        encoded = json.dumps(identity["manifest"], sort_keys=True, separators=(",", ":")).encode()
        identity["fingerprint_sha256"] = seed_racing.hashlib.sha256(encoded).hexdigest()
        for run in document["runs"]:
            if run["cohort_id"] == "synthetic":
                run["cohort_fingerprint_sha256"] = identity["fingerprint_sha256"]
                run["outcome"]["execution_backend"] = identity["manifest"]["execution_identity"]["backend"]
        seed_racing.validate_dataset(document)
        document["runs"][0]["outcome"]["execution_backend"] = "cpu-reference"
        with self.assertRaisesRegex(ValueError, "execution backend"):
            seed_racing.validate_dataset(document)
        del document["runs"][0]["outcome"]["execution_backend"]
        with self.assertRaisesRegex(ValueError, "execution backend"):
            seed_racing.validate_dataset(document)
        document["runs"][0]["outcome"]["execution_backend"] = \
            identity["manifest"]["execution_identity"]["backend"]
        forged_runtime_id = "sha256:" + "b" * 64
        identity["manifest"]["execution_identity"]["runtime_environment_id"] = forged_runtime_id
        identity["manifest"]["provenance"]["runtime_environment_id"] = forged_runtime_id
        identity["manifest"]["binary"]["runtime_environment"][
            "runtime_environment_id"] = forged_runtime_id
        encoded = json.dumps(identity["manifest"], sort_keys=True, separators=(",", ":")).encode()
        identity["fingerprint_sha256"] = seed_racing.hashlib.sha256(encoded).hexdigest()
        for run in document["runs"]:
            if run["cohort_id"] == "synthetic":
                run["cohort_fingerprint_sha256"] = identity["fingerprint_sha256"]
        with self.assertRaisesRegex(ValueError, "runtime environment"):
            seed_racing.validate_dataset(document)

    def test_dataset_rejects_unknown_terminal_status(self):
        document = json.loads(FIXTURE.read_text(encoding="utf-8"))
        document["runs"][0]["status"] = "complete"
        with self.assertRaisesRegex(ValueError, "supported terminal"):
            seed_racing.validate_dataset(document)

    def test_dataset_rejects_duplicate_run_ids(self):
        document = json.loads(FIXTURE.read_text(encoding="utf-8"))
        document["runs"][1]["run_id"] = document["runs"][0]["run_id"]
        with self.assertRaisesRegex(ValueError, "run_id.*unique"):
            seed_racing.validate_dataset(document)

    def test_truncated_jsonl_retains_only_valid_prefix(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "telemetry.jsonl"
            path.write_text('{"schema_version":1,"sequence":0,"run_id":"r","event":"run_start","elapsed_s":0}\n{"sequence":', encoding="utf-8")
            records, truncated = seed_racing.load_jsonl(path)
        self.assertEqual(len(records), 1)
        self.assertTrue(truncated)

    def test_jsonl_rejects_sequence_run_and_time_regressions(self):
        cases = [
            '{"schema_version":1,"sequence":2,"run_id":"r","event":"iteration","elapsed_s":1}',
            '{"schema_version":1,"sequence":1,"run_id":"other","event":"iteration","elapsed_s":1}',
            '{"schema_version":1,"sequence":1,"run_id":"r","event":"iteration","elapsed_s":-1}',
        ]
        first = '{"schema_version":1,"sequence":0,"run_id":"r","event":"run_start","elapsed_s":0}'
        with tempfile.TemporaryDirectory() as temporary:
            for index, invalid in enumerate(cases):
                path = Path(temporary) / f"telemetry-{index}.jsonl"
                path.write_text(first + "\n" + invalid + "\n", encoding="utf-8")
                records, truncated = seed_racing.load_jsonl(path)
                self.assertEqual(len(records), 1)
                self.assertTrue(truncated)

    def test_jsonl_rejects_nonfinite_duplicate_start_and_outer_phase_events(self):
        start = {"schema_version": 1, "sequence": 0, "run_id": "r",
                 "event": "run_start", "phase": None, "attempt": None, "elapsed_s": 0}
        phase = {"schema_version": 1, "sequence": 1, "run_id": "r",
                 "event": "phase_start", "phase": "outer", "attempt": 1, "elapsed_s": 1}
        invalid = [
            dict(start, elapsed_s=-1),
            dict(start, sequence=1, elapsed_s=1),
            dict(phase, sequence=2, event="iteration", phase="inner", attempt=2,
                 elapsed_s=2),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "telemetry.jsonl"
            path.write_text(json.dumps(invalid[0]) + "\n", encoding="utf-8")
            self.assertEqual(seed_racing.load_jsonl(path), ([], True))
            path.write_text(json.dumps(start) + "\n" + json.dumps(invalid[1]) + "\n",
                            encoding="utf-8")
            self.assertEqual(len(seed_racing.load_jsonl(path)[0]), 1)
            for invalid_record in (
                    dict(start, schema_version=True),
                    dict(start, sequence=False),
                    dict(start, event="unknown")):
                path.write_text(json.dumps(invalid_record) + "\n", encoding="utf-8")
                self.assertEqual(seed_racing.load_jsonl(path), ([], True))
            path.write_text(json.dumps(start) + "\n" + json.dumps(phase) + "\n" +
                            json.dumps(invalid[2]) + "\n", encoding="utf-8")
            self.assertEqual(len(seed_racing.load_jsonl(path)[0]), 2)
            path.write_text(json.dumps(start) + "\n" +
                            '{"schema_version":1,"sequence":1,"run_id":"r",'
                            '"event":"iteration","elapsed_s":1,"metric":1e999}\n',
                            encoding="utf-8")
            self.assertEqual(len(seed_racing.load_jsonl(path)[0]), 1)

    def test_heuristic_rejects_boolean_and_nonfinite_metrics(self):
        score = seed_racing.heuristic_score({"overused_wires": True,
                                             "unrouted_connections": float("nan"),
                                             "recent_progress": float("inf")})
        self.assertEqual(score[:3], (-float("inf"),) * 3)

    def test_jsonl_preserves_nested_phase_attempts(self):
        events = [
            {"schema_version": 1, "sequence": 0, "run_id": "r", "event": "run_start", "phase": None, "attempt": None, "elapsed_s": 0},
            {"schema_version": 1, "sequence": 1, "run_id": "r", "event": "phase_start", "phase": "timing_repair", "attempt": 1, "elapsed_s": 1},
            {"schema_version": 1, "sequence": 2, "run_id": "r", "event": "phase_start", "phase": "negotiation", "attempt": 2, "elapsed_s": 2},
            {"schema_version": 1, "sequence": 3, "run_id": "r", "event": "iteration", "phase": "negotiation", "attempt": 2, "elapsed_s": 3},
            {"schema_version": 1, "sequence": 4, "run_id": "r", "event": "phase_end", "phase": "negotiation", "attempt": 2, "elapsed_s": 4},
            {"schema_version": 1, "sequence": 5, "run_id": "r", "event": "repair_round", "phase": "timing_repair", "attempt": 1, "elapsed_s": 5},
            {"schema_version": 1, "sequence": 6, "run_id": "r", "event": "phase_end", "phase": "timing_repair", "attempt": 1, "elapsed_s": 6},
            {"schema_version": 1, "sequence": 7, "run_id": "r", "event": "run_end", "phase": None, "attempt": None, "elapsed_s": 7},
        ]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "telemetry.jsonl"
            path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
            records, truncated = seed_racing.load_jsonl(path)
            self.assertEqual(records, events)
            self.assertFalse(truncated)
            events[4]["attempt"] = 9
            path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
            records, truncated = seed_racing.load_jsonl(path)
            self.assertEqual(len(records), 4)
            self.assertTrue(truncated)

    def test_jsonl_without_terminal_event_is_incomplete(self):
        start = {"schema_version": 1, "sequence": 0, "run_id": "r", "event": "run_start",
                 "phase": None, "attempt": None, "elapsed_s": 0}
        phase = {"schema_version": 1, "sequence": 1, "run_id": "r", "event": "phase_start",
                 "phase": "setup", "attempt": 0, "elapsed_s": 1}
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "telemetry.jsonl"
            path.write_text(json.dumps(start) + "\n", encoding="utf-8")
            records, incomplete = seed_racing.load_jsonl(path)
            self.assertEqual(records, [start])
            self.assertTrue(incomplete)
            path.write_text(json.dumps(start) + "\n" + json.dumps(phase) + "\n", encoding="utf-8")
            records, incomplete = seed_racing.load_jsonl(path)
            self.assertEqual(records, [start, phase])
            self.assertTrue(incomplete)


class CollectorTests(unittest.TestCase):
    def manifest(self, temporary, command, per_run=2, repeats=1, seeds=None):
        if isinstance(command, list) and command and "{seed}" not in command:
            command = list(command) + ["{seed}"]
        input_path = Path(temporary) / "netlist.json"
        input_path.write_text("frozen", encoding="utf-8")
        return {
            "schema_version": 1,
            "cohort": {"id": "cohort", "design_id": "design", "mapped_design_id": "mapped-sha", "constraint_family": "constraints-sha"},
            "architecture": "test",
            "command": command,
            "seeds": seeds or [1],
            "repeats": repeats,
            "limits": {"per_run_seconds": per_run, "total_seconds": 5, "concurrency": 2},
            "inputs": [],
            "artifacts": {"final_report": "report.json", "telemetry": "telemetry.jsonl"},
            "required_clocks": ["clk"],
            "environment": {"SEED_RACING_TEST": "1"},
            "provenance": {"source_revision": "test", "dirty": False,
                           "runtime_environment_id": "auto"}
        }

    def test_dry_run_expands_argv_without_creating_or_overwriting(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            manifest = self.manifest(temporary, [sys.executable, "-c", "pass", "{seed}", "{run_dir}"], repeats=2, seeds=[4, 9])
            first = seed_racing.Collector(manifest, output).run(dry_run=True)
            second = seed_racing.Collector(manifest, output).run(dry_run=True)
            self.assertEqual(len(first), 4)
            self.assertFalse(output.exists())
            self.assertEqual(first[0]["argv"][-2], "4")
            self.assertTrue(set(item["run_id"] for item in first).isdisjoint(item["run_id"] for item in second))

    def test_collection_captures_output_manifests_hashes_and_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            code = "import pathlib,sys; print('stdout-'+sys.argv[1]); print('stderr', file=sys.stderr); pathlib.Path(sys.argv[2]).write_text('report')"
            input_path = Path(temporary) / "netlist.json"
            manifest = self.manifest(temporary, [sys.executable, "-c", code, "{seed}",
                                                 "{report}", str(input_path)], seeds=[2, 3])
            manifest["inputs"] = [{"path": str(input_path), "role": "mapped_netlist"}]
            results = seed_racing.Collector(manifest, output).run()
            self.assertEqual([item["status"] for item in results],
                             ["incomplete_evidence", "incomplete_evidence"])
            for result in results:
                run_dir = Path(result["artifacts"]["stdout"]["path"]).parent
                immutable = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(immutable["seed"], result["seed"])
                self.assertEqual(len(immutable["inputs"][0]["sha256"]), 64)
                self.assertEqual(len(immutable["cohort_identity_basis"]["fingerprint_sha256"]), 64)
                self.assertEqual(result["manifest_sha256"],
                                 seed_racing.sha256_file(run_dir / "manifest.json"))
                self.assertEqual(result["result_sha256"],
                                 seed_racing.sha256_file(run_dir / "result.json"))
                self.assertEqual(len(result["artifacts"]["final_report"]["sha256"]), 64)
                self.assertIn("stdout-", (run_dir / "stdout.log").read_text(encoding="utf-8"))
                self.assertIn("stderr", (run_dir / "stderr.log").read_text(encoding="utf-8"))
            self.assertNotEqual(Path(results[0]["artifacts"]["stdout"]["path"]).parent, Path(results[1]["artifacts"]["stdout"]["path"]).parent)

    def test_collection_creates_nested_artifact_directories(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            code = "import pathlib,sys; pathlib.Path(sys.argv[1]).write_text('report')"
            manifest = self.manifest(
                temporary, [sys.executable, "-c", code, "{report}"])
            manifest["artifacts"] = {
                "final_report": "reports/final/report.json",
                "telemetry": "logs/router/telemetry.jsonl",
            }
            result = seed_racing.Collector(manifest, output).run()[0]
            self.assertTrue(Path(result["artifacts"]["final_report"]["path"]).is_file())
            self.assertTrue((Path(result["artifacts"]["stdout"]["path"]).parent /
                             "logs/router").is_dir())

    def test_timeout_terminates_owned_child_and_records_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            manifest = self.manifest(temporary, [sys.executable, "-c", "import time; time.sleep(30)"], per_run=0.05)
            result = seed_racing.Collector(manifest, output).run()[0]
            self.assertEqual(result["status"], "timeout")
            self.assertEqual(result["termination_reason"], "per_run_timeout")

    def test_termination_tolerates_an_already_absent_process_group(self):
        process = mock.Mock(pid=12345)
        process.poll.return_value = None
        process.wait.return_value = 0
        with mock.patch.object(seed_racing.os, "killpg", side_effect=ProcessLookupError):
            seed_racing._terminate_owned_child(process)
        process.wait.assert_called_once_with(timeout=seed_racing.TERMINATION_GRACE_SECONDS)

    def test_termination_kills_remaining_group_and_reaps_leader(self):
        process = mock.Mock(pid=12345)
        process.poll.return_value = 0
        process.wait.return_value = -signal.SIGTERM
        signals = []

        def record_signal(_pid, sent_signal):
            signals.append(sent_signal)

        with mock.patch.object(seed_racing.os, "killpg", side_effect=record_signal), \
                mock.patch.object(seed_racing, "_live_process_group_members",
                                  side_effect=([12346], [12346], [], [])), \
                mock.patch.object(seed_racing, "TERMINATION_GRACE_SECONDS", 0):
            seed_racing._terminate_owned_child(process)
        self.assertEqual(signals, [signal.SIGTERM, signal.SIGKILL])
        process.wait.assert_called_once_with(timeout=0)

    def test_child_environment_excludes_unrecorded_parent_variables(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            code = ("import json,os,pathlib,sys; "
                    "pathlib.Path(sys.argv[1]).write_text(json.dumps(dict(os.environ)))")
            manifest = self.manifest(temporary, [sys.executable, "-c", code, "{report}"])
            with mock.patch.dict(os.environ, {"UNRECORDED_ROUTER_CONTROL": "must-not-leak"}, clear=False):
                result = seed_racing.Collector(manifest, output).run()[0]
            run_dir = Path(result["artifacts"]["stdout"]["path"]).parent
            child = json.loads((run_dir / "report.json").read_text(encoding="utf-8"))
            recorded = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))["environment"]
            self.assertNotIn("UNRECORDED_ROUTER_CONTROL", child)
            self.assertEqual(child, recorded)
            self.assertEqual(child["SEED_RACING_TEST"], "1")

    def test_expired_total_budget_does_not_start_run_setup(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            manifest = self.manifest(temporary, [sys.executable, "-c", "pass"])
            collector = seed_racing.Collector(manifest, output)
            spec = collector.plan()[0]
            result = collector._run_one(spec, -1.0)
            self.assertEqual(result["status"], "not_started_total_budget")
            self.assertEqual(result["termination_reason"],
                             "total_budget_expired_before_run_setup")
            self.assertFalse(spec.directory.exists())

    def test_total_budget_is_rechecked_immediately_before_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            marker = Path(temporary) / "launched"
            code = ("import pathlib,sys; "
                    "pathlib.Path(sys.argv[1]).write_text(\"launched\")")
            manifest = self.manifest(temporary, [sys.executable, "-c", code, str(marker)])

            def clock():
                return 10.0 if list(output.rglob("manifest.json")) else 0.0

            with mock.patch.object(seed_racing.time, "monotonic", side_effect=clock):
                result = seed_racing.Collector(manifest, output).run()[0]
            self.assertEqual(result["status"], "not_started_total_budget")
            self.assertEqual(result["termination_reason"],
                             "total_budget_expired_before_launch")
            self.assertFalse(marker.exists())

    def test_cancel_prevents_queued_runs_from_launching(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            launches = Path(temporary) / "launches"
            code = ("import pathlib,sys,time; "
                    "p=pathlib.Path(sys.argv[1]); "
                    "p.write_text(p.read_text()+'x' if p.exists() else 'x'); "
                    "time.sleep(30)")
            manifest = self.manifest(temporary, [sys.executable, "-c", code, str(launches)],
                                     per_run=2, seeds=[1, 2, 3])
            manifest["limits"]["concurrency"] = 1
            collector = seed_racing.Collector(manifest, output)
            completed = []
            worker = threading.Thread(target=lambda: completed.extend(collector.run()))
            worker.start()
            deadline = time.monotonic() + 2
            while not launches.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(launches.exists())
            collector.cancel()
            worker.join(timeout=5)
            self.assertFalse(worker.is_alive())
            self.assertEqual(launches.read_text(encoding="utf-8"), "x")
            self.assertEqual([result["status"] for result in completed],
                             ["cancelled", "cancelled", "cancelled"])

    def test_interrupt_writes_cancelled_summary_before_reraising(self):
        class InterruptingFuture:
            def __init__(self):
                self.was_cancelled = False

            def result(self):
                raise KeyboardInterrupt()

            def cancel(self):
                self.was_cancelled = True
                return True

            def cancelled(self):
                return self.was_cancelled

        class InterruptingExecutor:
            def __init__(self, max_workers):
                self.max_workers = max_workers
                self.submissions = 0

            def submit(self, _function, _spec, _deadline):
                self.submissions += 1
                if self.submissions == 2:
                    raise KeyboardInterrupt()
                return InterruptingFuture()

            def shutdown(self, wait, cancel_futures):
                self.shutdown_args = (wait, cancel_futures)

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            manifest = self.manifest(temporary, [sys.executable, "-c", "pass"], seeds=[1, 2])
            with mock.patch.object(seed_racing.concurrent.futures, "ThreadPoolExecutor",
                                   InterruptingExecutor):
                with self.assertRaises(KeyboardInterrupt):
                    seed_racing.Collector(manifest, output).run()
            summaries = list(output.glob("collection-*.json"))
            self.assertEqual(len(summaries), 1)
            results = json.loads(summaries[0].read_text(encoding="utf-8"))["results"]
            self.assertEqual([result["status"] for result in results], ["cancelled", "cancelled"])
            self.assertEqual(results[1]["termination_reason"], "collector_cancelled_before_submission")

    def test_collection_uses_immutable_input_snapshot(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            launches = Path(temporary) / "launches"
            manifest = self.manifest(temporary, [], seeds=[1, 2, 3])
            input_path = Path(temporary) / "netlist.json"
            manifest["inputs"] = [{"path": str(input_path), "role": "mapped_netlist"}]
            reports = Path(temporary) / "reports"
            reports.mkdir()
            code = ("import pathlib,sys; "
                    "launches=pathlib.Path(sys.argv[2]); "
                    "launches.write_text(launches.read_text()+sys.argv[1] if launches.exists() else sys.argv[1]); "
                    "source=pathlib.Path(" + repr(str(input_path)) + "); "
                    "source.write_text('changed') if sys.argv[1]=='1' else None; "
                    "pathlib.Path(sys.argv[4]+'/'+sys.argv[1]).write_text(pathlib.Path(sys.argv[3]).read_text())")
            manifest["command"] = [sys.executable, "-c", code, "{seed}", str(launches),
                                   str(input_path), str(reports)]
            manifest["limits"]["concurrency"] = 1
            results = seed_racing.Collector(manifest, output).run()
            self.assertEqual(launches.read_text(encoding="utf-8"), "123")
            self.assertEqual([result["status"] for result in results],
                             ["incomplete_evidence", "incomplete_evidence", "incomplete_evidence"])
            self.assertEqual([path.read_text(encoding="utf-8") for path in sorted(reports.iterdir())],
                             ["frozen", "frozen", "frozen"])
            for result in results:
                run_dir = Path(result["artifacts"]["stdout"]["path"]).parent
                immutable = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(len(immutable["inputs"][0]["sha256"]), 64)
                self.assertNotEqual(immutable["inputs"][0]["resolved_path"],
                                    immutable["inputs"][0]["snapshot_path"])

    def test_collection_rewrites_embedded_input_option_to_snapshot(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            launches = Path(temporary) / "launches"
            manifest = self.manifest(temporary, [], seeds=[1, 2, 3])
            input_path = Path(temporary) / "netlist.json"
            manifest["inputs"] = [{"path": str(input_path), "role": "mapped_netlist"}]
            reports = Path(temporary) / "reports"
            reports.mkdir()
            code = ("import pathlib,sys; "
                    "launches=pathlib.Path(sys.argv[2]); "
                    "launches.write_text(launches.read_text()+sys.argv[1] if launches.exists() else sys.argv[1]); "
                    "source=pathlib.Path(" + repr(str(input_path)) + "); "
                    "source.write_text('changed') if sys.argv[1]=='1' else None; "
                    "mapped=pathlib.Path(sys.argv[3].split('=',1)[1]); "
                    "pathlib.Path(sys.argv[4]+'/'+sys.argv[1]).write_text(mapped.read_text())")
            manifest["command"] = [sys.executable, "-c", code, "{seed}", str(launches),
                                   "--json=" + str(input_path), str(reports)]
            manifest["limits"]["concurrency"] = 1
            results = seed_racing.Collector(manifest, output).run()
            self.assertEqual(launches.read_text(encoding="utf-8"), "123")
            self.assertEqual([result["status"] for result in results],
                             ["incomplete_evidence", "incomplete_evidence", "incomplete_evidence"])
            self.assertEqual([path.read_text(encoding="utf-8") for path in sorted(reports.iterdir())],
                             ["frozen", "frozen", "frozen"])
            for result in results:
                run_dir = Path(result["artifacts"]["stdout"]["path"]).parent
                immutable = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
                launch_path = immutable["inputs"][0]["launch_path"]
                self.assertIn("--json=" + launch_path, immutable["argv"])

    def test_collection_freezes_executable_once_for_whole_cohort(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            runner = Path(temporary) / "runner"
            observed = Path(temporary) / "observed"
            observed.mkdir()
            script = (
                "#!" + sys.executable + "\n"
                "import json,pathlib,sys\n"
                "seed,original,observed,telemetry,report=sys.argv[1:]\n"
                "pathlib.Path(original).write_text('#!/bin/sh\\nexit 99\\n') if seed == '1' else None\n"
                "pathlib.Path(observed,seed).write_text('frozen')\n"
                "start={'schema_version':1,'sequence':0,'run_id':'r','event':'run_start','phase':None,'attempt':None,'elapsed_s':0}\n"
                "end={'schema_version':1,'sequence':1,'run_id':'r','event':'run_end','phase':None,'attempt':None,'elapsed_s':1,'routing_legal':True,'timing_gate_pass':True}\n"
                "pathlib.Path(telemetry).write_text(json.dumps(start)+'\\n'+json.dumps(end)+'\\n')\n"
                "clock={'name':'clk','available':True,'setup_wns_ns':0.1,'hold_wns_ns':0.1}\n"
                "pathlib.Path(report).write_text(json.dumps({'outcome':{'analogue_clocks':[clock]}}))\n"
            )
            runner.write_text(script, encoding="utf-8")
            runner.chmod(0o755)
            manifest = self.manifest(temporary, [str(runner), "{seed}", str(runner), str(observed),
                                                  "{telemetry}", "{report}"], seeds=[1, 2, 3])
            manifest["limits"]["concurrency"] = 1
            results = seed_racing.Collector(manifest, output).run()
            self.assertEqual([result["status"] for result in results],
                             ["completed", "completed", "completed"])
            self.assertEqual([path.read_text(encoding="utf-8") for path in sorted(observed.iterdir())],
                             ["frozen", "frozen", "frozen"])
            snapshots = set()
            hashes = set()
            for result in results:
                run_dir = Path(result["artifacts"]["stdout"]["path"]).parent
                immutable = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
                binary = immutable["binary"]
                self.assertEqual(binary["resolved_path"], str(runner.resolve()))
                self.assertEqual(immutable["argv"][0], binary["snapshot_path"])
                self.assertEqual(binary["sha256"],
                                 seed_racing.sha256_file(Path(binary["snapshot_path"])))
                snapshots.add(binary["snapshot_path"])
                hashes.add(binary["sha256"])
            self.assertEqual(len(snapshots), 1)
            self.assertEqual(len(hashes), 1)

    def test_collection_uses_open_descriptors_after_snapshot_paths_are_replaced(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            runner = Path(temporary) / "runner"
            input_path = Path(temporary) / "netlist.json"
            input_path.write_text("frozen", encoding="utf-8")
            observed = Path(temporary) / "observed"
            observed.mkdir()
            script = (
                "#!" + sys.executable + "\n"
                "import json,pathlib,sys\n"
                "seed,observed,input_path,run_dir=sys.argv[1:]\n"
                "manifest=json.loads(pathlib.Path(run_dir,'manifest.json').read_text())\n"
                "if seed == '1':\n"
                " binary=pathlib.Path(manifest['binary']['snapshot_path'])\n"
                " binary.chmod(0o755); binary.write_text('#!/bin/sh\\nexit 99\\n')\n"
                " snapshot=pathlib.Path(manifest['inputs'][0]['snapshot_path'])\n"
                " snapshot.chmod(0o644); snapshot.write_text('changed')\n"
                "pathlib.Path(observed,seed).write_text(pathlib.Path(input_path).read_text())\n"
            )
            runner.write_text(script, encoding="utf-8")
            runner.chmod(0o755)
            manifest = self.manifest(
                temporary, [str(runner), "{seed}", str(observed), str(input_path), "{run_dir}"],
                seeds=[1, 2, 3])
            manifest["inputs"] = [{"path": str(input_path), "role": "mapped_netlist"}]
            manifest["limits"]["concurrency"] = 1
            results = seed_racing.Collector(manifest, output).run()
            self.assertEqual([result["status"] for result in results],
                             ["incomplete_evidence", "incomplete_evidence", "incomplete_evidence"])
            self.assertEqual([path.read_text(encoding="utf-8") for path in sorted(observed.iterdir())],
                             ["frozen", "frozen", "frozen"])

    def test_collection_preserves_executable_relative_share_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            binary_dir = Path(temporary) / "bin"
            share_dir = binary_dir / "share"
            share_dir.mkdir(parents=True)
            (share_dir / "resource").write_text("runtime-data", encoding="utf-8")
            runner = binary_dir / "runner"
            runner.write_text(
                "#!" + sys.executable + "\n"
                "import os,pathlib,sys\n"
                "root=pathlib.Path(os.environ['NEXTPNR_EXECUTABLE_DIR'])\n"
                "pathlib.Path(sys.argv[1]).write_text((root/'share'/'resource').read_text())\n",
                encoding="utf-8")
            runner.chmod(0o755)
            manifest = self.manifest(temporary, [str(runner), "{report}"])
            result = seed_racing.Collector(manifest, output).run()[0]
            run_dir = Path(result["artifacts"]["stdout"]["path"]).parent
            immutable = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual((run_dir / "report.json").read_text(encoding="utf-8"), "runtime-data")
            self.assertEqual(immutable["binary"]["runtime_executable_dir"], str(binary_dir.resolve()))
            sealed_dir = Path(immutable["environment"]["NEXTPNR_EXECUTABLE_DIR"])
            self.assertNotEqual(sealed_dir, binary_dir.resolve())
            self.assertTrue(str(sealed_dir).startswith("/proc/self/fd/"))
            runtime_files = immutable["binary"]["runtime_environment"]["manifest"]["files"]
            self.assertIn(str((share_dir / "resource").resolve()),
                          [item["path"] for item in runtime_files])

    def test_runtime_share_uses_directory_descriptor_after_path_replacement(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            binary_dir = Path(temporary) / "bin"
            share_dir = binary_dir / "share"
            share_dir.mkdir(parents=True)
            (share_dir / "resource").write_text("runtime-data", encoding="utf-8")
            runner = binary_dir / "runner"
            runner.write_text(
                "#!" + sys.executable + "\n"
                "import os,pathlib,sys\n"
                "root=pathlib.Path(os.environ['NEXTPNR_EXECUTABLE_DIR'])\n"
                "display_bin=root.resolve(); snapshot_root=display_bin.parent\n"
                "moved=snapshot_root.with_name(snapshot_root.name+'-moved')\n"
                "snapshot_root.rename(moved)\n"
                "(display_bin/'share').mkdir(parents=True)\n"
                "(display_bin/'share'/'resource').write_text('replacement')\n"
                "pathlib.Path(sys.argv[1]).write_text((root/'share'/'resource').read_text())\n",
                encoding="utf-8")
            runner.chmod(0o755)
            manifest = self.manifest(temporary, [str(runner), "{report}"])
            result = seed_racing.Collector(manifest, output).run()[0]
            run_dir = Path(result["artifacts"]["stdout"]["path"]).parent
            self.assertEqual((run_dir / "report.json").read_text(encoding="utf-8"),
                             "runtime-data")

    def test_workers_use_frozen_shared_library_after_original_is_swapped_and_restored(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = root / "libmutable.so"
            replacement = root / "libreplacement.so"
            backup = root / "libbackup.so"
            output = root / "observed"
            output.mkdir()
            source = root / "library.c"
            source.write_text('const char *value(void) { return VALUE; }\n', encoding="utf-8")
            for destination, value in ((original, '"frozen"'), (replacement, '"replacement"')):
                seed_racing.subprocess.run(
                    ["cc", "-shared", "-fPIC", f"-DVALUE={value}", str(source), "-o",
                     str(destination)], check=True)
            runner_source = root / "runner.c"
            runner_source.write_text(
                "#include <stdio.h>\n#include <stdlib.h>\n#include <unistd.h>\n"
                "extern const char *value(void);\n"
                f'static const char *original = "{original}";\n'
                f'static const char *replacement = "{replacement}";\n'
                f'static const char *backup = "{backup}";\n'
                "int main(int argc, char **argv) {\n"
                " char path[4096]; snprintf(path, sizeof(path), \"%s/%s\", argv[2], argv[1]);\n"
                " FILE *f=fopen(path, \"w\"); fputs(value(), f); fclose(f);\n"
                " if (atoi(argv[1]) == 1) { rename(original, backup); rename(replacement, original); }\n"
                " else { unlink(original); rename(backup, original); }\n"
                " return 0; }\n", encoding="utf-8")
            runner = root / "runner"
            seed_racing.subprocess.run(
                ["cc", str(runner_source), "-L", str(root), "-lmutable",
                 "-Wl,-rpath," + str(root), "-o", str(runner)], check=True)
            manifest = self.manifest(temporary, [str(runner), "{seed}", str(output)],
                                     seeds=[1, 2])
            manifest["limits"]["concurrency"] = 1
            results = seed_racing.Collector(manifest, root / "runs").run()
            self.assertEqual([result["process_status"] for result in results],
                             ["completed", "completed"])
            self.assertEqual([path.read_text(encoding="utf-8")
                              for path in sorted(output.iterdir())], ["frozen", "frozen"])

    def test_collection_rejects_mutated_sealed_share_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            binary_dir = Path(temporary) / "bin"
            share_dir = binary_dir / "share"
            share_dir.mkdir(parents=True)
            (share_dir / "resource").write_text("runtime-data", encoding="utf-8")
            runner = binary_dir / "runner"
            runner.write_text(
                "#!" + sys.executable + "\n"
                "import os,pathlib\n"
                "path=pathlib.Path(os.environ['NEXTPNR_EXECUTABLE_DIR'])/'share'/'resource'\n"
                "path.chmod(0o644); path.write_text('mutated')\n"
                "path.parent.chmod(0o755); (path.parent/'added').write_text('extra')\n",
                encoding="utf-8")
            runner.chmod(0o755)
            manifest = self.manifest(temporary, [str(runner)])
            with self.assertRaisesRegex(RuntimeError, "runtime environment changed"):
                seed_racing.Collector(manifest, output).run()

    def test_himbaechel_requires_one_declared_explicit_chipdb(self):
        with tempfile.TemporaryDirectory() as temporary:
            runner = Path(temporary) / "nextpnr-himbaechel"
            runner.write_text("#!" + sys.executable + "\nimport sys\n", encoding="utf-8")
            runner.chmod(0o755)
            manifest = self.manifest(temporary, [str(runner), "--seed", "{seed}"])
            with self.assertRaisesRegex(ValueError, "requires exactly one explicit --chipdb"):
                seed_racing.Collector(manifest, Path(temporary) / "missing-chipdb").run()

            chipdb = Path(temporary) / "chipdb.bin"
            chipdb.write_text("chipdb", encoding="utf-8")
            manifest["inputs"] = [{"path": str(chipdb), "role": "chipdb"}]
            manifest["command"] = [str(runner), "--seed={seed}", "--chipdb=" + str(chipdb)]
            result = seed_racing.Collector(manifest, Path(temporary) / "frozen-chipdb").run()[0]
            run_dir = Path(result["artifacts"]["stdout"]["path"]).parent
            immutable = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
            self.assertIn("--chipdb=" + immutable["inputs"][0]["launch_path"], immutable["argv"])

            manifest["inputs"] = []
            with self.assertRaisesRegex(ValueError, "--chipdb must be declared"):
                seed_racing.Collector(manifest, Path(temporary) / "undeclared-chipdb").run()

    def test_collection_rejects_missing_declared_input_before_submission(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            marker = Path(temporary) / "launched"
            manifest = self.manifest(
                temporary,
                [sys.executable, "-c",
                 "import pathlib,sys; pathlib.Path(sys.argv[1]).write_text('launched')",
                 str(marker)])
            manifest["inputs"] = [{"path": str(Path(temporary) / "missing.json"),
                                   "role": "mapped_netlist"}]
            with self.assertRaisesRegex(ValueError, "cannot snapshot declared cohort input"):
                seed_racing.Collector(manifest, output).run()
            self.assertFalse(marker.exists())
            self.assertFalse(list(output.glob("cohort/seed-*")))

    def test_collection_rejects_declared_but_unused_input_before_submission(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            marker = Path(temporary) / "launched"
            input_path = Path(temporary) / "netlist.json"
            manifest = self.manifest(
                temporary,
                [sys.executable, "-c",
                 "import pathlib,sys; pathlib.Path(sys.argv[1]).write_text('launched')",
                 str(marker)])
            manifest["inputs"] = [{"path": str(input_path), "role": "mapped_netlist"}]
            with self.assertRaisesRegex(ValueError, "not bound to every command argv"):
                seed_racing.Collector(manifest, output).run()
            self.assertFalse(marker.exists())

    def test_collection_rewrites_equivalent_relative_input_spelling(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            input_path = Path(temporary) / "netlist.json"
            observed = Path(temporary) / "observed"
            manifest = self.manifest(
                temporary,
                [sys.executable, "-c",
                 "import pathlib,sys; pathlib.Path(sys.argv[2]).write_text(pathlib.Path(sys.argv[1]).read_text())",
                 "./netlist.json", str(observed)])
            manifest["cwd"] = temporary
            manifest["inputs"] = [{"path": "netlist.json", "role": "mapped_netlist"}]
            result = seed_racing.Collector(manifest, output).run()[0]
            self.assertEqual(result["status"], "incomplete_evidence")
            self.assertEqual(observed.read_text(encoding="utf-8"), "frozen")

    def test_collection_rejects_reused_cohort_id_with_different_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            first = self.manifest(temporary, [sys.executable, "-c", "pass"])
            seed_racing.Collector(first, output).run()
            marker = Path(temporary) / "must-not-run"
            code = f"import pathlib; pathlib.Path({str(marker)!r}).write_text('ran')"
            second = self.manifest(temporary, [sys.executable, "-c", code])
            with self.assertRaisesRegex(ValueError, "different fingerprint"):
                seed_racing.Collector(second, output).run()
            self.assertFalse(marker.exists())

    def test_runtime_change_fails_before_final_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            manifest = self.manifest(temporary, [sys.executable, "-c", "pass"])
            with mock.patch.object(seed_racing, "_verify_runtime_environment_evidence",
                                   return_value=False):
                with self.assertRaisesRegex(RuntimeError, "runtime environment changed"):
                    seed_racing.Collector(manifest, output).run()
            self.assertFalse((output / "cohort-cohort.json").exists())
            self.assertEqual(len(list(output.glob("collection-failure-*.json"))), 1)
            self.assertFalse(any(output.glob("cohort/*/result.json")))

    def test_collection_identity_binds_execution_limits(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            first = self.manifest(temporary, [sys.executable, "-c", "pass"])
            seed_racing.Collector(first, output).run()
            second = self.manifest(temporary, [sys.executable, "-c", "pass"])
            second["limits"]["concurrency"] = 1
            with self.assertRaisesRegex(ValueError, "different fingerprint"):
                seed_racing.Collector(second, output).run()

    def test_nextpnr_known_input_option_must_be_declared(self):
        with tempfile.TemporaryDirectory() as temporary:
            runner = Path(temporary) / "nextpnr-generic"
            runner.write_text("#!" + sys.executable + "\n", encoding="utf-8")
            runner.chmod(0o755)
            manifest = self.manifest(temporary, [str(runner), "--seed={seed}", "--json", "netlist.json"])
            with self.assertRaisesRegex(ValueError, "--json must be declared"):
                seed_racing.Collector(manifest, Path(temporary) / "runs").run()

    def test_expanded_seed_file_argument_must_be_declared(self):
        with tempfile.TemporaryDirectory() as temporary:
            script = Path(temporary) / "mutable.py"
            script.write_text("pass\n", encoding="utf-8")
            manifest = self.manifest(
                temporary, [sys.executable, "-c", "pass", "{seed}"],
                seeds=[str(script)])
            with self.assertRaisesRegex(ValueError, "expanded command file argument"):
                seed_racing.Collector(manifest, Path(temporary) / "runs").run()

    def test_runtime_evidence_resolves_env_shebang_interpreter(self):
        with tempfile.TemporaryDirectory() as temporary:
            script = Path(temporary) / "runner"
            script.write_text("#!/usr/bin/env python3\n", encoding="utf-8")
            environment = seed_racing._child_environment({})
            evidence = seed_racing._runtime_environment_evidence(script, environment)
            expected = str(Path(seed_racing.shutil.which("python3", path=environment["PATH"])).resolve())
            self.assertEqual(evidence["manifest"]["runtime_binary"], expected)
            self.assertIn(str(Path("/usr/bin/env").resolve()), evidence["manifest"]["launchers"])
            self.assertTrue(seed_racing._verify_runtime_environment_evidence(evidence))
            evidence["runtime_environment_id"] = "sha256:" + "0" * 64
            self.assertFalse(seed_racing._verify_runtime_environment_evidence(evidence))

    def test_static_elf_runtime_evidence_accepts_ldd_static_diagnostic(self):
        executable = Path("/bin/true").resolve()
        completed = seed_racing.subprocess.CompletedProcess(
            ["ldd", str(executable)], 1, "", "not a dynamic executable\n")
        with mock.patch.object(seed_racing.subprocess, "run", return_value=completed):
            evidence = seed_racing._runtime_environment_evidence(
                executable, seed_racing._child_environment({}))
        files = evidence["manifest"]["files"]
        self.assertIn(str(executable), [item["path"] for item in files])

    def test_normal_leader_exit_kills_residual_process_group(self):
        with tempfile.TemporaryDirectory() as temporary:
            marker = Path(temporary) / "descendant-wrote"
            child = ("import pathlib,signal,sys,time; "
                     "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                     "time.sleep(0.5); pathlib.Path(sys.argv[1]).write_text('bad')")
            parent = ("import subprocess,sys; "
                      f"subprocess.Popen([sys.executable, '-c', {child!r}, {str(marker)!r}])")
            manifest = self.manifest(temporary, [sys.executable, "-c", parent])
            result = seed_racing.Collector(manifest, Path(temporary) / "runs").run()[0]
            self.assertEqual(result["process_status"], "completed")
            time.sleep(0.6)
            self.assertFalse(marker.exists())

    def test_collection_classifies_route_timing_incomplete_and_process_outcomes(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            code = "\n".join([
                "import json,pathlib,sys",
                "seed=int(sys.argv[1]); telemetry=pathlib.Path(sys.argv[2]); report=pathlib.Path(sys.argv[3])",
                "start={'schema_version':1,'sequence':0,'run_id':'r','event':'run_start','phase':None,'attempt':None,'elapsed_s':0}",
                "telemetry.write_text(json.dumps(start)+'\\n') if seed != 5 else None",
                "end={'schema_version':1,'sequence':1,'run_id':'r','event':'run_end','phase':None,'attempt':None,'elapsed_s':1,'routing_legal':seed != 3,'timing_gate_pass':'invalid' if seed == 9 else seed != 8,'status':'timing_constraint_failure' if seed == 8 else 'routing_legal'}",
                "end.pop('timing_gate_pass') if seed == 10 else None",
                "telemetry.write_text(telemetry.read_text()+json.dumps(end)+'\\n') if seed not in (4,5) else None",
                "clock={'name':'clk','available':True,'setup_wns_ns':-0.1 if seed == 2 else 0.1,'hold_wns_ns':0.1}",
                "normalized={'outcome':{'analogue_clocks':[clock]}}",
                "fmax={'fmax':{'clk':{'achieved':90 if seed == 6 else 110,'constraint':100}}}",
                "report.write_text(json.dumps(fmax if seed in (6,7) else normalized)) if seed != 5 else None",
                "sys.exit(1 if seed in (3,5,8) else 0)",
            ])
            code = code.replace("{", "{{").replace("}", "}}")
            manifest = self.manifest(temporary, [sys.executable, "-c", code, "{seed}",
                                                 "{telemetry}", "{report}"],
                                     seeds=[1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
            results = seed_racing.Collector(manifest, output).run()
            self.assertEqual([result["status"] for result in results],
                             ["completed", "analogue_timing_failure", "routing_failure",
                              "incomplete_evidence", "process_failure",
                              "analogue_timing_failure", "incomplete_evidence",
                              "timing_constraint_failure", "incomplete_evidence",
                              "incomplete_evidence"])
            self.assertEqual(results[0]["outcome"]["legal_route"], True)
            self.assertEqual(results[0]["outcome"]["analogue_timing_pass"], True)
            self.assertEqual(results[1]["process_status"], "completed")
            self.assertEqual(results[1]["outcome"]["analogue_timing_pass"], False)
            self.assertEqual(results[2]["process_status"], "process_failure")
            self.assertFalse(results[2]["outcome"]["legal_route"])
            self.assertFalse(results[3]["outcome"]["telemetry_complete"])
            self.assertFalse(results[3]["outcome"]["evidence_complete"])
            self.assertEqual(results[6]["outcome"]["timing_evidence_reason"],
                             "hold_not_available_in_standard_report")

    def test_builtin_timing_summary_requires_final_analogue_model_and_hold(self):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / "report.json"
            clocks = {"clk": {"setup_wns_ns": 0.2, "hold_wns_ns": 0.1}}
            report.write_text(json.dumps({"timing_summary": {
                "final_analogue_model": False, "clocks": clocks}}))
            evidence = seed_racing._final_timing_evidence(report, ["clk"])
            self.assertIsNone(evidence["analogue_timing_pass"])
            self.assertEqual(evidence["reason"], "report_is_not_final_analogue_model")
            report.write_text(json.dumps({"timing_summary": {
                "final_analogue_model": True, "clocks": clocks}}))
            evidence = seed_racing._final_timing_evidence(report, ["clk"])
            self.assertTrue(evidence["analogue_timing_pass"])
            self.assertEqual(evidence["analogue_clocks"][0]["hold_wns_ns"], 0.1)
            clocks["clk"]["setup_wns_ns"] = 0.0
            report.write_text(json.dumps({"timing_summary": {
                "final_analogue_model": True, "clocks": clocks}}))
            self.assertFalse(seed_racing._final_timing_evidence(
                report, ["clk"])["analogue_timing_pass"])
            clocks["clk"]["setup_wns_ns"] = 0.2
            del clocks["clk"]["hold_wns_ns"]
            report.write_text(json.dumps({"timing_summary": {
                "final_analogue_model": True, "clocks": clocks}}))
            evidence = seed_racing._final_timing_evidence(report, ["clk"])
            self.assertIsNone(evidence["analogue_timing_pass"])
            self.assertEqual(evidence["reason"], "required_clock_timing_missing")

    def test_collection_requires_unique_timing_for_every_required_clock(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            code = "\n".join([
                "import json,pathlib,sys",
                "start={'schema_version':1,'sequence':0,'run_id':'r','event':'run_start','phase':None,'attempt':None,'elapsed_s':0}",
                "end={'schema_version':1,'sequence':1,'run_id':'r','event':'run_end','phase':None,'attempt':None,'elapsed_s':1,'routing_legal':True}",
                "pathlib.Path(sys.argv[1]).write_text(json.dumps(start)+'\\n'+json.dumps(end)+'\\n')",
                "clock={'name':'clk','available':True,'setup_wns_ns':0.1,'hold_wns_ns':0.1}",
                "pathlib.Path(sys.argv[2]).write_text(json.dumps({'outcome':{'analogue_clocks':[clock,clock]}}))",
            ])
            code = code.replace("{", "{{").replace("}", "}}")
            manifest = self.manifest(temporary, [sys.executable, "-c", code,
                                                 "{telemetry}", "{report}"])
            manifest["required_clocks"] = ["clk", "related"]
            result = seed_racing.Collector(manifest, output).run()[0]
            self.assertEqual(result["status"], "incomplete_evidence")
            self.assertEqual(result["outcome"]["timing_evidence_reason"],
                             "required_clock_timing_missing")
            self.assertEqual(result["outcome"]["required_clocks"], ["clk", "related"])
            self.assertEqual([clock["available"] for clock in result["outcome"]["analogue_clocks"]],
                             [False, False])

    def test_gpu_collection_fingerprints_actual_backend_and_rejects_mixed_devices(self):
        with tempfile.TemporaryDirectory() as temporary:
            runner = Path(temporary) / "nextpnr-mistral"
            runner.write_text("""#!/usr/bin/env python3
import json, pathlib, sys
def option(name): return sys.argv[sys.argv.index(name) + 1]
seed = int(option('--seed'))
backend = 'cuda:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:0000:01:00.0:GPU-A' if seed == 1 else 'cuda:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb:0000:02:00.0:GPU-B'
assert not (pathlib.Path(option('--gpu-telemetry')).parent / 'result.json').exists()
phase_end = {'schema_version':1,'sequence':2,'run_id':'r','event':'phase_end','phase':'setup','attempt':0,'elapsed_s':0.2}
if seed not in (3, 4): phase_end['backend'] = backend
records = [
 {'schema_version':1,'sequence':0,'run_id':'r','event':'run_start','phase':None,'attempt':None,'elapsed_s':0},
 {'schema_version':1,'sequence':1,'run_id':'r','event':'phase_start','phase':'setup','attempt':0,'elapsed_s':0.1},
 phase_end,
 {'schema_version':1,'sequence':3,'run_id':'r','event':'run_end','phase':None,'attempt':None,'elapsed_s':1,'routing_legal':True,'timing_gate_pass':True,'status':'routing_legal'}]
pathlib.Path(option('--gpu-telemetry')).write_text(''.join(json.dumps(r) + '\\n' for r in records))
clock = {'name':'clk','available':True,'setup_wns_ns':0.1,'hold_wns_ns':0.1}
pathlib.Path(option('--report')).write_text(json.dumps({'outcome':{'analogue_clocks':[clock]}}))
if seed == 4: sys.exit(1)
""", encoding="utf-8")
            runner.chmod(0o755)
            command = [str(runner), "--router", "gpu", "--seed", "{seed}",
                       "--gpu-telemetry", "{telemetry}", "--report", "{report}"]
            manifest = self.manifest(temporary, command, seeds=[1, 2])
            output = Path(temporary) / "mixed"
            with self.assertRaisesRegex(RuntimeError, "observed multiple execution backends"):
                seed_racing.Collector(manifest, output).run()
            self.assertFalse((output / "cohort-cohort.json").exists())

            manifest = self.manifest(temporary, command, seeds=[1, 3])
            output = Path(temporary) / "missing"
            with self.assertRaisesRegex(RuntimeError, "lack execution backend attestation"):
                seed_racing.Collector(manifest, output).run()
            self.assertFalse((output / "cohort-cohort.json").exists())
            failure = json.loads(next(output.glob("collection-failure-*.json")).read_text())
            self.assertEqual(len(failure["results"]), 2)

            manifest = self.manifest(temporary, command, seeds=[1, 4])
            output = Path(temporary) / "missing-failure"
            with self.assertRaisesRegex(RuntimeError, "lack execution backend attestation"):
                seed_racing.Collector(manifest, output).run()
            self.assertTrue(any(output.glob("collection-failure-*.json")))

            unverifiable_command = command[:5] + command[7:]
            manifest = self.manifest(temporary, unverifiable_command, seeds=[1])
            with self.assertRaisesRegex(ValueError, "--gpu-telemetry"):
                seed_racing.Collector(manifest, Path(temporary) / "unverifiable").run()

            timeout_runner = Path(temporary) / "nextpnr-generic"
            timeout_runner.write_text("#!" + sys.executable + "\nimport time; time.sleep(2)\n",
                                      encoding="utf-8")
            timeout_runner.chmod(0o755)
            timeout_command = [str(timeout_runner), "--router", "gpu", "--seed", "{seed}",
                               "--gpu-telemetry", "{telemetry}"]
            timeout_manifest = self.manifest(temporary, timeout_command, per_run=0.1)
            with self.assertRaisesRegex(RuntimeError, "no attested execution backend"):
                seed_racing.Collector(timeout_manifest, Path(temporary) / "generic-timeout").run()
            timeout_failure = json.loads(next(
                (Path(temporary) / "generic-timeout").glob("collection-failure-*.json")).read_text())
            self.assertTrue(timeout_failure["results"][0]["process_started"])
            self.assertEqual(timeout_failure["results"][0]["process_status"], "timeout")

            generic_runner = Path(temporary) / "nextpnr-generic-good"
            generic_runner.write_text(runner.read_text(encoding="utf-8"), encoding="utf-8")
            generic_runner.chmod(0o755)
            generic_command = [str(generic_runner), "--router", "gpu", "--seed", "{seed}",
                               "--gpu-telemetry", "{telemetry}", "--report", "{report}"]
            generic_result = seed_racing.Collector(
                self.manifest(temporary, generic_command, seeds=[1]),
                Path(temporary) / "generic-gpu").run()[0]
            self.assertEqual(generic_result["cohort_identity"]["manifest"][
                "execution_identity"]["backend"],
                "cuda:" + "a" * 32 + ":0000:01:00.0:GPU-A")

            non_cli_gpu_command = command[:1] + command[3:]
            manifest = self.manifest(temporary, non_cli_gpu_command, seeds=[1])
            with self.assertRaisesRegex(ValueError, "explicit --router"):
                seed_racing.Collector(manifest, Path(temporary) / "implicit-router").run()

    def test_explicit_cpu_mistral_collection_does_not_require_gpu_attestation(self):
        with tempfile.TemporaryDirectory() as temporary:
            runner = Path(temporary) / "nextpnr-mistral"
            runner.write_text("#!" + sys.executable + "\n", encoding="utf-8")
            runner.chmod(0o755)
            manifest = self.manifest(temporary, [str(runner), "--router", "router2",
                                                 "--seed", "{seed}"])
            result = seed_racing.Collector(
                manifest, Path(temporary) / "cpu-mistral").run()[0]
            self.assertEqual(result["status"], "incomplete_evidence")
            self.assertNotIn("execution_identity",
                             result["cohort_identity"]["manifest"])

    def test_gpu_capable_router_binding_ignores_tokens_after_double_dash(self):
        with tempfile.TemporaryDirectory() as temporary:
            runner = Path(temporary) / "nextpnr-generic"
            runner.write_text("#!" + sys.executable + "\n", encoding="utf-8")
            runner.chmod(0o755)
            manifest = self.manifest(
                temporary, [str(runner), "--seed", "{seed}", "--",
                            "--router", "gpu", "--gpu-telemetry", "{telemetry}"])
            with self.assertRaisesRegex(ValueError, "explicit --router"):
                seed_racing.Collector(manifest, Path(temporary) / "runs").run()

    def test_gpu_capable_collection_rejects_router_overrides(self):
        with tempfile.TemporaryDirectory() as temporary:
            runner = Path(temporary) / "nextpnr-generic"
            runner.write_text("#!" + sys.executable + "\n", encoding="utf-8")
            runner.chmod(0o755)
            hook = Path(temporary) / "hook.py"
            hook.write_text("ctx.settings['router'] = 'gpu'\n", encoding="utf-8")
            manifest = self.manifest(
                temporary, [str(runner), "--router", "router2", "--seed", "{seed}",
                            "--pre-route", str(hook)])
            manifest["inputs"] = [{"path": str(hook), "role": "python_hook"}]
            with self.assertRaisesRegex(ValueError, "route-mutating Python hook"):
                seed_racing.Collector(manifest, Path(temporary) / "hook-runs").run()

            design = Path(temporary) / "design.json"
            design.write_text(json.dumps({"settings": {"router": "gpu"}}), encoding="utf-8")
            manifest = self.manifest(
                temporary, [str(runner), "--router", "router2", "--seed", "{seed}",
                            "--json", str(design)])
            manifest["inputs"] = [{"path": str(design), "role": "mapped_netlist"}]
            with self.assertRaisesRegex(ValueError, "overrides declared router"):
                seed_racing.Collector(manifest, Path(temporary) / "json-runs").run()

    def test_router_audit_reads_the_sealed_input_not_the_display_copy(self):
        class DisplayTamperingCollector(seed_racing.Collector):
            def _snapshot_inputs(self):
                records = super()._snapshot_inputs()
                display = Path(records[0]["snapshot_path"])
                display.chmod(0o644)
                display.write_text(json.dumps({"settings": {"router": "gpu"}}),
                                   encoding="utf-8")
                return records

        with tempfile.TemporaryDirectory() as temporary:
            runner = Path(temporary) / "nextpnr-generic"
            runner.write_text("#!" + sys.executable + "\n", encoding="utf-8")
            runner.chmod(0o755)
            design = Path(temporary) / "design.json"
            design.write_text(json.dumps({"settings": {"router": "router2"}}),
                              encoding="utf-8")
            manifest = self.manifest(
                temporary, [str(runner), "--router", "gpu", "--seed", "{seed}",
                            "--gpu-telemetry", "{telemetry}", "--json", str(design)])
            manifest["inputs"] = [{"path": str(design), "role": "mapped_netlist"}]
            with self.assertRaisesRegex(ValueError, "overrides declared router"):
                DisplayTamperingCollector(manifest, Path(temporary) / "sealed-audit").run()

    def test_gpu_identity_exempts_only_explicitly_never_launched_runs(self):
        with tempfile.TemporaryDirectory() as temporary:
            runner = Path(temporary) / "nextpnr-generic"
            runner.write_text("#!" + sys.executable + "\n", encoding="utf-8")
            runner.chmod(0o755)
            manifest = self.manifest(temporary, [str(runner), "--router", "gpu",
                                                 "--seed", "{seed}",
                                                 "--gpu-telemetry", "{telemetry}"])
            collector = seed_racing.Collector(manifest, Path(temporary) / "runs")
            collector._frozen_binary = {"resolved_path": str(runner)}
            self.assertIsNone(collector._bind_execution_identity([
                {"run_id": "never", "status": "cancelled", "process_started": False}]))
            with self.assertRaisesRegex(RuntimeError, "no attested execution backend"):
                collector._bind_execution_identity([
                    {"run_id": "launched", "status": "timeout", "process_started": True}])
            with self.assertRaisesRegex(RuntimeError, "lacks exact device attestation"):
                collector._bind_execution_identity([{
                    "run_id": "old-hip", "status": "completed", "process_started": True,
                    "outcome": {"execution_backend":
                                "hip:unattested-ordinal-0:0000:01:00.0:GPU"},
                }])
            for backend in ("hip:ordinal-0:0000:01:00.0:GPU",
                            "hip:garbage:0000:01:00.0:GPU",
                            "cuda:" + "0" * 32 + ":0000:01:00.0:GPU"):
                with self.assertRaisesRegex(RuntimeError, "lacks exact device attestation"):
                    collector._bind_execution_identity([{
                        "run_id": "inexact", "status": "completed", "process_started": True,
                        "outcome": {"execution_backend": backend},
                    }])

    def test_manifest_requires_nextpnr_seed_option_binding(self):
        with tempfile.TemporaryDirectory() as temporary:
            for command in (["nextpnr-generic"],
                            ["nextpnr-generic", "--json", "output-{seed}.json"],
                            ["nextpnr-generic", "--seed", "1"],
                            ["nextpnr-generic", "--", "--seed", "{seed}"]):
                manifest = self.manifest(temporary, command)
                with self.assertRaisesRegex(ValueError, "--seed"):
                    seed_racing.validate_collection_manifest(manifest)
            manifest = self.manifest(
                temporary, ["nextpnr-generic", "--seed={seed}"])
            seed_racing.validate_collection_manifest(manifest)
            manifest = self.manifest(temporary, ["wrapper", "{seed}"])
            seed_racing.validate_collection_manifest(manifest)
            manifest["command"] = ["wrapper", "output-{seed}.json"]
            with self.assertRaisesRegex(ValueError, "exact.*seed"):
                seed_racing.validate_collection_manifest(manifest)

    def test_manifest_rejects_shell_string_and_unsafe_artifact(self):
        with tempfile.TemporaryDirectory() as temporary:
            manifest = self.manifest(temporary, "nextpnr --seed 1")
            with self.assertRaisesRegex(ValueError, "argv"):
                seed_racing.validate_collection_manifest(manifest)
            manifest = self.manifest(temporary, [sys.executable, "-c", "pass", "{seed}"])
            manifest["artifacts"] = {"escape": "../input.json"}
            with self.assertRaisesRegex(ValueError, "within"):
                seed_racing.validate_collection_manifest(manifest)
            for reserved in ("stdout", "stderr"):
                manifest = self.manifest(temporary, [sys.executable, "-c", "pass", "{seed}"])
                manifest["artifacts"] = {reserved: "custom.log"}
                with self.assertRaisesRegex(ValueError, "reserved"):
                    seed_racing.validate_collection_manifest(manifest)
            for overlapping in ({"x": "."}, {"x": "stdout.log/child"},
                                {"x": "artifact", "y": "artifact/child"}):
                manifest = self.manifest(temporary, [sys.executable, "-c", "pass", "{seed}"])
                manifest["artifacts"] = overlapping
                with self.assertRaisesRegex(ValueError, "overlap"):
                    seed_racing.validate_collection_manifest(manifest)
            manifest = self.manifest(temporary, [sys.executable, "-c", "pass", "{seed}"])
            manifest["required_clocks"] = ["clk", "clk"]
            with self.assertRaisesRegex(ValueError, "unique"):
                seed_racing.validate_collection_manifest(manifest)

    def test_manifest_rejects_oversized_numeric_limit(self):
        with tempfile.TemporaryDirectory() as temporary:
            manifest = self.manifest(temporary, [sys.executable, "-c", "pass", "{seed}"])
            manifest["limits"]["per_run_seconds"] = 10 ** 10000
            with self.assertRaisesRegex(ValueError, "positive finite"):
                seed_racing.validate_collection_manifest(manifest)


if __name__ == "__main__":
    unittest.main()
