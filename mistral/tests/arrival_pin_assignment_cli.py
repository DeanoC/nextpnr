#!/usr/bin/env python3
"""Check explicit LUT placement/arrival policies through real JSON/CLI loading."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
SETTINGS = ("mistral/arrival_pin_assignment", "mistral/isolated_lut_placement")


class ArrivalPinAssignmentCliTest(unittest.TestCase):
    option = "--arrival-pin-assignment"
    setting = SETTINGS[0]

    def run_design(self, enabled, both=False):
        with tempfile.TemporaryDirectory(prefix="arrival-pin-cli-") as directory:
            root = Path(directory)
            module = {"attributes": {"top": 1}, "ports": {}, "cells": {},
                      "netnames": {}, "settings": {key: "1" for key in SETTINGS}}
            source = root / "design.json"
            output = root / "packed.json"
            source.write_text(json.dumps({"modules": {"top": module}}))
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(source),
                       "--pack-only", "--write", str(output)]
            if enabled:
                command.append(self.option)
            if both:
                command.extend(["--arrival-pin-assignment", "--isolated-lut-placement"])
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("NEXTPNR_MISTRAL_")}
            result = subprocess.run(command, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True,
                                    env=env, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout)
            return json.loads(output.read_text())["modules"]["top"].get("settings", {})

    def test_saved_provenance_does_not_enable_policy(self):
        settings = self.run_design(False)
        for key in SETTINGS:
            self.assertNotIn(key, settings)

    def test_explicit_option_records_policy(self):
        settings = self.run_design(True)
        self.assertEqual(settings[self.setting], "1")
        for key in SETTINGS:
            if key != self.setting:
                self.assertNotIn(key, settings)

    def test_both_options_record_both_policies(self):
        settings = self.run_design(False, both=True)
        for key in SETTINGS:
            self.assertEqual(settings[key], "1")

    def run_script_design(self, enabled):
        with tempfile.TemporaryDirectory(prefix="arrival-pin-script-") as directory:
            root = Path(directory)
            script = root / "design.py"
            output = root / "packed.json"
            script.write_text("assert ctx.pack()\n")
            command = [BINARY, "--device", "5CSEBA6U23I7", "--run", str(script),
                       "--write", str(output)]
            if enabled:
                command.append(self.option)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("NEXTPNR_MISTRAL_")}
            result = subprocess.run(command, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, env=env, timeout=30)
            if "unrecognised option '--run'" in result.stdout:
                self.skipTest("binary was built without Python support")
            self.assertEqual(result.returncode, 0, result.stdout)
            return json.loads(output.read_text())["modules"]["top"].get("settings", {})

    def test_script_option_records_policy_without_json(self):
        self.assertEqual(self.run_script_design(True)[self.setting], "1")

    def test_script_policy_is_off_by_default(self):
        self.assertNotIn(self.setting, self.run_script_design(False))


class IsolatedLutPlacementCliTest(ArrivalPinAssignmentCliTest):
    option = "--isolated-lut-placement"
    setting = SETTINGS[1]


if __name__ == "__main__":
    unittest.main()
