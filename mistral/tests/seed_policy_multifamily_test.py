#!/usr/bin/env python3
from pathlib import Path
import sys
import tempfile
import hashlib
import json
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
import seed_policy_multifamily as screen


class ScreenTests(unittest.TestCase):
    def row(self, family, policy, success, cost, phase="train"):
        return dict(family=family, policy=policy, success=success, duration_seconds=cost, phase=phase)

    def test_fast_family_cannot_dominate_normalized_score(self):
        rows = [self.row("fast", "a", True, .01), self.row("fast", "b", True, .02),
                self.row("slow", "a", False, 600), self.row("slow", "b", True, 600)]
        result = screen.select(rows, ["fast", "slow"], ["a", "b"])
        self.assertEqual(result["ranking"], ["b", "a"])
        self.assertEqual(result["scores"], dict(a=.5, b=.75))
        self.assertAlmostEqual(result["training_cost_seconds"], 1200.03)

    def test_heldout_phase_rejected(self):
        with self.assertRaises(ValueError):
            screen.select([self.row("train", "a", True, 1, "heldout")], ["train"], ["a"])

    def test_heldout_family_rejected_even_if_mislabeled_train(self):
        with self.assertRaises(ValueError):
            screen.select([self.row("held", "a", True, 1)], ["train"], ["a"])

    def test_all_failures_remain_in_macro_mean(self):
        rows = [self.row("f", "critexp-5", False, 600), self.row("f", "critexp-8", False, 100)]
        result = screen.select(rows, ["f"], ["critexp-8", "critexp-5"])
        self.assertEqual(result["ranking"][0], "critexp-5")
        self.assertEqual(result["scores"]["critexp-5"], 0)

    def test_renaming_duplicate_mapped_family_does_not_make_it_heldout(self):
        families = [dict(name=str(i), phase="train" if i < 2 else "heldout", mapped_sha256=str(i))
                    for i in range(5)]
        screen.validate_split(families)
        families[2]["mapped_sha256"] = families[0]["mapped_sha256"]
        with self.assertRaises(ValueError):
            screen.validate_split(families)

    def test_invalid_split_rejected(self):
        with self.assertRaises(ValueError):
            screen.validate_split([])

    def test_duplicate_argv_option_rejected(self):
        with self.assertRaises(ValueError):
            screen.set_option(["--x", "1", "--x", "2"], "--x", 3)

    def test_training_bill_must_fit_before_candidate_label_is_available(self):
        cases = {("a", 1): self.row("held", "a", True, 10, "heldout")}
        result = screen.charged_replay(list(cases), cases, 600, 700)
        self.assertFalse(result["success"])
        self.assertEqual(result["completed"], [])
        self.assertEqual(result["consumed_seconds"], 600)

    def test_training_charge_and_restart_cost_both_paid(self):
        cases = {("a", 1): self.row("held", "a", False, 600, "heldout"),
                 ("b", 1): self.row("held", "b", True, 100, "heldout")}
        result = screen.charged_replay(list(cases), cases, 1000, 200)
        self.assertTrue(result["success"])
        self.assertEqual(result["time_to_first_success_seconds"], 900)
        self.assertFalse(screen.charged_replay(list(cases), cases, 899, 200)["success"])

    def test_selection_is_frozen_and_cannot_be_recreated_after_heldout(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            plan = dict(families=[dict(name="train", phase="train"), dict(name="held", phase="heldout")],
                        policies=["critexp-5"])
            rows = [self.row("train", "critexp-5", True, 10)]
            with mock.patch.object(screen, "read_rows", return_value=rows):
                first = screen.frozen_selection(output, plan, {})
                self.assertEqual(first, screen.frozen_selection(output, plan, {}))
                changed = [self.row("train", "critexp-5", True, 11)]
                with mock.patch.object(screen, "read_rows", return_value=changed), self.assertRaises(ValueError):
                    screen.frozen_selection(output, plan, {})
            with tempfile.TemporaryDirectory() as second:
                output = Path(second)
                (output / "held" / "collections").mkdir(parents=True)
                with mock.patch.object(screen, "read_rows", return_value=rows), self.assertRaises(ValueError):
                    screen.frozen_selection(output, plan, {})

    def test_preparation_reload_and_dry_run_bind_all_104_candidates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "nextpnr-mistral"
            binary.write_bytes(b"test binary")
            families = []
            for index in range(5):
                mapped = root / f"mapped-{index}.json"
                mapped.write_bytes(str(index).encode())
                template = dict(schema_version=1,
                                cohort=dict(id=str(index), design_id=str(index),
                                            mapped_design_id="sha256:" + hashlib.sha256(mapped.read_bytes()).hexdigest(),
                                            constraint_family="clocks"), architecture="mistral",
                                command=[str(binary), "--json", str(mapped), "--seed", "{seed}"],
                                seeds=[1], repeats=1, limits=dict(per_run_seconds=600, total_seconds=720, concurrency=1),
                                inputs=[dict(path=str(mapped), role="mapped_netlist")], required_clocks=["clk"],
                                environment=dict(CUDA_CACHE_DISABLE="1"),
                                provenance=dict(source_revision="test", dirty=False, runtime_environment_id="auto"))
                path = root / f"template-{index}.json"
                path.write_text(json.dumps(template))
                families.append(dict(name=str(index), phase="train" if index < 2 else "heldout",
                                     baseline="critexp-7", weight=10, template=str(path)))
            config = dict(binary=str(binary), binary_source_revision="test", families=families)
            plan = screen.prepare(config, root / "evidence")
            loaded, groups = screen.load_plan(root / "evidence")
            self.assertEqual(plan, loaded)
            count = sum(len(screen.seed_racing.Collector(m, root / "dry").run(dry_run=True))
                        for group in groups.values() for _, m in group)
            self.assertEqual(count, 104)


if __name__ == "__main__":
    unittest.main()
