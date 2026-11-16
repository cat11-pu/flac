#!/bin/bash
# Run harness on all malformed files; tally exit codes & sanitizer crashes.
HARNESS=${HARNESS:-fuzz/decode_harness}
DIR=${1:-fuzz/malformed}
declare -A tally
crashes=0
total=0
> fuzz/crash_list.txt
for f in "$DIR"/*.flac; do
    total=$((total+1))
    out=$(ASAN_OPTIONS=abort_on_error=0:exitcode=99 UBSAN_OPTIONS=halt_on_error=1:exitcode=98 timeout 10 "$HARNESS" "$f" 2>&1)
    rc=$?
    tally[$rc]=$(( ${tally[$rc]:-0} + 1 ))
    if [ $rc -ge 90 ] || [ $rc -eq 134 ] || [ $rc -eq 139 ] || [ $rc -eq 136 ] || [ $rc -eq 132 ] || [ $rc -eq 124 ]; then
        crashes=$((crashes+1))
        echo "=== CRASH rc=$rc $f" >> fuzz/crash_list.txt
        echo "$out" | head -25 >> fuzz/crash_list.txt
    fi
done
echo "TOTAL $total  CRASHES $crashes"
for k in "${!tally[@]}"; do echo "  exit $k : ${tally[$k]}"; done
