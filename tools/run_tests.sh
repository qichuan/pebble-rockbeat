#!/bin/sh
# Builds and runs the host-side judgment tests.
#
# These compile game.c and chart.c with the system compiler, not the ARM
# toolchain -- which only works because neither file touches a Pebble API. Keep
# it that way: the moment game.c calls time_ms(), persist_* or graphics_*, this
# stops building and the judgment windows go back to being testable only by
# hand, on an emulator, one button press at a time.
#
# Note there is no stub pebble.h here, unlike the sibling projects. game.h and
# chart.h include only stdint/stdbool/stddef, so the host compiler needs nothing
# faked at all.
set -e

cd "$(dirname "$0")"

# Every RB_DEBUG_* flag must be at its off value, checked before anything else
# so it fails fast. These get flipped constantly while capturing screenshots and
# the gameplay GIF, and one left on is INVISIBLE: a stray AUTOSTART skips the
# title screen, TIME_SCALE runs the song in slow motion, and the build compiles,
# installs and looks exactly like a good one. It has already happened -- the
# emulator ran the capture build for half an hour before two screenshots taken
# 12s apart showed the combo advancing by one note instead of ~47.
#
# TIME_SCALE is a divisor, so its off value is 1; every other flag is 0. The
# `|| true` is load-bearing under `set -e`: grep exits non-zero when it matches
# nothing, which is the case for a file with no flags at all.
BAD=$(grep -E '^#define RB_DEBUG_[A-Z_]+ +[0-9]+' ../watch/src/c/rb_config.h |
      awk '$3 != 0 && !($2 == "RB_DEBUG_TIME_SCALE" && $3 == 1)' || true)
if [ -n "$BAD" ]; then
  echo "FAIL: debug flag left on --"
  echo "$BAD"
  exit 1
fi

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -Wall -Wextra -Werror -O1 \
   -I../watch/src/c \
   game_test.c ../watch/src/c/game.c ../watch/src/c/chart.c \
   -o "$OUT/game_test"

"$OUT/game_test"
