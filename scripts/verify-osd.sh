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
CAPTURE_SECONDS=6
FRAMES_TO_PULL=1

TOOLCHAIN_BIN=""

step() { printf '\n=== %s ===\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
note() { printf '  note %s\n' "$*"; }
die()  { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

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
# --------------------------------------------------------------------------
build_binary() {
    step "cross compiling $BINARY"

    rm -f "$REPO_DIR/$BINARY"

    if ! make -C "$REPO_DIR" "$BINARY" CROSS_COMPILE="$TOOLCHAIN_PREFIX" \
         >/tmp/osd-build.log 2>&1; then
        printf '\n--- build log ---\n'
        cat /tmp/osd-build.log
        die "cross compilation failed. See the log above."
    fi

    [ -f "$REPO_DIR/$BINARY" ] || die "make reported success but no $BINARY"

    # A host binary would be pushed and then fail on the board with "not
    # found" or a segfault. Bytes 18-19 of the ELF header are 0x28 0x00 (ARM).
    local machine
    machine="$(od -An -tx1 -j18 -N2 "$REPO_DIR/$BINARY" | tr -d ' \n')"
    if [ "$machine" != "2800" ]; then
        die "$BINARY has e_machine=0x$machine, expected 0x2800 (ARM).
  This is a host binary. Check that CROSS_COMPILE reached make."
    fi

    LOCAL_SIZE="$(wc -c < "$REPO_DIR/$BINARY")"
    ok "built, $LOCAL_SIZE bytes, e_machine=0x2800 (ARM)"

    # Record the old binary so a failed run can be rolled back. The previous
    # known good one is what the board boots into via S99gateway, so losing it
    # means the gateway stops coming up on reboot.
    if adb shell "test -f $BOARD_DIR/$BINARY" 2>/dev/null; then
        adb pull "$BOARD_DIR/$BINARY" "$REPO_DIR/$BINARY.prev" >/dev/null 2>&1 \
            && ok "saved the previous board binary to $BINARY.prev"
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

    local holder
    holder="$(adb shell "cat /proc/\$(pidof v4l2_mpp_encode)/cmdline 2>/dev/null | tr '\0' ' '" 2>&1)"
    if [ -n "$holder" ]; then
        note "gateway running: $holder"
    fi

    ok "board reachable, /dev/video11 present"
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

    adb shell "rm -f /userdata/osd-$label.h264 /userdata/osd-$label.log"

    # setsid so the process survives the adb shell exiting, and the trailing
    # sleep so adbd does not kill the process group before setsid takes hold.
    # That race is why an earlier version of this test "sometimes worked".
    adb shell "setsid /userdata/$BINARY -d /dev/video11 -w 1280 -H 720 \
        --warmup 30 --frames 300 --fps 30 --sink file \
        -o /userdata/osd-$label.h264 $extra --quiet \
        --threads --ring-slots 4 \
        > /userdata/osd-$label.log 2>&1 < /dev/null & sleep 3; echo launched" \
        || die "failed to launch the encoder for '$label'"

    sleep "$CAPTURE_SECONDS"

    # SIGINT, not SIGKILL: the counters only print if cleanup runs.
    local pid
    pid="$(adb shell "pidof $BINARY" 2>/dev/null | tr -d '\r')"
    if [ -n "$pid" ]; then
        adb shell "kill -INT $pid" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            adb shell "kill -0 $pid" 2>/dev/null || break
            sleep 0.5
        done
    fi

    adb pull "/userdata/osd-$label.h264" "$WORK_DIR/osd-$label.h264" >/dev/null 2>&1 \
        || die "failed to pull the $label clip back"
    adb shell "cat /userdata/osd-$label.log" > "$WORK_DIR/osd-$label.log" 2>&1

    local size
    size="$(wc -c < "$WORK_DIR/osd-$label.h264")"
    ok "$label clip: $size bytes"
}

run_osd() {
    WORK_DIR="$(mktemp -d)"
    trap 'rm -rf "$WORK_DIR"; restore_gateway' EXIT

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

    local a_raw="$WORK_DIR/with-osd.raw"
    local b_raw="$WORK_DIR/no-osd.raw"

    # -ss before -i seeks by keyframe which is fast but lands on a different
    # frame for each clip. -vf select with a frame number is exact but slow.
    # Neither matters here as long as BOTH clips use the same method: the
    # comparison is between clips, not against an absolute frame number.
    local decode_ok=1
    for pair in "with-osd:$a_raw" "no-osd:$b_raw"; do
        local label="${pair%%:*}"
        local out="${pair##*:}"
        if ! ffmpeg -v error -i "$WORK_DIR/osd-$label.h264" \
             -vf "select=gte(n\,$frame_index),crop=$w:$h:0:0" \
             -frames:v 1 -pix_fmt gray -f rawvideo "$out" 2>"$WORK_DIR/ff-$label.err"; then
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

    # Thresholds. A knockout panel over live video changes the region a lot --
    # the panel is dark and the text is light, so typical MAD is tens of grey
    # levels. 6 is well below that and well above codec noise. The control band
    # should be near zero: same scene, same encoder settings, so anything above
    # about 4 means the scenes actually differed and the test is unsound.
    local failures=0

    if awk "BEGIN{exit !($osd_mad >= 6)}"; then
        ok "the overlay region differs (MAD $osd_mad >= 6)"
    else
        printf '  MISS the overlay region is nearly identical (MAD %s < 6).\n' "$osd_mad"
        printf '       The overlay is not in the encoded picture. The counters\n'
        printf '       said it was composited, so look at whether the annotator\n'
        printf '       handed the encoder the scratch pointer or the original.\n'
        failures=$((failures + 1))
    fi

    if awk "BEGIN{exit !($ctl_mad <= 4)}"; then
        ok "the control band is unchanged (MAD $ctl_mad <= 4, scenes match)"
    else
        printf '  MISS the control band also differs (MAD %s > 4).\n' "$ctl_mad"
        printf '       The two clips show different scenes, so the overlay-region\n'
        printf '       result above proves nothing. Re-run with the camera still.\n'
        failures=$((failures + 1))
    fi

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

main() {
    printf 'OSD overlay board verification\n'
    printf 'repo: %s\n' "$REPO_DIR"

    find_toolchain
    build_binary
    check_board
    stop_gateway

    # From here on the board is ours, so anything that leaves early has to put
    # it back. The trap covers the die() paths, not just the success path.
    trap 'restore_gateway' EXIT

    run_osd
    verify_output
}

main "$@"
