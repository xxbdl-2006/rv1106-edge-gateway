#!/bin/bash
# Reliable adb wrapper: the adb server keeps dying between invocations, so
# every call re-establishes it and retries once. Without this, roughly half
# the calls fail with "no devices/emulators found" and the failures are
# indistinguishable from real board problems.
export MSYS_NO_PATHCONV=1

adb_sh() {
    local attempt
    for attempt in 1 2 3; do
        adb start-server >/dev/null 2>&1
        if out=$(adb shell "$@" 2>&1) && ! printf '%s' "$out" | grep -q "no devices/emulators found"; then
            printf '%s\n' "$out"
            return 0
        fi
        sleep 2
    done
    printf 'ADB_FAILED after 3 attempts: %s\n' "$*" >&2
    return 1
}

adb_plain() {
    local attempt
    for attempt in 1 2 3; do
        adb start-server >/dev/null 2>&1
        if out=$(adb "$@" 2>&1); then
            printf '%s\n' "$out"
            return 0
        fi
        sleep 2
    done
    printf 'ADB_FAILED: %s\n' "$*" >&2
    return 1
}
