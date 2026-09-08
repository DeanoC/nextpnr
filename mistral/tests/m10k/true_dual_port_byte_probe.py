#!/usr/bin/env python3
"""Emit a target shell probe for true_dual_port_byte.v; use an exclusive kit lease."""
import argparse

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--width', type=int, choices=(16, 20), required=True)
p.add_argument('--raw', action='store_true', help='Check standalone primitive write outputs')
a = p.parse_args()
mask = (1 << a.width) - 1
lane = a.width // 2
lane_mask = (1 << lane) - 1
memory = [((i * 0x9e3779b9) ^ 0x65acbd) & mask for i in range(512)]
addresses, seeds, masks = [0, 0], [0, 0], [3, 3]
print('''#!/bin/sh
set -eu
put() { busybox devmem 0xFF706010 32 "$1"; sleep 0.02; }
check() {
 actual=$(busybox devmem 0xFF706014 32)
 care=$((0xffff0000 | $2))
 expected=$((0xd7150000 | ($1 & $2)))
 [ "$((actual & care))" -eq "$expected" ] || {
  echo "FAIL: expected=$expected care=$care GPI=$actual"; exit 1;
 }
}
''')


def put(port, enable=0, write=0, window=0):
    word = addresses[port] | (seeds[port] << 16) | (port << 26)
    word |= (masks[0] << 10) | (masks[1] << 12) | (1 << 14)
    word |= (window << 27) | (enable << 28) | (write << 30)
    print(f'put {word}')


def check(port, value, enable=0, write=0, care=None):
    for window in range((a.width + 15) // 16):
        put(port, enable, write, window)
        bits = 65535 if care is None else (care >> (16 * window)) & 65535
        print(f'check {(value >> (16 * window)) & 65535} {bits}')


def read(port, address):
    put(port)
    addresses[port] = address
    put(port)
    check(port, memory[address], 1 << port)
    put(port)


def written_bits(port):
    return sum(lane_mask << (lane * i) for i in (0, 1) if masks[port] & (1 << i))


def data(port):
    return (seeds[port] ^ (0x93a00 if port == 0 else 0x2bc00)) & mask


def update(port):
    bits = written_bits(port)
    address = addresses[port]
    memory[address] = (memory[address] & ~bits) | (data(port) & bits)


for port in (0, 1):
    for address in (0, 1, 7, 31, 511):
        read(port, address)
print("echo 'PASS: initialized reads through both ports'")
print('put 0x13579bdf')
for port in (0, 1):
    for address in (0, 7, 511):
        for byte_mask, seed in ((1, 0x155), (2, 0x2aa), (0, 0x3ff), (3, 0x123)):
            read(port, address)
            held = memory[address]
            addresses[port], seeds[port], masks[port] = address, seed, byte_mask
            put(port)
            # The inferred RTL holds Q during writes. Its mapped output-hold
            # logic hides the primitive's unspecified disabled-byte write Q.
            if a.raw:
                check(port, data(port), 1 << port, 1 << port, written_bits(port))
            else:
                check(port, held, 1 << port, 1 << port)
            put(port)
            update(port)
            read(port, address)
            read(1 - port, address)
print("echo 'PASS: low/high/zero/full masks on both writers and preserved storage'")
print("echo 'PASS: enabled-byte NEW_DATA'" if a.raw else "echo 'PASS: inferred output hold during writes'")
for port in (0, 1):
    addresses[port], seeds[port], masks[port] = 20 + port, 0x321 - port, 1 << port
    put(port)
put(0, enable=3, write=3)
put(0)
for port in (0, 1):
    update(port)
for port in (0, 1):
    read(port, 20 + port)
    read(1 - port, 20 + port)
print("echo 'PASS: simultaneous disjoint writes with different masks'")
for port in (0, 1):
    read(port, 0)
    held = memory[0]
    addresses[port], seeds[port], masks[port] = 31, 0x2aa, 3
    check(port, held, enable=0, write=1 << port)
    put(port)
    read(port, 31)
print("echo 'PASS: clock enable holds output and suppresses masked writes'")
