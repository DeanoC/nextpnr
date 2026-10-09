#!/usr/bin/env python3
"""Exercise the real observer, including termination while a visit is active."""
import json
from pathlib import Path
import subprocess
import tempfile
import time

root=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory() as tmp:
    directory=Path(tmp);binary=directory/'observer';profile=directory/'profile.json'
    subprocess.run(['g++','-std=c++17','-pthread','-I'+str(root/'common/route'),
        '-I'+str(root/'3rdparty/json11'),str(Path(__file__).with_suffix('.cpp')),
        str(root/'3rdparty/json11/json11.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary),str(profile)],check=True)
    value=json.loads(profile.read_text())
    assert value['completed'] and not value['active'] and value['calls_started']==value['calls_finished']==31
    assert len(value['slowest'])==20 and value['active_net_count']==0
    assert all(a['total_seconds']>=b['total_seconds'] for a,b in zip(value['slowest'],value['slowest'][1:]))
    process=subprocess.Popen([str(binary),str(profile),'hold'],stdout=subprocess.PIPE,text=True)
    try:
        assert process.stdout.readline().strip()=='active'
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            value=json.loads(profile.read_text())
            if value['active']:break
            time.sleep(.05)
        else:raise AssertionError('No active-net heartbeat')
        process.kill();process.wait(timeout=5)
        saved=json.loads(profile.read_text())
        assert not saved['completed'] and saved['active_net_count']==1
        row=saved['active'][0]
        assert row['active_calls']==1 and row['calls_started']==1 and row['calls_finished']==0
        assert row['net_index']==30 and row['active_seconds']>0
        assert row['name_truncated'] and row['net']=='x'*1023
    finally:
        if process.poll() is None:process.kill();process.wait()
    failure=subprocess.run([str(binary),str(directory/'missing/profile.json')],capture_output=True,text=True)
    assert failure.returncode==1 and 'Cannot write router2 profile' in failure.stderr
print('Router2 observer completion, concurrency, bounds, UTF-8 and killed-run tests passed')
