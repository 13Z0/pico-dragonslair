#!/usr/bin/env bash
# Copies the pictures tools/mkartwork.py lays out from your laserdisc image:
#
#   tools/ld2pico/build.sh native
#   tools/capture_footage.sh ~/roms/arcade/DLAIR/dlair.chd /tmp/dl_footage
#
# Each is a 320x240 PPM of one picture number (as the firmware shows it). To
# change a part of the artwork, copy another picture over its name:
#   tools/ld2pico/build/ld2pico dlair.chd --frame <picture> --ppm /tmp/dl_footage/left.ppm
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 <dlair.chd> <outdir>" >&2; exit 1; }
chd=$1
out=$2
ld2pico="$(dirname "$0")/ld2pico/build/ld2pico"
[ -x "$ld2pico" ] || { echo "build $ld2pico first: tools/ld2pico/build.sh native" >&2; exit 1; }
mkdir -p "$out"

grab() { "$ld2pico" "$chd" --frame "$1" --ppm "$out/$2.ppm"; echo "picture $1 -> $out/$2.ppm"; }

grab 360   screensaver   # the title
grab 480   badge         # the castle
grab 4500  left          # Dirk in the dungeon
grab 30500 middle        # Singe the dragon
grab 2500  right         # Dirk at the stairs
