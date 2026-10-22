#!/usr/bin/env bash
# Crash test: writes keys over one connection, kills tachyon with SIGKILL at a random moment
# (no chance to clean up or flush anything), restarts it on the same log and checks that every
# write it acknowledged is still there. Repeats; even-numbered runs use --fsync everysec.
#
# Killing the process proves crash safety, not power-cut safety: the OS still writes its cache
# to the disk after the process dies. A power cut is what --fsync always protects against.
#
# Usage: tools/crash_test.sh path/to/tachyon [runs, default 10]
set -euo pipefail

tachyon="$1"
runs="${2:-10}"
port=6382
data_dir="$(mktemp -d)"
pid=""

trap '[ -n "$pid" ] && kill -9 "$pid" 2>/dev/null; rm -rf "$data_dir"' EXIT

# start_server [tachyon arguments...]: starts tachyon on the shared log, waits up to 10 s.
start_server() {
    "$tachyon" --port "$port" --dir "$data_dir" "$@" >"$data_dir/server.out" 2>&1 &
    pid=$!
    for _ in $(seq 100); do
        redis-cli -p "$port" PING >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    echo "FAIL: tachyon didn't start. Its output:"
    cat "$data_dir/server.out"
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
sentinels=() # the last acknowledged key of every earlier run

policy=always # the flush policy of the running server, the one about to be killed
start_server --fsync "$policy"
for run in $(seq "$runs"); do
    acked="$data_dir/acked"
    : >"$acked"
    writer "$next" "$acked" 2>/dev/null & # "connection reset" when tachyon dies is expected
    writer_pid=$!
    sleep "0.$((RANDOM % 9 + 1))" # 0.1 to 0.9 s of writes

    kill -9 "$pid"
    wait "$pid" 2>/dev/null || true
    pid=""
    wait "$writer_pid" 2>/dev/null || true

    if [ ! -s "$acked" ]; then
        echo "FAIL: run $run: no write was acknowledged before the kill"
        exit 1
    fi
    first="$next"
    last="$(tail -n 1 "$acked")"
    total_acked=$((total_acked + last - first + 1))
    next=$((last + 2)) # key last+1 may or may not have been written; skip it

    # Restart on the same log, alternating the policy for the next run's kill.
    killed_policy="$policy"
    [ "$policy" = always ] && policy=everysec || policy=always
    start_server --fsync "$policy"
    grep -q "dropped a half-written" "$data_dir/server.out" && torn_tails=$((torn_tails + 1))

    # GET every key acknowledged in this run, plus the last key of every earlier run, and
    # compare with the values that were written.
    : >"$data_dir/gets"
    : >"$data_dir/expected"
    for i in $(seq "$first" "$last") "${sentinels[@]}"; do
        echo "GET key$i" >>"$data_dir/gets"
        echo "v$i" >>"$data_dir/expected"
    done
    redis-cli -p "$port" <"$data_dir/gets" >"$data_dir/actual"
    if ! diff -q "$data_dir/expected" "$data_dir/actual" >/dev/null; then
        echo "FAIL: run $run ($killed_policy): acknowledged writes are missing after the restart"
        diff "$data_dir/expected" "$data_dir/actual" | head -20
        exit 1
    fi
    sentinels+=("$last")
    echo "ok: run $run ($killed_policy): $((last - first + 1)) acknowledged writes all present"
done

kill -INT "$pid"
wait "$pid"
pid=""
echo "ok: $runs crash runs, $total_acked acknowledged writes, none lost" \
    "($torn_tails restarts dropped a half-written record)"
