#!/bin/sh
# End-to-end tests for kctool: WAV round trips and the reference server/client.
set -eu

KCTOOL=${KCTOOL:-$(dirname "$0")/../build/kctool}
T=$(mktemp -d)
SERVER_PID=
cleanup() { [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM

fail() { echo "FAIL $*"; exit 1; }
ok()   { echo "ok    $*"; }

head -c 3000 /dev/urandom > "$T/data.bin"

for baud in 300 600 1200; do
    for stop in 1 2; do
        "$KCTOOL" encode -b $baud -s $stop -l 1000 "$T/data.bin" "$T/a.wav"
        "$KCTOOL" decode -b $baud -s $stop "$T/a.wav" "$T/out.bin"
        cmp -s "$T/data.bin" "$T/out.bin" || fail "sine wav round trip $baud/$stop"
        ok "sine wav round trip $baud baud, $stop stop"
    done
    "$KCTOOL" encode -q -r 22050 -b $baud "$T/data.bin" "$T/a.wav"
    "$KCTOOL" decode -b auto "$T/a.wav" "$T/out.bin" 2>/dev/null
    cmp -s "$T/data.bin" "$T/out.bin" || fail "square 22k auto-baud $baud"
    ok "square wave, 22050 Hz, auto-baud detected $baud"
done

# streaming through pipes (as with aplay / arecord)
"$KCTOOL" encode "$T/data.bin" - | "$KCTOOL" decode - - > "$T/out.bin"
cmp -s "$T/data.bin" "$T/out.bin" || fail "pipe round trip"
ok "stdin/stdout pipe round trip"

# server / client
PORT=$(( 20000 + $$ % 20000 ))
mkdir "$T/tapes"
"$KCTOOL" serve -p $PORT -d "$T/tapes" 2>"$T/server.log" &
SERVER_PID=$!
i=0; until "$KCTOOL" dir 127.0.0.1:$PORT >/dev/null 2>&1; do
    i=$((i+1)); [ $i -lt 50 ] || fail "server did not start"; sleep 0.1
done
"$KCTOOL" put 127.0.0.1:$PORT game1 "$T/data.bin" baud=300 stop=2
"$KCTOOL" put 127.0.0.1:$PORT empty /dev/null
"$KCTOOL" get 127.0.0.1:$PORT game1 "$T/got.bin"
cmp -s "$T/data.bin" "$T/got.bin" || fail "server get"
"$KCTOOL" dir 127.0.0.1:$PORT > "$T/dir.txt"
grep -qx "game1 3000 baud=300 stop=2" "$T/dir.txt" || { cat "$T/dir.txt"; fail "dir listing"; }
grep -qx "empty 0" "$T/dir.txt" || { cat "$T/dir.txt"; fail "dir listing (empty)"; }
if "$KCTOOL" get 127.0.0.1:$PORT ../etc/passwd "$T/x" 2>/dev/null; then fail "path traversal accepted"; fi
if "$KCTOOL" get 127.0.0.1:$PORT nosuch "$T/x" 2>/dev/null; then fail "missing tape returned"; fi
ok "server put/get/dir and name checks"

echo "PASSED"
