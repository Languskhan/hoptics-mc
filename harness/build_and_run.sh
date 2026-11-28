#!/usr/bin/env bash
# Builds the Hoptics-MC screens for the PC and renders them: landscape (T-Deck Plus, M9) and portrait (Heltec V4 kits).
# Needs LVGL 9.6 sources: LVGL=/path/to/lvgl (default: fetched into build/lvgl).
set -euo pipefail
cd "$(dirname "$0")"
B=build; mkdir -p $B
L=${LVGL:-$B/lvgl}
[ -d "$L/src" ] || git clone -q --depth 1 -b v9.6.0 https://github.com/lvgl/lvgl.git "$L"
DEFS="-DLV_CONF_INCLUDE_SIMPLE"
INC="-I../hoptics/config -I../hoptics/ui -I$L -I$L/src"
# LVGL itself: built once, again when lv_conf.h changes
if [ ! -f $B/liblvgl.a ] || [ ../hoptics/config/lv_conf.h -nt $B/liblvgl.a ]; then
  rm -f $B/obj/*.o $B/liblvgl.a
  find "$L/src" -name '*.c' | while read f; do o=$B/obj/$(echo "$f" | md5sum | cut -c1-12).o; mkdir -p $B/obj; [ -f $o ] || gcc -c -O1 $DEFS $INC "$f" -o $o; done
  ar rcs $B/liblvgl.a $B/obj/*.o
fi
# the emoji fonts (large tables: compiled once)
for f in ../hoptics/ui/fonts/*.c; do o=$B/$(basename "$f" .c).o; [ "$o" -nt "$f" ] || gcc -c -O1 $DEFS $INC "$f" -o "$o"; done
# the boot pictures, one set per orientation (made once)
for O in land port; do
  A=$([ $O = land ] && echo _320x240 || echo "")
  mkdir -p $B/boot_$O
  [ -f $B/boot_$O/hx_boot_bg.h ] || python3 ../tools/png_to_lvgl.py $B/boot_$O/hx_boot_bg.h ../assets/boot_bg_ridgelines$A.png \
    ../assets/boot_bg_constellation$A.png ../assets/boot_bg_aurora$A.png > /dev/null
done
# PNGdec (Apache-2.0) for the street map's tile engine, and a set of made-up test tiles around Sacramento
P=$B/PNGdec
[ -d "$P/src" ] || git clone -q --depth 1 -b 1.1.7 https://github.com/bitbank2/PNGdec.git "$P"
if [ ! -f $B/libpngdec.a ]; then
  for f in "$P"/src/*.c "$P"/src/*.cpp; do g++ -x $( [ "${f##*.}" = c ] && echo c || echo c++ ) -c -O1 -w -D__LINUX__ -I"$P/src" "$f" -o $B/png_$(basename "$f").o; done
  ar rcs $B/libpngdec.a $B/png_*.o
fi
[ -d $B/tiles/10 ] || python3 make_test_tiles.py $B/tiles
INC="$INC -I$P/src -I../hoptics/ui/map"
DEFS="$DEFS -D__LINUX__" # PNGdec on a PC
CXX="g++ -std=c++17 -O1 -Wall -Wno-unused-function -Wno-deprecated-declarations $DEFS $INC"
$CXX -c ../hoptics/ui/hx_ui.cpp -o $B/hx_ui.o
$CXX -c harness.cpp -o $B/harness.o
$CXX -c ../hoptics/ui/map/hx_tiles.cpp -o $B/hx_tiles.o
for O in land port; do
  $CXX -I$B/boot_$O -c ../hoptics/ui/hx_bootscreen.cpp -o $B/boot_$O.o
  g++ $B/hx_ui.o $B/harness.o $B/hx_tiles.o $B/boot_$O.o $B/hx_emoji_*.o $B/hx_text_*.o $B/hx_sym_*.o $B/liblvgl.a $B/libpngdec.a -lm -pthread -o $B/harness_$O
done
rm -rf $B/tc_land $B/tc_port # saved tiles from the last run
mkdir -p shots/320x240 shots/240x320
$B/harness_land 320 240 shots/320x240 > /dev/null
$B/harness_port 240 320 shots/240x320 > /dev/null
echo done
