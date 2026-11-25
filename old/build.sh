#!/bin/sh
# build.sh - build MT7921.OS2 (OpenWatcom) and wrndtest.exe (gcc)
#
#   sh build.sh          build everything into ./out
#   sh build.sh clean    remove build output

GREEN='\033[1;32m'; RED='\033[1;31m'; CYAN='\033[1;36m'; NC='\033[0m'
TOP=$(pwd)
LOG="$TOP/build.log"
START=$(date +%s)

say()  { printf "${CYAN}==> %s${NC}\n" "$*"; echo "==> $*" >> "$LOG"; }
ok()   { printf "${GREEN}%s${NC}\n" "$*"; }
fail() { printf "${RED}FAILED: %s${NC} (see build.log)\n" "$*"; exit 1; }

: > "$LOG"

if [ "$1" = "clean" ]; then
    say "clean"
    (cd "$TOP/src" && wmake -h clean) >> "$LOG" 2>&1
    rm -rf "$TOP/out" "$TOP"/test/*.exe "$TOP"/test/*.o "$TOP"/test/*.obj
    ok "clean done"
    exit 0
fi

[ -n "$WATCOM" ] || fail "WATCOM is not set - run %WATCOM%\\owsetenv.cmd first"
mkdir -p "$TOP/out"

say "driver: wasm + wcc + wlink"
(cd "$TOP/src" && wmake -h) >> "$LOG" 2>&1 || fail "driver build"
cp "$TOP/src/mt7921.os2" "$TOP/src/mt7921.map" "$TOP/out/" || fail "copy driver"

say "test tool: gcc"
(cd "$TOP/test" && gcc -O2 -Wall -Zomf -I../src -o wrndtest.exe wrndtest.c) \
    >> "$LOG" 2>&1 || fail "wrndtest build"
cp "$TOP/test/wrndtest.exe" "$TOP/out/" || fail "copy wrndtest"

END=$(date +%s)
ok "done in $((END - START)) s:"
ls -l "$TOP/out"
