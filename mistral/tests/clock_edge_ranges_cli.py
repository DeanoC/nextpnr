#!/usr/bin/env python3
"""Check explicit clock-edge range selection and checkpoint identity."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('nextpnr', type=Path)
binary = parser.parse_args().nextpnr.resolve()
both = ['--mistral-routed-delay-ranges', '--mistral-clock-edge-ranges']
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / 'empty.json').write_text(json.dumps({'modules': {'top': {
        'attributes': {'top': '1'}, 'ports': {}, 'cells': {}, 'netnames': {}}}}))
    def run(options, success=True, message=None):
        result = subprocess.run([str(binary), '--device', '5CSEBA6U23I7', *options],
                                cwd=work, capture_output=True, text=True)
        text = result.stdout + result.stderr
        assert (result.returncode == 0) == success, (result.returncode, text)
        if message:
            assert message in text, text
    run(['--json', 'empty.json', '--pack-only', '--mistral-clock-edge-ranges'], False,
        '--mistral-clock-edge-ranges requires --mistral-routed-delay-ranges')
    run(['--json', 'empty.json', '--pack-only', *both,
         '--set', 'mistral/clock_edge_ranges=0', '--write', 'conflict.json'], False,
        "setting 'mistral/clock_edge_ranges' is set by both --set and a dedicated command-line option")
    assert not (work / 'conflict.json').exists()
    run(['--json', 'empty.json', '--pack-only', '--write', 'default.json'])
    run(['--json', 'empty.json', '--pack-only', *both, '--write', 'recorded.json'])
    replay = ['--json', 'recorded.json', '--no-pack', '--no-place', '--no-route']
    run([*replay, '--mistral-routed-delay-ranges', '--write', 'refused.json'], False,
        'pass --mistral-clock-edge-ranges again')
    assert not (work / 'refused.json').exists()
    run([*replay, *both, '--write', 'replay.json'])
    for name in ('recorded.json', 'replay.json'):
        settings = json.loads((work / name).read_text())['modules']['top']['settings']
        for key in ('mistral/clock_edge_ranges', 'mistral/routed_delay_ranges'):
            assert re.fullmatch(r'0*1', settings[key]), (name, key)
    default = json.loads((work / 'default.json').read_text())['modules']['top']['settings']
    assert 'mistral/clock_edge_ranges' not in default
print('PASS: default-off, range dependency, conflicts, checkpoint identity and explicit acknowledgement')
