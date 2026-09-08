#!/usr/bin/env python3
"""Emit a shell probe for true_dual_port_mixed.v; run under an exclusive kit lease."""
import argparse


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unit', type=int, choices=(8, 10), required=True)
p.add_argument('--a-lanes', type=int, choices=(1, 2), required=True)
p.add_argument('--b-lanes', type=int, choices=(1, 2), required=True)
a = p.parse_args()
lanes = (a.a_lanes, a.b_lanes)
mask = (1 << a.unit) - 1
memory = [((i * 73) ^ (i >> 1) ^ 0xa6) & mask for i in range(1024)]
addresses, seeds = [0, 0], [0, 0]
last_gp_word = 0
print('''#!/bin/sh
set -eu
put() { busybox devmem 0xFF706010 32 "$1"; sleep 0.02; }
check() {
 actual=$(busybox devmem 0xFF706014 32)
 expected=$((0xd7160000 | $1))
 [ "$((actual))" -eq "$expected" ] || {
  echo "FAIL: expected=$expected GPI=$actual"; exit 1;
 }
}
''')


def emit(word):
    global last_gp_word
    print(f'put {word}')
    last_gp_word = word


def put(port, enable=0, write=0, window=0):
    word = addresses[port] | (seeds[port] << 16) | (port << 26)
    word |= (1 << 14) | (window << 27) | (enable << 28) | (write << 30)
    controls, selector, payload = 0xf0000000, 1 << 26, 0x03ff03ff
    if (last_gp_word ^ word) & (selector | payload):
        # The departing port captures the GP bus continuously. First stop
        # both ports without changing their payload; then switch only the
        # selector. Otherwise selector skew can overwrite its staged address.
        if last_gp_word & controls:
            emit(last_gp_word & ~controls)
        if (last_gp_word ^ word) & selector:
            emit(last_gp_word ^ selector)
        # Settle the new port's address/data before any enable or write.
        emit(word & ~controls)
    emit(word)


def value(port, address):
    return sum(memory[address * lanes[port] + i] << (a.unit * i)
               for i in range(lanes[port]))


def data(port):
    return [((seeds[port] ^ ((i + 2 * port) * 0x93)) & mask)
            for i in range(lanes[port])]


def check(port, expected, enable=0, write=0):
    for window in range((a.unit * lanes[port] + 15) // 16):
        put(port, enable, write, window)
        print(f'check {(expected >> (16 * window)) & 65535}')


def read(port, address):
    # Remove WE before changing the staged address or data on the GP bus.
    put(port)
    addresses[port] = address
    put(port)
    check(port, value(port, address), enable=1 << port)
    put(port)


def update(port):
    start = addresses[port] * lanes[port]
    memory[start:start + lanes[port]] = data(port)


def read_region(start, stop):
    for port in (0, 1):
        for address in range(start // lanes[port], (stop - 1) // lanes[port] + 1):
            read(port, address)


for port in (0, 1):
    for address in (0, 1, 7, 31, 1024 // lanes[port] - 1):
        read(port, address)
print("echo 'PASS: initialized words through both port widths'")
emit(0x13579bdf)

# Check every narrow slice after each write, including the unmodified half
# of a wide word and adjacent words. The other port is disabled during writes.
for port in (0, 1):
    for base in (0, 32, 1020):
        for offset in range(0, 4, lanes[port]):
            put(port)
            addresses[port] = (base + offset) // lanes[port]
            seeds[port] = (0x155 + 0x37 * offset + 0x21 * port) & mask
            put(port)
            expected = sum(v << (a.unit * i) for i, v in enumerate(data(port)))
            check(port, expected, enable=1 << port, write=1 << port)
            put(port)
            update(port)
            read_region(max(0, base - 2), min(1024, base + 6))
print("echo 'PASS: both writers, NEW_DATA, cross-width readback and preserved neighbors'")

# Stage each port separately, then enable both at disjoint physical words.
for port in (0, 1):
    put(port)
    addresses[port] = (64 + 8 * port) // lanes[port]
    seeds[port] = (0x321 - 0x59 * port) & mask
    put(port)
put(0, enable=3, write=3)
put(0)
for port in (0, 1):
    update(port)
read_region(62, 76)
print("echo 'PASS: simultaneous disjoint writes and neighboring storage'")

for port in (0, 1):
    read(port, 0)
    held = value(port, 0)
    addresses[port] = 96 // lanes[port]
    seeds[port] = (0x2aa - port) & mask
    put(port)
    check(port, held, enable=0, write=1 << port)
    put(port)
    read_region(94, 100)
print("echo 'PASS: clock enable holds output and suppresses writes on both ports'")
