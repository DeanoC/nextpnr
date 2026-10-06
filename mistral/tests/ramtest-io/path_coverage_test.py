#!/usr/bin/env python3
"""Exercise coverage rejection using an actual complete RAM tester report."""
import argparse
import copy
import json
from pathlib import Path

from path_coverage import audit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    module = json.loads(args.checkpoint.read_text())['modules']['top']
    report = json.loads(args.report.read_text())
    baseline = audit(module, report)
    assert baseline['all_channels_checked']
    assert baseline['active_directions'] == 53
    assert baseline['expected_channels'] == 70

    # Remove a subchannel, keeping both the pin and its declaration intact.
    changed = copy.deepcopy(report)
    for net in changed['detailed_net_timings']:
        net['endpoints'] = [e for e in net['endpoints'] if e['port'] != 'PAD$timing$write$oe$external']
    result = audit(module, changed)
    assert result['declared_directions'] == 53 and result['checked_channels'] == 54
    assert not result['all_channels_checked']

    # Retain every reported arrival while excluding checks, as clock cuts do.
    for mutation in ('setup_checked', 'hold_checked', 'setup_slack_ns', 'hold_slack_ns', 'delay', 'event', 'source'):
        changed = copy.deepcopy(report)
        endpoint = next(e for n in changed['detailed_net_timings'] for e in n['endpoints']
                        if e['port'] == 'PAD$timing$read$rise$register')
        endpoint[mutation] = {
            'setup_checked': False, 'hold_checked': False,
            'setup_slack_ns': None, 'hold_slack_ns': float('nan'),
            'delay': [2, 1], 'event': 'negedge unknown',
            'source': {'cell': 'wrong_pad', 'port': 'PAD'},
        }[mutation]
        assert not audit(module, changed)['all_channels_checked'], mutation

    changed = copy.deepcopy(module)
    saved = json.loads(changed['settings']['timing/io_delays'])
    changed['settings']['timing/io_delays'] = json.dumps([r for r in saved if r['port'] != 'SDRAM_DQ[0]'])
    result = audit(changed, report)
    assert result['declared_directions'] == 51 and not result['all_channels_checked']

    # A positive internal clock summary cannot replace the actual pad checks.
    changed = copy.deepcopy(report)
    for clock in changed['timing_summary']['clocks'].values():
        clock.update(setup_wns_ns=100, hold_wns_ns=100)
    assert not audit(module, changed)['all_channel_slacks_nonnegative']
    changed['timing_summary']['final_analogue_model'] = False
    try:
        audit(module, changed)
    except ValueError:
        pass
    else:
        raise AssertionError('Non-final report accepted')
    print('PASS: complete coverage and 11 rejection/qualification checks')


if __name__ == '__main__':
    main()
