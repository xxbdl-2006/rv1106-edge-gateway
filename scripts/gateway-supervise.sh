#!/bin/sh
#
# Supervise the gateway process.
#
# Runs on the board from /userdata. Started by /etc/init.d/S99gateway at boot,
# never by hand.
#
# Two jobs:
#   1. Restart the gateway if it exits. A single crash must not take the camera
#      offline until somebody walks over to the board.
#   2. Keep the log bounded. The handoff forbids writing /userdata without
#      limit, and a gateway left running for days will otherwise fill it.
#
# Arguments come from /userdata/gateway.env so they can be changed without
# touching the init script.

# The gateway links the Rockit libraries out of /oem/usr/lib, which are only
# reachable through LD_LIBRARY_PATH. Pick it up no matter how this script was
# started, so the encoder cannot fail with a missing shared library.
#
# RkEnv.sh prepends "$HOME/usr/lib:$HOME/lib:" rather than assigning, so
# sourcing it again on top of an inherited value duplicates the prefix. That
# used to be guarded by GATEWAY_ENV_LOADED, which cannot work: the flag would
# have to survive into this process, and this is started by the init script as
# a separate process that does not export it. Every layer that sources RkEnv
# therefore adds another copy, and the encoder -- two layers down -- ends up
# with the duplication it inherits plus the one added here.
#
# The paths are needed, so sourcing stays; the value is de-duplicated after.
dedup_path() {
    _in="$1"
    _out=""
    _old_ifs="$IFS"
    IFS=':'
    for _p in $_in; do
        case ":$_out:" in
            *":$_p:"*) ;;
            *) _out="${_out:+$_out:}$_p" ;;
        esac
    done
    IFS="$_old_ifs"
    echo "$_out"
}

if [ -f /etc/profile.d/RkEnv.sh ]; then
    . /etc/profile.d/RkEnv.sh
    LD_LIBRARY_PATH="$(dedup_path "$LD_LIBRARY_PATH")"
    export LD_LIBRARY_PATH
fi

ENV_FILE="/userdata/gateway.env"
GATEWAY="/userdata/v4l2_mpp_encode"
LOG="/userdata/gateway.log"
PIDFILE="/tmp/gateway-supervisor.pid"
LOG_LIMIT_BYTES=2097152
RESTART_DELAY_SECONDS=5

if [ -f "$ENV_FILE" ]; then
    . "$ENV_FILE"
fi

# Publish our own pid instead of relying on start-stop-daemon --make-pidfile.
# Two reasons: the process is named "sh", so it cannot be found by name, and
# the pid file is the only handle the init script has. Pid file handling also
# keeps working when the supervisor is started through setsid.
echo $$ > "$PIDFILE"

GATEWAY_ARGS="${GATEWAY_ARGS:--d /dev/video11 -w 1280 -H 720 --warmup 30 --sink rtsp --rtsp-port 8554}"

# busybox here has no stat, so the size comes from wc. Using stat silently
# returned nothing and the log grew without bound.
file_size() {
    if [ -f "$1" ]; then
        wc -c < "$1" 2>/dev/null | tr -d ' '
    fi
}

trim_log() {
    size=$(file_size "$LOG")
    if [ -n "$size" ] && [ "$size" -gt "$LOG_LIMIT_BYTES" ]; then
        tail -n 2000 "$LOG" > "$LOG.tmp" 2>/dev/null
        mv "$LOG.tmp" "$LOG" 2>/dev/null
        log_line "log trimmed to $(file_size "$LOG") bytes"
    fi
}

log_line() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') $1" >> "$LOG"
}

log_line "supervisor starting as pid $$"
log_line "gateway arguments: $GATEWAY_ARGS"
log_line "path=$PATH"

TRIM_EVERY_TICKS=12   # 12 x 5s = one minute
TICK_SECONDS=5

while true; do
    trim_log

    log_line "gateway starting"
    "$GATEWAY" $GATEWAY_ARGS >> "$LOG" 2>&1 &
    gateway_pid=$!

    #
    # Trim while the gateway runs, not only between runs.
    #
    # The gateway writes about 1.5 KB per second, so an eight hour run adds
    # roughly 45 MB. Trimming only after it exits would let the log grow
    # unbounded for as long as the gateway stays up, which for a healthy
    # gateway is forever.
    #
    ticks=0
    while [ -d "/proc/$gateway_pid" ]; do
        sleep "$TICK_SECONDS"
        ticks=$((ticks + 1))
        if [ $((ticks % TRIM_EVERY_TICKS)) -eq 0 ]; then
            trim_log
        fi
    done

    wait "$gateway_pid"
    status=$?

    log_line "gateway exited with status $status, restarting in ${RESTART_DELAY_SECONDS}s"
    sleep "$RESTART_DELAY_SECONDS"
done
