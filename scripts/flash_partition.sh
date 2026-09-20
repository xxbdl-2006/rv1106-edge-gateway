#!/bin/sh

set -eu

if [ "$#" -ne 2 ] && [ "$#" -ne 4 ]; then
    echo "Usage: $0 PARTITION IMAGE [HASH_TOOL EXPECTED_HASH]" >&2
    exit 2
fi

PARTITION="$1"
IMAGE="$2"
HASH_TOOL="${3:-}"
EXPECTED_HASH="${4:-}"
TARGET="/dev/block/by-name/$PARTITION"

case "$PARTITION" in
    boot|uboot|idblock|env)
        ;;
    rootfs|userdata)
        echo "Refusing to write $PARTITION while the Linux system is running." >&2
        echo "Use the Rockchip upgrade tool, SD-card update, or a recovery flow." >&2
        exit 1
        ;;
    *)
        echo "Unsupported partition: $PARTITION" >&2
        exit 1
        ;;
esac

if [ ! -e "$TARGET" ]; then
    echo "Partition does not exist: $TARGET" >&2
    exit 1
fi

if [ ! -f "$IMAGE" ]; then
    echo "Image does not exist: $IMAGE" >&2
    exit 1
fi

LINK_TARGET="$(readlink "$TARGET" || true)"
if [ -z "$LINK_TARGET" ]; then
    echo "Could not resolve partition link: $TARGET" >&2
    exit 1
fi

BLOCK_NAME="$(basename "$LINK_TARGET")"
SYSFS_SIZE="/sys/class/block/$BLOCK_NAME/size"

if [ -r "$SYSFS_SIZE" ]; then
    PARTITION_BYTES=$(( $(cat "$SYSFS_SIZE") * 512 ))
elif command -v blockdev >/dev/null 2>&1; then
    PARTITION_BYTES="$(blockdev --getsize64 "$TARGET")"
else
    echo "Cannot determine partition size for $TARGET" >&2
    exit 1
fi

IMAGE_BYTES="$(wc -c < "$IMAGE" | tr -d ' ')"

if [ "$IMAGE_BYTES" -eq 0 ]; then
    echo "Image is empty: $IMAGE" >&2
    exit 1
fi

if [ "$IMAGE_BYTES" -gt "$PARTITION_BYTES" ]; then
    echo "Image is larger than partition." >&2
    echo "Image bytes:     $IMAGE_BYTES" >&2
    echo "Partition bytes: $PARTITION_BYTES" >&2
    exit 1
fi

if grep -q " $TARGET " /proc/mounts 2>/dev/null; then
    echo "Refusing to write mounted partition: $TARGET" >&2
    exit 1
fi

if [ -n "$HASH_TOOL" ]; then
    if ! command -v "$HASH_TOOL" >/dev/null 2>&1; then
        echo "Hash tool is unavailable on the board: $HASH_TOOL" >&2
        exit 1
    fi

    ACTUAL_HASH="$("$HASH_TOOL" "$IMAGE" | awk '{print $1}')"
    EXPECTED_HASH="$(printf '%s' "$EXPECTED_HASH" | tr 'A-F' 'a-f')"
    ACTUAL_HASH="$(printf '%s' "$ACTUAL_HASH" | tr 'A-F' 'a-f')"

    if [ "$ACTUAL_HASH" != "$EXPECTED_HASH" ]; then
        echo "Image hash mismatch." >&2
        echo "Expected: $EXPECTED_HASH" >&2
        echo "Actual:   $ACTUAL_HASH" >&2
        exit 1
    fi
fi

echo "Partition : $PARTITION"
echo "Target    : $TARGET -> $LINK_TARGET"
echo "Image     : $IMAGE"
echo "Image size: $IMAGE_BYTES bytes"
echo "Part size : $PARTITION_BYTES bytes"
echo "Writing image..."

sync
dd if="$IMAGE" of="$TARGET" bs=1M conv=fsync
sync

echo "Flash complete: $PARTITION"
