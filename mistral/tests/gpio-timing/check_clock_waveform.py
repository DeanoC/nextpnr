#!/usr/bin/env python3
"""Recompute the real fitted model and reject corrupt/mismatched references."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
from clock_waveform import extract, evaluate, pulse_rows


def rejected(name, function):
    try:
        function()
    except (ValueError, FileNotFoundError):
        print('rejected ' + name)
    else:
        raise AssertionError('accepted ' + name)


def check(root, model):
    root = Path(root)
    assert [extract(root/f'load{load}') for load in (0,15,30)] == model['loads']
    for load in (0,15,30):
        ideal = evaluate(model,load,5000,5000,0)
        narrowed = evaluate(model,load,4400,4700,200)
        assert narrowed['high_min_ps'] == ideal['high_min_ps']-500
        assert narrowed['low_min_ps'] == ideal['low_min_ps']-800
        assert not narrowed['physical_signoff']
    # Independently compare the evaluator with each real pulse report.
    # Vendor 'high' spans the mux's low phase for this inverted forwarder.
    for sample in model['loads']:
        for scenario in sample['scenarios']:
            pulses = scenario['vendor_pulses']
            result = evaluate(model, sample['load_pf'],
                              pulses['low']['ingress_width_ps'],
                              pulses['high']['ingress_width_ps'], 0)
            for kind in ('high', 'low'):
                assert result[kind+'_min_ps'] <= pulses[kind]['raw_width_ps']+2
                assert pulses[kind]['raw_width_ps']-result[kind+'_min_ps'] < 1000
    rejected('noninverted model', lambda: evaluate(dict(model, inverted=False),15,4400,4700,0))
    for load in (1,14.9,29.9,31,float('nan')):
        rejected('unsampled load '+str(load),lambda: evaluate(model,load,5000,5000,0))
    rejected('negative uncertainty', lambda: evaluate(model,15,5000,5000,-1))
    rejected('nonfinite ingress', lambda: evaluate(model,15,float('inf'),5000,0))
    project = root/'load0/clock-forward-inverted'
    pulse = (project/'pulse-check/7_slow_1100mv_100c/pulse.rpt').read_text()
    # The raw endpoint subtraction must agree with the vendor width only after
    # adding the separately recorded credit; the model never takes that credit.
    rejected('modified credit', lambda: pulse_rows(pulse.replace('0.663','0.763')))
    rejected('missing pulse edge', lambda: pulse_rows(pulse.split('Path #2:')[0]))
    with tempfile.TemporaryDirectory() as tmp:
        dst = Path(tmp)/'load0'; target = dst/'clock-forward-inverted'
        for name in model['loads'][0]['hashes']:
            src = project/name; out = target/name
            out.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,out)
        evidence = dst/'evidence.json'; original = evidence.read_text()
        bad = json.loads(original);bad['forwarded_clock_polarities']['clock-forward-inverted']=False
        evidence.write_text(json.dumps(bad))
        rejected('wrong polarity',lambda:extract(dst));evidence.write_text(original)
        bad = json.loads(original);bad['output_load_pf']=15
        evidence.write_text(json.dumps(bad))
        rejected('load declaration mismatch',lambda:extract(dst));evidence.write_text(original)
        report=target/'7_slow_1100mv_100c/output-rise-setup.rpt';saved=report.read_text()
        require_changed = saved.replace('DDR_OUT~output|o','DDR_OUT~wrong_output|o')
        assert require_changed != saved
        report.write_text(require_changed)
        rejected('changed raw transfer',lambda:extract(dst));report.write_text(saved)
        report=target/'pulse-check/7_slow_1100mv_100c/pulse.rpt';saved=report.read_text()
        report.write_text(saved.replace('Slow 1100mV 100C Model','Fast 1100mV 100C Model'))
        rejected('mixed corner',lambda:extract(dst));report.write_text(saved)
        report.write_text(saved.replace('|muxsel','|wrong_mux'))
        rejected('changed pulse boundary',lambda:extract(dst));report.write_text(saved)
        report.write_text(saved.replace('1.697','1.797'))
        rejected('changed pulse local delay',lambda:extract(dst))
    print('PASS: 12 same-corner/load scenarios,24 pulse decompositions; unknown loads and malformed evidence reject')


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('model',type=Path)
    a=p.parse_args();check(a.root,json.loads(a.model.read_text()))
