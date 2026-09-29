#!/bin/bash
#
# Soak the gateway with the real IMU overlay for a bounded number of frames.
#
#   bash scripts/imu-soak.sh              # 30 minutes
#   DURATION_MIN=60 bash scripts/imu-soak.sh
#
# Everything the short verifications cannot answer is a question about time, and
# the board answers it whether or not this machine is watching: the encoder runs
# under setsid on the board and writes both its own log and a CSV of memory,
# file descriptors, threads, CPU and temperature. That is the point of the lay-
# out. The USB link to this board drops every twenty minutes or so, and a run
# that has to be watched from Windows is a run that gets invalidated by a cable.
#
# The encoder is bounded with --frames rather than killed. A bounded run
# ends by itself and prints its whole summary - frames, frame rate, overlay and
# sensor counters - where a process stopped by a signal may not.
#
# What is actually being looked for, in the order it would be found:
#
#   fds      Should be flat. gpio_sysfs.c holds one descriptor for `direction`
#            and one for `value` per pin, opened once at setup, so there is no
#            per transaction open to leak. A climb here means that changed.
#   rss_kb   A climb in the tail, not a high value. The first megabyte and a
#            half is the allocator warming up, which is why the baseline eight
#            hour run was judged on the tail.
#   errors   The sensor counter in the final summary. A bus that wedges once in
#            ten thousand transactions needs tens of thousands of transactions,
#            which is what the duration buys: 30 minutes at 10 Hz is 18000.
#   |a|      Drifts while the part warms up, then settles. Only a run long
#            enough to pass the warm up can say the at rest magnitude is stable.

set -u

# Without this, Git Bash rewrites every argument that looks like a POSIX path
# for the Windows adb, and that includes the DESTINATION: `adb push f /userdata/`
# becomes a push into C:/.../PortableGit/.../userdata/ and fails with "remote No
# such file or directory", which reads like the board is broken. It has to be
# exported here rather than left to the caller's shell, because every tool call
# starts a fresh shell.
export MSYS_NO_PATHCONV=1

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD_DIR="/userdata"
BINARY="v4l2_mpp_encode"

DURATION_MIN="${DURATION_MIN:-30}"
INTERVAL_S="${INTERVAL_S:-60}"
ADB_WAIT_S="${ADB_WAIT_S:-150}"

# the encoder counts. 30 fps is what the capture offers once the 3A server has
# converged, so this is the duration expressed in the unit the program uses.
FRAMES="${FRAMES:-$((DURATION_MIN * 60 * 30))}"

# The command line comes from scripts/gateway.env below - the same file
# install_autostart.ps1 puts on the board, so what gets soaked is what ships.
# Soaked as it would run rather than as something invented here: a soak of a
# different configuration proves something about a configuration nobody runs.

SOAK_LOG="$BOARD_DIR/imu-soak.log"
SOAK_CSV="$BOARD_DIR/soak.csv"

# MSYS_NO_PATHCONV above keeps board paths literal, but it also stops Git Bash
# from rewriting POSIX paths for Windows programs - and adb and readelf are
# Windows programs. So every path that names a file on THIS machine has to be
# converted explicitly, while every path that names a file on the board must
# not be. Getting this backwards is not a crash: adb reports "No such file or
# directory" for a file that is plainly there.
# The command line comes from scripts/gateway.env, which is the same file
# install_autostart.ps1 puts on the board. Soaking anything else proves
# something about a configuration nobody runs - and worse, it silently stops
# being reproducible once someone edits the real one.
#
# Read after the helpers above: this wants to die if the file is missing.
GATEWAY_ENV="$REPO_DIR/scripts/gateway.env"
[ -f "$GATEWAY_ENV" ] || die "missing $GATEWAY_ENV; it defines what gets soaked"
# shellcheck source=/dev/null
. "$GATEWAY_ENV"
PROD_ARGS="${GATEWAY_ARGS:-}"
[ -n "$PROD_ARGS" ] || die "$GATEWAY_ENV sets no GATEWAY_ARGS"
case " $PROD_ARGS " in
    *" --sink rtsp "*) ;;
    *) die "this soaks the production line, so it needs --sink rtsp in $GATEWAY_ENV" ;;
esac
#
# Whatever is configured gets soaked, overlay included, so there is nothing to
# add here. Set EXTRA_ARGS to try a variant of the production line rather than
# a different line, which keeps the comparison honest and the commands short.
#
PROD_ARGS="${PROD_ARGS}${EXTRA_ARGS:+ $EXTRA_ARGS}"

to_native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

step() { printf '\n=== %s ===\n' "$1"; }
ok()   { printf '  ok   %s\n' "$1"; }
note() { printf '  ..   %s\n' "$1"; }
die()  { printf '\nFAILED: %s\n' "$1" >&2; exit 1; }

adb_sh() { adb shell "$@" 2>/dev/null | tr -d '\r'; }

require_adb() {
    local waited=0 hinted=0
    while [ "$waited" -lt "$ADB_WAIT_S" ]; do
        if adb devices 2>/dev/null | grep -q 'device$'; then
            return 0
        fi
        if [ "$hinted" -eq 0 ]; then
            printf '\n  ..   no adb device; waiting up to %ss for the board.\n' \
                "$ADB_WAIT_S"
            printf '       If the USB link is gone, replug the board - this\n'
            printf '       script picks up where it left off.\n'
            hinted=1
        fi
        sleep 2
        waited=$((waited + 2))
    done
    die "no adb device after ${ADB_WAIT_S}s. The board has to be reachable."
}

wait_for_exit() {
    local name="$1" limit="$2" i=0
    while [ "$i" -lt "$limit" ]; do
        pid="$(adb_sh "pidof $name")"
        [ -z "$pid" ] && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

on_board_running() {
    [ -n "$(adb_sh "pidof $1")" ]
}

capture_holder() {
    adb_sh "fuser /dev/video11 2>/dev/null"
}

stop_gateway() {
    step "stopping the gateway"

    local sup enc
    sup="$(adb_sh "pidof gateway-supervise.sh")"
    enc="$(adb_sh "pidof $BINARY")"

    # Supervisor first: killing the encoder alone just gets it restarted.
    if [ -n "$sup" ]; then
        adb_sh "kill $sup" >/dev/null
        ok "supervisor $sup stopped"
    else
        note "no supervisor running"
    fi

    if [ -n "$enc" ]; then
        adb_sh "kill $enc" >/dev/null
        if wait_for_exit "$BINARY" 10; then
            ok "encoder $enc stopped"
        else
            adb_sh "kill -9 $enc" >/dev/null
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
                adb_sh "kill ${holder%% *}" >/dev/null
                wait_for_exit rkipc 10 || true
                ok "rkipc released the node"
                ;;
        esac
    else
        ok "capture node free"
    fi

    if on_board_running rkaiq_3A_server; then
        # Left up on purpose. A freshly started 3A server has not converged
        # exposure yet and capture only offers 25 fps until it has, which would
        # make this run's frame rate a fact about the 3A server rather than
        # about the overlay.
        ok "rkaiq_3A_server already running (left alone, exposure converged)"
    else
        adb_sh ". /etc/profile.d/RkEnv.sh >/dev/null 2>&1; setsid rkaiq_3A_server &" \
            >/dev/null
        sleep 3
        note "started rkaiq_3A_server; capture may run at 25 fps until it settles"
    fi
}

restore_gateway() {
    step "restoring the gateway"
    require_adb
    adb_sh "/etc/init.d/S99gateway start" >/dev/null
    sleep 10
    if on_board_running "$BINARY"; then
        ok "gateway back up (pid $(adb_sh "pidof $BINARY"))"
    else
        note "gateway did not come back: adb shell \"/etc/init.d/S99gateway start\""
    fi
}

cleanup() {
    adb_sh "kill \$(cat /tmp/soak-monitor.pid 2>/dev/null) 2>/dev/null" >/dev/null
    restore_gateway
}
trap cleanup EXIT

main() {
    step "preconditions"
    require_adb

    [ -f "$REPO_DIR/$BINARY" ] || die "$BINARY is missing; build it in the VM first."
    if command -v readelf >/dev/null 2>&1; then
        readelf -h "$(to_native_path "$REPO_DIR/$BINARY")" 2>/dev/null \
            | grep -q 'ARM' \
            || die "$BINARY is not an ARM binary; it was built for this machine."
        ok "$BINARY is an ARM binary"
    else
        note "readelf not on PATH; skipped the architecture check"
    fi

    # The previous run's history has to go, or a climb in this run reads as a
    # continuation of the last one. Kept as .prev rather than deleted.
    adb_sh "if [ -s $SOAK_CSV ]; then mv $SOAK_CSV $SOAK_CSV.prev; fi" >/dev/null
    ok "rotated $SOAK_CSV"

    step "installing the sampler"
    local local_size board_size
    local_size="$(wc -c < "$REPO_DIR/scripts/soak-monitor.sh" | tr -d ' ')"
    adb push "$(to_native_path "$REPO_DIR/scripts/soak-monitor.sh")" \
        "$BOARD_DIR/" >/dev/null 2>&1 \
        || die "adb push soak-monitor.sh failed"
    adb_sh "chmod 755 $BOARD_DIR/soak-monitor.sh"
    board_size="$(adb_sh "wc -c < $BOARD_DIR/soak-monitor.sh")"
    [ "$local_size" = "$board_size" ] \
        || die "soak-monitor.sh truncated in transit: local $local_size, board $board_size"
    ok "soak-monitor.sh installed, $board_size bytes (${INTERVAL_S}s interval)"

    stop_gateway

    step "starting the run"
    adb_sh ". /etc/profile.d/RkEnv.sh >/dev/null 2>&1; \
            setsid $BOARD_DIR/soak-monitor.sh $INTERVAL_S \
                >/dev/null 2>&1 </dev/null & \
            sleep 2; echo sampler started" >/dev/null
    sleep 1

    adb_sh ". /etc/profile.d/RkEnv.sh >/dev/null 2>&1; cd $BOARD_DIR && \
            setsid ./$BINARY $PROD_ARGS --frames $FRAMES \
                >$SOAK_LOG 2>&1 </dev/null & \
            sleep 2; echo encoder started" >/dev/null
    sleep 5

    local pid
    pid="$(adb_sh "pidof $BINARY")"
    if [ -z "$pid" ]; then
        # Print it here rather than sending someone to the board for it: the
        # usual cause is an option this script got wrong, and the usage text
        # says so in one line.
        printf '\n  --- %s ---\n' "$SOAK_LOG"
        adb_sh "head -n 5 $SOAK_LOG"
        die "the encoder did not start; see $SOAK_LOG on the board."
    fi
    ok "encoder running (pid $pid), $FRAMES frames, about ${DURATION_MIN} minutes"

    step "watching"
    printf '  (the run continues on the board; a dropped USB link does not stop it)\n\n'

    local waited=0 last_report=0
    while [ "$waited" -lt $((DURATION_MIN * 60 + 600)) ]; do
        sleep "$INTERVAL_S"
        waited=$((waited + INTERVAL_S))

        # A missing device here is not a failure: the process is on the board.
        adb devices 2>/dev/null | grep -q 'device$' || {
            printf '  %4dm  (board unreachable, run continues)\n' \
                $((waited / 60))
            continue
        }

        pid="$(adb_sh "pidof $BINARY")"
        if [ -z "$pid" ]; then
            printf '\n  encoder exited after about %dm - collecting.\n' \
                $((waited / 60))
            break
        fi

        if [ $((waited - last_report)) -ge "$INTERVAL_S" ]; then
            last_report=$waited
            local row
            row="$(adb_sh "tail -n 1 $SOAK_CSV")"
            printf '  %4dm  %s\n' $((waited / 60)) "$row"
        fi
    done

    step "run log"
    adb_sh "grep -E 'IMU attached|part at|bus read' $SOAK_LOG | head -4"
    adb_sh "grep -E 'Captured|Average FPS' $SOAK_LOG"
    adb_sh "grep -E 'OSD (annotated|sensor)' $SOAK_LOG"

    step "memory, descriptors, threads"
    adb_sh "head -n 1 $SOAK_CSV"
    adb_sh "tail -n 6 $SOAK_CSV"

    step "pin hygiene"
    local pins
    pins="$(adb_sh "ls /sys/class/gpio | grep -E '^gpio(70|71)$'")"
    if [ -n "$pins" ]; then
        printf '\n  LEAKED: %s still exported\n' "$pins" >&2
    else
        ok "gpio70/71 released"
    fi

    printf '\n  full history: %s on the board (previous run: %s.prev)\n' \
        "$SOAK_CSV" "$SOAK_CSV"
}

main "$@"
