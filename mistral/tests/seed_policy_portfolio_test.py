#!/usr/bin/env python3
import sys
import hashlib
import tempfile
import unittest
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


if __name__ == "__main__":
    unittest.main()
