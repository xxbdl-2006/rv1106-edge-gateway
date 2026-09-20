#!/bin/sh

set -eu

SERVER=/oem/usr/bin/rkaiq_3A_server
PIDFILE=/tmp/rkaiq_3A_server.pid

if pidof rkipc >/dev/null 2>&1; then
    echo "rkipc is running and manages its own 3A lifecycle."
    exit 0
fi

if pidof rkaiq_3A_server >/dev/null 2>&1; then
    echo "rkaiq_3A_server is already running."
    exit 0
fi

if [ ! -x "$SERVER" ]; then
    echo "3A server is missing: $SERVER" >&2
    exit 1
fi

start-stop-daemon \
    --start \
    --background \
    --make-pidfile \
    --pidfile "$PIDFILE" \
    --exec "$SERVER" \
    -- --silent

sleep 2

if ! pidof rkaiq_3A_server >/dev/null 2>&1; then
    echo "rkaiq_3A_server failed to start." >&2
    exit 1
fi

echo "rkaiq_3A_server started."
