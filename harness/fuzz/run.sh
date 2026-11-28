#!/bin/bash
# Fuzzes the parsers that read what strangers send (the web server's JSON and paths, the live feed's messages) with
# libFuzzer under ASan/UBSan, for N seconds each (default 60). Exits non-zero on the first crash, leak or undefined
# behaviour, with the input that caused it in the log. Needs clang. Run from anywhere.
set -e
cd "$(dirname "$0")"
T=${1:-60}
N=../../hoptics/net
B=$(mktemp -d)
F="-g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -I$N -I../../hoptics/hal"
clang++ -std=gnu++14 $F fuzz_parse.cpp $N/hx_parse.cpp -o $B/fuzz_parse
clang++ -std=gnu++14 $F fuzz_feed.cpp $N/hx_feed.cpp -o $B/fuzz_feed
for t in fuzz_parse fuzz_feed; do
  mkdir -p $B/corpus_$t
  echo "== $t ($T s)"
  (cd $B && ./$t -max_total_time=$T -max_len=4096 -print_final_stats=1 corpus_$t 2>&1 | tail -6) || { echo "FAIL $t"; exit 1; }
done
echo PASS
