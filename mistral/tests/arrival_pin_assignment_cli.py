#!/usr/bin/env python3
"""Check explicit arrival-pin policy through real JSON/CLI loading."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
SETTING = "mistral/arrival_pin_assignment"


class ArrivalPinAssignmentCliTest(unittest.TestCase):
    def run_design(self, enabled):
        with tempfile.TemporaryDirectory(prefix="arrival-pin-cli-") as directory:
            root = Path(directory)
            module = {"attributes": {"top": 1}, "ports": {}, "cells": {},
                      "netnames": {}, "settings": {SETTING: "1"}}
            source = root / "design.json"
            output = root / "packed.json"
            source.write_text(json.dumps({"modules": {"top": module}}))
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(source),
                       "--pack-only", "--write", str(output)]
            if enabled:
                command.append("--arrival-pin-assignment")
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("NEXTPNR_MISTRAL_")}
            result = subprocess.run(command, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True,
                                    env=env, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout)
            return json.loads(output.read_text())["modules"]["top"].get("settings", {})

    def test_saved_provenance_does_not_enable_policy(self):
        self.assertNotIn(SETTING, self.run_design(False))

    def test_explicit_option_records_policy(self):
        self.assertEqual(self.run_design(True)[SETTING], "1")


if __name__ == "__main__":
    unittest.main()
