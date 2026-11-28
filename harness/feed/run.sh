#!/bin/bash
# The live feed on a PC: its message parser, then its WebSocket client (the ESP32 code, on stand-in headers with
# real sockets) against server.py, a stand-in for CoreScope's /ws. Run from anywhere.
set -e
cd "$(dirname "$0")"
N=../../hoptics/net
H=../../hoptics/hal
B=$(mktemp -d)
g++ -std=gnu++11 -Wall -fsanitize=address,undefined -g -I$N -I$H parse_test.cpp $N/hx_feed.cpp -o $B/parse_test
$B/parse_test
g++ -std=gnu++11 -Wall -fsanitize=address,undefined -g -DARDUINO_ARCH_ESP32 -Istubs -I$N -I$H main.cpp stubs_impl.cpp $N/hx_feed.cpp -pthread -o $B/ws
for mode in ok moved refuse overflow huge; do
  timeout 30 python3 server.py $mode > $B/server.log 2>&1 &
  sleep 0.5
  timeout 20 $B/ws ws://127.0.0.1:8765/ws 3 > $B/client.log 2>&1 || true
  wait
  echo "== $mode"; cat $B/server.log $B/client.log
  if grep -q "ERROR: AddressSanitizer\|runtime error:" $B/client.log; then echo "FAIL $mode: sanitizer report"; exit 1; fi
  if [ $mode = overflow ] && ! grep -q "bad frame" $B/client.log; then echo "FAIL overflow: the bad frame wasn't refused"; exit 1; fi
done
