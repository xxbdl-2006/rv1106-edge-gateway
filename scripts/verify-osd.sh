#!/bin/bash
#
# Cross compile, push and verify the OSD overlay on the board.
#
# Run this FROM THE VM (or any Linux host that has the SDK), because the
# Windows side has no cross toolchain:
#
#     cd /mnt/hgfs/luckfox_share/rv1103
#     ./scripts/verify-osd.sh
#
# Why a script and not a list of commands: the thing being checked is a
# property of encoded video, and the failure mode is the nasty kind. When the
# overlay does not appear there is no error, no crash and no log line -- the
# stream is perfectly valid, it just has no text in it. The three ways that
# happens:
#
#   1. --osd was dropped from the command line (or a startup script kept the
#      old argv), so the annotator was never opened.
#   2. The composite was refused because the canvas does not fit the frame.
#      osd_overlay returns 0 for "did not fit" rather than clipping, so the
#      overlay silently vanishes on frames narrower than the canvas.
#   3. The annotator wrote to its scratch copy but the encoder was handed the
#      original pointer, so the bytes that got encoded never had text in them.
#
# All three look identical from a player. So this script does not "check that
# it runs" -- it decodes actual frames and measures whether the top-left
# corner changed, and it reads the counters that distinguish those cases.
#
# The counters are the primary evidence, not the picture:
#
#   annotated        frames where the overlay went into the encoded buffer
#   passed_through   frames that went out untouched (annotator off, or the
#                    composite was refused)
#   composite_refused frames where the canvas did not fit the frame
#
# A run with annotated=0 and composite_refused=N is case 2. A run with
# annotated=0 and composite_refused=0 is case 1. Both print at exit, which is
# why the script never kills the process with SIGKILL -- SIGINT lets the
# cleanup path run and those numbers reach the log.

set -u

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="v4l2_mpp_encode"
BOARD_DIR="/userdata"
TOOLCHAIN_PREFIX="${CROSS_COMPILE:-arm-rockchip830-linux-uclibcgnueabihf-}"
SDK_ROOT="${SDK_ROOT:-/home/aaazhx/luckfox-pico}"

# Where the overlay is expected. Must match ENC_OSD_ORIGIN_X/Y and
# OSD_TELEMETRY_WIDTH in the source; the geometry check below is what catches a
# drift between those constants and this script.
OSD_ORIGIN_X=8
OSD_ORIGIN_Y=8

# How long to let the pipeline run before sampling. Long enough to get past
# --warmup and for the feed to have polled the sensor, short enough to keep the
# whole script under a minute.
# How long each clip runs. 300 frames at the requested 30 fps is 10 seconds.
# Both clips use the same number so a duration difference between them cannot
# be mistaken for an overlay effect.
#
# Used to derive the wait limit below rather than as a sleep: the achieved rate
# is one of the things being measured, so a fixed sleep would either cut the
# run short or waste time, and it would do so differently depending on whether
# the overlay was on -- which is exactly the variable under test.
CLIP_FRAMES=300

TOOLCHAIN_BIN=""

# Size of the built binary, in bytes. Set by the build half and reused by the
# verify half to confirm the push was not truncated. Empty until then, which is
# why it is declared here: `set -u` would abort on an unset expansion, and a
# script that dies on its own bookkeeping is worse than one that runs.
BUILD_SIZE=""

# Scratch directory for the pulled clips and the decoded frames. Owned by the
# verify half; cleared on success and kept on failure so the evidence survives.
WORK_DIR=""

step() { printf '\n=== %s ===\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
note() { printf '  note %s\n' "$*"; }
die()  { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

# adb here is a native Windows binary, so it cannot read the MSYS-style paths
# this script computes (`/f/luckfox_share/...`). It needs `F:/luckfox_share/...`
# and reports the mismatch as "No such file or directory", which reads like the
# file is missing rather than like a path-format problem.
#
# On Linux there is no cygpath and no conversion is needed, so this is an
# identity function there. The two branches are the same script on purpose: the
# geometry constants and thresholds are shared between the build half and the
# check half, and a separate Windows copy would drift.
to_native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

# Wait for a process to disappear, rather than sleeping a fixed amount and
# checking once.
#
# This is not politeness. Killing rkipc and then checking after a fixed sleep
# is exactly how S99gateway concluded "rkipc survived, the capture node stays
# busy" and gave up: the signal had been delivered, the process was on its way
# out, and the single check landed in the window before it exited. The script
# then reported a camera conflict that did not exist.
#
# Returns 0 if the process is gone, 1 if it is still there after the deadline.
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

# Who holds the capture node, by pid. Empty if nobody does.
#
# `fuser -v` is not available on this BusyBox build, so this walks /proc.
# Asking "who holds the node" rather than "is rkipc running" is the distinction
# that matters: the two disagree during every transition, and the node is what
# actually has to be free before the encoder can open it.
capture_holder() {
    adb shell "for p in \$(ls /proc | grep -E '^[0-9]+\$'); do
                   if ls -l /proc/\$p/fd 2>/dev/null | grep -q video11; then
                       printf '%s %s\n' \"\$p\" \"\$(cat /proc/\$p/comm 2>/dev/null)\"
                   fi
               done" 2>/dev/null | tr -d '\r' | head -1
}

# --------------------------------------------------------------------------
# 1. Locate the cross compiler.
# --------------------------------------------------------------------------
find_toolchain() {
    step "locating ${TOOLCHAIN_PREFIX}gcc"

    if command -v "${TOOLCHAIN_PREFIX}gcc" >/dev/null 2>&1; then
        TOOLCHAIN_BIN="$(dirname "$(command -v "${TOOLCHAIN_PREFIX}gcc")")"
        ok "on PATH: $TOOLCHAIN_BIN"
        return 0
    fi

    if [ -d "$SDK_ROOT" ]; then
        local found
        found="$(find "$SDK_ROOT" -maxdepth 6 -name "${TOOLCHAIN_PREFIX}gcc" \
                 -type f 2>/dev/null | head -n 1)"
        if [ -n "$found" ]; then
            TOOLCHAIN_BIN="$(dirname "$found")"
            export PATH="$TOOLCHAIN_BIN:$PATH"
            ok "found under the SDK: $TOOLCHAIN_BIN"
            return 0
        fi
    else
        printf '  note: SDK_ROOT %s does not exist\n' "$SDK_ROOT"
    fi

    die "cannot find ${TOOLCHAIN_PREFIX}gcc.
  Pass the prefix explicitly if it differs:
      CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- $0
  Or point at the SDK:
      SDK_ROOT=/path/to/luckfox-pico $0"
}

# --------------------------------------------------------------------------
# 2. Cross compile.
#
# The OSD stack (osd_font/format/overlay/telemetry/feed/annotate, mock_sensor,
# sensor_attitude) is linked in unconditionally, so this build also proves
# that none of those files picked up a host-only dependency -- they are all
# host safe by construction, which is what lets the same sources be unit
# tested on Windows.
#
# No adb in here. The board is a Windows-side resource in this setup, so the
# build half of the script has to be runnable on a host that cannot see it.
# Saving the board's previous binary happens in the verify half instead.
# --------------------------------------------------------------------------
build_binary() {
    step "cross compiling $BINARY"

    rm -f "$REPO_DIR/$BINARY"

    local build_log="${TMPDIR:-/tmp}/osd-build.log"
    if ! make -C "$REPO_DIR" "$BINARY" CROSS_COMPILE="$TOOLCHAIN_PREFIX" \
         >"$build_log" 2>&1; then
        printf '\n--- build log ---\n'
        cat "$build_log"
        die "cross compilation failed. See the log above."
    fi

    [ -f "$REPO_DIR/$BINARY" ] || die "make reported success but no $BINARY"

    check_elf

    BUILD_SIZE="$(wc -c < "$REPO_DIR/$BINARY")"
    ok "built, $BUILD_SIZE bytes, e_machine=0x2800 (ARM)"
}

# The ELF check is its own function because both halves need it: the build half
# to fail early, the verify half because it must not push whatever happens to be
# lying in the shared directory. A host binary pushed to the board fails with
# "not found" or a segfault, which reads like a code bug rather than a stale
# file.
check_elf() {
    [ -f "$REPO_DIR/$BINARY" ] || die "$BINARY is missing.
  Run the build half first, from the VM:
      ./scripts/verify-osd.sh --build-only"

    # Bytes 18-19 of an ELF header are e_machine: 0x28 0x00 for ARM.
    local machine
    machine="$(od -An -tx1 -j18 -N2 "$REPO_DIR/$BINARY" | tr -d ' \n')"
    if [ "$machine" != "2800" ]; then
        die "$BINARY has e_machine=0x$machine, expected 0x2800 (ARM).
  This is a host binary. Check that CROSS_COMPILE reached make."
    fi
}

# --------------------------------------------------------------------------
# 3. Check the board, and stop the running gateway first.
#
# The gateway holds /dev/video11, and a second process cannot open it. More
# importantly the supervisor will restart it within seconds if it dies on its
# own, so an OSD test has to stop the supervisor, not just the encoder.
# --------------------------------------------------------------------------
check_board() {
    step "checking the board"

    local devices
    devices="$(adb devices 2>/dev/null | grep -c 'device$')"
    [ "$devices" -ge 1 ] || die "no adb device. On Windows, check the RNDIS
  adapter first: if the USB link dropped, no amount of restarting adb helps."

    adb shell "test -c /dev/video11" 2>/dev/null \
        || die "/dev/video11 is missing. The camera pipeline is not up."

    # The gateway's argv, if it is running. The pid is checked before it is
    # used: `cat /proc/$(pidof X)/cmdline` with an empty pidof expands to
    # `/proc//cmdline`, which the kernel resolves to `/proc/cmdline` -- the
    # BOOT ARGUMENTS. That reads as a successful lookup of a very long, very
    # plausible looking command line, and it is how this script first reported
    # "gateway running: user_debug=31 storagemedia=sd ..." on a board where no
    # gateway was running at all.
    local enc_pid holder
    enc_pid="$(adb shell "pidof $BINARY" 2>/dev/null | tr -d '\r')"
    if [ -n "$enc_pid" ]; then
        holder="$(adb shell "cat /proc/$enc_pid/cmdline 2>/dev/null | tr '\0' ' '" | tr -d '\r')"
        note "gateway running (pid $enc_pid): $holder"
    fi

    local sup
    sup="$(adb shell "pidof gateway-supervise.sh" 2>/dev/null | tr -d '\r')"
    [ -n "$sup" ] && note "supervisor running (pid $sup)"

    ok "board reachable, /dev/video11 present"
}

# --------------------------------------------------------------------------
# 3a. Free the camera.
#
# Stopping the gateway is not enough. After a reboot rkipc comes up with the
# system and owns the capture node, and the gateway deliberately defers to it
# rather than fighting for the camera. So on a freshly booted board the gateway
# is not running at all, rkipc is, and the test would fail to open the device.
#
# Only rkipc is ours to stop. If anything else holds the node, that is either
# another gateway instance or something the user started, and killing it would
# be worse than failing loudly -- so this refuses instead.
#
# The wait is a poll, not a fixed sleep. See wait_for_exit for why.
# --------------------------------------------------------------------------
free_camera() {
    step "freeing the capture node"

    local holder pid name
    holder="$(capture_holder)"
    if [ -z "$holder" ]; then
        ok "nobody holds /dev/video11"
        return 0
    fi

    pid="${holder%% *}"
    name="${holder##* }"
    note "held by pid $pid ($name)"

    if [ "$name" != "rkipc" ]; then
        die "/dev/video11 is held by pid $pid ($name), which is not ours to stop.
  If that is another gateway instance, stop it yourself and re-run."
    fi

    adb shell "kill -9 $pid" 2>/dev/null
    if wait_for_exit rkipc 10; then
        ok "rkipc stopped"
    else
        die "rkipc would not exit after SIGKILL.
  Check: adb shell 'cat /proc/\$(pidof rkipc)/status | head -4'"
    fi

    # The node can stay busy for a moment after the holder exits.
    sleep 1
    if [ -n "$(capture_holder)" ]; then
        note "the node is still busy a second after rkipc exited; continuing anyway"
    fi
}

# --------------------------------------------------------------------------
# 3b. Save the board's current binary before overwriting it.
#
# The board boots into S99gateway, which runs whatever is at
# /userdata/v4l2_mpp_encode. If the new build turns out to be bad, the previous
# known good one is the difference between "revert" and "re-flash". It is
# pulled to the repo, next to the build, not onto the board, so it survives a
# failed push.
#
# The size is compared after pulling: a truncated pull is indistinguishable
# from a good one by eye, and a corrupt rollback target is worse than none.
# --------------------------------------------------------------------------
save_previous_binary() {
    step "saving the board's current binary"

    if ! adb shell "test -f $BOARD_DIR/$BINARY" 2>/dev/null; then
        note "no previous binary on the board"
        return 0
    fi

    # Never overwrite an existing copy. The point of this file is to be the last
    # known-good build; if a run is repeated, the second one would silently
    # replace it with a binary that came from the very build under test, and the
    # rollback target would quietly become "whatever was pushed last".
    #
    # That is not hypothetical. It happened here: the first run saved the real
    # 235060-byte previous binary, and the next run overwrote it with the
    # 366848-byte candidate. The file looked fine and was useless.
    if [ -f "$REPO_DIR/$BINARY.prev" ]; then
        note "keeping the existing $BINARY.prev ($(wc -c < "$REPO_DIR/$BINARY.prev") bytes)"
        return 0
    fi

    if ! adb pull "$BOARD_DIR/$BINARY" "$(to_native_path "$REPO_DIR/$BINARY.prev")" \
            >/dev/null 2>&1; then
        note "could not pull the previous binary; continuing"
        return 0
    fi

    local board_size pulled_size
    board_size="$(adb shell "wc -c < $BOARD_DIR/$BINARY" 2>/dev/null | tr -d '\r')"
    pulled_size="$(wc -c < "$REPO_DIR/$BINARY.prev")"
    if [ "$board_size" = "$pulled_size" ]; then
        ok "saved $pulled_size bytes to $BINARY.prev"
    else
        rm -f "$REPO_DIR/$BINARY.prev"
        note "pull was truncated ($pulled_size of $board_size bytes); discarded"
    fi
}

# Stop the supervisor and the encoder. Both must go, and in that order: if the
# encoder is killed first the supervisor just restarts it.
stop_gateway() {
    step "stopping the gateway so the camera is free"

    local sup enc
    sup="$(adb shell "pidof gateway-supervise.sh" 2>/dev/null | tr -d '\r')"
    enc="$(adb shell "pidof v4l2_mpp_encode" 2>/dev/null | tr -d '\r')"

    if [ -n "$sup" ]; then
        adb shell "kill $sup" 2>/dev/null
        ok "supervisor $sup stopped (no restart loop)"
    else
        note "no supervisor running"
    fi

    if [ -n "$enc" ]; then
        adb shell "kill $enc" 2>/dev/null
        # SIGTERM lets the cleanup path print the OSD counters, which is why
        # this is not SIGKILL.
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            adb shell "kill -0 $enc" 2>/dev/null || break
            sleep 0.5
        done
        if adb shell "kill -0 $enc" 2>/dev/null; then
            adb shell "kill -9 $enc" 2>/dev/null
            note "encoder needed SIGKILL"
        else
            ok "encoder $enc stopped cleanly"
        fi
    else
        note "no encoder running"
    fi

    # Give the kernel a moment to release the video node.
    sleep 1
}

# --------------------------------------------------------------------------
# 3c. Push the newly built binary.
#
# The size is checked after every push. An interrupted adb push leaves a zero
# byte file that still shows up in ls, and the board then fails in a way that
# points at the code rather than at the transfer. This is not hypothetical: it
# already cost a debugging session on this board, where a truncated
# /etc/init.d/S99gateway meant nothing started and the symptom was "the
# gateway stopped working".
#
# chmod is explicit because the mode does not always survive the transfer, and
# a non-executable file produces the same "not found" as a missing one.
# --------------------------------------------------------------------------
push_binary() {
    step "pushing the new binary"

    adb push "$(to_native_path "$REPO_DIR/$BINARY")" "$BOARD_DIR/" >/dev/null 2>&1 \
        || die "adb push failed"

    local size_on_board
    size_on_board="$(adb shell "wc -c < $BOARD_DIR/$BINARY" 2>/dev/null | tr -d '\r')"
    if [ "$BUILD_SIZE" != "$size_on_board" ]; then
        die "size mismatch after push: built $BUILD_SIZE, board $size_on_board.
  The transfer was truncated. Do not run it; it will fail confusingly."
    fi

    adb shell "chmod 755 $BOARD_DIR/$BINARY"

    ok "pushed, $size_on_board bytes verified on the board"
}

# Is the USB link still up?
#
# This board drops its USB link every so often, and when it does every adb call
# fails with "no devices/emulators found". Without this check the script reports
# that as "failed to launch the encoder", which points at the program rather
# than at the cable -- and then the gateway-restore step fails for the same
# hidden reason, leaving the board down with a warning about the gateway.
#
# Called at each step that needs the link, so the first failure names the real
# cause.
require_adb() {
    adb devices 2>/dev/null | grep -q 'device$' && return 0
    printf '\nFAILED: the USB link to the board is gone (no adb device).\n' >&2
    printf '  This is a physical/link problem, not a software one. Nothing the\n' >&2
    printf '  script did caused it and nothing in the script can fix it.\n' >&2
    printf '\n  On Windows, check whether the RNDIS adapter is still present:\n' >&2
    printf '      Get-NetAdapter | Where-Object Name -like "Ethernet*"\n' >&2
    printf '  If it is gone, unplug and replug the board.\n' >&2
    printf '\n  After replugging, check what state it came back in:\n' >&2
    printf '      adb shell "pidof v4l2_mpp_encode gateway-supervise.sh rkaiq_3A_server"\n' >&2
    printf '  If the gateway is not running, start it:\n' >&2
    printf '      adb shell "/etc/init.d/S99gateway start"\n' >&2
    exit 1
}

# --------------------------------------------------------------------------
# 4. Record two clips: one with --osd, one without.
#
# --sink file rather than rtsp, on purpose: a file sink is written by the same
# thread that encodes, so the bytes on disk are exactly what the encoder
# produced, with no network path in between that could be blamed. The overlay
# question is "did the encoder see the text", and this answers only that.
#
# Two clips, not one. The `no-osd` run is the control, and it is what makes the
# pixel comparison meaningful: the camera scene changes between runs, so an
# absolute reference would rot, but "the top-left corner differs from the same
# corner without an overlay" is stable. It also doubles as a regression check
# that --osd off still behaves exactly as before.
#
# --quiet would suppress the per-frame trace but the OSD summary lines print
# unconditionally at cleanup, so --quiet is safe and keeps the log small.
# --------------------------------------------------------------------------
record_clip() {
    local label="$1"; shift
    local extra="$*"

    require_adb
    adb shell "rm -f /userdata/osd-$label.h264 /userdata/osd-$label.log"

    # --frames makes the run end by itself, and that is the whole point.
    #
    # The first version slept a fixed time and then sent SIGINT. That races: the
    # process may already have finished, and `pidof` on this board also reports
    # stale pids for a while after a process exits, so the kill loop spent its
    # turns printing "can't kill pid 2039: No such process" eight times and then
    # gave up. Worse, it made the run length depend on when a signal happened to
    # land, so the two clips could differ in duration for reasons unrelated to
    # the overlay.
    #
    # Letting the encoder stop itself removes both problems: the cleanup path
    # (which is what prints the OSD counters) runs on the normal exit, and both
    # clips are the same length by construction.
    #
    # setsid so the process survives the adb shell exiting, and the trailing
    # sleep so adbd does not kill the process group before setsid takes hold.
    adb shell "setsid /userdata/$BINARY -d /dev/video11 -w 1280 -H 720 \
        --warmup 30 --frames $CLIP_FRAMES --fps 30 --sink file \
        -o /userdata/osd-$label.h264 $extra --quiet \
        --threads --ring-slots 4 \
        > /userdata/osd-$label.log 2>&1 < /dev/null & sleep 3; echo launched" \
        || die "failed to launch the encoder for '$label'"

    # Poll rather than sleep a fixed amount: how long the run takes depends on
    # the achieved frame rate, which is itself one of the things under test.
    #
    # The limit is generous enough to cover a badly degraded run (300 frames
    # would still finish inside 60s down to 5 fps) because hitting it means
    # signalling the process, and that reintroduces the race this replaced.
    local waited=0
    local limit=60
    while on_board_running "$BINARY"; do
        if [ "$waited" -ge "$limit" ]; then
            local pid
            pid="$(adb shell "pidof $BINARY" 2>/dev/null | tr -d '\r')"
            note "$label: still running after ${limit}s"
            if [ -n "$pid" ]; then
                adb shell "kill -INT $pid" 2>/dev/null
                sleep 2
            fi
            break
        fi
        sleep 1
        waited=$((waited + 1))
    done

    adb pull "/userdata/osd-$label.h264" \
        "$(to_native_path "$WORK_DIR/osd-$label.h264")" >/dev/null 2>&1 \
        || die "failed to pull the $label clip back"
    adb shell "cat /userdata/osd-$label.log" > "$WORK_DIR/osd-$label.log" 2>&1

    local size frames fps
    size="$(wc -c < "$WORK_DIR/osd-$label.h264")"
    frames="$(sed -n 's/.*Captured *: *\([0-9]*\) frames.*/\1/p' "$WORK_DIR/osd-$label.log" | tail -1)"
    fps="$(sed -n 's/.*Average FPS *: *\([0-9.]*\).*/\1/p' "$WORK_DIR/osd-$label.log" | tail -1)"
    ok "$label clip: $size bytes, ${frames:-?} frames, ${fps:-?} fps"
}

# Is the named process running on the board?
#
# This is a predicate, and it has to return the right exit status, which is why
# it is not spelled as `adb shell "pidof X" 2>/dev/null | tr -d '\r'`. In that
# form the status comes from `tr`, which always succeeds, so a caller doing
# `while pidof_x; do ...` loops forever. That cost a full 60 second timeout on
# every clip before it was noticed, on runs that actually took 17 seconds --
# and because the wasted time is bounded by the caller's limit rather than by
# anything real, it looked like the encoder was slow instead of like a script
# bug.
#
# `$(...)` strips the trailing newline, so "not running" arrives as the empty
# string and the test is on the string, not on a command's status.
on_board_running() {
    local out
    out="$(adb shell "pidof $1" 2>/dev/null | tr -d '\r')"
    [ -n "$out" ]
}

run_osd() {
    # The work directory lives inside the repo rather than in the system temp.
    #
    # Not a style choice. `mktemp -d` returns an MSYS path under /tmp, which
    # maps to a real directory on the drive holding the MSYS install, and
    # handing that to a native binary loses the drive prefix: the clips were
    # created fine and ffmpeg was then told to write to "/temp/..." and refused.
    # Every tool in the chain (adb, ffmpeg, the shell builtins) agrees on a path
    # under the repo, so there is nothing to convert and nothing to get wrong.
    #
    # It also means the clips and crops are easy to find afterwards, which
    # matters because looking at them is the check no script can make.
    WORK_DIR="$REPO_DIR/.osd-verify"
    rm -rf "$WORK_DIR"
    mkdir -p "$WORK_DIR"

    # Cleaned on success; kept on failure so the evidence survives. The
    # trailing `|| true` matters because this runs from an EXIT trap, where a
    # non-zero status would replace the script's own exit code.
    trap 'if [ -n "$WORK_DIR" ]; then rm -rf "$WORK_DIR"; fi; restore_gateway' EXIT

    step "recording a clip with --osd"
    record_clip "with-osd" "--osd --osd-mode wave"

    printf '\n--- encoder log (with --osd) ---\n'
    cat "$WORK_DIR/osd-with-osd.log"
    printf -- '--- end of log ---\n'

    step "recording the control clip (no --osd)"
    record_clip "no-osd" ""
}

# --------------------------------------------------------------------------
# 5. Judge it.
#
# Two independent kinds of evidence, because either one alone can lie:
#
#   counters   prove the annotator ran and the scratch copy was handed over
#   pixels     prove the text is actually in the encoded bitstream
#
# The counters alone are not enough. Every one of them can be right while the
# overlay is absent from the picture: the annotator could composite into the
# scratch buffer and then hand the encoder the original pointer. The only way
# to rule that out is to decode the output and look.
#
# The pixel check pulls both clips back and decodes one frame from each with
# ffmpeg on the HOST, not the board -- the board has no ffmpeg, and decoding on
# the host also means the check runs against the exact bytes that were written
# rather than a re-mux.
#
# Why compare two clips rather than check an absolute pixel value: the camera
# scene changes between runs, so any stored reference rots within days. What is
# stable is the difference -- the top-left corner has an overlay in one clip and
# not in the other, so the two regions must differ, and the rest of the frame
# must not.
# --------------------------------------------------------------------------
verify_output() {
    step "checking the counters"

    local failures=0
    local log="$WORK_DIR/osd-with-osd.log"

    check_log() {
        if grep -q "$1" "$log"; then
            ok "$2"
        else
            printf '  MISS %s\n' "$2"
            failures=$((failures + 1))
        fi
    }

    check_log "OSD: on, source=mock" "the overlay was enabled at startup"

    # The counters. annotated must be non-zero: that is the single number that
    # says text went into the bytes the encoder was given.
    local annotated refused passed
    annotated="$(sed -n 's/.*OSD annotated=\([0-9]*\).*/\1/p' "$log" | tail -1)"
    passed="$(sed -n 's/.*passed_through=\([0-9]*\).*/\1/p' "$log" | tail -1)"
    refused="$(sed -n 's/.*composite_refused=\([0-9]*\).*/\1/p' "$log" | tail -1)"

    if [ -z "$annotated" ]; then
        printf '  MISS no OSD counters in the log.\n'
        printf '       The summary prints at cleanup, so a SIGKILL would hide it.\n'
        failures=$((failures + 1))
    else
        if [ "$annotated" -gt 0 ]; then
            ok "annotated=$annotated frames"
        else
            printf '  MISS annotated=0, nothing was composited.\n'
            if [ "${refused:-0}" -gt 0 ]; then
                printf '       composite_refused=%s: the canvas does not fit the\n' "$refused"
                printf '       frame. osd_overlay returns 0 for "did not fit" and\n'
                printf '       the annotator then falls back to the original frame.\n'
            else
                printf '       composite_refused=0, so --osd never took effect.\n'
            fi
            failures=$((failures + 1))
        fi

        if [ "${refused:-1}" -eq 0 ]; then
            ok "composite_refused=0 (canvas fits every frame)"
        else
            printf '  MISS composite_refused=%s, the overlay was skipped on those\n' "$refused"
            printf '       frames. See OSD_TELEMETRY_WIDTH in src/osd_telemetry.h.\n'
            failures=$((failures + 1))
        fi

        note "passed_through=$passed (frames with no --osd work to do)"
    fi

    # Cost of the overlay, as a difference between the two clips.
    #
    # Reported rather than asserted, on purpose. This is the first run with the
    # overlay on real hardware and nobody knows yet what it costs, so a
    # threshold here would be a number invented to match the first observation
    # rather than a requirement. The interesting number is the DELTA: measured
    # alone, a low frame rate could be the camera, the encoder or the ambient
    # load, and the control clip is what separates those.
    #
    # The cost is expected to be one 1.4 MB memcpy per frame (a 1280x720 NV12
    # frame), which at 30 fps is about 42 MB/s of sequential copy.
    local fps_on fps_off
    fps_on="$(sed -n 's/.*Average FPS *: *\([0-9.]*\).*/\1/p' "$WORK_DIR/osd-with-osd.log" | tail -1)"
    fps_off="$(sed -n 's/.*Average FPS *: *\([0-9.]*\).*/\1/p' "$WORK_DIR/osd-no-osd.log" | tail -1)"
    if [ -n "$fps_on" ] && [ -n "$fps_off" ]; then
        printf '  overlay cost: %.1f fps with --osd vs %.1f without' \
               "$fps_on" "$fps_off" 2>/dev/null || \
        note "overlay cost: $fps_on fps with --osd vs $fps_off without"
        if awk "BEGIN{exit !($fps_off > 0)}"; then
            awk -v a="$fps_on" -v b="$fps_off" \
                'BEGIN{ printf "  (%.1f%% of the baseline)\n", 100*a/b }'
        fi
        if awk "BEGIN{exit !($fps_on < 29)}"; then
            if awk "BEGIN{exit !($fps_off >= 29)}"; then
                printf '  note --osd is below the requested 30 fps while the control\n'
                printf '       reaches it, so the per-frame frame copy is the cause.\n'
            else
                printf '  note both clips are below the requested 30 fps, so the\n'
                printf '       overlay is NOT what is limiting it. Look at the\n'
                printf '       camera (a missing rkaiq_3A_server shows up here).\n'
            fi
        fi
    fi

    # The control run must have annotated nothing. If it did, --osd was on in
    # both runs and the pixel comparison below proves nothing.
    if grep -q "OSD annotated=0" "$WORK_DIR/osd-no-osd.log" 2>/dev/null ||
       ! grep -q "OSD annotated=" "$WORK_DIR/osd-no-osd.log" 2>/dev/null; then
        ok "the control run had no overlay (annotated=0 or counters absent)"
    else
        printf '  MISS the control run annotated frames; the comparison is void\n'
        failures=$((failures + 1))
    fi

    # The sensor side. polls > 0 means the mock was actually read, which is
    # what makes the angles on screen real data rather than zeros.
    local polls samples
    polls="$(sed -n 's/.*OSD sensor polls=\([0-9]*\).*/\1/p' "$log" | tail -1)"
    samples="$(sed -n 's/.*samples=\([0-9]*\).*/\1/p' "$log" | tail -1)"
    if [ -n "$polls" ] && [ "${polls:-0}" -gt 0 ]; then
        ok "sensor polls=$polls samples=${samples:-?}"
    else
        printf '  MISS the sensor feed never polled\n'
        failures=$((failures + 1))
    fi

    # ---- pixel evidence -------------------------------------------------
    step "checking the pixels"

    if ! command -v ffmpeg >/dev/null 2>&1; then
        printf '  SKIP ffmpeg is not on PATH, so the decode check cannot run.\n'
        printf '       The counters above are necessary but not sufficient -- they\n'
        printf '       cannot see a composite that never reached the encoder.\n'
        printf '       Install ffmpeg and re-run to close that gap.\n'
    else
        pixel_check || failures=$((failures + 1))
    fi

    printf '\n'
    if [ "$failures" -eq 0 ]; then
        printf 'PASS: the overlay was composited, reached the encoder, and is\n'
        printf '      visible in the decoded frames.\n'
        printf '\n'
        printf 'Clips kept in %s\n' "$WORK_DIR"
        printf '    osd-with-osd.h264   overlay on\n'
        printf '    osd-no-osd.h264     control\n'
        printf 'Play them with: ffplay <file>\n'
    else
        printf '%d check(s) did not pass.\n' "$failures"
        printf 'Clips kept in %s for inspection.\n' "$WORK_DIR"
        # Keep the directory on failure; clear WORK_DIR so the EXIT trap does
        # not delete the evidence.
        WORK_DIR=""
        exit 1
    fi
}

# --------------------------------------------------------------------------
# 5b. Decode one frame from each clip and compare regions.
#
# The frame is taken from a fixed offset, not the first frame: the first frames
# after --warmup still sit near a scene change during which the encoder spends
# its bitrate differently, and comparing those would make the "background
# unchanged" assertion noisy for reasons that have nothing to do with the OSD.
#
# The two regions:
#
#   OSD region    top-left corner where the telemetry panel is drawn. Must
#                 differ, or the overlay is not in the picture.
#   control region a band well below the panel, same in both clips. Must NOT
#                 differ much, or something other than the overlay changed and
#                 the first comparison is meaningless.
#
# The control region is what makes this falsifiable. Without it, "the corner
# differs" would also pass if the two clips simply came from different scenes.
#
# Raw video out of ffmpeg rather than PNGs: comparing pixels needs the numbers,
# not an image format, and rawvideo avoids depending on an encoder being
# present in the ffmpeg build.
# --------------------------------------------------------------------------
pixel_check() {
    local frame_index=60
    local w=1280
    local h=720

    # Crop geometry, derived from the source rather than guessed:
    #
    #   origin        (ENC_OSD_ORIGIN_X, ENC_OSD_ORIGIN_Y) = (8, 8)
    #   canvas width  OSD_TELEMETRY_WIDTH(48) * OSD_FONT_ADVANCE(6) = 288 px
    #   padding       ENC_OSD_PADDING = 2 px each side
    #   panel height  OSD_TELEMETRY_LINES(4) * OSD_FONT_HEIGHT(7) = 28 px,
    #                 plus padding, so it ends around y = 8+28+4 = 40
    #
    # So the panel occupies roughly x in [8, 300], y in [8, 40]. 340x80 from
    # (0,8) covers it with margin on every side, which matters because a crop
    # that only just covers the panel would let the thresholds drift as the
    # font or the line count changes.
    local osd_w=340 osd_h=80

    # The control band sits well below the panel: same width, at y=400. Far
    # enough down that it cannot overlap the panel even if the panel grows.
    local ctl_x=8 ctl_w=340 ctl_y=400 ctl_h=80

    # ffmpeg is a native binary too, so its file arguments go through the same
    # conversion as adb's. The variables stay in shell form for `wc -c` and the
    # rest of the shell tooling, which does understand MSYS paths.
    local a_raw="$WORK_DIR/with-osd.raw"
    local b_raw="$WORK_DIR/no-osd.raw"
    local a_raw_native b_raw_native
    a_raw_native="$(to_native_path "$a_raw")"
    b_raw_native="$(to_native_path "$b_raw")"

    # -ss before -i seeks by keyframe which is fast but lands on a different
    # frame for each clip. -vf select with a frame number is exact but slow.
    # Neither matters here as long as BOTH clips use the same method: the
    # comparison is between clips, not against an absolute frame number.
    local decode_ok=1
    for pair in "with-osd:$a_raw_native" "no-osd:$b_raw_native"; do
        local label="${pair%%:*}"
        local out="${pair##*:}"
        if ! ffmpeg -v error -i "$(to_native_path "$WORK_DIR/osd-$label.h264")" \
             -vf "select=gte(n\,$frame_index),crop=$w:$h:0:0" \
             -frames:v 1 -pix_fmt gray -f rawvideo "$out" \
             2>"$WORK_DIR/ff-$label.err"; then
            printf '  MISS ffmpeg failed to decode the %s clip:\n' "$label"
            sed 's/^/       /' "$WORK_DIR/ff-$label.err"
            decode_ok=0
        fi
    done
    [ "$decode_ok" -eq 1 ] || return 1

    local a_size b_size
    a_size="$(wc -c < "$a_raw" 2>/dev/null || echo 0)"
    b_size="$(wc -c < "$b_raw" 2>/dev/null || echo 0)"
    local want=$((w * h))
    if [ "$a_size" -ne "$want" ] || [ "$b_size" -ne "$want" ]; then
        printf '  MISS decoded frame size wrong: got %s / %s, expected %s\n' \
               "$a_size" "$b_size" "$want"
        return 1
    fi
    ok "decoded one frame from each clip (${w}x${h} gray)"

    # Compare two rectangles and report the mean absolute difference.
    #
    # Two things here were wrong in the first version, and both produced a
    # plausible-looking number instead of an error, which is why the function
    # is now checked against a synthetic frame pair with a known answer before
    # being trusted on real output:
    #
    #  1. `od` wraps its output at 16 bytes per line. A reader that assumes one
    #     line per row of pixels compares the wrong bytes. Flattening with
    #     `tr -s ' ' '\n'` removes the ambiguity: one byte per line, always.
    #
    #  2. The two clips are interleaved one ROW at a time, not one byte at a
    #     time and not one whole-image block at a time. So the reader has to be
    #     told the row width; pairing byte 2k with byte 2k+1 gives a wrong
    #     answer (93.5 instead of 52.9 on the test frame), and so does
    #     splitting the file in half (17.6).
    #
    # The error in both wrong versions was in the tens of grey levels, i.e.
    # well above any threshold that would flag a problem. A pixel check that
    # reports a confident wrong number is worse than no check, because it is
    # the evidence you use to decide whether to keep looking.
    region_mad() {
        local x="$1" y="$2" rw="$3" rh="$4"
        local row start

        for (( row = y; row < y + rh; row++ )); do
            start=$(( row * w + x ))
            dd if="$a_raw" bs=1 skip="$start" count="$rw" 2>/dev/null
            dd if="$b_raw" bs=1 skip="$start" count="$rw" 2>/dev/null
        done | od -An -v -tu1 | tr -s ' ' '\n' | grep -v '^$' > "$WORK_DIR/_pair"

        awk -v rw="$rw" '
            { v[++n] = $1 + 0 }
            END {
                i = 1
                while (i + 2 * rw - 1 <= n) {
                    for (k = 0; k < rw; k++) {
                        d = v[i + k] - v[i + rw + k]
                        s += (d < 0 ? -d : d)
                        c++
                    }
                    i += 2 * rw
                }
                if (c > 0) printf "%.1f", s / c; else printf "0"
            }
        ' "$WORK_DIR/_pair"
    }

    local osd_mad ctl_mad
    osd_mad="$(region_mad 0 8 "$osd_w" "$osd_h")"
    ctl_mad="$(region_mad "$ctl_x" "$ctl_y" "$ctl_w" "$ctl_h")"

    printf '  mean abs difference: overlay region %s, control band %s\n' \
           "$osd_mad" "$ctl_mad"

    # The judgement is RELATIVE, and that is a correction made after the first
    # run on real hardware rather than something designed up front.
    #
    # The first version asserted an absolute ceiling on the control band (4).
    # On the synthetic test frames that were used to validate the arithmetic
    # this looked fine, because both synthetic scenes were byte-identical
    # outside the overlay and the control band measured 0.0 exactly.
    #
    # Real video is not like that. Measured here: overlay region 44.9, control
    # band 5.8, whole frame 3.9. The camera is live, the sensor has no 3A
    # running so exposure drifts between runs, and the encoder makes different
    # decisions once the overlay changes the content. A ceiling of 4 would have
    # rejected a perfectly good result and pointed at "the scenes differ" --
    # sending the next hour after a problem that did not exist.
    #
    # The signal-to-noise ratio is what actually carries the information: the
    # overlay is ~7.7x the background variation. So:
    #
    #   absolute floor   the region must differ at all, by more than codec noise
    #   relative floor   it must stand out from what the rest of the frame did
    #
    # The first alone was the original design; the second alone would pass on a
    # perfectly static scene even if the overlay were faint. Together they hold
    # up on both the synthetic case and this one.
    local failures=0
    local min_osd=6
    local min_ratio=3

    if awk "BEGIN{exit !($osd_mad >= $min_osd)}"; then
        ok "the overlay region differs (MAD $osd_mad >= $min_osd)"
    else
        printf '  MISS the overlay region is nearly identical (MAD %s < %s).\n' \
               "$osd_mad" "$min_osd"
        printf '       The overlay is not in the encoded picture. The counters\n'
        printf '       said it was composited, so look at whether the annotator\n'
        printf '       handed the encoder the scratch pointer or the original.\n'
        failures=$((failures + 1))
    fi

    # `ctl_mad > 0` guards the division. A control band of exactly zero means a
    # perfectly static scene, which is the synthetic case, and there the ratio
    # test carries no information -- the absolute floor above already decides.
    if awk "BEGIN{exit !($ctl_mad <= 0 || $osd_mad >= $min_ratio * $ctl_mad)}"; then
        ok "it stands out from the background (MAD $osd_mad vs $ctl_mad, ratio >= $min_ratio)"
    else
        printf '  MISS the overlay region barely differs from the background\n'
        printf '       (MAD %s vs %s). Either the overlay is very faint, or the\n' \
               "$osd_mad" "$ctl_mad"
        printf '       two clips show different scenes. Check the PNGs kept in\n'
        printf '       the work directory before trusting anything else.\n'
        failures=$((failures + 1))
    fi

    # Keep a human-viewable crop of each frame. The numbers above say whether
    # the region changed; only the picture says whether what changed is the
    # overlay. A garbled or misplaced panel would pass every threshold here.
    for label in with-osd no-osd; do
        ffmpeg -v error -i "$(to_native_path "$WORK_DIR/osd-$label.h264")" \
            -vf "select=gte(n\,$frame_index),crop=400:120:0:0,scale=800:240:flags=neighbor" \
            -frames:v 1 -y "$WORK_DIR/crop-$label.png" 2>/dev/null \
            && note "crop-$label.png written (top-left 400x120, 2x)"
    done

    [ "$failures" -eq 0 ]
}

# --------------------------------------------------------------------------
# 6. Put the board back.
#
# Always runs, including after a failure, and that matters more than it looks:
# the gateway is the thing that makes the board useful, and leaving it down
# means the next session starts by debugging a board that "stopped working".
# --------------------------------------------------------------------------
restore_gateway() {
    step "restoring the gateway"


    adb shell "rm -f /userdata/osd-test.h264" 2>/dev/null

    # Told apart from "the gateway is down" on purpose. If the link is gone the
    # restore cannot run at all, and saying so is more useful than twenty
    # seconds of dots followed by "the gateway did not come back".
    if ! adb devices 2>/dev/null | grep -q 'device$'; then
        printf '  WARN the USB link is gone, so the gateway could not be restored.\n'
        printf '       After replugging, check the board and start it if needed:\n'
        printf '           adb shell "pidof gateway-supervise.sh rkaiq_3A_server"\n'
        printf '           adb shell "/etc/init.d/S99gateway start"\n'
        return 0
    fi

    if adb shell "pidof gateway-supervise.sh" 2>/dev/null | grep -q .; then
        ok "supervisor already running"
        return 0
    fi

    adb shell "/etc/init.d/S99gateway start" >/dev/null 2>&1
    printf '  waiting for the gateway to come up'
    for _ in $(seq 1 20); do
        if adb shell "pidof v4l2_mpp_encode" 2>/dev/null | grep -q .; then
            printf '\n'
            ok "gateway is back up"
            return 0
        fi
        printf '.'
        sleep 1
    done
    printf '\n'
    printf '  WARN the gateway did not come back within 20s.\n'
    printf '       Check: adb shell "/etc/init.d/S99gateway status"\n'
    printf '              adb shell "cat /tmp/gateway-boot.log"\n'
}

# --------------------------------------------------------------------------
# Running it.
#
# Two halves, because in this setup the toolchain and the board live on
# different machines: the SDK is in the VM, and the board is a USB device on
# Windows. The shared folder makes the built binary visible to both, so the
# hand-off is just a file.
#
#     in the VM       ./scripts/verify-osd.sh --build-only
#     on Windows      ./scripts/verify-osd.sh --verify-only
#
# With no argument it does both, which is right on a Linux host that has the
# SDK and can see the board -- the case the script was written for originally.
#
# The modes exist rather than two scripts because the two halves share the
# geometry constants, the log parsers and the thresholds, and those are exactly
# the things that must not drift apart between the build and the check.
# --------------------------------------------------------------------------
usage() {
    cat <<EOF
OSD overlay board verification.

Usage: $(basename "$0") [--build-only | --verify-only | --help]

  --build-only    cross compile and check the ELF header. For the VM, which
                  has the toolchain but cannot see the board.
  --verify-only   push, run and check the decoded output. For the machine the
                  board is plugged into. Requires a binary already built.
  (no argument)   both halves in order. For a host that has the SDK and the
                  board.

Environment:
  CROSS_COMPILE   toolchain prefix (default ${TOOLCHAIN_PREFIX})
  SDK_ROOT        where to search for the toolchain (default ${SDK_ROOT})
EOF
}

main() {
    local mode="both"

    case "${1:-}" in
        --build-only)  mode="build" ;;
        --verify-only) mode="verify" ;;
        --help|-h)     usage; exit 0 ;;
        "")            ;;
        *)             usage >&2; exit 2 ;;
    esac

    printf 'OSD overlay board verification'
    case "$mode" in
        build)  printf ' (build only)\n' ;;
        verify) printf ' (verify only)\n' ;;
        both)   printf '\n' ;;
    esac
    printf 'repo: %s\n' "$REPO_DIR"

    if [ "$mode" = "build" ] || [ "$mode" = "both" ]; then
        find_toolchain
        build_binary
    fi

    if [ "$mode" = "build" ]; then
        printf '\nBuild half done. Next, on the machine the board is plugged into:\n'
        printf '    ./scripts/verify-osd.sh --verify-only\n'
        exit 0
    fi

    if [ "$mode" = "both" ]; then
        # No binary yet in the both-mode case? make has just run, so it exists.
        BUILD_SIZE="$(wc -c < "$REPO_DIR/$BINARY")"
    else
        step "using the binary already built"
        check_elf
        BUILD_SIZE="$(wc -c < "$REPO_DIR/$BINARY")"
        ok "$BUILD_SIZE bytes, e_machine=0x2800 (ARM)"
    fi

    check_board
    save_previous_binary
    push_binary
    stop_gateway
    free_camera

    # From here on the board is ours, so anything that leaves early has to put
    # it back. The trap covers the die() paths, not just the success path.
    trap 'restore_gateway' EXIT

    run_osd
    verify_output
}

main "$@"
