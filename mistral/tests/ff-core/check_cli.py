#!/usr/bin/env python3
"""Check explicit profile recording/reload acknowledgement; no timing signoff."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('nextpnr', type=Path)
    args = parser.parse_args()
    binary = args.nextpnr.resolve()
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        (work / 'empty.json').write_text(json.dumps({'modules': {'top': {
            'attributes': {'top': '1'}, 'ports': {}, 'cells': {}, 'netnames': {}}}}))
        def run(options, success=True, message=None):
            result = subprocess.run([str(binary), '--device', '5CSEBA6U23I7', *options],
                                    cwd=work, text=True, capture_output=True)
            text = result.stdout + result.stderr
            assert (result.returncode == 0) == success, (result.returncode, text)
            if message:
                assert message in text, text
        run(['--json', 'empty.json', '--pack-only', '--mistral-ff-core-reference',
             '--set', 'mistral/ff_core_reference=0', '--write', 'conflict.json'], success=False,
            message="setting 'mistral/ff_core_reference' is set by both --set and a dedicated command-line option")
        assert not (work / 'conflict.json').exists()
        run(['--json', 'empty.json', '--pack-only', '--write', 'default.json'])
        run(['--json', 'empty.json', '--pack-only', '--mistral-ff-core-reference', '--write', 'recorded.json'])
        replay = ['--json', 'recorded.json', '--no-pack', '--no-place', '--no-route']
        run([*replay, '--write', 'refused.json'], success=False,
            message='pass --mistral-ff-core-reference again')
        assert not (work / 'refused.json').exists()
        run([*replay, '--mistral-ff-core-reference', '--write', 'replay.json'])
        for name in ('recorded.json', 'replay.json'):
            value = json.loads((work / name).read_text())['modules']['top']['settings']['mistral/ff_core_reference']
            assert re.fullmatch(r'0*1', value), value
        default = json.loads((work / 'default.json').read_text())['modules']['top']['settings']
        assert 'mistral/ff_core_reference' not in default
    print('PASS default-off, recording, missing-option rejection and explicit acknowledgement')


if __name__ == '__main__':
    main()
