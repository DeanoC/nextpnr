#!/bin/sh
# Run under a kit.py lease with the matching WIDTH of dual_clock.v loaded.
set -eu
width=${1:-20}
case "$width" in 20|40) ;; *) echo 'Usage: probe.sh [20|40]' >&2; exit 2;; esac
write_gpo() { busybox devmem 0xFF706010 32 "$1"; }
check() {
    sleep 0.02
    status=$(busybox devmem 0xFF706014 32)
    [ "$(((status >> 16) & 65535))" -eq 54289 ] || {
        echo "FAIL: missing M10K signature $status" >&2; exit 1;
    }
    actual=$((status & 65535))
    [ "$actual" -eq "$1" ] || {
        echo "FAIL: expected=$1 actual=$actual status=$status" >&2; exit 1;
    }
}
full_word() {
    word=$(($1 & 1048575))
    if [ "$width" -eq 40 ]; then word=$(((((~word) & 1048575) << 20) | word)); fi
}
# Select every 16-bit output window with writes disabled. This also covers
# bits 39:20, which use the second physical data group in 40-bit mode.
read_word() {
    window=0
    while [ "$((window * 16))" -lt "$width" ]; do
        write_gpo "$(($1 | (window << 27)))"
        check "$((($2 >> (window * 16)) & 65535))"
        window=$((window + 1))
    done
}
for address in 0 1 2 3 7 15 31 63 127 255; do
    full_word "$(((address * 73) ^ (address >> 1) ^ 166))"
    read_word "$((0x60000000 | address))" "$word"
done
echo "PASS: all $width data bits initialized, 50 MHz write / 25 MHz read clocks"
write_gpo $((0x13579bdf))
sleep 0.02
full_word 239
old_word=$word
full_word $((0xa5a3c))
new_word=$word
read_word $((0x60000001)) "$old_word"
read_word $((0x40000001)) "$old_word"
# Stop only the read clock; write a value that exercises the high data bits.
write_gpo $((0xc0000007 | (0xa5a3c << 9)))
sleep 0.02
read_word $((0x40000007)) "$old_word"
read_word $((0x60000007)) "$new_word"
echo 'PASS: read clock stopped while write clock updates memory; resume reads new value'
read_word $((0x60000001)) "$old_word"
read_word $((0x20000007)) "$old_word"
read_word $((0x60000007)) "$new_word"
# A changed data bus without write enable must leave memory unchanged.
write_gpo $((0x60000007 | (0x1234 << 9)))
sleep 0.02
read_word $((0x60000007)) "$new_word"
echo 'PASS: read-enable hold/resume and write-enable hold'
