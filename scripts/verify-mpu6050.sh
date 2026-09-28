#!/bin/bash
#
# Cross compile, push and verify the MPU6050 bit-banged driver on the board.
#
# Run this FROM THE VM (or any Linux host that has the SDK), because the
# Windows side has no cross toolchain:
#
#     cd /mnt/hgfs/luckfox_share/rv1103
#     ./scripts/verify-mpu6050.sh
#
# The point of the script rather than a list of commands in a doc: there are
# five steps that must happen in order, each with a precondition, and the
# interesting failure modes (wrong endianness, a stale binary, a toolchain
# missing from PATH) all look like "the sensor is broken" from the outside.
# Each step here checks its own precondition and says which one failed.
#
# The reference for the expected numbers is the Python implementation that
# already ran on this board: tools/i2c-bitbang.py. It found 0x68, reported
# WHO_AM_I = 0x70 and |a| = 1.054 g. The C port must agree with it -- if the
# two disagree, the port is wrong, not the sensor.

set -u

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="mpu6050-probe"
BOARD_DIR="/userdata"
TOOLCHAIN_PREFIX="${CROSS_COMPILE:-arm-rockchip830-linux-uclibcgnueabihf-}"
SDK_ROOT="${SDK_ROOT:-/home/aaazhx/luckfox-pico}"

# Set by find_toolchain(). Empty until then.
TOOLCHAIN_BIN=""

step() { printf '\n=== %s ===\n' "$*"; }
ok()   { printf '  ok   %s\n' "$*"; }
die()  { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

# --------------------------------------------------------------------------
# 1. Locate the cross compiler.
#
# `CROSS_COMPILE=prefix-` alone is not enough: the prefix has to resolve, which
# means the toolchain's bin directory has to be on PATH. The SDK keeps it deep
# inside the tree under a host-specific directory, so rather than hard code a
# path that changes with SDK version, search for it and say where it was found.
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
      SDK_ROOT=/path/to/luckfox-pico $0
  Expected location is under \$SDK_ROOT, in a directory named after the host
  toolchain, something like .../prebuilts/gcc/linux-x86/arm/.../bin."
}

# --------------------------------------------------------------------------
# 2. Cross compile.
#
# The rpath in LDFLAGS is for the Rockit build and is not needed here, but the
# probe links only libc and librt so it comes out self contained either way.
# --------------------------------------------------------------------------
build_binary() {
    step "cross compiling $BINARY"

    rm -f "$REPO_DIR/$BINARY"

    if ! make -C "$REPO_DIR" "$BINARY" CROSS_COMPILE="$TOOLCHAIN_PREFIX" \
         >/tmp/mpu6050-build.log 2>&1; then
        printf '\n--- build log ---\n'
        cat /tmp/mpu6050-build.log
        die "cross compilation failed. Warnings are fatal here on purpose:
  this code is checked with -Wall -Wextra -Wpedantic, and a warning that
  scrolls past is how the read_byte *out bug survived an earlier review."
    fi

    [ -f "$REPO_DIR/$BINARY" ] || die "make reported success but no $BINARY"

    # A host binary would run here and fail confusingly on the board with
    # "not found" or a segfault. Check e_machine before pushing: bytes 18-19
    # of the ELF header are 0x28 0x00 for ARM.
    local machine
    machine="$(od -An -tx1 -j18 -N2 "$REPO_DIR/$BINARY" | tr -d ' \n')"
    if [ "$machine" != "2800" ]; then
        die "$BINARY has e_machine=0x$machine, expected 0x2800 (ARM).
  This is a host binary. Check that CROSS_COMPILE reached make."
    fi

    ok "built, $(wc -c < "$REPO_DIR/$BINARY") bytes, e_machine=0x2800 (ARM)"
}

# --------------------------------------------------------------------------
# 3. Check the board is reachable and the pins are still ours.
#
# MUX UNCLAIMED is the precondition for the whole approach: a pin claimed by a
# driver cannot be driven from userspace through sysfs, and the failure looks
# like "the part never answers" rather than a permission error.
#
# This step also pins down the sysfs semantics the driver depends on. The one
# that bit: writing `value` while a pin is an input fails with EPERM on Linux
# ("write error: Operation not permitted"). A driver that sets the latch before
# switching to output - the natural order for bare-metal open-drain - therefore
# fails on its very first line_low, with 225 io errors and nothing on the bus,
# while a Python implementation writing direction first works fine on the same
# pins. That cost a debugging session, so the assumption is now checked
# explicitly rather than assumed.
# --------------------------------------------------------------------------
check_board() {
    step "checking the board"

    local devices
    devices="$(adb devices 2>/dev/null | grep -c 'device$')"
    [ "$devices" -ge 1 ] || die "no adb device. On Windows, check the RNDIS
  adapter first: if interface 'Ethernet 3' is gone the USB link dropped and
  no amount of restarting adb will bring it back."

    local mux
    mux="$(adb shell "grep -E 'pin (70|71) ' /sys/kernel/debug/pinctrl/*/pinmux-pins" 2>&1)"
    printf '%s\n' "$mux" | grep -q UNCLAIMED \
        || die "pins 70/71 are not both unclaimed:
$mux
  Something has taken them. Nothing can fix this from userspace."
    ok "$(printf '%s' "$mux" | head -n 1 | tr -s ' ')"

    # The gateway owns gpio pins of its own but not these two; make sure it is
    # running anyway so a pass here is not confused by a half booted board.
    if adb shell "pidof v4l2_mpp_encode" 2>/dev/null | grep -q '[0-9]'; then
        ok "gateway is running (rtsp pipeline unaffected by this test)"
    else
        printf '  note: gateway not running; harmless for this test\n'
    fi

    check_sysfs_semantics
}

# Confirm that writing value works in the order the driver uses, and fails in
# the order that looks natural but is wrong. Both halves matter: if the second
# one ever starts succeeding, the ordering constraint has changed and the
# comment in sysfs_line_low needs revisiting.
check_sysfs_semantics() {
    local probe="70"
    local result

    result="$(adb shell "
        echo $probe > /sys/class/gpio/export 2>/dev/null
        d=/sys/class/gpio/gpio$probe
        echo out > \$d/direction 2>/dev/null
        if echo 0 > \$d/value 2>/dev/null; then echo OUT_THEN_VALUE_OK; else echo OUT_THEN_VALUE_FAIL; fi
        echo in > \$d/direction 2>/dev/null
        if echo 0 > \$d/value 2>/dev/null; then echo IN_THEN_VALUE_OK; else echo IN_THEN_VALUE_FAIL; fi
        echo in > \$d/direction 2>/dev/null
        echo $probe > /sys/class/gpio/unexport 2>/dev/null
    " 2>&1)"

    printf '%s\n' "$result" | grep -q OUT_THEN_VALUE_OK \
        || die "cannot write value after direction=out on gpio$probe:
$result
  The driver's line_low does exactly this. If it fails here it fails there."

    if printf '%s\n' "$result" | grep -q IN_THEN_VALUE_OK; then
        printf '  note: writing value while direction=in now succeeds.\n'
        printf '        The ordering constraint documented in sysfs_line_low\n'
        printf '        has changed on this kernel; the comment is stale.\n'
    else
        ok "sysfs ordering confirmed (out before value; in rejects writes)"
    fi
}

# --------------------------------------------------------------------------
# 4. Push and run.
#
# The probe needs root to write /sys/class/gpio; adb shell is root on this
# image. It is run with --dump so the sampling path is exercised too, not just
# the address scan.
# --------------------------------------------------------------------------
run_probe() {
    step "pushing and running on the board"

    adb push "$REPO_DIR/$BINARY" "$BOARD_DIR/" >/dev/null 2>&1 \
        || die "adb push failed"

    # An interrupted push leaves a zero byte file that still shows up in ls,
    # and the board then fails in a way that points at the code. Compare sizes.
    local local_size remote_size
    local_size="$(wc -c < "$REPO_DIR/$BINARY")"
    remote_size="$(adb shell "wc -c < $BOARD_DIR/$BINARY" 2>/dev/null | tr -d '\r')"
    [ "$local_size" = "$remote_size" ] \
        || die "size mismatch after push: local $local_size, board $remote_size"
    ok "pushed, $remote_size bytes verified on the board"

    adb shell "chmod 755 $BOARD_DIR/$BINARY"

    printf '\n--- mpu6050-probe --dump ---\n'
    # 2>&1 because the tool writes its diagnostics to stderr.
    adb shell "$BOARD_DIR/$BINARY --dump" 2>&1
    printf -- '--- end of probe output ---\n'

    printf '\nIf the probe above reported nothing at 0x68, cross check with the\n'
    printf 'Python implementation that already worked on this board:\n'
    printf '    adb shell "python3 /userdata/i2c-bitbang.py scan"\n'
    printf 'If Python finds it and this does not, the port is wrong.\n'
}

# --------------------------------------------------------------------------
# 5. Judge the result.
#
# The exact numbers matter, and there are four of them, so they are checked
# rather than eyeballed. |a| raw must NOT be 1.000 on this part: the module
# leans 9.3 degrees, so gravity spills onto X and the raw magnitude sits near
# 1.062. After the calibration is applied it must come back to 1.000. Those
# two are the whole point of the run.
# --------------------------------------------------------------------------
verify_output() {
    step "checking the result"

    local out="$1"
    local failures=0

    check() {
        if printf '%s' "$out" | grep -q "$1"; then
            ok "$2"
        else
            printf '  MISS %s\n' "$2"
            failures=$((failures + 1))
        fi
    }

    check "0x68: present"     "the part answers at 0x68"
    check " 0 nack(s)"        "every byte was acknowledged"
    check " 0 io error"       "no gpio write errors"

    # The two magnitudes, if the dump ran. These are the checks that would have
    # caught the accelerometer bias mistake, so they are not optional.
    local raw cal
    raw="$(printf '%s' "$out" | sed -n 's/.*|a| raw *= *\([0-9.]*\).*/\1/p')"
    cal="$(printf '%s' "$out" | sed -n 's/.*|a| calibrated *= *\([0-9.]*\).*/\1/p')"

    if [ -n "$cal" ]; then
        if awk "BEGIN{exit !($cal >= 0.95 && $cal <= 1.05)}"; then
            ok "|a| calibrated = $cal g (gravity preserved)"
        else
            printf '  MISS |a| calibrated = %s g, expected ~1.000\n' "$cal"
            printf '       near 0.000 means the bias absorbed gravity;\n'
            printf '       near %s means the bias was not applied\n' "$raw"
            failures=$((failures + 1))
        fi
    else
        printf '  note: no calibrated magnitude in the output (did --dump run?)\n'
    fi

    if [ -n "$raw" ]; then
        if awk "BEGIN{exit !($raw >= 0.9 && $raw <= 1.2)}"; then
            ok "|a| raw = $raw g (uncalibrated, leans as expected)"
        else
            printf '  MISS |a| raw = %s g, expected 0.9 to 1.2\n' "$raw"
            failures=$((failures + 1))
        fi
    fi

    printf '\n'
    if [ "$failures" -eq 0 ]; then
        printf 'PASS: the C port matches the Python implementation.\n'
    else
        printf '%d check(s) did not pass. The C transport is not yet equivalent\n' \
               "$failures"
        printf 'to the Python one that already worked on this board.\n'
        exit 1
    fi
}

main() {
    printf 'MPU6050 board verification\n'
    printf 'repo: %s\n' "$REPO_DIR"

    find_toolchain
    build_binary
    check_board

    # Capture rather than tee: run_probe has to stay in this shell, otherwise
    # its die() exits a subshell and the script would carry on as if the push
    # had worked.
    local captured
    captured="$(run_probe)"
    printf '%s\n' "$captured"

    verify_output "$captured"
}

main "$@"
