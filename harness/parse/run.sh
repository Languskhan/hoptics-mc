#!/bin/bash
# The web server's text helpers (paths on the card, %-escapes, JSON) on a PC, under ASan/UBSan. Run from anywhere.
set -e
cd "$(dirname "$0")"
B=$(mktemp -d)
g++ -std=gnu++11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -g -I../../hoptics/net parse_test.cpp ../../hoptics/net/hx_parse.cpp -o $B/parse_test
$B/parse_test
