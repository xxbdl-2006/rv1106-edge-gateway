#!/usr/bin/env python3

import argparse
from pathlib import Path

from PIL import Image


def clamp(value):
    return max(0, min(255, int(value)))


def convert_nv12(data, width, height, uv_offset, uv_stride, swap_uv):
    y_plane = data[: width * height]
    uv_plane = data[uv_offset:]
    pixels = []

    for row in range(height):
        output_row = []
        uv_row = row // 2

        for column in range(width):
            y_value = y_plane[row * width + column]
            uv_index = uv_row * uv_stride + (column // 2) * 2

            first = uv_plane[uv_index]
            second = uv_plane[uv_index + 1]
            if swap_uv:
                u_value = second - 128
                v_value = first - 128
            else:
                u_value = first - 128
                v_value = second - 128

            red = y_value + 1.402 * v_value
            green = y_value - 0.344136 * u_value - 0.714136 * v_value
            blue = y_value + 1.772 * u_value
            output_row.append((clamp(red), clamp(green), clamp(blue)))

        pixels.extend(output_row)

    image = Image.new("RGB", (width, height))
    image.putdata(pixels)
    return image


def main():
    parser = argparse.ArgumentParser(
        description="Convert a raw NV12 buffer into a PNG for layout diagnostics."
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument(
        "--uv-offset",
        type=int,
        help="byte offset of the UV plane; default is width * height",
    )
    parser.add_argument(
        "--uv-stride",
        type=int,
        help="bytes per UV row; default is width",
    )
    parser.add_argument(
        "--swap-uv",
        action="store_true",
        help="interpret the UV plane as VU, which is NV21 ordering",
    )
    args = parser.parse_args()

    width = args.width
    height = args.height
    if width <= 0 or height <= 0 or (height % 2) != 0:
        raise SystemExit("width must be positive and height must be even")

    uv_offset = args.uv_offset if args.uv_offset is not None else width * height
    uv_stride = args.uv_stride if args.uv_stride is not None else width

    data = args.input.read_bytes()
    required = uv_offset + uv_stride * (height // 2)
    if len(data) < required:
        raise SystemExit(
            f"input is too small: need {required} bytes, got {len(data)} bytes"
        )

    image = convert_nv12(data, width, height, uv_offset, uv_stride, args.swap_uv)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
