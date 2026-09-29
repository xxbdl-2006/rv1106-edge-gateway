#!/bin/bash
#
# Attribute a frame rate difference to a cause.
#
#     ./scripts/fps-osd-compare.sh [frames]
#
# Runs the same encode three times -- no overlay, mock overlay, real IMU
# overlay -- and prints the frame rate of each. Run it after a change that
# touches the overlay path, or after a run whose fps looked wrong.
#
# Why it exists: a single fps number cannot be read on its own. The encoder
# has been seen at 30.000 fps in the long soak and at 21.359 fps on the first
# IMU run, and that difference has three candidate causes -- the IMU's I2C
# bursts, the overlay compositing, or the file sink / ring configuration the
# run happened to use. All three predict the same single number, so measuring
# one configuration measures almost nothing. What separates them is the
# *difference* between configurations measured back to back, on the same
# board, in the same minute.
#
# The three runs are ordered cheapest first so that a failure in the IMU run
# still leaves the other two columns filled in.
#
# The gateway is stopped while this runs and always restarted, including on
# failure, via an EXIT trap.

set -u

export MSYS_NO_PATHCONV=1

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="v4l2_mpp_encode"
BOARD_DIR="/userdata"
FRAMES="${1:-300}"
ADB_WAIT_S="${ADB_WAIT_S:-150}"

WORK_DIR="$REPO_DIR/.fps-compare"
GATEWAY_WAS_RUNNING=0

step() { printf '\n=== %s ===\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
note() { printf '  note %s\n' "$*"; }
die()  { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

# e_machine at offset 18; 0x28 is ARM. An x86 binary pushed to the board fails
# in a way that points at the code rather than at the build.
check_elf() {
    local machine
    machine="$(od -An -tx1 -j18 -N2 "$1" 2>/dev/null | tr -d ' \n')"
    [ "$machine" = "2800" ]
}

# The USB link drops on its own and has taken a run with it twice. Waiting for
# it to come back beats dying, because a replug that happens while this script
# is sitting here just works. See the longer comment in verify-imu.sh.
require_adb() {
    local waited=0 hinted=0

    while :; do
        adb devices 2>/dev/null | grep -q 'device$' && return 0

        if [ "$hinted" = "0" ]; then
            printf '\nThe board is not answering over USB. Replug it if the\n' >&2
            printf '  RNDIS adapter is gone; waiting up to %s s.\n' "$ADB_WAIT_S" >&2
            hinted=1
        fi

        if [ "$waited" -ge "$ADB_WAIT_S" ]; then
            printf '\nFAILED: no adb device after %s s.\n' "$ADB_WAIT_S" >&2
            return 1
        fi

        sleep 2
        waited=$((waited + 2))
    done
}

on_board_running() {
    local pid
    pid="$(adb shell "pidof $1" 2>/dev/null | tr -d '\r')"
    [ -n "$pid" ]
}

wait_for_exit() {
    local name="$1" timeout_s="${2:-10}" waited=0
    while on_board_running "$name"; do
        if [ "$waited" -ge "$((timeout_s * 2))" ]; then
            return 1
        fi
        sleep 0.5
        waited=$((waited + 1))
    done
    return 0
}

capture_holder() {
    adb shell "for p in \$(ls /proc | grep -E '^[0-9]+\$'); do
                   if ls -l /proc/\$p/fd 2>/dev/null | grep -q video11; then
                       printf '%s %s\n' \"\$p\" \"\$(cat /proc/\$p/comm 2>/dev/null)\"
                   fi
               done" 2>/dev/null | tr -d '\r' | head -1
}

stop_gateway() {
    step "stopping the gateway"

    local sup enc
    sup="$(adb shell "pidof gateway-supervise.sh" 2>/dev/null | tr -d '\r')"
    enc="$(adb shell "pidof $BINARY" 2>/dev/null | tr -d '\r')"

    [ -n "$sup" ] || [ -n "$enc" ] && GATEWAY_WAS_RUNNING=1

    # Supervisor first: killing the encoder alone just gets it restarted.
    if [ -n "$sup" ]; then
        adb shell "kill $sup" 2>/dev/null
        ok "supervisor $sup stopped"
    else
        note "no supervisor running"
    fi

    if [ -n "$enc" ]; then
        adb shell "kill $enc" 2>/dev/null
        if wait_for_exit "$BINARY" 10; then
            ok "encoder $enc stopped"
        else
            adb shell "kill -9 $enc" 2>/dev/null
            wait_for_exit "$BINARY" 5 || true
            note "encoder needed SIGKILL"
        fi
    else
        note "no encoder running"
    fi

    sleep 1

    local holder
    holder="$(capture_holder)"
    if [ -n "$holder" ]; then
        case "$holder" in
            *rkipc*)
                adb shell "kill ${holder%% *}" 2>/dev/null
                wait_for_exit rkipc 10 || true
                ok "rkipc released the capture node"
                ;;
            *)
                note "capture node still held by: $holder"
                ;;
        esac
    else
        ok "capture node free"
    fi

    if ! on_board_running rkaiq_3A_server; then
        adb shell ". /etc/profile.d/RkEnv.sh >/dev/null 2>&1; setsid rkaiq_3A_server &" \
            >/dev/null 2>&1
        sleep 2
        note "started rkaiq_3A_server"
    fi
}

restore_gateway() {
    [ "$GATEWAY_WAS_RUNNING" = "1" ] || return 0

    step "restoring the gateway"

    # Waited for here too: this is the last thing the script does, and giving up
    # on it is what leaves the board with no gateway at all.
    require_adb || return 0

    adb shell "/etc/init.d/S99gateway start" >/dev/null 2>&1

    local waited=0
    while [ "$waited" -lt 40 ]; do
        if on_board_running "$BINARY"; then
            ok "gateway is back (pid $(adb shell "pidof $BINARY" 2>/dev/null | tr -d '\r'))"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done

    printf '  WARNING: the gateway did not come back within 40 s.\n' >&2
    printf '  Check: adb shell "/etc/init.d/S99gateway status"\n' >&2
    return 0
}

trap restore_gateway EXIT

# One configuration. Prints the numbers that matter and nothing else, so the
# three columns below can be compared line by line.
#
# Average FPS is the number, but dropped_busy is the one that says *why*: it
# counts frames the capture thread produced while the encoder was still busy
# with the previous one, i.e. the encoder, not the sensor, is the bottleneck.
run_config() {
    local label="$1"
    shift
    local log="$WORK_DIR/$label.log"
    local out="$BOARD_DIR/fps-$label.h264"

    printf '\n--- %s ---\n' "$label"

    # Checked before every run, not just once at the top: the link has dropped
    # in the middle of a sweep, and a configuration measured while the board
    # was unreachable produces an empty row that looks like a result.
    require_adb || return 1

    adb shell ". /etc/profile.d/RkEnv.sh >/dev/null 2>&1; cd $BOARD_DIR && \
./$BINARY -d /dev/video11 -w 1280 -H 720 --warmup 30 --sink file \
-o $out --threads --ring-slots 4 --frames $FRAMES $*" 2>&1 \
        | tr -d '\r' > "$log"

    local fps dropped skipped annotated
    fps="$(grep -oE 'Average FPS *: *[0-9]+\.[0-9]+' "$log" | grep -oE '[0-9]+\.[0-9]+' | head -1)"
    dropped="$(grep -o 'dropped_busy=[0-9]*' "$log" | head -1 | cut -d= -f2)"
    skipped="$(grep -o 'skipped=[0-9]*' "$log" | head -1 | cut -d= -f2)"
    annotated="$(grep -o 'annotated=[0-9]*' "$log" | head -1 | cut -d= -f2)"

    printf '  fps=%s dropped_busy=%s capture_skipped=%s annotated=%s\n' \
        "${fps:-?}" "${dropped:-?}" "${skipped:-?}" "${annotated:--}"

    adb shell "rm -f $out" >/dev/null 2>&1

    RESULT_FPS="$fps"
    RESULT_DROPPED="$dropped"
    RESULT_SKIPPED="$skipped"
}

# One retry, because the link dropping mid run costs a whole configuration and
# a sweep is only worth anything if every row was really measured.
run_config_once() {
    run_config "$@"
    if [ -z "${RESULT_FPS:-}" ]; then
        note "no result, waiting for the board and retrying"
        require_adb && run_config "$@"
    fi
}

# Which interval actually costs nothing is a question about the bus, not about
# the code, so it is answered by sweeping. Set SWEEP_MS to a comma separated
# list and this replaces the three fixed configurations with a baseline plus one
# run per interval - all in the same session, which is the only way the numbers
# mean anything (see the header).
#
# The 10 ms point is kept in every sweep on purpose. It is the value that was
# proven to cost 30 fps, so it is the control: if it does not still lose frames
# here, the measurement is not measuring what it claims to.
run_sweep() {
    run_config_once none
    printf '\n  baseline (no overlay): %s fps\n' "$RESULT_FPS"

    local ms
    for ms in $(printf '%s' "$SWEEP_MS" | tr ',' ' '); do
        run_config_once "imu-${ms}ms" --osd --osd-source mpu6050 \
            --osd-imu-interval-ms "$ms"
    done

    step "result"
    printf '  Read each row against the baseline above. The interval is right\n'
    printf '  when the frame rate stops moving and dropped_busy stays at 0.\n'
    printf '  Logs: %s\n' "$WORK_DIR"
}

main() {
    rm -rf "$WORK_DIR"
    mkdir -p "$WORK_DIR"

    step "preconditions"
    require_adb || exit 1
    [ -f "$REPO_DIR/$BINARY" ] \
        || die "$BINARY is missing; build it in the VM first."
    check_elf "$REPO_DIR/$BINARY" \
        || die "$BINARY is not ARM. Rebuild with CROSS_COMPILE set."

    stop_gateway

    if [ -n "${SWEEP_MS:-}" ]; then
        run_sweep
        return 0
    fi

    local local_size board_size attempt=0
    local_size="$(wc -c < "$REPO_DIR/$BINARY")"
    while :; do
        adb push "$(cygpath -m "$REPO_DIR/$BINARY" 2>/dev/null \
            || printf '%s' "$REPO_DIR/$BINARY")" "$BOARD_DIR/" >/dev/null 2>&1
        board_size="$(adb shell "wc -c < $BOARD_DIR/$BINARY" 2>/dev/null | tr -d '\r')"
        [ "$local_size" = "$board_size" ] && break

        attempt=$((attempt + 1))
        [ "$attempt" -ge 3 ] && die "could not push $BINARY intact: local $local_size, board ${board_size:-nothing}"
        require_adb || exit 1
    done
    adb shell "chmod 755 $BOARD_DIR/$BINARY"
    ok "pushed $BINARY ($board_size bytes verified)"

    step "three configurations, ${FRAMES} frames each"

    run_config_once none
    local fps_none="$RESULT_FPS" drop_none="$RESULT_DROPPED"

    run_config_once mock --osd --osd-source mock
    local fps_mock="$RESULT_FPS" drop_mock="$RESULT_DROPPED"

    run_config_once mpu6050 --osd --osd-source mpu6050
    local fps_imu="$RESULT_FPS" drop_imu="$RESULT_DROPPED"

    step "result"
    printf '  %-10s %10s %14s\n' "config" "fps" "dropped_busy"
    printf '  %-10s %10s %14s\n' "none" "$fps_none" "$drop_none"
    printf '  %-10s %10s %14s\n' "mock" "$fps_mock" "$drop_mock"
    printf '  %-10s %10s %14s\n' "mpu6050" "$fps_imu" "$drop_imu"
    printf '\n  Read the gap between none and mock as the cost of compositing,\n'
    printf '  and the gap between mock and mpu6050 as the cost of the I2C bursts.\n'
    printf '  Logs: %s\n' "$WORK_DIR"
}

main "$@"
