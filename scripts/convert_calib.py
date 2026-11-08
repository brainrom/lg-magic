#!/usr/bin/env python3
"""Convert LG Magic airmouse calibration to the kernel blob format.

The kernel module only understands the versioned, little-endian fixed-point
format defined below.  Values are signed 16.16 fixed-point numbers stored in
32 bits.

Use --from-legacy to migrate a blob written by older versions of this script
(8 native-endian IEEE-754 binary32 values).
"""

import argparse
import json
import math
import struct
import sys

MAGIC = b"LGC2"
VERSION = 1
Q16_SHIFT = 16
Q16_ONE = 1 << Q16_SHIFT
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1

# magic, version, size, gyro_bias[3], gyro_scale[3], alpha, mouse_k
BLOB_STRUCT = struct.Struct("<4sHH8i")
BLOB_SIZE = BLOB_STRUCT.size
LEGACY_SIZE = 32

GYRO_BIAS_LIMIT = 100.0
GYRO_SCALE_LIMIT = 10.0

assert BLOB_SIZE == 40


def to_q16(value):
    """Convert a float to signed Q16.16, rounding half away from zero."""
    value = float(value)
    if not math.isfinite(value):
        raise ValueError(f"calibration value is not finite: {value!r}")

    scaled = value * Q16_ONE
    if not math.isfinite(scaled):
        raise ValueError(f"calibration value does not fit in Q16.16: {value!r}")

    if scaled >= 0:
        rounded = int(math.floor(scaled + 0.5))
    else:
        rounded = int(math.ceil(scaled - 0.5))

    if not INT32_MIN <= rounded <= INT32_MAX:
        raise ValueError(f"calibration value does not fit in signed Q16.16: {value!r}")

    return rounded


def from_q16(value):
    return value / Q16_ONE


def _check_abs(name, value, limit):
    value = float(value)
    if not math.isfinite(value) or abs(value) > limit:
        raise ValueError(f"{name} must be finite and within +/-{limit:g}: {value!r}")
    return value


def _check_unit(name, value):
    value = float(value)
    if not math.isfinite(value) or not 0.0 <= value <= 1.0:
        raise ValueError(f"{name} must be finite and within [0, 1]: {value!r}")
    return value


def build_blob(gyro_bias, gyro_scale, alpha, mouse_k):
    """Build a version 1 calibration blob from float values."""
    if len(gyro_bias) != 3 or len(gyro_scale) != 3:
        raise ValueError("gyro_bias and gyro_scale must each contain exactly 3 values")

    values = [
        _check_abs(f"gyro_bias[{i}]", value, GYRO_BIAS_LIMIT)
        for i, value in enumerate(gyro_bias)
    ]
    values += [
        _check_abs(f"gyro_scale[{i}]", value, GYRO_SCALE_LIMIT)
        for i, value in enumerate(gyro_scale)
    ]
    values.append(_check_unit("alpha", alpha))
    values.append(_check_unit("mouse_k", mouse_k))

    return BLOB_STRUCT.pack(MAGIC, VERSION, BLOB_SIZE,
                            *(to_q16(value) for value in values))


def parse_blob(data):
    """Parse a version 1 blob; returns Q16.16 integers."""
    if len(data) < BLOB_SIZE:
        raise ValueError(f"calibration blob is too short: {len(data)} bytes")

    magic, version, size, *values = BLOB_STRUCT.unpack_from(data)
    if magic != MAGIC:
        raise ValueError(f"bad calibration magic: {magic!r}")
    if version != VERSION:
        raise ValueError(f"unsupported calibration version: {version}")
    if size != BLOB_SIZE:
        raise ValueError(f"unexpected calibration size in header: {size}")

    return {
        "magic": magic,
        "version": version,
        "size": size,
        "gyro_bias": values[0:3],
        "gyro_scale": values[3:6],
        "alpha": values[6],
        "mouse_k": values[7],
    }


def parse_legacy_blob(data, byteorder="little"):
    """Parse the old 8 x IEEE-754 binary32 blob."""
    if len(data) < LEGACY_SIZE:
        raise ValueError(f"legacy calibration blob is too short: {len(data)} bytes")

    endian = {"little": "<", "big": ">"}[byteorder]
    return struct.unpack(endian + "3f3fff", data[:LEGACY_SIZE])


def load_json_values(path, alpha, mouse_k):
    with open(path, "r", encoding="utf-8") as source:
        data = json.load(source)

    if not isinstance(data, dict):
        raise ValueError("JSON: top level value must be an object")
    gyro = data.get("gyro")
    if not isinstance(gyro, dict):
        raise ValueError("JSON: gyro section is missing")
    bias = gyro.get("bias")
    scale = gyro.get("scale")

    if not isinstance(bias, list) or len(bias) != 3:
        raise ValueError("JSON: gyro.bias must contain exactly 3 values")
    if not isinstance(scale, list) or len(scale) != 3:
        raise ValueError("JSON: gyro.scale must contain exactly 3 values")

    return bias, scale, alpha, mouse_k


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Convert airmouse calibration to the fixed-point kernel blob.")
    parser.add_argument("input_file", help="input JSON file, or legacy blob with --from-legacy")
    parser.add_argument("output_file", help="output calibration blob")
    parser.add_argument("--alpha", type=float, help="low-pass filter coefficient (0.0-1.0)")
    parser.add_argument("--mouse_k", type=float, help="airmouse sensitivity multiplier (0.0-1.0)")
    parser.add_argument("--from-legacy", action="store_true",
                        help="convert an old 8 x float32 blob instead of JSON")
    parser.add_argument("--legacy-endian", choices=("little", "big"), default="little",
                        help="byte order of the legacy blob (default: little)")

    args = parser.parse_args(argv)

    try:
        if args.from_legacy:
            if args.alpha is not None or args.mouse_k is not None:
                raise ValueError("--alpha and --mouse_k cannot be used with --from-legacy")

            with open(args.input_file, "rb") as source:
                values = parse_legacy_blob(source.read(), args.legacy_endian)
            gyro_bias = values[0:3]
            gyro_scale = values[3:6]
            alpha = values[6]
            mouse_k = values[7]
        else:
            if args.alpha is None or args.mouse_k is None:
                raise ValueError("--alpha and --mouse_k are required for JSON input")

            gyro_bias, gyro_scale, alpha, mouse_k = load_json_values(
                args.input_file, args.alpha, args.mouse_k)

        blob = build_blob(gyro_bias, gyro_scale, alpha, mouse_k)

        with open(args.output_file, "wb") as output:
            output.write(blob)

    except (OSError, TypeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"Saved {BLOB_SIZE}-byte fixed-point calibration to {args.output_file}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
