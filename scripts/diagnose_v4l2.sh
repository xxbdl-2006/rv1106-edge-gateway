#!/bin/sh

set -u

echo "=== rkipc processes ==="
if command -v pidof >/dev/null 2>&1; then
    pidof rkipc || true
else
    ps | grep '[r]kipc' || true
fi

echo
echo "=== video4linux device names ==="
for name_file in /sys/class/video4linux/video*/name; do
    [ -e "$name_file" ] || continue
    printf '%s: ' "$name_file"
    cat "$name_file"
done

echo
echo "=== media device nodes ==="
ls -l /dev/media* 2>/dev/null || true

echo
echo "=== v4l2-ctl devices ==="
if command -v v4l2-ctl >/dev/null 2>&1; then
    v4l2-ctl --list-devices || true
else
    echo "v4l2-ctl is not installed"
fi

echo
echo "=== media topology ==="
if command -v media-ctl >/dev/null 2>&1; then
    for media_node in /dev/media*; do
        [ -e "$media_node" ] || continue
        echo "--- $media_node ---"
        media-ctl -p -d "$media_node" || true
    done
else
    echo "media-ctl is not installed"
fi

echo
echo "=== per-node capabilities and formats ==="
if command -v v4l2-ctl >/dev/null 2>&1; then
    for video_node in /dev/video*; do
        [ -e "$video_node" ] || continue
        echo "--- $video_node ---"
        v4l2-ctl -d "$video_node" --info || true
        v4l2-ctl -d "$video_node" --list-formats-ext || true
    done
else
    echo "Run /userdata/v4l2_capture --info and --list-formats for each node"
fi
