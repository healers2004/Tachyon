#!/usr/bin/env bash
# Full-disk test: runs tachyon on a tiny 2 MB RAM disk and fills it.
#  1. A SAVE that doesn't fit must fail with an error, leave no temp file, and keep serving.
#  2. When the log itself can't be written, tachyon must stop without acknowledging the
#     writes it couldn't save. Restarted from a copy of its files on a roomy disk, it must
#     have every write it did acknowledge.
#  3. With automatic snapshots on, a snapshot that stops fitting must back off, not be retried
#     on every pass of the event loop.
# Needs sudo (to mount the RAM disk). Linux only.
# Usage: tools/disk_full_test.sh path/to/tachyon
set -euo pipefail
source "$(dirname "$0")/lib.sh"

tachyon="$1"
port=6383
small="$(mktemp -d)"
roomy="$(mktemp -d)"
work="$(mktemp -d)"
pid=""

cleanup() {
    [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null
    sudo umount "$small" 2>/dev/null || true
    rm -rf "$small" "$roomy" "$work"
}
trap cleanup EXIT

sudo mount -t tmpfs -o size=2m tmpfs "$small"
sudo chmod 777 "$small"

start_server() { # start_server <dir> [snapshot-after bytes, default 0 = only on SAVE]
    "$tachyon" --port "$port" --dir "$1" --snapshot-after "${2:-0}" >"$work/server.out" 2>&1 &
    pid=$!
    wait_for_port "$port" && return 0
    echo "FAIL: tachyon didn't start"
    cat "$work/server.out"
    exit 1
}

value="$(head -c 4000 /dev/zero | tr '\0' 'x')" # 4 KB per write

start_server "$small"

# Fill about 1.2 MB of the 2 MB disk, so the log fits but a snapshot of it won't.
for i in $(seq 300); do
    [ "$(redis-cli -p "$port" SET "key$i" "$value")" = "OK" ] || {
        echo "FAIL: write $i failed too early"
        exit 1
    }
    echo "$i" >>"$work/acked"
done

reply="$(redis-cli -p "$port" SAVE)"
case "$reply" in
ERR\ *) ;;
*)
    echo "FAIL: SAVE on a full disk replied '$reply', expected an error"
    exit 1
    ;;
esac
if ls "$small" | grep -q '\.tmp$'; then
    echo "FAIL: the failed snapshot left its temp file behind"
    exit 1
fi
[ "$(redis-cli -p "$port" GET key1)" = "$value" ] || {
    echo "FAIL: tachyon stopped serving after the failed SAVE"
    exit 1
}
redis-cli -p "$port" INFO persistence | grep -q "^rdb_last_bgsave_status:err" || {
    echo "FAIL: INFO doesn't report the failed snapshot"
    exit 1
}
echo "ok: SAVE on a full disk fails cleanly and tachyon keeps serving ($reply)"

# Keep writing until the log can't grow. Each acknowledged key is recorded.
for i in $(seq 301 1000); do
    reply="$(redis-cli -p "$port" SET "key$i" "$value" 2>/dev/null || true)"
    [ "$reply" = "OK" ] || break
    echo "$i" >>"$work/acked"
done
status=0
wait_for_exit "$pid" || true
wait "$pid" || status=$?
pid=""
if [ "$status" -ne 1 ]; then
    echo "FAIL: on a full disk tachyon exited with $status, expected 1"
    cat "$work/server.out"
    exit 1
fi
grep -q "No space left on device" "$work/server.out" || {
    echo "FAIL: the log doesn't say the disk is full:"
    cat "$work/server.out"
    exit 1
}
echo "ok: a full disk stops tachyon without acknowledging unsaved writes"

# Move the files to a disk with room and check every acknowledged write survived.
cp -a "$small/." "$roomy/"
start_server "$roomy"
missing=0
while read -r i; do
    got="$(redis-cli -p "$port" GET "key$i")"
    [ "$got" = "$value" ] || missing=$((missing + 1))
done <"$work/acked"
kill -INT "$pid"
wait "$pid" || true
pid=""
if [ "$missing" -ne 0 ]; then
    echo "FAIL: $missing acknowledged write(s) missing after the restart"
    exit 1
fi
echo "ok: all $(wc -l <"$work/acked") acknowledged writes survived the full disk"

# 3. Automatic snapshots on a fresh small disk: they succeed at first, then stop fitting as the
# data grows. A failed one must wait for more log before trying again; retrying on every pass
# of the event loop would print "snapshot failed" dozens of times a second.
sudo umount "$small"
sudo mount -t tmpfs -o size=2m tmpfs "$small"
sudo chmod 777 "$small"
start_server "$small" 200000
for i in $(seq 1000); do
    [ "$(redis-cli -p "$port" SET "auto$i" "$value" 2>/dev/null || true)" = "OK" ] || break
done
wait_for_exit "$pid" || true
wait "$pid" 2>/dev/null || true
pid=""
saved="$(grep -c "snapshot saved" "$work/server.out" || true)"
failed="$(grep -c "snapshot failed" "$work/server.out" || true)"
# Backing off allows one try per 200 KB of new log, and the log can't outgrow the 2 MB disk:
# at most 10 failures. Retrying on every pass would fail on nearly every write (hundreds).
if [ "$saved" -lt 1 ] || [ "$failed" -lt 1 ] || [ "$failed" -gt 10 ]; then
    echo "FAIL: expected some snapshots, then at most 10 failures; got $saved saved, $failed failed"
    exit 1
fi
echo "ok: automatic snapshots back off on a full disk ($saved saved, then $failed failed)"
