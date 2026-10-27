#!/usr/bin/env python3
"""Exercise the real LUT-reference CLI and checkpoint acknowledgement."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('nextpnr',type=Path)
    binary=parser.parse_args().nextpnr.resolve()
    with tempfile.TemporaryDirectory() as directory:
        work=Path(directory)
        (work/'empty.json').write_text(json.dumps({'modules':{'top':{'attributes':{'top':'1'},'ports':{},'cells':{},'netnames':{}}}}))
        def run(options,success=True,message=None):
            result=subprocess.run([str(binary),'--device','5CSEBA6U23I7',*options],cwd=work,capture_output=True,text=True)
            output=result.stdout+result.stderr
            assert (result.returncode==0)==success,(result.returncode,output)
            if message:assert message in output,output
        flag='--mistral-lut-core-reference'
        setting='mistral/lut_core_reference'
        run(['--json','empty.json','--pack-only',flag,'--set',setting+'=0','--write','conflict.json'],False,
            "setting 'mistral/lut_core_reference' is set by both --set and a dedicated command-line option")
        assert not (work/'conflict.json').exists()
        run(['--json','empty.json','--pack-only','--set',setting+'=1','--write','implicit.json'],False,
            'pass --mistral-lut-core-reference again')
        assert not (work/'implicit.json').exists()
        run(['--json','empty.json','--pack-only','--write','ordinary.json'])
        run(['--json','empty.json','--pack-only',flag,'--write','selected.json'])
        replay=['--json','selected.json','--no-pack','--no-place','--no-route']
        run([*replay,'--write','refused.json'],False,'pass --mistral-lut-core-reference again')
        assert not (work/'refused.json').exists()
        run([*replay,flag,'--write','replayed.json'])
        for name in ('selected.json','replayed.json'):
            settings=json.loads((work/name).read_text())['modules']['top']['settings']
            assert re.fullmatch(r'0*1',settings[setting])
        assert setting not in json.loads((work/'ordinary.json').read_text())['modules']['top']['settings']
    print('PASS: default-off, conflict rejection, explicit recording and checkpoint acknowledgement')
