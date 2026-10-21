#!/usr/bin/env python3
"""Decode this core's 3x8x8 text in a 1280x720 HDMI capture using its RTL font."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess


def qualify_rows(rows):
    """Turn decoded text rows into a sweep result, or raise ValueError."""
    if rows.get(5) not in ('PASS', 'FAIL') or not re.fullmatch(r'INVR W 6/6  100 MHZ', rows.get(1, '')):
        raise ValueError('Not a completed six-pattern 100 MHz sweep')
    err = re.fullmatch(r'ERR ([0-9A-F]{8}) WAS ([0-9A-F]{4})', rows.get(3, ''))
    if err is None:
        raise ValueError('ERR row is not a completed sweep')
    addr = re.fullmatch(r'ADDR 03FFFFFF AT ([0-9A-F]{8}) ([0-9A-F]{8})', rows.get(2, ''))
    if addr is None:
        raise ValueError('Address row is not a completed sweep')
    total, was = err.groups()
    first, last = addr.groups()
    patterns = {}
    for row, name in enumerate(['0000', 'FFFF', '5555', 'AAAA', 'ADDR', 'INVR'], 21):
        match = re.fullmatch(name + r' +([0-9A-F]{8})', rows.get(row, ''))
        if match is None:
            raise ValueError('Pattern table has missing or additional rate results')
        patterns[name] = int(match[1], 16)
    if sum(patterns.values()) != int(total, 16):
        raise ValueError('Pattern counts do not match total')
    ports = {}
    for row, port in enumerate(['P0', 'P1', 'P2'], 8):
        match = re.fullmatch(port + r' ADDR W 7/7 [0-9A-F]{8} E ([0-9A-F]{8}) (PASS|FAIL)', rows.get(row, ''))
        if match is None:
            raise ValueError('HPS DDR sweep is not complete')
        ports[port] = dict(state=match[2], errors=int(match[1], 16))
    return dict(state=rows[5], total=total, was=was, first=first, last=last, patterns=patterns, ports=ports)


def self_check():
    rows = {
        1: 'INVR W 6/6  100 MHZ',
        2: 'ADDR 03FFFFFF AT 00000001 00000002',
        3: 'ERR 00000003 WAS 00AB',
        5: 'PASS',
        8: 'P0 ADDR W 7/7 00000000 E 00000000 PASS',
        9: 'P1 ADDR W 7/7 00000000 E 00000001 FAIL',
        10: 'P2 ADDR W 7/7 00000000 E 00000000 PASS',
        21: '0000 00000001', 22: 'FFFF 00000002', 23: '5555 00000000',
        24: 'AAAA 00000000', 25: 'ADDR 00000000', 26: 'INVR 00000000',
    }
    result = qualify_rows(rows)
    assert result['state'] == 'PASS' and result['patterns']['FFFF'] == 2
    assert result['ports']['P1']['errors'] == 1
    broken = dict(rows)
    broken[3] = 'ERR not-a-row'
    try:
        qualify_rows(broken)
    except ValueError as error:
        assert 'ERR row' in str(error), error
    except AttributeError:
        raise AssertionError('a bad ERR row still crashes with AttributeError')
    else:
        raise AssertionError('a bad ERR row was accepted')
    print('PASS: a bad ERR row is a ValueError')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-check', action='store_true',
                        help='Check that a bad result row raises ValueError, then exit')
    parser.add_argument('--font', type=Path)
    parser.add_argument('--screenshot', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--pixel-threshold', type=int, default=100,
                        help='Maximum RGB channel threshold for lit pixels; ASUS capture uses 60')
    parser.add_argument('--inspect', action='store_true',
                        help='Emit raw decoded rows and glyph errors without qualifying a test result')
    args = parser.parse_args()
    if args.self_check:
        self_check()
        return
    if not args.font or not args.screenshot or not args.output:
        parser.error('--font, --screenshot and --output are required')
    if not 0 < args.pixel_threshold < 255:
        parser.error('--pixel-threshold must be between 1 and 254')
    png = args.screenshot.read_bytes()
    if png[:8] != b'\x89PNG\r\n\x1a\n' or struct.unpack('>II', png[16:24]) != (1280, 720):
        raise ValueError('Expected 1280x720 PNG capture; other scaling is not qualified')
    glyphs = {char: int(bits, 16) for char, bits in
              re.findall(r'"(.)": bitmap = 64\x27h([0-9A-F]+)', args.font.read_text())}
    glyphs[' '] = 0
    if not set('0123456789ABCDEF').issubset(glyphs):
        raise ValueError('Missing historical hex glyphs')
    rgb = subprocess.check_output(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', str(args.screenshot),
                                   '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'], timeout=20)
    if len(rgb) != 1280*720*3:
        raise ValueError('Unexpected decoded image size')

    def tile(x, y):
        bits = 0
        for row in range(8):
            for col in range(8):
                index = 3*((y+row*3+1)*1280+x+col*3+1)
                bits = (bits << 1) | (max(rgb[index:index+3]) > args.pixel_threshold)
        return bits

    score, x, y = min((sum((tile(x+24*col, y+24*row)^glyphs[char]).bit_count()
                           for row, text in [(0, 'SDRAM'), (7, 'HPS DDR'), (8, 'P0'), (9, 'P1'), (10, 'P2')]
                           for col, char in enumerate(text)), x, y)
                      for x in range(155, 177) for y in range(-1, 4))
    if score:
        raise ValueError('Header/font alignment is not exact')
    rows, errors = {}, {}
    for row in range(1, 28):
        text, mismatches = '', []
        for col in range((1280-x)//24):
            bits = tile(x+col*24, y+row*24)
            error, char = min(((bits^value).bit_count(), char) for char, value in glyphs.items())
            text += char if error <= 2 else '?'
            mismatches.append(error)
        rows[row], errors[row] = text.rstrip(), max(mismatches)
    critical = [1, 2, 3, 4, 5, 8, 9, 10, 20, 21, 22, 23, 24, 25, 26]
    if args.inspect:
        result = dict(classification='raw font inspection only; no qualified test result',
                      text_rows=rows, row_bit_errors=errors,
                      alignment=dict(x=x, y=y, scale=3, header_bit_errors=score),
                      pixel_threshold=args.pixel_threshold,
                      screenshot_sha256=hashlib.sha256(png).hexdigest(),
                      font_sha256=hashlib.sha256(args.font.read_bytes()).hexdigest(),
                      script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
        args.output.write_text(json.dumps(result, indent=2)+'\n')
        print(result['classification'])
        return
    if any(errors[row] for row in critical):
        raise ValueError('Timing/result glyphs are not exact; inspect the capture')
    qualified = qualify_rows(rows)
    result = dict(classification='HDMI observation decoded against original RTL glyphs', state=qualified['state'],
                  rate_mhz=100, completed_patterns=6, errors=int(qualified['total'], 16),
                  pattern_errors=qualified['patterns'],
                  first_fault_hex=qualified['first'], last_fault_hex=qualified['last'],
                  first_got_hex=qualified['was'], hps_ddr=qualified['ports'],
                  alignment=dict(x=x, y=y, scale=3, header_bit_errors=score),
                  pixel_threshold=args.pixel_threshold,
                  critical_glyph_bit_errors=0, text_rows=rows, row_bit_errors=errors,
                  screenshot_sha256=hashlib.sha256(png).hexdigest(),
                  font_sha256=hashlib.sha256(args.font.read_bytes()).hexdigest(),
                  script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({key: result[key] for key in ['state', 'errors', 'pattern_errors', 'first_fault_hex', 'last_fault_hex']}))


if __name__ == '__main__':
    main()
