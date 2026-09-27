#!/bin/bash
#
# Start the two hour soak run in one step.
#
#   bash scripts/start-2h-soak.sh            # sync pipeline (default)
#   bash scripts/start-2h-soak.sh --threads  # threaded pipeline
#   bash scripts/start-2h-soak.sh --hours 8  # longer run
#
# Everything that bit us before is handled here:
#
#   - adb server is restarted per call; it kept dying between invocations
#   - the gateway is started through S99gateway, which waits for the camera
#   - gateway.env is rewritten so the run parameters are explicit
#   - the log is truncated and soak.csv removed, so the numbers start at zero
#   - ffmpeg uses -t rather than Ctrl+C, which is the only way to get a
#     clean exit code to prove the client finished on its own
#
# The ffmpeg part runs in the foreground and blocks for the whole run.

set -u

HOURS=2
GATEWAY_EXTRA=""
while [ $# -gt 0 ]; do
    case "$1" in
        --threads) GATEWAY_EXTRA="$GATEWAY_EXTRA --threads --ring-slots 4" ;;
        --hours)   HOURS="$2"; shift ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

SECONDS_TO_RUN=$((HOURS * 3600))
STAMP=$(date +%Y%m%d-%H%M%S)
OUT_DIR="D:/luckfox_share"
LOG="$OUT_DIR/soak-${HOURS}h-$STAMP.log"
PROG="$OUT_DIR/soak-${HOURS}h-progress-$STAMP.txt"
H264="$OUT_DIR/soak-${HOURS}h-$STAMP.h264"

export MSYS_NO_PATHCONV=1

adb_sh() {
    local i
    for i in 1 2 3; do
        adb start-server >/dev/null 2>&1
        if out=$(adb shell "$@" 2>&1) && ! printf '%s' "$out" | grep -q "no devices/emulators found"; then
            printf '%s\n' "$out"; return 0
        fi
        sleep 2
    done
    echo "ADB_FAILED: $*" >&2; return 1
}

echo "=== 0. 确认设备在线 ==="
adb start-server >/dev/null 2>&1
if ! adb devices 2>/dev/null | grep -q "device$"; then
    echo "板子不在线。检查 USB 连接后重试。" >&2
    adb devices >&2
    exit 1
fi
adb devices | grep "device$"

echo
echo "=== 1. 写入运行参数并重启网关 ==="
GATEWAY_ARGS="-d /dev/video11 -w 1280 -H 720 --warmup 30 --sink rtsp --rtsp-port 8554 --quiet${GATEWAY_EXTRA}"
adb_sh "echo 'GATEWAY_ARGS=\"$GATEWAY_ARGS\"' > /userdata/gateway.env"
adb_sh "cat /userdata/gateway.env"

adb_sh "/etc/init.d/S99gateway stop" >/dev/null 2>&1
adb_sh ": > /userdata/gateway.log"
adb_sh "rm -f /userdata/soak.csv"
adb_sh "/etc/init.d/S99gateway start" >/dev/null 2>&1

echo "等待接管（rkipc 已死时会白等 20 秒，属已知现象）..."
sleep 30

echo
echo "=== 2. 确认三个进程都在 ==="
adb_sh "echo -n 'supervisor: '; cat /tmp/gateway-supervisor.pid 2>/dev/null || echo NONE; echo -n 'encoder  : '; pidof v4l2_mpp_encode || echo NONE; echo -n '3a       : '; pidof rkaiq_3A_server || echo NONE"

echo
echo "=== 3. 确认参数生效（看 Capture mode）==="
adb_sh "grep -a -m1 'Capture mode' /userdata/gateway.log || echo '(横幅已被截断，属正常)'"
adb_sh "cat /userdata/gateway.env"

echo
echo "=== 4. 启动采样器（每 60 秒一行）==="
adb_sh "setsid /userdata/soak-monitor.sh 60 >/dev/null 2>&1 < /dev/null & sleep 2; echo started"

echo
echo "=== 5. 拉流 ${HOURS} 小时（前台阻塞）==="
echo "输出: $H264"
echo "日志: $LOG"
echo

ffmpeg -rtsp_transport tcp -i rtsp://172.32.0.93:8554/live/0 \
       -c copy -t "$SECONDS_TO_RUN" -nostats -progress "$PROG" -y "$H264" 2> "$LOG"
FFMPEG_EXIT=$?

echo "exit=$FFMPEG_EXIT" >> "$LOG"

echo
echo "=== 6. 收尾 ==="
adb_sh "kill \$(cat /tmp/soak-monitor.pid) 2>/dev/null; echo so_monitor_stopped" >/dev/null 2>&1

echo "ffmpeg 退出码: $FFMPEG_EXIT  （0 = 干净收尾）"
echo
echo "--- 收尾判据 ---"
echo "-- 末行 progress （应为 progress=end）:"
grep -a "progress=" "$PROG" | tail -1
echo "-- 丢帧检查（应无输出）:"
grep -aE "drop_frames=|dup_frames=" "$PROG" | grep -v "=0$" || echo "  drop/dup 全程为 0  ✓"
echo "-- 最后一帧时间:"
grep -a "out_time=" "$PROG" | tail -1
echo
echo "--- 单例健康检查 ---"
adb_sh "tail -n 3 /userdata/soak.csv"

echo
echo "--- 失步检查（必须为 0）---"
adb_sh "grep -a -c 'RTSP: RTSP/1.0' /userdata/gateway.log"

echo
echo "取回数据:"
echo "  adb pull /userdata/soak.csv $OUT_DIR/soak-${HOURS}h-$STAMP.csv"
echo "  adb pull /userdata/gateway.log $OUT_DIR/gateway-${HOURS}h-$STAMP.log"
