#!/usr/bin/env python3
import json
import os
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
        winner = next(run for run in document["runs"] if run["run_id"] == "late-winner")
        winner["outcome"]["required_clocks"].append("related:clk->memory")
        result = seed_racing.validate_dataset(document)
        winner = next(run for run in result if run["run_id"] == "late-winner")
        self.assertFalse(winner["timing_available"])
        self.assertFalse(winner["success"])
        self.assertEqual(winner["missing_required_clocks"], ["related:clk->memory"])

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
        terminal_success = dict(self.by_id["late-winner"], run_id="terminal-success", duration_seconds=2)
        terminal_failure = dict(self.by_id["early-leader-late-failure"],
                                run_id="terminal-failure", duration_seconds=3)
        active = dict(self.by_id["late-winner"], run_id="active", duration_seconds=10)
        result = seed_racing.successive_halving(
            [terminal_success, terminal_failure, active], [5], [1], 0, 9, restart=False)
        self.assertTrue(result["at_least_one_success"])
        self.assertIn("terminal-success", result["retained"])
        self.assertNotIn("terminal-failure", result["retained"])
        self.assertEqual(result["stages"][0]["terminal_successes"], ["terminal-success"])
        self.assertEqual(result["stages"][0]["terminal_failures"], ["terminal-failure"])
        self.assertEqual(result["stages"][0]["promoted"], ["active"])

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

    def test_jsonl_preserves_nested_phase_attempts(self):
        events = [
            {"schema_version": 1, "sequence": 0, "run_id": "r", "event": "run_start", "phase": None, "attempt": None, "elapsed_s": 0},
            {"schema_version": 1, "sequence": 1, "run_id": "r", "event": "phase_start", "phase": "repair", "attempt": 1, "elapsed_s": 1},
            {"schema_version": 1, "sequence": 2, "run_id": "r", "event": "phase_start", "phase": "negotiation", "attempt": 2, "elapsed_s": 2},
            {"schema_version": 1, "sequence": 3, "run_id": "r", "event": "iteration", "phase": "negotiation", "attempt": 2, "elapsed_s": 3},
            {"schema_version": 1, "sequence": 4, "run_id": "r", "event": "phase_end", "phase": "negotiation", "attempt": 2, "elapsed_s": 4},
            {"schema_version": 1, "sequence": 5, "run_id": "r", "event": "phase_end", "phase": "repair", "attempt": 1, "elapsed_s": 5},
            {"schema_version": 1, "sequence": 6, "run_id": "r", "event": "run_end", "phase": None, "attempt": None, "elapsed_s": 6},
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
        input_path = Path(temporary) / "netlist.json"
        input_path.write_text("frozen", encoding="utf-8")
        return {
            "schema_version": 1,
            "cohort": {"id": "cohort", "design_id": "design", "mapped_design_id": "mapped-sha", "constraint_family": "constraints-sha"},
            "command": command,
            "seeds": seeds or [1],
            "repeats": repeats,
            "limits": {"per_run_seconds": per_run, "total_seconds": 5, "concurrency": 2},
            "inputs": [{"path": str(input_path), "role": "mapped_netlist"}],
            "artifacts": {"final_report": "report.json", "telemetry": "telemetry.jsonl"},
            "environment": {"SEED_RACING_TEST": "1"},
            "provenance": {"source_revision": "test", "dirty": False}
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
            manifest = self.manifest(temporary, [sys.executable, "-c", code, "{seed}", "{report}"], seeds=[2, 3])
            results = seed_racing.Collector(manifest, output).run()
            self.assertEqual([item["status"] for item in results], ["completed", "completed"])
            for result in results:
                run_dir = Path(result["artifacts"]["stdout"]["path"]).parent
                immutable = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(immutable["seed"], result["seed"])
                self.assertEqual(len(immutable["inputs"][0]["sha256"]), 64)
                self.assertEqual(len(result["artifacts"]["final_report"]["sha256"]), 64)
                self.assertIn("stdout-", (run_dir / "stdout.log").read_text(encoding="utf-8"))
                self.assertIn("stderr", (run_dir / "stderr.log").read_text(encoding="utf-8"))
            self.assertNotEqual(Path(results[0]["artifacts"]["stdout"]["path"]).parent, Path(results[1]["artifacts"]["stdout"]["path"]).parent)

    def test_timeout_terminates_owned_child_and_records_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "runs"
            manifest = self.manifest(temporary, [sys.executable, "-c", "import time; time.sleep(30)"], per_run=0.05)
            result = seed_racing.Collector(manifest, output).run()[0]
            self.assertEqual(result["status"], "timeout")
            self.assertEqual(result["termination_reason"], "per_run_timeout")

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
            input_path = Path(manifest["inputs"][0]["path"])
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
                             ["completed", "completed", "completed"])
            self.assertEqual([path.read_text(encoding="utf-8") for path in sorted(reports.iterdir())],
                             ["frozen", "frozen", "frozen"])
            for result in results:
                run_dir = Path(result["artifacts"]["stdout"]["path"]).parent
                immutable = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(len(immutable["inputs"][0]["sha256"]), 64)
                self.assertNotEqual(immutable["inputs"][0]["resolved_path"],
                                    immutable["inputs"][0]["snapshot_path"])

    def test_manifest_rejects_shell_string_and_unsafe_artifact(self):
        with tempfile.TemporaryDirectory() as temporary:
            manifest = self.manifest(temporary, "nextpnr --seed 1")
            with self.assertRaisesRegex(ValueError, "argv"):
                seed_racing.validate_collection_manifest(manifest)
            manifest = self.manifest(temporary, ["nextpnr"])
            manifest["artifacts"] = {"escape": "../input.json"}
            with self.assertRaisesRegex(ValueError, "within"):
                seed_racing.validate_collection_manifest(manifest)


if __name__ == "__main__":
    unittest.main()
