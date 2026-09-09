#!/usr/bin/env python3
"""Emit a target shell probe for true_dual_port.v; use an exclusive kit lease."""
import argparse

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--width', type=int, choices=(8, 10, 16, 20), required=True)
a = p.parse_args()
mask = (1 << a.width) - 1
depth = 512 if a.width > 10 else 1024
memory = [((i * 0x9e3779b9) ^ 0x65acbd) & mask for i in range(depth)]
addresses = [0, 0]
seeds = [0, 0]
print('''#!/bin/sh
set -eu
put() { busybox devmem 0xFF706010 32 "$1"; sleep 0.02; }
check() {
 actual=$(busybox devmem 0xFF706014 32)
 [ "$((actual))" -eq "$((0xd7140000 | $1))" ] || {
  echo "FAIL: expected=$1 GPI=$actual"; exit 1;
 }
}
''')


def put(port, enable=0, write=0, window=0):
    # Keep stored address/data stable while raising and lowering write enable.
    word = addresses[port] | (seeds[port] << 16) | (port << 26)
    word |= (1 << 14) | (window << 27) | (enable << 28) | (write << 30)
    print(f'put {word}')


def check(port, value, enable=0, write=0):
    for window in range((a.width + 15) // 16):
        put(port, enable, write, window)
        print(f'check {(value >> (16 * window)) & 65535}')


def read(port, address):
    put(port)  # Stop any preceding operation before changing the address.
    addresses[port] = address
    put(port)  # Let the asynchronous GP address settle before enabling RAM.
    check(port, memory[address], 1 << port)
    put(port)


for port in (0, 1):
    for address in (0, 1, 7, 31, depth - 1):
        read(port, address)
print("echo 'PASS: initialized reads through both ports'")
print('put 0x13579bdf')
for port in (0, 1):
    for address, seed in ((0, 0), (7, 0x155), (depth - 1, 0x3ff)):
        put(port)
        addresses[port], seeds[port] = address, seed
        put(port)
        value = (seed ^ (0x93a00 if port == 0 else 0x2bc00)) & mask
        check(port, value, 1 << port, 1 << port)
        put(port)  # Deassert WE before selecting the opposite port.
        memory[address] = value
        read(port, address)
        read(1 - port, address)
print("echo 'PASS: both writers, own-port new data and opposite-port readback'")
# Concurrent writes use distinct, pre-settled addresses.
for port in (0, 1):
    addresses[port], seeds[port] = 20 + port, 0x123 + port
    put(port)
put(0, enable=3, write=3)
put(0)
for port in (0, 1):
    memory[20 + port] = (seeds[port] ^ (0x93a00 if port == 0 else 0x2bc00)) & mask
    read(port, 20 + port)
    read(1 - port, 20 + port)
print("echo 'PASS: simultaneous disjoint writes'")
for port in (0, 1):
    read(port, 0)
    held = memory[0]
    addresses[port], seeds[port] = 31, 0x2aa
    check(port, held, enable=0, write=1 << port)
    put(port)
    read(port, 31)
print("echo 'PASS: clock enable holds output and suppresses writes'")
