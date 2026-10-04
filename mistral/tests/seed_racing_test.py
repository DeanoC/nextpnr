#!/usr/bin/env python3
import json
import sys
import tempfile
import unittest
from pathlib import Path


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
            path.write_text('{"seq": 0}\n{"seq":', encoding="utf-8")
            records, truncated = seed_racing.load_jsonl(path)
        self.assertEqual(records, [{"seq": 0}])
        self.assertTrue(truncated)


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
