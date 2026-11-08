#!/usr/bin/env python3

import argparse
import json
import struct
import sys

def main():
    parser = argparse.ArgumentParser(description="Convert JSON calibration file to calibration blob.")
    parser.add_argument("json_file", help="Path to input JSON file")
    parser.add_argument("output_file", help="Path to output binary file")
    parser.add_argument("--alpha", type=float, required=True, help="Alpha filter coefficient")
    parser.add_argument("--mouse_k", type=float, required=True, help="Airmouse sensitivity")

    args = parser.parse_args()

    # Load JSON
    try:
        with open(args.json_file, "r") as f:
            data = json.load(f)
    except Exception as e:
        print(f"Error reading JSON: {e}")
        sys.exit(1)

    gyro = data.get("gyro", {})
    bias = gyro.get("bias", [])
    scale = gyro.get("scale", [])

    # Ensure arrays are exactly length 3
    if len(bias) != 3 or len(scale) != 3:
        print("Error: bias and scale must each have exactly 3 elements.")
        sys.exit(1)

    try:
        packed_data = struct.pack(
            "3f3fff",  # 3 floats, 3 floats, float, float
            *map(float, bias),
            *map(float, scale),
            args.alpha,
            args.mouse_k
        )
    except struct.error as e:
        print(f"Struct packing error: {e}")
        sys.exit(1)

    # Save binary file
    with open(args.output_file, "wb") as bf:
        bf.write(packed_data)

    print(f"Saved struct to {args.output_file}")

if __name__ == "__main__":
    main()
