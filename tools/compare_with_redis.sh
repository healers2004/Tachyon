#!/usr/bin/env bash
# Runs the same commands against a real Redis and against tachyon, and compares every reply.
# redis-cli --no-raw prints the reply type, so (nil), (empty array) and "" all differ.
# Lines starting with "sorted:" compare their elements in any order (HGETALL's order is the
# hash table's, which differs between the two).
# Usage: tools/compare_with_redis.sh path/to/tachyon    (needs redis-server and redis-cli)
set -euo pipefail

tachyon="$1"
redis_port=6390
tachyon_port=6391
redis_pid=""
tachyon_pid=""

trap 'kill $redis_pid $tachyon_pid 2>/dev/null || true' EXIT

redis-server --port "$redis_port" --save '' --appendonly no >/dev/null &
redis_pid=$!
"$tachyon" --port "$tachyon_port" --no-persist >/dev/null &
tachyon_pid=$!
for port in "$redis_port" "$tachyon_port"; do
    for _ in $(seq 50); do
        redis-cli -p "$port" PING >/dev/null 2>&1 && break
        sleep 0.1
    done
done

# reply <port> <words...>: the formatted reply, with array numbering removed if sorting.
reply() {
    local port="$1"
    shift
    redis-cli --no-raw -p "$port" "$@" 2>&1
}

count=0
failures=0
while IFS= read -r line; do
    if [ -z "$line" ] || [ "${line:0:1}" = "#" ]; then
        continue
    fi
    sorted=false
    if [ "${line#sorted: }" != "$line" ]; then
        sorted=true
        line="${line#sorted: }"
    fi
    read -r -a words <<<"$line" # split on spaces; the commands below have no quoted arguments

    expected="$(reply "$redis_port" "${words[@]}")"
    actual="$(reply "$tachyon_port" "${words[@]}")"
    if [ "$sorted" = true ]; then
        expected="$(echo "$expected" | sed 's/^ *[0-9]*) //' | sort)"
        actual="$(echo "$actual" | sed 's/^ *[0-9]*) //' | sort)"
    fi
    count=$((count + 1))
    if [ "$expected" != "$actual" ]; then
        failures=$((failures + 1))
        echo "MISMATCH: $line"
        echo "  redis:   $(echo "$expected" | tr '\n' '|')"
        echo "  tachyon: $(echo "$actual" | tr '\n' '|')"
    fi
done <<'EOF'
FLUSHALL
# Basics and errors
PING
PING hello
PING a b
ECHO hi
ECHO
GET
GET missing
FLY
FLY a b c
DBSIZE x

# Strings and SET options
SET k v
GET k
SET k v2 NX
SET k2 v XX
SET k v3 XX
GET k
SET n v NX
SET k v EX 0
SET k v EX -1
SET k v EX abc
SET k v EX 9223372036854775807
SET k v PX 9223372036854775807
SET k v EX 10 PX 100
SET k v NX XX
SET k v EX
SET k v FOO
SET k v KEEPTTL EX 10
SET k v EX 100
TTL k
SET k w KEEPTTL
TTL k
GET k
SET k x
TTL k
SET k v PX 100000
TTL k
SET k v PXAT 1
GET k
EXISTS k
SET k v ex 100 nx
SET k v nx ex 100
TTL k
DEL k k2 n missing
EXISTS k k2 n

# Expiry
SET e v
TTL e
TTL missing
EXPIRE missing 10
EXPIRE e abc
EXPIRE e 9223372036854775807
EXPIRE e 100
TTL e
PERSIST e
PERSIST e
TTL e
PERSIST missing
EXPIRE e -5
EXISTS e
SET e v
PEXPIREAT e 1
EXISTS e

# Hashes
HSET h a 1 b 2
HSET h a 1 c
HSET h a
HSET h c 3 a 9
sorted: HGETALL h
HGET h a
HGET h zz
HGET missing a
HLEN h
HLEN missing
HDEL h a zz
HLEN h
HDEL h b c
EXISTS h
HGETALL missing
HDEL missing a
TYPE h
HSET h only one
HGETALL h
EXPIRE h 100
TTL h

# Lists
RPUSH l a b c
LPUSH l x y
LRANGE l 0 -1
LRANGE l -2 -1
LRANGE l 1 1
LRANGE l -100 0
LRANGE l 3 100
LRANGE l 5 10
LRANGE l 3 1
LRANGE l 0 abc
LRANGE missing 0 -1
LLEN l
LLEN missing
LPOP l
RPOP l
LPOP l 0
LPOP l 2
LPOP l 5
EXISTS l
LPOP missing
RPOP missing
LPOP missing 2
LPOP missing 0
LPOP l -1
LPOP l a
LPOP l 1 2
RPUSH l a b c d e
LTRIM l 1 -2
LRANGE l 0 -1
LTRIM l 0 0
LRANGE l 0 -1
LTRIM l 5 1
EXISTS l
LTRIM missing 0 1
LTRIM l a b

# Wrong types
SET s v
LPUSH s a
RPUSH s a
LPOP s
RPOP s
LRANGE s 0 1
LTRIM s 0 1
LLEN s
HSET s a b
HGET s a
HGETALL s
HDEL s a
HLEN s
HSET hh f v
GET hh
LLEN hh
RPUSH ll a
GET ll
HGET ll a
SET hh now-a-string
TYPE hh
GET hh

# Types and the keyspace
TYPE s
TYPE ll
TYPE missing
DBSIZE
FLUSHALL FOO
FLUSHALL SYNC extra
FLUSHALL ASYNC
DBSIZE
FLUSHALL
DBSIZE
EOF

# ---- Pub/sub: whole sessions on one connection, compared byte for byte ----
# (redis-cli opens a new connection per command, and subscribed mode lives on one connection.)

P=""
cmd() { # appends one command, in RESP form, to $P
    local word
    P+="*$#"$'\r\n'
    for word in "$@"; do P+="\$${#word}"$'\r\n'"$word"$'\r\n'; done
}

# session <port>: sends $P on one connection and prints every byte back within 0.5 s.
session() {
    exec 3<>"/dev/tcp/127.0.0.1/$1"
    printf '%s' "$P" >&3
    timeout 0.5 cat <&3 || true
    exec 3>&-
}

# delivery <port>: one connection subscribes, another publishes; prints both sides' replies.
delivery() {
    exec 4<>"/dev/tcp/127.0.0.1/$1"
    P=""
    cmd SUBSCRIBE news
    printf '%s' "$P" >&4
    sleep 0.2
    redis-cli -p "$1" PUBLISH news "hello world"
    redis-cli -p "$1" PUBLISH news ""
    redis-cli -p "$1" PUBLISH elsewhere x
    timeout 0.5 cat <&4 || true
    exec 4>&-
}

compare_bytes() { # compare_bytes <name> <redis output> <tachyon output>
    count=$((count + 1))
    if [ "$2" != "$3" ]; then
        failures=$((failures + 1))
        echo "MISMATCH: pub/sub session '$1'"
        echo "  redis:   $(printf '%s' "$2" | tr '\r\n' '~|')"
        echo "  tachyon: $(printf '%s' "$3" | tr '\r\n' '~|')"
    fi
}

compare_session() { # compare_session <name>, using the commands in $P
    compare_bytes "$1" "$(session "$redis_port")" "$(session "$tachyon_port")"
}

P=""
cmd SUBSCRIBE a b
cmd SUBSCRIBE a
cmd GET k
cmd SET k v
cmd GET
cmd FLY
cmd PING
cmd PING x
cmd PUBLISH a m
cmd UNSUBSCRIBE a zz
cmd UNSUBSCRIBE
cmd UNSUBSCRIBE
cmd PING
compare_session "subscribed mode"

P=""
cmd UNSUBSCRIBE x
cmd SUBSCRIBE
cmd PUBLISH a
cmd PUBLISH nobody msg
cmd QUIT
cmd PING
compare_session "errors and QUIT"

P=""
cmd QUIT extra
compare_session "QUIT with an argument"

compare_bytes "message delivery" "$(delivery "$redis_port")" "$(delivery "$tachyon_port")"

echo "compared $count commands and sessions: $failures mismatch(es)"
[ "$failures" -eq 0 ]
