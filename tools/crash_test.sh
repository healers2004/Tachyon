#!/usr/bin/env bash
# Crash test: writes keys over one connection, crashes tachyon, restarts it on the same data
# directory and checks that every write it acknowledged is still there. Automatic snapshots
# start after 16 KB of log, so random kills also land in the middle of snapshots. The runs
# cycle through:
#   kill -9 at a random moment, with --fsync always, then with --fsync everysec;
#   and an abort at each step of a snapshot taken with SAVE while writes are in flight
#   (--debug-crash-at, a test-only hook): part-way through the temp file, after it's complete,
#   right after the rename, and after the new log is created.
#
# Killing the process proves crash safety, not power-cut safety: the OS still writes its cache
# to the disk after the process dies. A power cut is what --fsync always protects against.
#
# Usage: tools/crash_test.sh path/to/tachyon [runs, default 12]
set -euo pipefail
# The deliberate aborts mustn't leave core files behind. (bash still prints an "Aborted" notice
# for each one; that's expected.)
ulimit -c 0
source "$(dirname "$0")/lib.sh"

tachyon="$1"
runs="${2:-12}"
port=6382
data_dir="$(mktemp -d)"
work_dir="$(mktemp -d)" # the test's own files, kept out of the data directory
pid=""

trap '[ -n "$pid" ] && kill -9 "$pid" 2>/dev/null; rm -rf "$data_dir" "$work_dir"' EXIT

modes=(kill-always kill-everysec snapshot-partial snapshot-written snapshot-renamed new-log-created)

# start_server [tachyon arguments...]: starts tachyon on the shared data, waits up to 10 s.
start_server() {
    "$tachyon" --port "$port" --dir "$data_dir" --snapshot-after 16384 "$@" \
        >"$work_dir/server.out" 2>&1 &
    pid=$!
    wait_for_port "$port" 10 && return 0
    echo "FAIL: tachyon didn't start. Its output:"
    cat "$work_dir/server.out"
    exit 1
}

# writer FIRST_INDEX ACKED_FILE: sends SET key<i> v<i> one at a time over one connection, and
# appends i to ACKED_FILE only after the +OK reply has arrived. Stops when the server dies.
writer() {
    local i="$1" acked="$2" key value command reply
    exec 7<>"/dev/tcp/127.0.0.1/$port" || return 0
    while true; do
        key="key$i"
        value="v$i"
        # Built first and sent with one write: printf with arguments writes in pieces, and
        # TCP's Nagle algorithm then holds the later pieces back (~40 ms per command).
        printf -v command '*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n' \
            "${#key}" "$key" "${#value}" "$value"
        printf '%s' "$command" >&7 || return 0
        IFS= read -r -t 5 reply <&7 || return 0
        [ "$reply" = $'+OK\r' ] || return 0
        echo "$i" >>"$acked"
        i=$((i + 1))
    done
}

next=1
total_acked=0
torn_tails=0
snapshots=0
sentinels=() # the last acknowledged key of every earlier run

for run in $(seq "$runs"); do
    mode="${modes[$(((run - 1) % ${#modes[@]}))]}"
    case "$mode" in
    kill-always) start_server --fsync always ;;
    kill-everysec) start_server --fsync everysec ;;
    *) start_server --debug-crash-at "$mode" ;;
    esac

    acked="$work_dir/acked"
    : >"$acked"
    writer "$next" "$acked" 2>/dev/null & # "connection reset" when tachyon dies is expected
    writer_pid=$!

    if [ "${mode#kill-}" != "$mode" ]; then
        sleep "0.$((RANDOM % 9 + 1))" # 0.1 to 0.9 s of writes
        kill -9 "$pid"
        wait "$pid" 2>/dev/null || true
    else
        # Let some writes be acknowledged, then ask for a snapshot while they keep coming. The
        # server aborts itself at the crash point during it (or during an automatic snapshot,
        # if one starts first). Wait up to 20 s for that.
        sleep "0.$((RANDOM % 5 + 2))"
        redis-cli -p "$port" SAVE >/dev/null 2>&1 || true
        status=0
        if ! wait_for_exit "$pid" 20; then
            echo "FAIL: run $run: tachyon never reached the crash point $mode"
            exit 1
        fi
        wait "$pid" 2>/dev/null || status=$?
        if [ "$status" -ne 134 ]; then # 128 + SIGABRT: it really stopped at the crash point
            echo "FAIL: run $run: expected an abort at $mode, got exit status $status"
            cat "$work_dir/server.out"
            exit 1
        fi
    fi
    pid=""
    wait "$writer_pid" 2>/dev/null || true
    snapshots=$((snapshots + $(grep -c "snapshot saved" "$work_dir/server.out" || true)))

    if [ ! -s "$acked" ]; then
        echo "FAIL: run $run ($mode): no write was acknowledged before the crash"
        exit 1
    fi
    first="$next"
    last="$(tail -n 1 "$acked")"
    total_acked=$((total_acked + last - first + 1))
    next=$((last + 2)) # key last+1 may or may not have been written; skip it

    # Restart normally and check every key acknowledged in this run, plus the last key of
    # every earlier run, against the values that were written.
    start_server
    grep -q "dropped a half-written" "$work_dir/server.out" && torn_tails=$((torn_tails + 1))
    : >"$work_dir/gets"
    : >"$work_dir/expected"
    for i in $(seq "$first" "$last") "${sentinels[@]}"; do
        echo "GET key$i" >>"$work_dir/gets"
        echo "v$i" >>"$work_dir/expected"
    done
    redis-cli -p "$port" <"$work_dir/gets" >"$work_dir/actual"
    if ! diff -q "$work_dir/expected" "$work_dir/actual" >/dev/null; then
        echo "FAIL: run $run ($mode): acknowledged writes are missing after the restart"
        diff "$work_dir/expected" "$work_dir/actual" | head -20
        exit 1
    fi
    sentinels+=("$last")
    kill -INT "$pid"
    wait "$pid"
    pid=""
    echo "ok: run $run ($mode): $((last - first + 1)) acknowledged writes all present"
done

echo "ok: $runs crash runs, $total_acked acknowledged writes, none lost" \
    "($snapshots snapshots taken, $torn_tails restarts dropped a half-written record)"
