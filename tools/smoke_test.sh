#!/usr/bin/env bash
# End-to-end check: starts tachyon, talks to it with the real redis-cli, sends an HTTP request
# that must be rejected, then stops it with SIGINT (Ctrl+C) and expects a clean exit.
# Usage: tools/smoke_test.sh path/to/tachyon
set -euo pipefail

tachyon="$1"
port=6380

"$tachyon" &
pid=$!
# If anything below fails, don't leave tachyon running.
trap 'kill "$pid" 2>/dev/null || true' EXIT

# Wait up to 5 s for tachyon to start listening.
for _ in $(seq 50); do
    redis-cli -p "$port" PING >/dev/null 2>&1 && break
    sleep 0.1
done

# check <expected output> <redis-cli arguments...>
# redis-cli prints raw replies when its output isn't a terminal: "1" not "(integer) 1",
# and an empty line for nil.
check() {
    local expected="$1"
    shift
    local actual
    actual="$(redis-cli -p "$port" "$@")"
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: redis-cli $* returned '$actual', expected '$expected'"
        exit 1
    fi
    echo "ok: $* -> '$actual'"
}

check PONG PING
check hello ECHO hello
check OK SET foo bar
check bar GET foo
check 1 EXISTS foo
check 1 DEL foo
check "" GET foo
check "ERR unknown command 'FLY'" FLY

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

# Ctrl+C must shut tachyon down with exit code 0.
kill -INT "$pid"
status=0
wait "$pid" || status=$?
trap - EXIT
if [ "$status" -ne 0 ]; then
    echo "FAIL: tachyon exited with $status after SIGINT"
    exit 1
fi
echo "ok: clean shutdown on SIGINT"
