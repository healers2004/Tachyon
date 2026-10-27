#!/usr/bin/env bash
# Benchmarks tachyon against Redis and Valkey on this machine with redis-benchmark, and prints
# the results as a markdown table (requests per second, higher is better).
#
# Each server runs alone with a fresh data directory, in three setups:
#   memory:    no persistence (Redis/Valkey: --save '' --appendonly no; tachyon: --no-persist)
#   durable:   fsync before every acknowledged write (--appendonly yes --appendfsync always;
#              tachyon: --fsync always)
#   pipelined: memory setup, 16 commands per round trip (-P 16)
# Always 50 clients. Run it on an otherwise idle machine, with a Release build of tachyon.
#
# Usage: tools/benchmark.sh path/to/tachyon [requests per test, default 200000]
set -euo pipefail
source "$(dirname "$0")/lib.sh"

tachyon="$1"
requests="${2:-200000}"
port=6400
pid=""
data=""

trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; [ -n "$data" ] && rm -rf "$data"' EXIT

# start <server> <setup>: starts redis, valkey or tachyon on $port in that setup.
start() {
    data="$(mktemp -d)"
    case "$1:$2" in
    redis:durable | valkey:durable)
        "$1-server" --port "$port" --dir "$data" --save '' --appendonly yes \
            --appendfsync always >/dev/null &
        ;;
    redis:* | valkey:*) "$1-server" --port "$port" --save '' --appendonly no >/dev/null & ;;
    tachyon:durable) "$tachyon" --port "$port" --dir "$data" --fsync always >/dev/null & ;;
    tachyon:*) "$tachyon" --port "$port" --no-persist >/dev/null & ;;
    esac
    pid=$!
    wait_for_port "$port" && return 0
    echo "FAIL: $1 didn't start" >&2
    exit 1
}

stop() {
    kill "$pid"
    wait "$pid" 2>/dev/null || true
    pid=""
    rm -rf "$data"
    data=""
}

# bench <tests> [extra redis-benchmark options]: prints "TEST rps" lines.
bench() {
    local tests="$1"
    shift
    redis-benchmark -p "$port" -c 50 -n "$requests" -t "$tests" --csv "$@" 2>/dev/null |
        tail -n +2 | tr -d '"' | awk -F, '{ printf "%s %d\n", $1, $2 }'
}

# table <title> <setup> <tests> [extra options]: one table, one row per test.
table() {
    local title="$1" setup="$2" tests="$3"
    shift 3
    declare -A rps
    local server line
    for server in redis valkey tachyon; do
        start "$server" "$setup"
        while read -r line; do
            rps["$server:${line% *}"]="${line#* }"
        done < <(bench "$tests" "$@")
        stop
    done
    echo
    echo "**$title**"
    echo
    echo "| Test | Redis 7.0 | Valkey 7.2 | Tachyon | Tachyon / Redis |"
    echo "| --- | ---: | ---: | ---: | ---: |"
    local test
    for test in $(echo "$tests" | tr ',' ' ' | tr '[:lower:]' '[:upper:]'); do
        local r="${rps[redis:$test]:-0}" v="${rps[valkey:$test]:-0}" t="${rps[tachyon:$test]:-0}"
        local ratio="n/a"
        [ "$r" -gt 0 ] && ratio="$(awk -v t="$t" -v r="$r" 'BEGIN { printf "%.2f", t / r }')"
        echo "| $test | $r | $v | $t | $ratio |"
    done
}

echo "Machine: $(nproc) CPUs, $(uname -sr), $(redis-benchmark --version | cut -d' ' -f1-2)," \
    "$requests requests per test, 50 clients"
table "In memory" memory ping_mbulk,set,get,lpush,lpop,hset
# A quarter of the requests: every write waits for the disk, so this is far slower.
table "Durable: fsync before every acknowledged write" durable set,lpush,hset \
    -n "$((requests / 4))"
table "Pipelined: 16 commands per round trip, in memory" memory set,get -P 16
