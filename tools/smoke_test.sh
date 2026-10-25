#!/usr/bin/env bash
# End-to-end check: starts tachyon, talks to it with the real redis-cli and redis-benchmark,
# sends an HTTP request that must be rejected, misbehaves as several kinds of client, then
# stops it with SIGINT (Ctrl+C) while clients are still connected and expects a clean exit.
# Then checks that data survives a restart, and that --no-persist keeps nothing.
# Usage: tools/smoke_test.sh path/to/tachyon
set -euo pipefail

tachyon="$1"
port=6380
pid=""
data_dir="$(mktemp -d)"

# If anything below fails, don't leave tachyon running or temp files behind.
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$data_dir"' EXIT

# start_server [tachyon arguments...]: starts tachyon on $port and waits up to 5 s for it.
start_server() {
    "$tachyon" "$@" &
    pid=$!
    for _ in $(seq 50); do
        redis-cli -p "$port" PING >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    echo "FAIL: tachyon $* did not start listening on $port"
    exit 1
}

# stop_server: Ctrl+C must shut tachyon down within 5 s with exit code 0.
stop_server() {
    kill -INT "$pid"
    for _ in $(seq 50); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    local status=0
    if kill -0 "$pid" 2>/dev/null; then
        echo "FAIL: tachyon still running 5 s after SIGINT"
        exit 1
    fi
    wait "$pid" || status=$?
    pid=""
    if [ "$status" -ne 0 ]; then
        echo "FAIL: tachyon exited with $status after SIGINT"
        exit 1
    fi
}

# check <expected output> <redis-cli arguments...>
# redis-cli prints raw replies when its output isn't a terminal: "1" not "(integer) 1",
# and an empty line for nil. timeout: a server that stops responding fails the test.
check() {
    local expected="$1"
    shift
    local actual
    actual="$(timeout 5 redis-cli -p "$port" "$@")"
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: redis-cli $* returned '$actual', expected '$expected'"
        exit 1
    fi
    echo "ok: $* -> '$actual'"
}

# Bad command lines are refused before anything starts.
if "$tachyon" --port 99999 2>/dev/null; then
    echo "FAIL: --port 99999 was accepted"
    exit 1
fi
echo "ok: invalid --port rejected"
if "$tachyon" --fsync sometimes 2>/dev/null; then
    echo "FAIL: --fsync sometimes was accepted"
    exit 1
fi
echo "ok: invalid --fsync rejected"
if "$tachyon" --dir "$data_dir/missing" 2>/dev/null; then
    echo "FAIL: a missing --dir was accepted"
    exit 1
fi
echo "ok: missing --dir rejected"

start_server --dir "$data_dir"

check PONG PING
check hello ECHO hello
check OK SET foo bar
check bar GET foo
check 1 EXISTS foo
check 1 DEL foo
check "" GET foo
check "ERR unknown command 'FLY', with args beginning with: " FLY

# A web page posting to localhost must get a protocol error and be disconnected.
# bash's /dev/tcp opens a TCP connection as file descriptor 3.
exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'POST / HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
reply="$(head -c 15 <&3)"
exec 3<&-
if [ "$reply" != "-ERR Protocol e" ]; then
    echo "FAIL: HTTP request got '$reply', expected a protocol error"
    exit 1
fi
echo "ok: HTTP request rejected"

# A client that sends half a command and goes quiet must not hold up anyone else.
# It stays connected until shutdown.
exec 4<>"/dev/tcp/127.0.0.1/$port"
printf '*3\r\n$3\r\nSET' >&4
check PONG PING
echo "ok: idle half-sent command doesn't block others"

# A client that disconnects mid-command is dropped without disturbing the server.
exec 5<>"/dev/tcp/127.0.0.1/$port"
printf '*2\r\n$4\r\nECHO\r\n$5\r\nhel' >&5
exec 5>&-
check PONG PING
echo "ok: client disconnecting mid-command"

# A slow reader pipelines a million PINGs but never reads a reply. tachyon must stop reading
# from it once its reply queue is full, and keep serving everyone else.
exec 6<>"/dev/tcp/127.0.0.1/$port"
yes $'*1\r\n$4\r\nPING\r' | head -n 1000000 >&6 &
writer=$!
sleep 1
check PONG PING
echo "ok: slow reader doesn't block others"

# 50 concurrent clients. PING_INLINE is skipped: tachyon rejects inline commands.
bench="$(timeout 120 redis-benchmark -p "$port" -c 50 -n 20000 -t ping_mbulk,set,get -q 2>&1)"
# -q still prints \r-separated progress lines; keep only the final summary per test.
echo "$bench" | tr '\r' '\n' | grep 'requests per second'
if echo "$bench" | grep -qi error; then
    echo "FAIL: redis-benchmark reported errors"
    exit 1
fi
check PONG PING
echo "ok: redis-benchmark with 50 clients"

# Shut down while the half-command client and the slow reader are still connected.
stop_server
exec 4>&- 6>&-
kill "$writer" 2>/dev/null || true
echo "ok: clean shutdown on SIGINT with clients connected"

# Everything written before the shutdown is replayed from the log on the next start.
start_server --dir "$data_dir"
check 1 EXISTS key:__rand_int__ # written by redis-benchmark's SET test before the restart
check OK SET survivor "still here"
check 2 RPUSH survivor-list a b
check 1 HSET survivor-hash f v
check OK SET survivor-ttl v EX 1000
stop_server
start_server --dir "$data_dir"
check "still here" GET survivor
check "$(printf 'a\nb')" LRANGE survivor-list 0 -1
check v HGET survivor-hash f
# The log holds the absolute deadline, so the TTL kept counting down; it didn't restart.
ttl="$(redis-cli -p "$port" TTL survivor-ttl)"
if [ "$ttl" -gt 1000 ] || [ "$ttl" -lt 990 ]; then
    echo "FAIL: TTL after restart is $ttl, expected 990-1000"
    exit 1
fi
stop_server
echo "ok: every type and its TTL survive a restart"

# --no-persist writes no log and forgets everything on exit. --port moves the server.
port=6381
memory_dir="$(mktemp -d)"
start_server --port "$port" --no-persist --dir "$memory_dir"
check OK SET gone soon
stop_server
start_server --port "$port" --no-persist --dir "$memory_dir"
check "" GET gone

# Expiry in real time. Lazy: an expired key reads as missing. Active: the once-a-second sweep
# removes a key nobody reads, which DBSIZE (it counts expired keys too) shows.
check OK SET short v PX 300
check v GET short
sleep 0.5
check "" GET short
check OK SET unread v PX 200
check OK SET kept v
check 2 DBSIZE
sleep 1.5
check 1 DBSIZE
echo "ok: keys expire on access and in the background sweep"
stop_server
if [ -n "$(ls -A "$memory_dir")" ]; then
    echo "FAIL: --no-persist wrote files"
    exit 1
fi
rmdir "$memory_dir"
echo "ok: --no-persist and --port $port"
