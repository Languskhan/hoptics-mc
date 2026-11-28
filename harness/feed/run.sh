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
for mode in ok moved refuse overflow huge secure downgrade; do
  url=ws://127.0.0.1:8765/ws
  case $mode in secure|downgrade) url=wss://127.0.0.1:8765/ws ;; esac
  timeout 30 python3 -u server.py $mode > $B/server.log 2>&1 &
  sleep 0.5
  timeout 20 $B/ws $url 3 > $B/client.log 2>&1 || true
  wait
  echo "== $mode"; cat $B/server.log $B/client.log
  if grep -q "ERROR: AddressSanitizer\|runtime error:" $B/client.log; then echo "FAIL $mode: sanitizer report"; exit 1; fi
  case $mode in
    ok|moved|secure) grep -q "PACKETS [1-9]" $B/client.log || { echo "FAIL $mode: no packets"; exit 1; } ;;
    overflow) grep -q "bad frame" $B/client.log || { echo "FAIL overflow: the bad frame wasn't refused"; exit 1; } ;;
    downgrade) ! grep -q FOLLOWED $B/server.log && grep -q "insecure address" $B/client.log ||
      { echo "FAIL downgrade: the move off TLS wasn't refused"; exit 1; } ;;
  esac
done
for what in lock task ring msg; do # each allocation failing: the feed stays off and says why, with no server needed
  FEED_NOMEM=$what timeout 20 $B/ws ws://127.0.0.1:8765/ws 2 > $B/client.log 2>&1 || true
  echo "== no memory for $what"; cat $B/client.log
  if grep -q "ERROR: AddressSanitizer\|runtime error:" $B/client.log; then echo "FAIL no memory for $what: sanitizer report"; exit 1; fi
  if ! grep -q "STATE 4 err='Not enough memory for the feed'" $B/client.log; then echo "FAIL no memory for $what: no error shown"; exit 1; fi
done
echo "PASS"
