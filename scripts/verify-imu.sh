#!/bin/bash
#
# Verify the real IMU data source on the board.
#
# Run this FROM WINDOWS (the board is plugged in here):
#
#     ./scripts/verify-imu.sh
#
# The build half is not in this script on purpose. Cross compiling only works
# in the VM, and this machine has no ARM toolchain, so the binary is expected
# to already exist:
#
#     cd /mnt/hgfs/luckfox_share/rv1103          # in the VM
#     make clean && make CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
#     make CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- imu-sample
#
# What is being checked, and why it needs a script rather than a glance:
#
#   1. The part answers and is the part we think it is (WHO_AM_I).
#   2. It streams samples at rate with zero bus errors.
#   3. A part sitting still reads 1.000 g -- not 0.000, which is what a
#      calibration that cancelled gravity produces, and not 1.062, which is
#      what an uncalibrated sample looks like because the module leans.
#   4. The encoder's overlay path consumes it: every frame annotated, nothing
#      refused, no sensor errors, and the frame rate did not move.
#   5. The GPIO pins are released afterwards.
#
# Points 3 and 5 are the ones that fail silently. A wrong |a| still draws a
# confident, level, plausible overlay, and a leaked gpio70/71 export does not
# hurt this run at all -- it makes the *next* one fail to open the bus, in a
# process that has already exited. Neither shows up as an error anywhere else.
#
# The gateway is stopped while this runs and always restarted, including on
# failure, via an EXIT trap.

set -u

# MSYS rewrites the remote paths handed to adb; see the long comment in
# verify-osd.sh. Harmless on Linux.
export MSYS_NO_PATHCONV=1

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="v4l2_mpp_encode"
PROBE="imu-sample"
BOARD_DIR="/userdata"

SECONDS_OF_SAMPLES="${SECONDS_OF_SAMPLES:-20}"
CLIP_FRAMES="${CLIP_FRAMES:-300}"
ADB_WAIT_S="${ADB_WAIT_S:-150}"

WORK_DIR="$REPO_DIR/.imu-verify"
IMU_LOG="$WORK_DIR/imu-sample.log"
OSD_LOG="$WORK_DIR/encoder-osd.log"

GATEWAY_WAS_RUNNING=0
BOARD_SIZE_PROBE=""

step() { printf '\n=== %s ===\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
note() { printf '  note %s\n' "$*"; }
die()  { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

to_native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

# The USB link to this board drops on its own, and it has now done so twice in
# the middle of a run - once right after the 3A server was started. It is a
# link problem, not a software one, and replugging brings it back within half a
# minute. Dying on the first miss means the run has to be driven again from the
# top, and worse, leaves the gateway stopped; waiting means a replug that
# happens while this is running just works.
#
# The hint is printed once, not every second, because the person being asked to
# replug cannot act faster than that and a screen full of it reads as a crash.
require_adb() {
    local waited=0 hinted=0

    while :; do
        adb devices 2>/dev/null | grep -q 'device$' && return 0

        if [ "$hinted" = "0" ]; then
            printf '\nThe board is not answering over USB.\n' >&2
            printf '  If the cable is seated, just wait; if the RNDIS adapter is gone,\n' >&2
            printf '  replug the board. Waiting up to %s s.\n' "$ADB_WAIT_S" >&2
            hinted=1
        fi

        if [ "$waited" -ge "$ADB_WAIT_S" ]; then
            printf '\nFAILED: no adb device after %s s.\n' "$ADB_WAIT_S" >&2
            printf '  This is a physical/link problem, not a software one.\n' >&2
            printf '  After replugging, if the gateway did not come back:\n' >&2
            printf '      adb shell "/etc/init.d/S99gateway start"\n' >&2
            exit 1
        fi

        sleep 2
        waited=$((waited + 2))
    done
}

on_board_running() {
    local name="$1"
    local pid
    pid="$(adb shell "pidof $name" 2>/dev/null | tr -d '\r')"
    [ -n "$pid" ]
}

# `kill -0` on this board answers "alive" for a dead process, so waiting is
# done with pidof. See verify-osd.sh for the full story.
wait_for_exit() {
    local name="$1" timeout_s="${2:-10}"
    local waited=0
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

# --------------------------------------------------------------------------
# 1. Preconditions: both binaries exist, are ARM, and there is room on the
#    board.
# --------------------------------------------------------------------------
check_elf() {
    local file="$1"
    local machine
    machine="$(od -An -tx1 -j18 -N2 "$file" 2>/dev/null | tr -d ' \n')"
    [ "$machine" = "2800" ]
}

check_artifacts() {
    step "checking the built binaries"

    for f in "$BINARY" "$PROBE"; do
        [ -f "$REPO_DIR/$f" ] || die "$f is missing. Build it in the VM first:
  cd /mnt/hgfs/luckfox_share/rv1103
  make CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- $f"
        check_elf "$REPO_DIR/$f" || die "$f is not an ARM binary (e_machine != 0x28).
  It was built for the host. Rebuild with CROSS_COMPILE set."
        ok "$f is ARM ($(wc -c < "$REPO_DIR/$f") bytes)"
    done

    # -kP: POSIX output, one line per mount, columns in a defined order. The
    # plain form on this board put something other than the free count in
    # field 4, which printed "2% KB free" and quietly skipped the check.
    local free_kb
    free_kb="$(adb shell "df -kP /userdata | tail -1" 2>/dev/null | tr -d '\r' \
        | awk '{print $4}')"
    if [ -n "$free_kb" ] && [ "$free_kb" -lt 20480 ] 2>/dev/null; then
        die "only ${free_kb} KB free on /userdata; a clip needs a few MB. Clear
  the debug files first (this partition has filled up before)."
    fi
    note "/userdata has ${free_kb} KB free"
}

# --------------------------------------------------------------------------
# 2. Stop the gateway and free the camera.
# --------------------------------------------------------------------------
stop_gateway() {
    step "stopping the gateway"

    local sup enc
    sup="$(adb shell "pidof gateway-supervise.sh" 2>/dev/null | tr -d '\r')"
    enc="$(adb shell "pidof v4l2_mpp_encode" 2>/dev/null | tr -d '\r')"

    if [ -n "$enc" ] || [ -n "$sup" ]; then
        GATEWAY_WAS_RUNNING=1
    fi

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
        note "capture node still held by: $holder"
        case "$holder" in
            *rkipc*)
                adb shell "kill ${holder%% *}" 2>/dev/null
                wait_for_exit rkipc 10 || true
                ok "rkipc released the node"
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
    require_adb

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

# --------------------------------------------------------------------------
# 3. Push both binaries, verifying the size each time.
#
# An interrupted push leaves a zero byte file that still shows up in ls, and
# the board then fails in a way that points at the code rather than at the
# transfer.
# --------------------------------------------------------------------------
push_binary() {
    local name="$1"
    local local_size board_size

    local_size="$(wc -c < "$REPO_DIR/$name")"

    # Retried because the link drops mid transfer: a push that fails leaves a
    # truncated file on the board, which is why the size is checked below
    # rather than trusted.
    local attempt=0
    while :; do
        adb push "$(to_native_path "$REPO_DIR/$name")" "$BOARD_DIR/" >/dev/null 2>&1
        board_size="$(adb shell "wc -c < $BOARD_DIR/$name" 2>/dev/null | tr -d '\r')"
        [ "$local_size" = "$board_size" ] && break

        attempt=$((attempt + 1))
        if [ "$attempt" -ge 3 ]; then
            break
        fi
        require_adb || exit 1
    done

    if [ "$local_size" != "$board_size" ]; then
        die "size mismatch after pushing $name: local $local_size, board $board_size.
  The transfer was truncated; do not run it."
    fi
    adb shell "chmod 755 $BOARD_DIR/$name"
    ok "pushed $name ($board_size bytes verified)"
}

# --------------------------------------------------------------------------
# 4. Run the IMU through the sensor_source interface.
#
# This is the primary evidence. It prints the WHO_AM_I, the sample rate, the
# error count and the at-rest magnitude, and it exits non-zero if the magnitude
# is not 1.000 or if any read failed.
# --------------------------------------------------------------------------
run_imu_sample() {
    step "reading the IMU through the sensor_source interface (${SECONDS_OF_SAMPLES}s)"

    adb shell "cd $BOARD_DIR && ./$PROBE --seconds $SECONDS_OF_SAMPLES" 2>&1 \
        | tr -d '\r' | tee "$IMU_LOG"

    local status="${PIPESTATUS[0]}"
    if [ "$status" != "0" ]; then
        die "$PROBE exited $status. See $IMU_LOG"
    fi

    grep -q "PASS" "$IMU_LOG" || die "$PROBE did not report PASS; see $IMU_LOG"

    # Pull the numbers out so the summary at the end can repeat them without
    # re-reading the whole log.
    local who samples errors magnitude
    who="$(grep -o 'WHO_AM_I=0x[0-9A-F]*' "$IMU_LOG" | head -1)"
    samples="$(grep -o 'samples=[0-9]*' "$IMU_LOG" | tail -1)"
    errors="$(grep -o 'errors=[0-9]*' "$IMU_LOG" | tail -1)"
    magnitude="$(grep -o '|a|=[0-9.]* g' "$IMU_LOG" | tail -1)"

    echo "$who" | grep -q '0x' || die "no WHO_AM_I line: the part did not answer"
    ok "identified: $who"
    ok "streamed: $samples, $errors"
    ok "at rest: $magnitude"

    case "$errors" in
        errors=0) ;;
        *) die "the IMU reported $errors during the run" ;;
    esac
}

# --------------------------------------------------------------------------
# 5. No leaked GPIO exports.
#
# A leaked gpio70/71 costs nothing now and breaks the next open, which is the
# worst kind of bug: the failure appears in a process that has already exited.
# --------------------------------------------------------------------------
check_gpio_clean() {
    step "checking for leaked GPIO exports"

    local leaked
    leaked="$(adb shell "ls /sys/class/gpio 2>/dev/null | grep -E '^gpio(70|71)\$'" \
        2>/dev/null | tr -d '\r')"

    if [ -n "$leaked" ]; then
        printf '  WARNING: still exported after the run: %s\n' "$leaked" >&2
        printf '  The next open will fail to export them. Clean up with:\n' >&2
        printf '      adb shell "echo 70 > /sys/class/gpio/unexport; echo 71 > /sys/class/gpio/unexport"\n' >&2
        return 0
    fi

    ok "gpio70/71 are not exported; the pins were released"
}

# --------------------------------------------------------------------------
# 6. The overlay path with the real source.
#
# --sink file, because the question is whether the encoder saw the text, and a
# file sink writes exactly the bytes the encoder produced with no network path
# in between to be blamed.
#
# --frames rather than a timeout: the program then exits on its own and prints
# the counters, which a signal would race with.
# --------------------------------------------------------------------------
run_encoder_with_imu_osd() {
    step "encoding ${CLIP_FRAMES} frames with --osd --osd-source mpu6050"

    adb shell ". /etc/profile.d/RkEnv.sh >/dev/null 2>&1; cd $BOARD_DIR && \
./$BINARY -d /dev/video11 -w 1280 -H 720 --warmup 30 --sink file \
-o $BOARD_DIR/imu-osd.h264 --threads --ring-slots 4 \
--osd --osd-source mpu6050 --frames $CLIP_FRAMES" 2>&1 \
        | tr -d '\r' | tee "$OSD_LOG"

    if ! grep -q "OSD annotated" "$OSD_LOG"; then
        die "the encoder never printed its OSD counters; see $OSD_LOG"
    fi

    local annotated refused polls samples errors
    annotated="$(grep -o 'annotated=[0-9]*' "$OSD_LOG" | head -1 | cut -d= -f2)"
    refused="$(grep -o 'composite_refused=[0-9]*' "$OSD_LOG" | head -1 | cut -d= -f2)"
    polls="$(grep -o 'polls=[0-9]*' "$OSD_LOG" | head -1 | cut -d= -f2)"
    samples="$(grep -o 'samples=[0-9]*' "$OSD_LOG" | head -1 | cut -d= -f2)"
    errors="$(grep -o 'errors=[0-9]*' "$OSD_LOG" | head -1 | cut -d= -f2)"

    printf '  annotated=%s composite_refused=%s sensor polls=%s samples=%s errors=%s\n' \
        "$annotated" "$refused" "$polls" "$samples" "$errors"

    [ "$annotated" = "$CLIP_FRAMES" ] \
        || die "annotated=$annotated, expected $CLIP_FRAMES: some frames went out
  without the overlay (passed_through>0 means the annotator was not reached)."
    [ "$refused" = "0" ] \
        || die "composite_refused=$refused: the canvas did not fit the frame, so the
  overlay silently vanished on those frames."
    [ "$errors" = "0" ] \
        || die "the sensor source reported $errors errors while encoding."
    [ "${samples:-0}" -gt 0 ] 2>/dev/null \
        || die "the feed produced no samples (polls=$polls, samples=${samples:-none}):
  the source is connected but idle."

    ok "every frame annotated, no refusals, no sensor errors"

    # The program prints "Average FPS  : 21.359", not "21.359 fps". Matching
    # the wrong form silently reports "not reported" instead of the number
    # this whole script exists to watch.
    local fps
    fps="$(grep -oE 'Average FPS *: *[0-9]+\.[0-9]+' "$OSD_LOG" \
        | grep -oE '[0-9]+\.[0-9]+' | head -1)"
    note "frame rate: ${fps:-not reported} fps"
    note "compare against 30.001 fps for the same encode with no overlay:"
    note "    $REPO_DIR/scripts/fps-osd-compare.sh"
}

# --------------------------------------------------------------------------

main() {
    rm -rf "$WORK_DIR"
    mkdir -p "$WORK_DIR"

    step "preconditions"
    require_adb
    check_artifacts

    stop_gateway
    push_binary "$BINARY"
    push_binary "$PROBE"

    run_imu_sample
    check_gpio_clean
    run_encoder_with_imu_osd
    check_gpio_clean

    step "PASS"
    printf '  The real IMU is feeding the overlay.\n'
    printf '  Logs and clips: %s\n' "$WORK_DIR"
}

main "$@"
