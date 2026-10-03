#!/usr/bin/env python3
"""Offline qualification-gate tests; no nextpnr build or GPU is required."""

from contextlib import redirect_stderr, redirect_stdout
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("spectrum_plateau", HERE / "spectrum_plateau.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class SpectrumQualification(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="spectrum-plateau-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.fixture = self.root / "fixture"
        self.fixture.mkdir()
        raw = b'{"modules":{"TOP":{}}}\n'
        inputs = {"synth.json.gz": gzip.compress(raw), "socket.qsf": b"# exact pins\n",
                  "clocks.sdc": b"# exact clocks\n"}
        for name, value in inputs.items():
            (self.fixture / name).write_bytes(value)
        hashes = {name: hashlib.sha256(value).hexdigest() for name, value in inputs.items()}
        hashes["synth.json"] = hashlib.sha256(raw).hexdigest()
        (self.fixture / "fixture.json").write_text(json.dumps(
            {"qsf": "socket.qsf", "default_seed": 4, "sha256": hashes}))
        self.calls = 0

    def fake_tool(self, mode):
        """A real subprocess supplies controlled compiler evidence and artifacts."""
        path = self.root / f"nextpnr-{self.calls}"
        counter = self.root / f"counter-{self.calls}"
        retained_log = runner.FIXTURES / "retained" / "seed-4-route.log"
        source = '''#!/usr/bin/env python3
import json
from pathlib import Path
import sys
import time
MODE = MODE_VALUE
COUNTER = Path(COUNTER_VALUE)
CLOCKS = CLOCKS_VALUE
RETAINED_LOG = Path(RETAINED_LOG_VALUE)
def arg(name):
    return sys.argv[sys.argv.index(name) + 1]
assert arg('--device') == '5CSEBA6U23I7'
assert arg('--freq') == '74.25'
assert arg('--placer-heap-timingweight') == '2000'
assert arg('--placer-heap-critexp') == '5'
assert arg('--router') == 'gpu'
assert '--timing-allow-fail' in sys.argv
assert '--detailed-timing-report' in sys.argv
assert '--compress-rbf' in sys.argv
if MODE == 'packing':
    print("ERROR: M10K 'machine.rom.lane0': Cyclone V M10K does not support asynchronous reads; use MLAB, logic, or a registered M10K read.")
    sys.exit(125)
if MODE in ('plateau', 'plateau_wrong_backend'):
    text = RETAINED_LOG.read_text()
    if MODE == 'plateau_wrong_backend':
        text = text.replace('backend hip:', 'backend cuda:')
    print(text, end='')
    sys.exit(125)
backend = 'cpu-reference' if MODE == 'cpu' else 'cuda:wrong GPU' if MODE == 'cuda' else 'hip:test GPU'
if MODE != 'missing_backend':
    print(f'Info:     backend {backend} ready in 0.01s (estimate 0.075 ns/x, 0.200 ns/y)', flush=True)
overuse = 1 if MODE == 'nonzero_overuse' else 0
print(f'Info:     iter=158 wires=151162 overused=0 overuse={overuse} tmgfail=0 nets=1 batches=1 archfail=0', flush=True)
if MODE == 'timeout':
    time.sleep(5)
count = int(COUNTER.read_text()) + 1 if COUNTER.exists() else 1
COUNTER.write_text(str(count))
if MODE != 'missing_route_complete':
    print('Info: Routing complete.')
checksum = count if MODE == 'checksum_change' else 1
print(f'Info: Checksum: {hex(checksum)}')
if MODE != 'missing_signoff':
    print('Info: Running signoff timing analysis...')
clocks = {name: {'achieved': target - 1, 'constraint': target} for name, target in CLOCKS.items()}
if MODE == 'quantized_constraints':
    constraints = {'machine.ram.pixel_clk': 74.25006866455078,
                   'system_clock.faithful.clocks[0]': 52.22477340698242,
                   'system_clock.faithful.clocks[1]': 12.288031578063965}
    for name, value in constraints.items():
        clocks[name]['constraint'] = value
if MODE == 'missing_clock':
    clocks.pop('system_clock.faithful.clocks[1]')
if MODE == 'wrong_constraint':
    clocks['system_clock.faithful.clocks[1]']['constraint'] = 10
if MODE == 'invalid_timing':
    clocks['machine.ram.pixel_clk']['achieved'] = float('nan')
if MODE == 'timing_change':
    clocks['machine.ram.pixel_clk']['achieved'] += count
Path(arg('--report')).write_text(json.dumps({'fmax': clocks}))
Path(arg('--write')).write_text('{"modules":{"TOP":{}}}')
if MODE != 'missing_rbf':
    Path(arg('--rbf')).write_bytes(str(count).encode() if MODE == 'rbf_change' else b'bitstream')
if MODE != 'missing_normal_finish':
    print('Info: Program finished normally.')
'''
        source = (source.replace("MODE_VALUE", repr(mode)).replace("COUNTER_VALUE", repr(str(counter)))
                  .replace("CLOCKS_VALUE", repr(runner.CLOCKS))
                  .replace("RETAINED_LOG_VALUE", repr(str(retained_log))))
        path.write_text(source)
        path.chmod(0o755)
        return path

    def invoke(self, mode="success", extra=()):
        self.calls += 1
        binary = self.fake_tool(mode)
        output = self.root / f"output-{self.calls}"
        args = ["--nextpnr", str(binary), "--fixture", str(self.fixture), "--output", str(output)]
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            status = runner.main(args + list(extra))
        return status, json.loads((output / "summary.json").read_text()), output

    def test_real_fixture_hashes_and_failure_evidence(self):
        for name, seed, qsf in (("retained", 4, "socket.qsf"), ("reconstructed", 2, "constraints.qsf")):
            manifest, hashes, synth = runner.load_fixture(runner.FIXTURES / name)
            self.assertEqual((manifest["default_seed"], manifest["qsf"]), (seed, qsf))
            self.assertEqual(hashlib.sha256(synth).hexdigest(), hashes["synth.json"])
        text = (runner.FIXTURES / "retained" / "seed-4-route.log").read_text()
        result = runner.log_evidence(text, 125)
        self.assertEqual(result["outcome"], "initial_plateau")
        self.assertEqual(result["last_iteration"]["iteration"], 104)
        self.assertFalse(result["reached_zero_overuse"])
        for wrong_exit in (0, 1, -11):
            with self.subTest(exit=wrong_exit):
                self.assertNotEqual(runner.log_evidence(text, wrong_exit)["outcome"], "initial_plateau")
        text = (runner.FIXTURES / "retained" / "seed-5-route.log").read_text()
        result = runner.log_evidence(text, None)
        self.assertEqual(result["outcome"], "timeout")
        self.assertEqual(result["last_iteration"]["iteration"], 202)
        self.assertTrue(result["reached_zero_overuse"])
        self.assertFalse(result["route_complete"])
        self.assertFalse(result["signoff"])

    def test_legal_route_passes_independently_of_timing_miss_and_repeats(self):
        status, summary, output = self.invoke(extra=("--repeat", "2"))
        self.assertEqual(status, 0)
        self.assertTrue(summary["passed"])
        first, second = summary["runs"]
        self.assertEqual(first["seed"], 4)
        self.assertEqual(first["outcome"], "route_complete")
        self.assertFalse(first["analogue_timing"]["fes_timing_pass"])
        self.assertFalse(first["analogue_timing"]["signoff_pass"])
        self.assertTrue(second["repeat_matches"])
        self.assertNotEqual(first["log"], second["log"])
        self.assertEqual(summary["nextpnr_sha256"], runner.sha256(Path(summary["nextpnr"])))
        self.assertEqual((output / "synth.json").read_bytes(), gzip.decompress((self.fixture / "synth.json.gz").read_bytes()))

    def test_route_gate_requires_completion_backend_outputs_and_valid_clocks(self):
        for mode in ("missing_backend", "cuda", "cpu", "nonzero_overuse", "missing_route_complete",
                     "missing_normal_finish", "missing_signoff", "missing_clock", "wrong_constraint",
                     "invalid_timing", "missing_rbf"):
            with self.subTest(mode=mode):
                status, summary, _ = self.invoke(mode)
                self.assertEqual(status, 1)
                self.assertFalse(summary["passed"])
                self.assertTrue(summary["runs"][0]["errors"])
        self.assertEqual(self.invoke("cpu", ("--gpu-cpu",))[0], 0)
        self.assertEqual(self.invoke("success", ("--gpu-cpu",))[0], 1)

    def test_quantized_real_constraints_and_saved_run_reassessment(self):
        status, summary, output = self.invoke("quantized_constraints")
        self.assertEqual(status, 0)
        saved = summary["runs"][0]
        saved.update({"outcome": "incomplete", "passed": False, "errors": ["obsolete clock gate error"],
                      "rbf_sha256": "obsolete digest", "repeat_matches": False})
        run_output = output / "seed-4-run-1"
        before = {path.name: path.read_bytes() for path in run_output.iterdir()}
        result = runner.assess_run(SimpleNamespace(gpu_cpu=False, expect_failure=None), run_output, saved)
        self.assertIs(result, saved)
        self.assertTrue(result["passed"])
        self.assertEqual(result["outcome"], "route_complete")
        self.assertEqual(result["errors"], [])
        self.assertIsNone(result["repeat_matches"])
        self.assertEqual(result["rbf_sha256"], runner.sha256(run_output / "core.rbf"))
        self.assertEqual(before, {path.name: path.read_bytes() for path in run_output.iterdir()})

    def test_expected_failure_is_specific_and_requires_plateau_backend(self):
        self.assertEqual(self.invoke("plateau", ("--expect-failure", "plateau"))[0], 0)
        self.assertEqual(self.invoke("packing", ("--expect-failure", "packing"))[0], 0)
        for mode, expected in (("plateau", "packing"), ("packing", "plateau"),
                               ("plateau_wrong_backend", "plateau"), ("success", "plateau")):
            with self.subTest(mode=mode, expected=expected):
                self.assertEqual(self.invoke(mode, ("--expect-failure", expected))[0], 1)
        packing = ("ERROR: M10K 'lane0': Cyclone V M10K does not support asynchronous reads; "
                   "use MLAB, logic, or a registered M10K read.")
        for wrong_exit in (0, 1, -11):
            self.assertNotEqual(runner.log_evidence(packing, wrong_exit)["outcome"], "packing_rejected")

    def test_timeout_preserves_partial_zero_overuse_and_never_passes(self):
        for extra in ((), ("--expect-failure", "plateau")):
            status, summary, _ = self.invoke("timeout", ("--timeout", "0.5") + extra)
            result = summary["runs"][0]
            self.assertEqual(status, 1)
            self.assertEqual(result["outcome"], "timeout")
            self.assertTrue(result["reached_zero_overuse"])
            self.assertTrue(Path(result["log"]).is_file())
            self.assertFalse(result["route_complete"])

    def test_repeat_detects_checksum_bitstream_and_timing_changes(self):
        for mode in ("checksum_change", "rbf_change", "timing_change"):
            with self.subTest(mode=mode):
                status, summary, _ = self.invoke(mode, ("--repeat", "2"))
                self.assertEqual(status, 1)
                self.assertFalse(summary["runs"][1]["repeat_matches"])
                self.assertIn("differs", summary["runs"][1]["errors"][-1])

    def test_stale_output_and_tampered_fixture_fail_before_running(self):
        binary = self.fake_tool("success")
        output = self.root / "stale"
        output.mkdir()
        sentinel = output / "summary.json"
        sentinel.write_text("existing evidence")
        args = ["--nextpnr", str(binary), "--fixture", str(self.fixture), "--output", str(output)]
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
            runner.main(args)
        self.assertEqual(caught.exception.code, 2)
        self.assertEqual(sentinel.read_text(), "existing evidence")
        self.assertFalse((output / "synth.json").exists())
        (self.fixture / "socket.qsf").write_text("changed pins")
        fresh = self.root / "fresh"
        args[-1] = str(fresh)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
            runner.main(args)
        self.assertEqual(caught.exception.code, 2)
        self.assertFalse(fresh.exists())


if __name__ == "__main__":
    unittest.main()
