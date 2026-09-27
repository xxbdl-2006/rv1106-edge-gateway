#!/bin/sh
#
# Long run sampler for the RV1106 gateway.
#
#   adb push scripts/soak-monitor.sh /userdata/
#   adb shell "setsid /userdata/soak-monitor.sh 60 >/dev/null 2>&1 < /dev/null & sleep 2; echo started"
#   adb shell "tail -n 30 /userdata/soak.csv"        # progress
#   adb shell "kill \$(cat /tmp/soak-monitor.pid)"   # stop
#
# Writes one CSV row per interval so that a multi-hour run can be judged
# afterwards rather than described from memory. The three things that actually
# bite on an embedded board, and that a "it played for eight hours" answer would
# hide, are:
#
#   rss_kb  - a slow climb means a leak, and the board will eventually die
#   fds     - sockets and files that are opened and never closed
#   threads - the RTSP server creates threads per client; a task leak only shows
#             up after many connect and disconnect cycles
#
# Temperature is recorded too, because a card in a closed enclosure can throttle
# the encoder and quietly drop frames.

INTERVAL="${1:-60}"
CSV="/userdata/soak.csv"
PIDFILE="/tmp/soak-monitor.pid"

# Rockchip libraries, and anything the encoder links, live under /oem.
if [ -f /etc/profile.d/RkEnv.sh ]; then
    . /etc/profile.d/RkEnv.sh
fi

echo $$ > "$PIDFILE"

# /proc/PID/status labels carry a colon ("VmRSS:"), so it has to be stripped
# before comparing or every lookup silently returns nothing.
field() {
    awk -v key="$2" '{ name = $1; sub(/:$/, "", name); if (name == key) { print $2; exit } }' "/proc/$1/status" 2>/dev/null
}

ticks() {
    awk '{ print $14 + $15 }' "/proc/$1/stat" 2>/dev/null
}

# busybox on this board has no stat command, so sizes come from wc.
file_size() {
    if [ -f "$1" ]; then
        wc -c < "$1" 2>/dev/null | tr -d ' '
    fi
}

thread_count() {
    ls "/proc/$1/task" 2>/dev/null | wc -l
}

temperature_mc() {
    for zone in /sys/class/thermal/thermal_zone*; do
        if [ -r "$zone/temp" ]; then
            cat "$zone/temp"
            return
        fi
    done
    echo ""
}

alive() {
    pidof "$1" >/dev/null 2>&1 && echo 1 || echo 0
}

# The supervisor is a shell script, so its process is called "sh" and pidof by
# name would always answer 0. Its pid file is the only reliable handle.
supervisor_alive() {
    if [ -f /tmp/gateway-supervisor.pid ]; then
        holder=$(cat /tmp/gateway-supervisor.pid 2>/dev/null)
        if [ -n "$holder" ] && [ -d "/proc/$holder" ]; then
            echo 1
            return
        fi
    fi
    echo 0
}

if [ ! -s "$CSV" ]; then
    echo "time,uptime_s,pid,rss_kb,fds,threads,cpu_pct,temp_mc,encoder_alive,supervisor_alive,three_a_alive,free_mem_kb,gw_log_bytes" > "$CSV"
fi

previous_ticks=""
previous_seconds=""

while true; do
    pid=$(pidof v4l2_mpp_encode 2>/dev/null)
    now=$(date +%s)
    uptime_s=$(awk '{ printf "%d", $1 }' /proc/uptime 2>/dev/null)
    temp=$(temperature_mc)

    free_mem=$(awk '/MemAvailable/ { print $2 }' /proc/meminfo 2>/dev/null)
    log_bytes=$(file_size /userdata/gateway.log)

    if [ -n "$pid" ]; then
        rss=$(field "$pid" VmRSS)
        fds=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)
        threads=$(thread_count "$pid")
        current_ticks=$(ticks "$pid")

        cpu_pct=""
        if [ -n "$previous_ticks" ] && [ -n "$previous_seconds" ] && [ -n "$current_ticks" ]; then
            elapsed=$((now - previous_seconds))
            if [ "$elapsed" -gt 0 ]; then
                # USER_HZ is 100 on this board, so ticks divided by the elapsed
                # seconds is already a percentage.
                cpu_pct=$(( (current_ticks - previous_ticks) / elapsed ))
            fi
        fi

        previous_ticks=$current_ticks
        previous_seconds=$now
    else
        rss=""
        fds=""
        threads=""
        cpu_pct=""
        previous_ticks=""
        previous_seconds=""
    fi

    echo "$(date '+%Y-%m-%d %H:%M:%S'),$uptime_s,$pid,$rss,$fds,$threads,$cpu_pct,$temp,$(alive v4l2_mpp_encode),$(supervisor_alive),$(alive rkaiq_3A_server),$free_mem,$log_bytes" >> "$CSV"

    # Keep the file small enough for the flash, while keeping several thousand
    # rows of history.
    csv_bytes=$(file_size "$CSV")
    if [ -n "$csv_bytes" ] && [ "$csv_bytes" -gt 2097152 ]; then
        tail -n 5000 "$CSV" > "$CSV.tmp" 2>/dev/null
        mv "$CSV.tmp" "$CSV" 2>/dev/null
    fi

    sleep "$INTERVAL"
done
