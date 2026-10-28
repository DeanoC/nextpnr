#!/usr/bin/env python3
"""Check the explicit LUT-route minimum CLI, dependencies and checkpoint gate."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
flag = '--mistral-lut-route-min-reference'
setting = 'mistral/lut_route_min_reference'
deps = ['--mistral-lut-core-reference', '--mistral-routed-delay-ranges']
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work/'empty.json').write_text(json.dumps({'modules': {'top': {'attributes': {'top':'1'},
        'ports':{}, 'cells':{}, 'netnames':{}}}}))
    def run(args, success=True, message=None):
        r = subprocess.run([str(binary),'--device','5CSEBA6U23I7',*args], cwd=work, capture_output=True, text=True)
        assert (r.returncode == 0) == success, (r.returncode, r.stdout, r.stderr)
        if message:
            assert message in r.stdout+r.stderr, (r.stdout, r.stderr)
    basic = ['--json','empty.json','--pack-only']
    run([*basic,flag], False, 'requires 5CSEBA6U23I7')
    run([*basic,flag,deps[0]], False, '--mistral-routed-delay-ranges')
    run([*basic,flag,deps[1]], False, '--mistral-lut-core-reference')
    run([*basic,*deps,flag,'--set',setting+'=0','--write','conflict.json'],False,
        "setting 'mistral/lut_route_min_reference' is set by both --set and a dedicated command-line option")
    run([*basic,*deps,'--set',setting+'=1'],False,'pass --mistral-lut-route-min-reference again')
    run([*basic,'--write','ordinary.json'])
    run([*basic,*deps,flag,'--write','selected.json'])
    replay = ['--json','selected.json','--no-pack','--no-place','--no-route']
    run([*replay,*deps,'--write','refused.json'],False,'pass --mistral-lut-route-min-reference again')
    assert not (work/'refused.json').exists()
    run([*replay,*deps,flag,'--write','replayed.json'])
    for name in ('selected.json','replayed.json'):
        settings = json.loads((work/name).read_text())['modules']['top']['settings']
        assert int(settings[setting], 2) == 1
    assert setting not in json.loads((work/'ordinary.json').read_text())['modules']['top']['settings']
print('PASS: dependencies, default identity, conflict rejection and checkpoint acknowledgement')
