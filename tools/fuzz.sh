#!/usr/bin/env bash
# Builds the libFuzzer targets with Clang and runs each one for a while. Every function in
# tachyon and horizon is built with coverage tracking (-fsanitize=fuzzer-no-link), so libFuzzer
# can tell which inputs reach new code, and with ASan and UBSan, so any memory or arithmetic
# error stops the run with a report and the input that caused it.
# Usage: tools/fuzz.sh [seconds per target, default 60] [build dir, default ~/tachyon-fuzz]
set -euo pipefail

seconds="${1:-60}"
build="${2:-$HOME/tachyon-fuzz}"
source_dir="$(cd "$(dirname "$0")/.." && pwd)"
flags="-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g"

cmake -S "$source_dir" -B "$build" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_FLAGS="$flags" -DTACHYON_FUZZ=ON >/dev/null
cmake --build "$build" --target fuzz_resp_parser fuzz_log_reader >/dev/null

# Seeds: a few valid inputs to start from, so libFuzzer doesn't have to discover RESP from
# scratch. Everything it finds interesting is added to the corpus folder and kept between runs.
mkdir -p "$build/corpus/resp" "$build/corpus/log"
printf '*1\r\n$4\r\nPING\r\n' >"$build/corpus/resp/ping"
printf '*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n' \
    >"$build/corpus/resp/pipeline"
# One record: length 9, CRC32 of "123456789" (0xCBF43926, lowest byte first), payload.
printf '\x09\x00\x00\x00\x26\x39\xf4\xcb123456789' >"$build/corpus/log/one-record"

rm -f "$build"/crash-* # problems from an earlier run, already looked at
for target in resp_parser log_reader; do
    corpus="$build/corpus/resp"
    [ "$target" = log_reader ] && corpus="$build/corpus/log"
    echo "fuzzing $target for $seconds s..."
    # -max_len: inputs up to 64 KB, well under the parser's 1 MB limit, so both ways of
    # feeding the same input stay comparable. -timeout: any input taking over 10 s is a bug.
    status=0
    "$build/tools/fuzz/fuzz_$target" "$corpus" -max_total_time="$seconds" -max_len=65536 \
        -timeout=10 -print_final_stats=1 -artifact_prefix="$build/crash-$target-" \
        >"$build/$target.out" 2>&1 || status=$?
    grep -E "stat::number_of_executed_units|stat::new_units_added|ERROR|SUMMARY" \
        "$build/$target.out" || true
    # libFuzzer exits non-zero and writes a crash-* file holding the input when it finds one.
    if [ "$status" -ne 0 ]; then
        echo "FAIL: $target found a problem (full report: $build/$target.out); the input:"
        ls "$build"/crash-"$target"-*
        exit 1
    fi
done
echo "ok: no crashes, leaks or failed checks"
