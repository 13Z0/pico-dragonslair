#!/usr/bin/env bash
# Builds ld2pico, the laserdisc converter:
#
#   tools/ld2pico/build.sh           ->  build/ld2pico      (this system, command line)
#                                        build/ld2pico.exe  (Windows, with a window)
#   tools/ld2pico/build.sh native    ->  only build/ld2pico
#   tools/ld2pico/build.sh windows   ->  only build/ld2pico.exe
#
# ld2pico.exe needs x86_64-w64-mingw32-gcc and x86_64-w64-mingw32-windres
# (Debian/Ubuntu: apt install mingw-w64). It links only Windows' own DLLs, so
# the exe is all a user needs.
set -euo pipefail
cd "$(dirname "$0")"
what=${1:-all}

LIBCHDR=third_party/libchdr
CORE=(ldp_core.c ldp_chd.c)
CHDR=($LIBCHDR/src/libchdr_bitstream.c $LIBCHDR/src/libchdr_huffman.c $LIBCHDR/src/libchdr_flac.c)

# The version is the repository's: v0.1-3-gabc1234 gives 0,1,0,0.
VERSION=$(git describe --tags --always --dirty 2>/dev/null || echo dev)
if [[ $VERSION =~ ^v?([0-9]+)\.([0-9]+)(\.([0-9]+))? ]]; then
    NUM="${BASH_REMATCH[1]},${BASH_REMATCH[2]},${BASH_REMATCH[4]:-0},0"
else
    NUM="0,0,0,0"
fi

mkdir -p build
cat > build/version.h <<EOF
#define LDP_VERSION "$VERSION"
#define LDP_VERSION_NUM $NUM
EOF

if [[ $what == all || $what == native ]]; then
    gcc -std=gnu11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -I $LIBCHDR/include \
        ldp_cli.c ldp_posix.c "${CORE[@]}" "${CHDR[@]}" -lm -lpthread -o build/ld2pico 2> build/native.log ||
        { cat build/native.log; exit 1; }
    echo "built $(pwd)/build/ld2pico"
fi

if [[ $what == all || $what == windows ]]; then
    CROSS=${CROSS:-x86_64-w64-mingw32-}
    CC="${CROSS}gcc"
    WINDRES="${CROSS}windres"
    for tool in "$CC" "$WINDRES"; do
        command -v "$tool" >/dev/null || { echo "ERROR: $tool not found (apt install mingw-w64)" >&2; exit 1; }
    done
    CFLAGS="-std=gnu11 -O2 -ffunction-sections -fdata-sections -I $LIBCHDR/include"
    "$WINDRES" -I build -O coff ld2pico.rc -o build/ld2pico.res
    objs=()
    for src in "${CHDR[@]}"; do
        o=build/win_$(basename "${src%.c}").o
        "$CC" $CFLAGS -w -c "$src" -o "$o"
        objs+=("$o")
    done
    for src in "${CORE[@]}" ldp_win32.c ldp_gui.c; do
        o=build/win_${src%.c}.o
        "$CC" $CFLAGS -Wall -Wextra -municode -include build/version.h -c "$src" -o "$o"
        objs+=("$o")
    done
    "$CC" -municode -mwindows -static -s -Wl,--gc-sections -o build/ld2pico.exe "${objs[@]}" build/ld2pico.res \
        -lcomctl32 -lole32 -lshell32 -luuid
    echo "built $(pwd)/build/ld2pico.exe ($VERSION, $(stat -c%s build/ld2pico.exe) bytes)"
fi
