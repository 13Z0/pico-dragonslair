#!/usr/bin/env bash
# Build the host harnesses:
#   hosttest/dl_host    - the Dragon's Lair core (machine, laserdisc, decoder, ROM loader)
#   hosttest/pld_check  - checks a lair.pld file: every frame, in order and by seeking
#   hosttest/zex_host   - the Z80 core on the ZEXDOC/ZEXALL instruction tests
#   tools/ld2pico/build/ld2pico - the laserdisc converter, command line
#
# The core in dlair/ has no Pico dependencies; this compiles it for Linux
# with ASan. Drop -fsanitize=address (and use -O2) for timing measurements:
#   OPT="-O2" hosttest/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OPT=${OPT:--O1 -g -fsanitize=address -fno-omit-frame-pointer}
CFLAGS=(
  $OPT -std=c11 -Wall -Wextra -Wno-unused-parameter
  -D_DEFAULT_SOURCE
  -I dlair -I third_party/miniz
  -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_DEFLATE_APIS
  -DMINIZ_NO_ZLIB_APIS -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
)

CORE=(
  dlair/z80.c
  dlair/ay8910.c
  dlair/dl_machine.c
  dlair/ldv1000.c
  dlair/ldplayer.c
  dlair/ldstream.c
  dlair/pld.c
  dlair/scoreboard.c
  dlair/romload.c
  third_party/miniz/miniz.c
)

gcc "${CFLAGS[@]}" hosttest/dl_host.c "${CORE[@]}" -lm -o hosttest/dl_host
echo "built hosttest/dl_host"
if [ -f hosttest/pld_check.c ]; then
  gcc "${CFLAGS[@]}" hosttest/pld_check.c dlair/ldstream.c dlair/pld.c third_party/miniz/miniz.c -o hosttest/pld_check
  echo "built hosttest/pld_check"
fi
gcc "${CFLAGS[@]}" hosttest/zex_host.c dlair/z80.c -o hosttest/zex_host
echo "built hosttest/zex_host"
tools/ld2pico/build.sh native
