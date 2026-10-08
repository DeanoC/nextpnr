import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import analogue_fanout_screen as experiment
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "python"))
import seed_racing


class GateTests(unittest.TestCase):
    def setUp(self):
        self.row = dict(status="analogue_timing_failure", legal_route=True)
        self.report = dict(timing_summary=dict(clocks={
            "pixel_clk": dict(setup_wns_ns=-.1, hold_wns_ns=.7),
            "system": dict(setup_wns_ns=1, hold_wns_ns=.7)}),
            critical_paths=[dict(to="posedge pixel_clk", **{"from": "posedge pixel_clk"}, path=[dict(net=experiment.NET)])],
            detailed_net_timings=[dict(net=experiment.NET, endpoints=[dict(cell=str(i), port="D") for i in range(65)])])

    def test_reproduced_target_accepts(self):
        self.assertTrue(experiment.reproduce_gate(self.row, self.report))

    def test_success_or_timeout_is_not_reproduction(self):
        for status in ("completed", "timeout"):
            row = dict(self.row, status=status)
            self.assertFalse(experiment.reproduce_gate(row, self.report))

    def test_other_clock_failure_blocks(self):
        self.report["timing_summary"]["clocks"]["system"]["hold_wns_ns"] = -.1
        self.assertFalse(experiment.reproduce_gate(self.row, self.report))

    def test_cross_domain_path_does_not_qualify(self):
        self.report["critical_paths"][0]["from"] = "<async>"
        self.assertFalse(experiment.reproduce_gate(self.row, self.report))

    def test_duplicate_sinks_do_not_cross_cutoff(self):
        self.report["detailed_net_timings"][0]["endpoints"][-1] = dict(cell="0", port="D")
        self.assertFalse(experiment.reproduce_gate(self.row, self.report))

    def test_zero_setup_respects_strict_positive_gate(self):
        self.report["timing_summary"]["clocks"]["pixel_clk"]["setup_wns_ns"] = 0
        self.assertTrue(experiment.reproduce_gate(self.row, self.report))


class RunnerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repo = self.root / "nextpnr"
        self.repo.mkdir()
        self.output = self.root / "evidence"
        self.binary = self.root / "build-mistral-cuda" / "nextpnr-mistral"
        self.binary.parent.mkdir()
        self.binary.write_bytes(b"synthetic binary")
        (self.repo / "input.json").write_bytes(b"real input")
        self.template = dict(schema_version=1, architecture="mistral",
            cohort=dict(id="fixture", design_id="fixture", mapped_design_id="fixture", constraint_family="fixture"),
            command=[str(self.binary), "--json", "input.json", "--seed", "{seed}",
                     "--placer-heap-critexp", "2", "--router", "gpu"],
            inputs=[dict(role="mapped_netlist", path="input.json")], cwd=str(self.repo),
            seeds=[34], repeats=1, required_clocks=["pixel_clk"], environment={},
            limits=dict(per_run_seconds=600, total_seconds=720, concurrency=1),
            provenance=dict(source_revision=experiment.SOURCE, dirty=False, runtime_environment_id="auto"),
            artifacts=dict(telemetry="telemetry.jsonl", final_report="timing.json", bitstream="core.rbf"))
        self.template_path = self.root / "template.json"
        experiment.write_new(self.template_path, self.template)

    def prepare(self):
        with mock.patch.object(experiment.subprocess, "check_output", side_effect=[experiment.SOURCE + "\n", b""]):
            return experiment.prepare(self.repo, self.template_path, self.output)

    def fixture(self):
        plan = self.prepare()
        for case in plan["cases"][:2]:
            directory = self.output / "collections" / case["name"]
            directory.mkdir(parents=True)
            report = dict(timing_summary=dict(clocks={"pixel_clk": dict(setup_wns_ns=-.1, hold_wns_ns=.7)}),
                          critical_paths=[], detailed_net_timings=[])
            report_path = directory / "report.json"
            experiment.write_new(report_path, report)
            manifest = json.loads((self.output / (case["name"] + ".json")).read_bytes())
            bound = copy.deepcopy(manifest)
            bound.update(binary=dict(sha256=plan["binary_sha256"]), inputs=plan["inputs"], execution_identity={"runtime": "same"})
            row = dict(status="analogue_timing_failure", success=False, legal_route=True,
                       duration_seconds=1, final_multi_clock_margin_ns=-.1)
            summary = dict(cohort_identity=dict(manifest=bound), synthetic_rows=[row], results=[dict(artifacts=dict(
                final_report=dict(available=True, path=str(report_path), sha256=experiment.sha(report_path.read_bytes()))))])
            experiment.write_new(directory / "collection-fixture.json", summary)
        for patch in (mock.patch.object(seed_racing, "assemble_dataset", side_effect=lambda paths: json.loads(paths[0].read_bytes())["synthetic_rows"]),
                      mock.patch.object(seed_racing, "validate_dataset", side_effect=lambda document: document)):
            patch.start()
            self.addCleanup(patch.stop)
        return plan

    def finalize_fixture(self):
        plan = self.fixture()
        with mock.patch.object(seed_racing.Collector, "run") as collector:
            experiment.run(self.output)
            collector.assert_not_called()
        return plan

    def test_relative_inputs_resolve_against_generated_cwd_not_invoker(self):
        (self.root / "input.json").write_bytes(b"wrong input")
        previous = Path.cwd()
        try:
            os.chdir(self.root)
            plan = self.prepare()
            self.assertEqual(plan["inputs"][0]["path"], str(self.repo / "input.json"))
            self.assertEqual(plan["inputs"][0]["sha256"], experiment.sha(b"real input"))
            with mock.patch.object(seed_racing.Collector, "run", return_value=[{}]) as collector:
                experiment.run(self.output, dry_run=True)
                self.assertEqual(collector.call_count, 8)
        finally:
            os.chdir(previous)

    def test_missing_relative_input_does_not_use_invoker_file(self):
        (self.repo / "input.json").unlink()
        (self.root / "input.json").write_bytes(b"wrong input")
        previous = Path.cwd()
        try:
            os.chdir(self.root)
            with self.assertRaises(FileNotFoundError):
                self.prepare()
        finally:
            os.chdir(previous)

    def test_saved_decision_is_plan_bound_and_reauthenticated_without_writes(self):
        plan = self.finalize_fixture()
        path = self.output / "decision.json"
        original = path.read_bytes()
        self.assertEqual(json.loads(original)["plan_sha256"], experiment.sha(experiment.canonical(plan)))
        with mock.patch.object(seed_racing.Collector, "run") as collector, \
                mock.patch.object(experiment, "authenticated", wraps=experiment.authenticated) as authenticate:
            experiment.run(self.output)
            collector.assert_not_called()
            self.assertEqual(authenticate.call_count, 2)
        self.assertEqual(path.read_bytes(), original)

    def test_corrupt_unbound_and_wrong_plan_decisions_fail_closed(self):
        self.fixture()
        path = self.output / "decision.json"
        for value in (b"{", b"{}", b'{"plan_sha256":"wrong"}'):
            path.write_bytes(value)
            with mock.patch.object(seed_racing.Collector, "run") as collector, self.assertRaises(ValueError):
                experiment.run(self.output)
            collector.assert_not_called()
            self.assertEqual(path.read_bytes(), value)

    def test_altered_artifact_fails_after_finalization(self):
        self.finalize_fixture()
        path = self.output / "decision.json"
        original = path.read_bytes()
        next((self.output / "collections").glob("*/report.json")).write_bytes(b"tampered report")
        with mock.patch.object(seed_racing.Collector, "run") as collector, self.assertRaisesRegex(ValueError, "report hash mismatch"):
            experiment.run(self.output)
        collector.assert_not_called()
        self.assertEqual(path.read_bytes(), original)

    def test_missing_recorded_collection_never_admits_replacement(self):
        self.finalize_fixture()
        directory = next((self.output / "collections").iterdir())
        directory.rename(directory.with_name("removed-collection"))
        with mock.patch.object(seed_racing.Collector, "run") as collector, self.assertRaisesRegex(ValueError, "no new runs admitted"):
            experiment.run(self.output)
        collector.assert_not_called()

    def test_invented_observation_fails_after_reauthentication(self):
        self.finalize_fixture()
        path = self.output / "decision.json"
        saved = json.loads(path.read_bytes())
        saved["observed"][0]["seconds"] = 100
        path.write_bytes(experiment.canonical(saved))
        with mock.patch.object(seed_racing.Collector, "run") as collector, self.assertRaisesRegex(ValueError, "differs from authenticated"):
            experiment.run(self.output)
        collector.assert_not_called()

    def test_unrecorded_collection_fails_closed(self):
        self.finalize_fixture()
        (self.output / "collections" / "extra").mkdir()
        with mock.patch.object(seed_racing.Collector, "run") as collector, self.assertRaisesRegex(ValueError, "unrecorded collections"):
            experiment.run(self.output)
        collector.assert_not_called()


if __name__ == "__main__":
    unittest.main()
