#!/bin/sh
# Run on the designated target only while holding its kit.py lease and after
# loading diagnostic/top.rbf. This script does not program or claim hardware.
set -eu
read_gpi() { busybox devmem 0xFF706014 32; }
write_gpo() { busybox devmem 0xFF706010 32 "$1"; }
check_signature() {
    if [ "$((($1 >> 16) & 65535))" -ne 55057 ]; then # 0xD711
        echo "FAIL: PLL diagnostic signature missing: $1" >&2
        exit 1
    fi
}
for trial in 1 2 3; do
    initial=$(read_gpi)
    check_signature "$initial"
    if [ "$((initial & 32768))" -ne 0 ]; then
        echo "FAIL: a measurement is already active" >&2
        exit 1
    fi
    request=$((1 - ((initial >> 14) & 1)))
    previous=$(busybox devmem 0xFF706010 32)
    command=$(((previous & ~3) | (request << 1)))
    write_gpo "$command"
    polls=0
    while :; do
        status=$(read_gpi)
        check_signature "$status"
        if [ "$((status & 32768))" -eq 0 ] && [ "$(((status >> 14) & 1))" -eq "$request" ]; then
            break
        fi
        polls=$((polls + 1))
        if [ "$polls" -ge 50 ]; then
            echo "FAIL: measurement timeout: $status" >&2
            exit 1
        fi
        sleep 0.02
    done
    low=$((status & 255))
    write_gpo "$((command | 1))"
    sleep 0.02
    high=$(read_gpi)
    check_signature "$high"
    if [ "$((high & 65280))" -ne "$((status & 65280))" ]; then
        echo "FAIL: status changed during snapshot read" >&2
        exit 1
    fi
    count=$((low | ((high & 255) << 8)))
    if [ "$((status & 12288))" -ne 8192 ] || [ "$count" -lt 2047 ] || [ "$count" -gt 2049 ]; then
        echo "FAIL: trial=$trial count=$count status=$status (expected 2048 +/- 1, locked, no sampled lock loss)" >&2
        exit 1
    fi
    echo "PASS: trial=$trial count=$count status=$status high=$high"
done
