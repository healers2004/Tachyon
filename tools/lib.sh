# Helpers shared by the test scripts in tools/. Load it with: source "$(dirname "$0")/lib.sh"

# wait_for_port <port> [seconds, default 5]: waits until a server on <port> answers PING.
# Returns 1 if it never does.
wait_for_port() {
    local tries=$((${2:-5} * 10))
    for _ in $(seq "$tries"); do
        redis-cli -p "$1" PING >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    return 1
}

# wait_for_exit <pid> [seconds, default 5]: waits until the process has exited.
# Returns 1 if it's still running.
wait_for_exit() {
    local tries=$((${2:-5} * 10))
    for _ in $(seq "$tries"); do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.1
    done
    ! kill -0 "$1" 2>/dev/null
}
