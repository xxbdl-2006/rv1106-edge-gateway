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

WORK_DIR="$REPO_DIR/.fps-compare"
GATEWAY_WAS_RUNNING=0

step() { printf '\n=== %s ===\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
note() { printf '  note %s\n' "$*"; }
die()  { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

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

main() {
    rm -rf "$WORK_DIR"
    mkdir -p "$WORK_DIR"

    step "preconditions"
    adb devices 2>/dev/null | grep -q 'device$' \
        || die "no adb device. Check the USB link before blaming the software."
    [ -f "$REPO_DIR/$BINARY" ] \
        || die "$BINARY is missing; build it in the VM first."

    stop_gateway

    adb push "$(cygpath -m "$REPO_DIR/$BINARY" 2>/dev/null || printf '%s' "$REPO_DIR/$BINARY")" \
        "$BOARD_DIR/" >/dev/null 2>&1 || die "adb push $BINARY failed"
    adb shell "chmod 755 $BOARD_DIR/$BINARY"
    ok "pushed $BINARY"

    step "three configurations, ${FRAMES} frames each"

    run_config none
    local fps_none="$RESULT_FPS" drop_none="$RESULT_DROPPED"

    run_config mock --osd --osd-source mock
    local fps_mock="$RESULT_FPS" drop_mock="$RESULT_DROPPED"

    run_config mpu6050 --osd --osd-source mpu6050
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
