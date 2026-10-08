#!/usr/bin/env python3
import sys
import hashlib
import tempfile
import json
import os
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
import seed_policy_portfolio as portfolio


class PortfolioTests(unittest.TestCase):
    def row(self, policy, cost, success, phase="train"):
        return dict(policy=policy, duration_seconds=cost, success=success, phase=phase)

    def test_training_selection_counts_failed_cost(self):
        rows = [self.row("a", 10, True), self.row("a", 600, False),
                self.row("b", 20, True), self.row("b", 20, False)]
        ranking, statistics = portfolio.ranked_training_policies(rows, ["a", "b"], "a")
        self.assertEqual(ranking, ["b", "a"])
        self.assertEqual(statistics["a"]["cost_seconds"], 610)

    def test_heldout_selection_rejected(self):
        with self.assertRaises(ValueError):
            portfolio.ranked_training_policies([self.row("a", 1, True, "heldout")], ["a"], "a")

    def test_tie_prefers_baseline(self):
        ranking, _ = portfolio.ranked_training_policies(
            [self.row("a", 2, True), self.row("z", 2, True)], ["a", "z"], "z")
        self.assertEqual(ranking, ["z", "a"])

    def test_missing_policy_rejected(self):
        with self.assertRaises(ValueError):
            portfolio.ranked_training_policies([], ["a"], "a")

    def test_portfolio_visits_each_candidate_once(self):
        order = portfolio.schedule([17, 18, 19, 20], ["a", "b", "c"], 4, True)
        self.assertEqual(len(order), 12)
        self.assertEqual(set(order), {(p, s) for p in "abc" for s in [17, 18, 19, 20]})
        self.assertEqual(order, portfolio.schedule([20, 19, 18, 17], ["a", "b", "c"], 4, True))
        self.assertEqual({p for p, _ in order[:3]}, set("abc"))

    def test_same_seed_order_for_single_policy(self):
        a = portfolio.schedule([1, 2, 3], ["a"], 0, False)
        b = portfolio.schedule([1, 2, 3], ["b"], 0, False)
        self.assertEqual([s for _, s in a], [s for _, s in b])

    def test_censored_winner_label_is_not_visible(self):
        cases = {("a", 1): self.row("a", 601, True)}
        result = portfolio.replay([("a", 1)], cases, 600)
        self.assertFalse(result["success"])
        self.assertEqual(result["consumed_seconds"], 600)
        self.assertEqual(result["completed"], [])

    def test_restart_cost_paid_in_full(self):
        cases = {("a", 1): self.row("a", 600, False), ("b", 1): self.row("b", 30, True)}
        result = portfolio.replay(list(cases), cases, 630)
        self.assertTrue(result["success"])
        self.assertEqual(result["time_to_first_success_seconds"], 630)
        self.assertFalse(portfolio.replay(list(cases), cases, 629)["success"])

    def test_all_failures_exhaust_candidates_not_budget(self):
        cases = {("a", 1): self.row("a", 10, False)}
        result = portfolio.replay(list(cases), cases, 600)
        self.assertFalse(result["success"])
        self.assertEqual(result["consumed_seconds"], 10)

    def test_censored_repeat_is_inconclusive(self):
        rows = [dict(policy="a", seed=17, phase=phase, status="timeout_per_run")
                for phase in ("train", "repeat")]
        check = portfolio.repeat_checks(rows, ["a"], [17])[0]
        self.assertTrue(check["classification"].startswith("inconclusive"))

    def test_repeat_checks_verify_bytes_not_just_labels(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "artifact"
            path.write_bytes(b"same")
            record = dict(available=True, path=str(path), sha256=hashlib.sha256(b"same").hexdigest())
            rows = [dict(policy="a", seed=17, phase=phase, status="completed",
                         artifact_records=dict(final_report=record, bitstream=record))
                    for phase in ("train", "repeat")]
            self.assertEqual(portfolio.repeat_checks(rows, ["a"], [17])[0]["classification"], "repeatable")
            path.write_bytes(b"changed")
            with self.assertRaises(ValueError):
                portfolio.repeat_checks(rows, ["a"], [17])

    def test_completed_repeat_missing_artifact_rejected(self):
        rows = [dict(policy="a", seed=17, phase=phase, status="completed", artifact_records={})
                for phase in ("train", "repeat")]
        with self.assertRaises(ValueError):
            portfolio.repeat_checks(rows, ["a"], [17])

    def test_legal_timing_failures_still_have_repeatable_final_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "artifact"
            path.write_bytes(b"timing failed")
            record = dict(available=True, path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
            rows = [dict(policy="a", seed=17, phase=phase, status="analogue_timing_failure",
                         artifact_records=dict(final_report=record, bitstream=record))
                    for phase in ("train", "repeat")]
            self.assertEqual(portfolio.repeat_checks(rows, ["a"], [17])[0]["classification"], "repeatable")


class PathResolutionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.fixture = self.root / "fixture"
        (self.fixture / "bin").mkdir(parents=True)
        self.binary = self.fixture / "bin" / "nextpnr-mistral"
        self.binary.write_bytes(b"test executable")
        self.binary.chmod(0o755)
        (self.fixture / "synth.json").write_bytes(b"fixture netlist")
        # A conflicting file at the caller's cwd must never supply the digest.
        (self.root / "synth.json").write_bytes(b"wrong netlist")
        self.manifest = dict(
            cohort=dict(id="template", design_id="test", mapped_design_id="mapped", constraint_family="clocks"),
            architecture="mistral", command=["bin/nextpnr-mistral", "--seed", "{seed}",
                                            "--placer-heap-critexp", "5", "--placer-heap-timingweight", "2000"],
            seeds=[1], repeats=1, limits=dict(per_run_seconds=600, total_seconds=720, concurrency=1),
            inputs=[dict(path="synth.json", role="mapped_netlist")], cwd=str(self.fixture),
            provenance=dict(source_revision="test", dirty=False, runtime_environment_id="auto"))

    def prepare_and_load(self):
        old_cwd = Path.cwd()
        try:
            os.chdir(self.root)
            declaration = portfolio.prepare(self.manifest, self.root / "evidence")
            loaded, manifests = portfolio.load_declared(self.root / "evidence")
        finally:
            os.chdir(old_cwd)
        self.assertEqual(loaded, declaration)
        self.assertEqual(len(manifests), 9)
        self.assertEqual(declaration["binary_sha256"], hashlib.sha256(self.binary.read_bytes()).hexdigest())
        self.assertEqual(declaration["input_sha256"]["mapped_netlist:0"],
                         hashlib.sha256(b"fixture netlist").hexdigest())

    def test_relative_binary_and_inputs_use_manifest_cwd(self):
        self.prepare_and_load()

    def test_executable_uses_declared_child_path(self):
        self.manifest["command"][0] = "nextpnr-mistral"
        self.manifest["environment"] = dict(PATH=str(self.binary.parent))
        with mock.patch.dict(os.environ, {"PATH": str(self.root)}):
            self.prepare_and_load()

    def test_absolute_inputs_ignore_cwd(self):
        self.manifest["inputs"][0]["path"] = str(self.fixture / "synth.json")
        self.manifest["command"][0] = str(self.binary)
        self.prepare_and_load()


class PopulationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.manifest = dict(cohort=dict(id="c"), architecture="mistral", artifacts={},
                             command=["pnr"], required_clocks=["clk"], seeds=[17], repeats=1,
                             environment={}, limits=dict(per_run_seconds=600))
        self.item = dict(cohort_id="c", phase="train", policy="a", seeds=[17])
        self.bound = dict(self.manifest, binary=dict(sha256="binary"), inputs=[],
                          execution_identity=dict(backend="cuda", runtime_environment_id="frozen"))
        self.row = dict(run_id="r", cohort_id="c", seed=17, process_started=True)
        portfolio.write_new(self.root / "declaration.json", dict(binary_sha256="binary", input_sha256={}))
        (self.root / "collections" / "c").mkdir(parents=True)

    def read(self, rows=None):
        path = self.root / "collections" / "c" / "collection-1.json"
        path.write_text(json.dumps(dict(cohort_identity=dict(manifest=self.bound),
                                        results=[dict(run_id="r", artifacts={})])))
        with mock.patch.object(portfolio.seed_racing, "assemble_dataset", return_value={}), \
                mock.patch.object(portfolio.seed_racing, "validate_dataset", return_value=rows or [self.row]):
            return portfolio.read_populations(self.root, [(self.item, self.manifest)])

    def test_bound_population_accepted(self):
        self.assertEqual(self.read()[0]["phase"], "train")

    def test_changed_population_rejected(self):
        self.bound["seeds"] = [18]
        with self.assertRaises(ValueError):
            self.read()

    def test_changed_binary_rejected(self):
        self.bound["binary"] = dict(sha256="other")
        with self.assertRaises(ValueError):
            self.read()

    def test_duplicate_candidates_rejected(self):
        with self.assertRaises(ValueError):
            self.read([dict(self.row), dict(self.row)])

    def test_unstarted_candidate_is_not_a_failure_label(self):
        self.row["process_started"] = False
        with self.assertRaises(ValueError):
            self.read()


if __name__ == "__main__":
    unittest.main()
