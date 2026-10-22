# Tachyon

Tachyon is a Redis-compatible key-value server written in C++20, with its own storage engine,
**Horizon**, for crash-safe persistence. It speaks RESP2, the Redis wire protocol, so the real
`redis-cli` and `redis-benchmark` work against it unchanged. It builds and runs on Linux and
Windows with no dependencies beyond the C++ standard library.

## Components

| Component | Role |
| --- | --- |
| `tachyon/` | The server: RESP parser and reply writer, commands, event loop, networking |
| `horizon/` | Storage engine library: write-ahead log and crash recovery; snapshots planned |

Horizon knows nothing about networking or RESP; it stores and replays opaque records, so any
program can use it. All OS-specific code lives in `tachyon/src/platform/`, with one POSIX and one
Win32 implementation behind a single header.

## Supported commands

| Command | Notes |
| --- | --- |
| `PING [message]` | |
| `ECHO message` | |
| `GET key` | |
| `SET key value` | Options (`EX`, `PX`, `NX`, `XX`) are planned |
| `DEL key [key ...]` | Returns the number of keys removed |
| `EXISTS key [key ...]` | Returns the number of keys that exist |

Only RESP arrays are accepted as commands, so stray input such as an HTTP request is rejected
instead of being run.

## Build and test

Requires CMake 3.20+ and a C++20 compiler (GCC, Clang or MSVC). GoogleTest is downloaded at
configure time.

```
cmake -S . -B build
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

On Linux, `tools/smoke_test.sh build/tachyon/tachyon` runs the real `redis-cli` and
`redis-benchmark` (50 concurrent clients) against the server, along with misbehaving clients:
an HTTP request, a half-sent command, a disconnect mid-command and a client that never reads.

```
redis-benchmark -p 6380 -c 50 -t ping_mbulk,set,get
```

`PING_INLINE` is skipped because Tachyon accepts only RESP arrays, not inline commands.

`tools/crash_test.sh build/tachyon/tachyon 100` checks durability: it writes keys, kills the
server with `kill -9` at a random moment, restarts it and verifies every acknowledged write, 100
times in a row.

## Quick start

Tachyon listens on `127.0.0.1:6380`, so it never clashes with a real Redis on 6379.

```
./build/tachyon/tachyon          # Windows: build\tachyon\Debug\tachyon.exe
redis-cli -p 6380 SET greeting hello
redis-cli -p 6380 GET greeting
```

Press Ctrl+C to shut the server down cleanly, even with clients connected.

| Option | Default | Meaning |
| --- | --- | --- |
| `--port N` | `6380` | TCP port to listen on (always on 127.0.0.1) |
| `--dir PATH` | `.` | Existing directory for the log file, `tachyon.wal` |
| `--fsync always\|everysec` | `always` | When the log is flushed to disk (see below) |
| `--no-persist` | off | Keep data in memory only; write nothing to disk |

## Durability

Every write command is appended to a write-ahead log through Horizon before its reply is sent.
Each record is a 4-byte length, a CRC32 checksum and the command itself, in the same RESP form a
client sends. On startup Tachyon replays the log into memory before accepting clients.

- `--fsync always` (default): the log is flushed to disk (`fsync` on Linux,
  `FlushFileBuffers` on Windows) before any reply that acknowledges a write. Writes that arrive
  together share one flush (group commit). An acknowledged write survives a crash or a power cut.
- `--fsync everysec`: the log is handed to the OS before each reply but flushed to disk at most
  once a second. An acknowledged write survives the process crashing, but a power cut can lose
  about the last second.
- A crash in the middle of writing a record leaves an incomplete or damaged last record. Replay
  drops it (it was never acknowledged) and logging continues after the last valid record. Damage
  anywhere else stops startup with an error instead of silently dropping data.

The crash test kills the process, which proves crash safety, not power-cut safety: after a
killed process the operating system still writes its cache to disk. The log grows without limit
until snapshots and compaction arrive in Phase 6.

## How it serves many clients

Like Redis, Tachyon runs every command on one thread. All sockets are non-blocking, and an
event loop built on `poll()` (Linux) or `WSAPoll()` (Windows) waits on all of them at once and
handles whichever are ready. Each connection has its own read buffer (the parser, holding any
half-received command) and write buffer (replies the OS hasn't accepted yet), so a slow or idle
client never holds up the others. A client that stops reading its replies is no longer read
from once 1 MB of replies is queued for it. Up to 1000 clients are accepted.

## Status and roadmap

| Phase | Focus | Status |
| --- | --- | --- |
| 0–1 | Build, tests, CI, RESP parser, core commands, blocking TCP server | Done |
| 2 | Non-blocking event loop (`poll` / `WSAPoll`) and many clients | Done |
| 3 | Durability with a write-ahead log | Done |
| 4 | Hashes, lists and key expiry | Planned |
| 5 | Pub/sub | Planned |
| 6 | Snapshots and log compaction | Planned |
| 7 | Fuzzing, benchmarks against Redis, v1.0 release | Planned |

## License

MIT. See [LICENSE](LICENSE).
