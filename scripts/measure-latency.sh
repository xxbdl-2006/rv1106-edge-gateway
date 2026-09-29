#!/usr/bin/env bash
#
# End-to-end latency measurement: what a viewer actually waits for.
#
# "Latency" for a live stream is two numbers and the project had been quoting
# one vague figure for both:
#
#   time to first picture  - how long a viewer stares at nothing after asking.
#                            Two candidate causes with opposite owners: the
#                            encoder only starts a new viewer at an IDR, so the
#                            wait could be bounded by the GOP (the gateway can
#                            tune it, `-g`); or it could be the client's own
#                            handshake and internal buffering (nothing the
#                            gateway can do about it).
#
#   steady-state spacing   - how evenly frames arrive once they do. Drift here
#                            would mean something is buffering and the delay
#                            grows over time, which is worse than a fixed one.
#
# The GOP sweep is what separates the two causes. Shrink the GOP from 30 to 5:
# if the time to first picture moves, it is IDR alignment and the gateway owns
# it. If it does not move, the wait is client-side, and quoting it as gateway
# latency was wrong.
#
# The camera can only be opened by one process, so the production gateway is
# stopped for the run and restored afterwards, including on failure. Same trade
# verify-osd.sh already makes.
#
# 🔴 IS THIS SAFE TO SWEEP?  No, not as it stands, and it left the board in a
# crash loop once already.
#
# Starting and stopping encoder instances back to back wedges the ISP. The
# second instance then gets "VIDIOC_DQBUF timed out" and exits, and the
# production gateway cannot capture either afterwards -- it restarts every 5 s
# and dies again. Restarting rkaiq_3A_server is NOT enough to clear it; the
# board needs a reboot.
#
# So more than one GOP per invocation is refused unless you opt in with
# ALLOW_ISP_HAZARD=1, and then you should reset the ISP between configs and
# expect to reboot if it goes wrong. Measuring the production stream from the
# client side (frame-arrival.py, first-frame-stats.py) carries none of this
# risk and is the better default.
#
# Usage:
#   bash scripts/measure-latency.sh
#   GOP_LIST="5 30" ALLOW_ISP_HAZARD=1 RUNS=3 bash scripts/measure-latency.sh
#
set -u

export MSYS_NO_PATHCONV=1

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BOARD_BIN="/userdata/v4l2_mpp_encode"
TEST_PORT="${TEST_PORT:-8654}"
RUNS="${RUNS:-4}"
FRAMES="${FRAMES:-180}"
GOP_LIST="${GOP_LIST:-30 15 5}"

WORK_DIR="$REPO_DIR/.latency"
mkdir -p "$WORK_DIR"

die() { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }
note() { printf '==> %s\n' "$*"; }

board() { adb shell "$1" 2>/dev/null | tr -d '\r'; }

# Every encoder process is called v4l2_mpp_encode, the test instance included,
# so pidof cannot tell the gateway from our own probe. The supervisor pid file
# can: the gateway has one, a bare encoder does not.
encoder_pid()     { board "pidof v4l2_mpp_encode"; }
supervisor_pid()  { board "cat /tmp/gateway-supervisor.pid 2>/dev/null"; }
gateway_up()      { [ -n "$(supervisor_pid)" ]; }

require_adb() {
    local waited=0 hinted=0
    while :; do
        adb devices 2>/dev/null | grep -q 'device$' && return 0
        [ "$hinted" = 0 ] && { printf '板子没响应，重插一下；最多等 150 s\n' >&2; hinted=1; }
        [ "$waited" -ge 150 ] && return 1
        sleep 2; waited=$((waited + 2))
    done
}

port_open() { [ "$(board "netstat -tln 2>/dev/null | grep -c ':$1 '")" != "0" ]; }

restore_gateway() {
    note "把生产网关放回去"
    [ -n "$(encoder_pid)" ] && { board "kill -9 \$(pidof v4l2_mpp_encode)" >/dev/null; sleep 1; }
    board "/etc/init.d/S99gateway start" >/dev/null
    local waited=0
    while [ "$waited" -lt 60 ]; do
        if gateway_up; then
            note "网关已恢复（supervisor $(supervisor_pid)）"
            return 0
        fi
        sleep 3; waited=$((waited + 3))
    done
    printf '警告：网关没自动起来，手动 /etc/init.d/S99gateway restart\n' >&2
    return 1
}
trap restore_gateway EXIT

require_adb || die "adb 不可用"
command -v ffmpeg >/dev/null || die "找不到 ffmpeg，加进 PATH"
ffmpeg -version 2>/dev/null | head -1

# Guard the hazard described at the top: one config is fine, a sweep is not.
CONFIG_COUNT=$(printf '%s\n' $GOP_LIST | grep -c .)
if [ "$CONFIG_COUNT" -gt 1 ] && [ "${ALLOW_ISP_HAZARD:-0}" != "1" ]; then
    cat >&2 <<'WARN'
拒绝执行：一次跑多个 GOP 会反复开关 /dev/video11，把 ISP 卡死。
上次的后果是第二个实例拿不到帧（VIDIOC_DQBUF timed out），生产网关随后
每 5 秒崩溃重启，而且重启 rkaiq_3A_server 不足以恢复，必须重启板子。

要跑请显式确认：ALLOW_ISP_HAZARD=1 GOP_LIST="5 30" bash scripts/measure-latency.sh
但请先用单档（GOP_LIST 只给一个值），并在档之间重置 ISP。

从客户端侧测生产流（frame-arrival.py / first-frame-stats.py）没有这个风险，
是更合适的默认做法。
WARN
    exit 1
fi

note "确认 /userdata/v4l2_mpp_encode 与 frame-arrival.py 都在"
[ -n "$(board "ls -l $BOARD_BIN 2>/dev/null")" ] || die "$BOARD_BIN 不在板上"
[ -f "$REPO_DIR/scripts/frame-arrival.py" ] || die "缺 frame-arrival.py"

note "停掉生产网关（摄像头只能被一个进程打开）"
board "/etc/init.d/S99gateway stop" >/dev/null
waited=0
while [ -n "$(encoder_pid)" ] && [ "$waited" -lt 30 ]; do sleep 2; waited=$((waited + 2)); done
[ -n "$(encoder_pid)" ] && die "网关还占着摄像头（pid $(encoder_pid)）"

# Enough frames for the runs plus slack, since -n counts frames, not seconds.
TEST_FRAMES=$(( RUNS * FRAMES + 300 ))

: > "$WORK_DIR/results.txt"

for gop in $GOP_LIST; do
    note "=== GOP=$gop（-n $TEST_FRAMES ≈ $((TEST_FRAMES / 30))s）==="
    board "rm -f /tmp/latency-gop.log"
    board "setsid $BOARD_BIN -d /dev/video11 -w 1280 -H 720 --warmup 30 \
           --sink rtsp --rtsp-port $TEST_PORT --threads --ring-slots 4 \
           -g $gop -n $TEST_FRAMES --quiet \
           >/tmp/latency-gop.log 2>&1 < /dev/null & sleep 2; echo launched" >/dev/null

    port_open "$TEST_PORT" || die "GOP=$gop：端口 $TEST_PORT 没起来"
    note "端口已开，开始采样"

    for run in $(seq 1 "$RUNS"); do
        printf -- "--- GOP=%s run %s ---\n" "$gop" "$run" | tee -a "$WORK_DIR/results.txt"
        python "$REPO_DIR/scripts/frame-arrival.py" \
            "rtsp://172.32.0.93:$TEST_PORT/live/0" "$FRAMES" \
            | tee -a "$WORK_DIR/results.txt"
        sleep 1
    done

    note "等测试实例自己退出并打印汇总"
    waited=0
    while [ -n "$(encoder_pid)" ] && [ "$waited" -lt 90 ]; do sleep 3; waited=$((waited + 3)); done
    board "grep -E 'Captured|Average FPS|Ring|dropped' /tmp/latency-gop.log | tail -10" \
        | tee -a "$WORK_DIR/results.txt"
    printf '\n' | tee -a "$WORK_DIR/results.txt"
done

note "结果在 $WORK_DIR/results.txt"
