# Tachyon

Tachyon is a Redis-compatible key-value server written in C++20, with its own storage engine,
**Horizon**, for crash-safe persistence. It speaks RESP2, the Redis wire protocol, so the real
`redis-cli`, `redis-benchmark` and Redis client libraries work against it unchanged. It builds
and runs on Linux and Windows with no dependencies beyond the C++ standard library.

- **Compatible:** every supported command gives the same reply as Redis 7, byte for byte,
  including error messages. A script checks this against a real Redis in CI.
- **Fast enough to compare:** a single-threaded `poll()` event loop, like Redis's, serving
  hundreds of clients at once (see [Benchmarks](#benchmarks)).
- **Crash-safe:** a write-ahead log with checksums and group commit, plus snapshots that are
  safe to interrupt at any step. A crash test kills the server mid-write and mid-snapshot 100
  times in a row and checks that no acknowledged write is lost.
- **Tested hard:** unit tests, end-to-end tests with the real Redis tools, fuzzing with
  libFuzzer, and AddressSanitizer and UndefinedBehaviorSanitizer on every Linux CI run.

## Architecture

```
  redis-cli, redis-benchmark, any Redis client library
                     │  RESP2 over TCP (default 127.0.0.1:6380)
                     ▼
 ┌──────────────────────────── tachyon ─────────────────────────────┐
 │  server/     event loop, connections, pub/sub, SAVE and INFO      │
 │  protocol/   RESP parser and reply writer                         │
 │  commands/   command table, strings, hashes, lists, expiry        │
 │  platform/   sockets: posix/ and win32/ behind one header         │
 └──────────────────────────────┬────────────────────────────────────┘
                                │ horizon/engine.h and horizon/log.h only
                                ▼
 ┌──────────────────────────── horizon ─────────────────────────────┐
 │  engine      snapshot + log generations, recovery, compaction     │
 │  log         append-only records: [length][CRC32][payload]        │
 │  platform/   files: fsync, atomic rename (posix/ and win32/)      │
 └──────────────────────────────┬────────────────────────────────────┘
                                ▼
                 tachyon.snapshot, tachyon.log.<N>  (in --dir)
```

- **Horizon** knows nothing about networking or Redis. It stores and replays opaque records,
  so any program can use it as a storage engine.
- **Tachyon** uses Horizon only through `CommandLog`, and runs without it (`--no-persist`).
- **OS-specific code** lives only in each component's `platform/` folder; CMake picks the POSIX
  or Win32 file for the machine it's building on.

The life of a write: the event loop reads the bytes; the parser waits for a complete command;
relative expiry times are made absolute; the command is appended to the log batch and run
against the in-memory store; after every ready client has been read, one commit flushes the
batch to disk; only then are the replies sent.

## Quick start

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/tachyon/tachyon                 # Windows: build\tachyon\Release\tachyon.exe

redis-cli -p 6380 SET greeting hello EX 60
redis-cli -p 6380 GET greeting
redis-cli -p 6380 RPUSH queue job1 job2
redis-cli -p 6380 INFO keyspace
```

Tachyon listens on `127.0.0.1:6380`, so it never clashes with a real Redis on 6379, and nothing
else on the network can reach it. Press Ctrl+C to shut it down cleanly, even with clients
connected.

| Option | Default | Meaning |
| --- | --- | --- |
| `--port N` | `6380` | TCP port to listen on (always on 127.0.0.1) |
| `--dir PATH` | `.` | Existing directory for `tachyon.snapshot` and `tachyon.log.<N>` |
| `--fsync always\|everysec` | `always` | When the log is flushed to disk (see [Durability](#durability)) |
| `--snapshot-after BYTES` | `67108864` | Take a snapshot once the log reaches this size (or the last snapshot's, if bigger); `0` = only on `SAVE` |
| `--no-persist` | off | Keep data in memory only; write nothing to disk |
| `--loglevel LEVEL` | `notice` | `debug`, `verbose`, `notice` or `warning`, as in Redis |
| `--version` | | Print the version and exit |

## Supported commands

| Group | Commands |
| --- | --- |
| Connection | `PING [message]`, `ECHO message`, `QUIT` |
| Strings | `GET`, `SET key value [NX \| XX] [EX s \| PX ms \| EXAT t \| PXAT t \| KEEPTTL]`, `DEL key [key ...]`, `EXISTS key [key ...]` |
| Hashes | `HSET key field value [field value ...]`, `HGET`, `HGETALL`, `HDEL key field [field ...]`, `HLEN` |
| Lists | `LPUSH`, `RPUSH key element [element ...]`, `LPOP`, `RPOP key [count]`, `LRANGE`, `LTRIM key start stop`, `LLEN` |
| Expiry | `EXPIRE key seconds`, `PEXPIREAT key unix-ms`, `TTL`, `PERSIST` |
| Keyspace | `TYPE`, `DBSIZE`, `FLUSHALL [SYNC \| ASYNC]` |
| Pub/sub | `SUBSCRIBE channel [channel ...]`, `UNSUBSCRIBE [channel ...]`, `PUBLISH channel message` |
| Server | `SAVE`, `INFO [section ...]` |

Behaviour follows Redis: using a key as the wrong type gives the `WRONGTYPE` error; emptied
hashes and lists are deleted; expired keys are removed when accessed and by a sweep about once
a second; a subscribed connection only accepts `SUBSCRIBE`, `UNSUBSCRIBE`, `PING` and `QUIT`;
pub/sub messages aren't stored, so a channel with no subscribers drops them. Only RESP arrays
are accepted as commands, so an HTTP request sent to the port is rejected on its first byte.

**Not supported yet:** other data types (sets, sorted sets, streams), `INCR` and friends,
pattern subscriptions (`PSUBSCRIBE`), `PUBSUB`, `RESET`, `CONFIG`, `SET ... GET`, `EXPIRE`'s
`NX`/`XX`/`GT`/`LT`, inline (plain-text) commands, RESP3, authentication and replication.
`HGETALL` returns fields in hash-table order; Redis keeps insertion order for small hashes.

## Durability

Every write command is appended to a write-ahead log through Horizon **before** its reply is
sent. Each record is a 4-byte length, a CRC32 checksum and the command itself, in the same RESP
form a client sends. Relative expiry times are logged as absolute ones (`EXPIRE k 10` becomes
`PEXPIREAT k <time>`, as Redis does), so a deadline doesn't restart when the log is replayed.

**Guarantees:**

- `--fsync always` (default): the log is flushed to disk (`fsync` on Linux, `FlushFileBuffers`
  on Windows) before any reply that acknowledges a write. Writes that arrive together share one
  flush (group commit). **An acknowledged write survives a crash or a power cut.**
- `--fsync everysec`: the log reaches the OS before each reply but is flushed at most once a
  second. An acknowledged write survives the process crashing; a power cut can lose about the
  last second.
- A crash in the middle of writing a record leaves an incomplete or damaged *last* record.
  Recovery drops it (it was never acknowledged) and logging continues after it. Damage anywhere
  else stops startup with an error, rather than silently losing the records after it.

**Snapshots** keep the log short. On `SAVE`, or once the log passes `--snapshot-after` bytes
(or the size of the last snapshot, if that's bigger, so a large dataset isn't rewritten after
every few writes), Tachyon writes the whole dataset to a temp file, flushes it, and renames it over the old
snapshot (`MoveFileEx` with `MOVEFILE_WRITE_THROUGH` on Windows; plus a directory `fsync` on
Linux). The rename is atomic, so the snapshot file is always either the complete old one or the
complete new one. Each log file has a generation number, and the snapshot names the generation
that continues after it, so a crash at any step either keeps the old snapshot and old log or uses
the new snapshot and new log: no change is ever lost or applied twice. Startup loads the
snapshot, then replays only the log after it. Snapshots run synchronously (Windows has no
`fork()`), which pauses the server for the length of the snapshot.

**When the disk is full:** a snapshot that can't be written fails cleanly (the temp file is
removed, the old snapshot and log stay in charge, the server keeps serving, and `INFO` shows
`rdb_last_bgsave_status:err`). A log write that fails stops the server without acknowledging
that round of writes, since memory would otherwise be ahead of the disk; every write
acknowledged before is safe. Free some space and restart.

**Upgrading from a pre-1.0 build:** those wrote one log file, `tachyon.wal`. If a data directory
has it and no newer files, Tachyon renames it to `tachyon.log.1` (same format) and loads it.

**What the tests prove:** `tools/crash_test.sh` kills the process (`kill -9` at random moments,
and an abort at each step of a snapshot), restarts it and checks every acknowledged write. That
proves crash safety. It doesn't prove power-cut safety: after a killed process, the operating
system still writes its cache to disk. Power-cut safety rests on the fsync order above.

## Benchmarks

`tools/benchmark.sh` runs Redis 7.0.15, Valkey 7.2.13 and Tachyon 1.0.0 (Release build) one
at a time on the same machine, with the same `redis-benchmark` settings: 50 clients, 200,000
requests per test (50,000 for the durable table). Requests per second, higher is better.
Machine: 16 vCPUs under WSL2 (Linux 5.15) on Windows 11, everything over loopback.

**In memory** (Redis/Valkey `--save '' --appendonly no`, Tachyon `--no-persist`)

| Test | Redis 7.0 | Valkey 7.2 | Tachyon | Tachyon / Redis |
| --- | ---: | ---: | ---: | ---: |
| PING_MBULK | 46,816 | 46,468 | 59,541 | 1.27 |
| SET | 50,100 | 49,714 | 60,459 | 1.21 |
| GET | 47,551 | 49,079 | 61,785 | 1.30 |
| LPUSH | 48,239 | 48,875 | 60,150 | 1.25 |
| LPOP | 48,297 | 50,709 | 58,190 | 1.20 |
| HSET | 47,281 | 50,163 | 61,842 | 1.31 |

**Durable: fsync before every acknowledged write** (Redis/Valkey `--appendonly yes
--appendfsync always`, Tachyon `--fsync always`)

| Test | Redis 7.0 | Valkey 7.2 | Tachyon | Tachyon / Redis |
| --- | ---: | ---: | ---: | ---: |
| SET | 4,852 | 4,872 | 4,748 | 0.98 |
| LPUSH | 4,852 | 4,856 | 4,859 | 1.00 |
| HSET | 4,754 | 4,739 | 4,749 | 1.00 |

**Pipelined: 16 commands per round trip, in memory**

| Test | Redis 7.0 | Valkey 7.2 | Tachyon | Tachyon / Redis |
| --- | ---: | ---: | ---: | ---: |
| SET | 819,672 | 784,313 | 896,860 | 1.09 |
| GET | 847,457 | 862,069 | 934,579 | 1.10 |

**How to read this.** Repeated runs vary by about 5% here, so the in-memory gap is real *on
this setup*, but it doesn't mean Tachyon is a faster Redis:

- Redis does more for every command: per-command statistics, LRU/LFU access times,
  keyspace-notification and ACL checks, several memory encodings per type, and a far larger
  feature set. Tachyon only does what its 31 commands need.
- Tachyon's event loop uses `poll()`, which looks at every connection on each pass. Redis uses
  `epoll`, which only reports the ready ones, so Redis pulls ahead with thousands of clients;
  50 clients is where `poll()` is still cheap.
- With an fsync before every acknowledgement, all three servers are limited by the disk
  (about 4,800 flushes a second here), so durable throughput is the same. All three batch the
  writes that arrive together into one flush (group commit).
- Pipelining removes most per-request network overhead, so the servers converge.

A profile (Valgrind's callgrind, since `perf` isn't available under WSL) drove one optimization
before these runs: the command lookup copied and lowercased every command name and compared it
against every table row, four times per command. Comparing in place cut the instructions per
request by 28%. The largest remaining costs are memory allocation and copying, one string per
argument and per reply.

Reproduce with `tools/benchmark.sh build/tachyon/tachyon` (a Release build, on an idle machine).

## Testing

| What | How |
| --- | --- |
| Unit tests (131) | GoogleTest: parser (every split position), log and snapshot recovery (every crash window), commands, expiry with a fake clock, pub/sub lifetimes |
| End to end | `tools/smoke_test.sh`: the real `redis-cli` and `redis-benchmark` (50 clients), misbehaving clients (HTTP, half-sent commands, a client that never reads), pub/sub between two `redis-cli` sessions, restart and snapshot |
| Compatibility | `tools/compare_with_redis.sh`: 150 commands and whole pub/sub sessions sent to a real Redis and to Tachyon; any difference fails |
| Crash safety | `tools/crash_test.sh <tachyon> 102`: 102 crashes, mid-write and mid-snapshot, no acknowledged write lost |
| Full disk | `tools/disk_full_test.sh`: a 2 MB RAM disk filled up: failed `SAVE`, failed log write, and automatic snapshots backing off (needs sudo) |
| Fuzzing | `tools/fuzz.sh [seconds]`: libFuzzer on the RESP parser and the log reader, with ASan and UBSan; each target checks a property, not just "no crash" |
| Sanitizers | Every Linux CI build runs the tests under AddressSanitizer and UndefinedBehaviorSanitizer |

CI (GitHub Actions) builds and tests on Ubuntu (GCC, sanitizers) and Windows (MSVC) on every
push, runs all the Linux scripts above, and fuzzes each target for a minute.

## Building

Requires CMake 3.20+ and a C++20 compiler: GCC or Clang on Linux, MSVC (Visual Studio 2022) on
Windows. GoogleTest is downloaded at configure time.

```
cmake -S . -B build
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

With sanitizers (Linux):

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
```

The scripts in `tools/` need `redis-tools` (and `redis-server` for the comparison); the fuzz
script needs Clang.

## Project layout

```
tachyon/src/   server: protocol/, commands/, server/, platform/{posix,win32}/, main.cpp
tachyon/tests/ unit tests
horizon/       storage engine: include/horizon/ (public API), src/, tests/
tools/         smoke, comparison, crash, full-disk, benchmark and fuzz scripts; fuzz/ targets
```

## License

MIT. See [LICENSE](LICENSE).
