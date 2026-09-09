#!/usr/bin/env python3
"""Emit a target-side shell probe for mixed_width.v (run under a kit.py lease)."""
import argparse

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--unit',type=int,choices=(8,10),default=10)
p.add_argument('--write-lanes',type=int,choices=(1,2,4),required=True)
p.add_argument('--read-lanes',type=int,choices=(1,2,4),required=True)
a=p.parse_args()
u,w,r=a.unit,a.write_lanes,a.read_lanes
mask=(1<<u)-1
memory=[((i*73)^(i>>1)^0xa6)&mask for i in range(1024)]
print('''#!/bin/sh
set -eu
put() { busybox devmem 0xFF706010 32 "$1"; sleep 0.02; }
check() {
 actual=$(busybox devmem 0xFF706014 32)
 [ "$((actual))" -eq "$((0xd7130000 | $1))" ] || {
  echo "FAIL: expected=$1 GPI=$actual"; exit 1;
 }
}
''')

def read(addr):
    value=sum(memory[addr*r+i]<<(u*i) for i in range(r))
    for win in range((u*r+15)//16):
        print(f'put {0x60000000 | addr | (win<<26)}')
        print(f'check {(value>>(16*win))&65535}')

for addr in (0,1,7,31,1024//r-1):
    read(addr)
print("echo 'PASS: initialized mixed-width words and read windows'")
print('put 0x13579bdf')
# Whole-word writes and individually addressed narrow lanes, including both
# ends of memory. Read every wide word touched after stopping the write.
for base in (32,1024-max(w,r)):
    for lane in range(max(w,r)//w):
        addr=base//w+lane
        seed=(0x155+lane*0x27)&mask
        # Settle asynchronous GP address/data before asserting WE; deassert it
        # before changing address again to avoid incidental writes during skew.
        command = 0x40000000 | (seed << 16) | addr
        print(f'put {command}')
        print(f'put {command | 0x80000000}')
        print(f'put {command}')
        for i in range(w): memory[addr*w+i]=(seed^(i*0x93))&mask
        for readaddr in range(base//r,(base+max(w,r))//r): read(readaddr)
print("echo 'PASS: writes with stopped read clock, lane order and neighboring lanes'")
# Read-enable hold: choose a new address with read clock running but RE low.
read(0)
print('put 0x20000007')
# Select window zero while the read address remains held at zero.
print('put 0x20000007')
print(f'check {sum(memory[i]<<(u*i) for i in range(r)) & 65535}')
print("echo 'PASS: read-enable hold'")
