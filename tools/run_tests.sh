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
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -Wall -Wextra -Werror -O1 \
   -I../watch/src/c \
   game_test.c ../watch/src/c/game.c ../watch/src/c/chart.c \
   -o "$OUT/game_test"

"$OUT/game_test"
