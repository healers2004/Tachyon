# Tachyon

Tachyon is a Redis-compatible key-value server written in C++20, with its own storage engine,
**Horizon**, for crash-safe persistence. It speaks RESP2, the Redis wire protocol, so the real
`redis-cli` and `redis-benchmark` work against it unchanged. It builds and runs on Linux and
Windows with no dependencies beyond the C++ standard library.

## Components

| Component | Role |
| --- | --- |
| `tachyon/` | The server: RESP parser and reply writer, commands, networking |
| `horizon/` | Storage engine library: write-ahead log, snapshots, crash recovery (in progress) |

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

On Linux, `tools/smoke_test.sh build/tachyon/tachyon` runs the real `redis-cli` against the
server.

## Quick start

Tachyon listens on `127.0.0.1:6380`, so it never clashes with a real Redis on 6379.

```
./build/tachyon/tachyon          # Windows: build\tachyon\Debug\tachyon.exe
redis-cli -p 6380 SET greeting hello
redis-cli -p 6380 GET greeting
```

Press Ctrl+C to shut the server down cleanly.

## Status and roadmap

Currently the server handles one client at a time and keeps data in memory only.

| Phase | Focus | Status |
| --- | --- | --- |
| 0–1 | Build, tests, CI, RESP parser, core commands, blocking TCP server | Done |
| 2 | Non-blocking event loop (`poll` / `WSAPoll`) and many clients | Planned |
| 3 | Durability with a write-ahead log | Planned |
| 4 | Hashes, lists and key expiry | Planned |
| 5 | Pub/sub | Planned |
| 6 | Snapshots and log compaction | Planned |
| 7 | Fuzzing, benchmarks against Redis, v1.0 release | Planned |

## License

MIT. See [LICENSE](LICENSE).
