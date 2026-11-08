#!/usr/bin/env python3
import argparse
import csv
import sys

from imu_reader import find_imu_device, read_samples

FIELDS = ["counter", "dt", "ax", "ay", "az", "gx", "gy", "gz"]


def main():
    parser = argparse.ArgumentParser(description="Capture raw IMU samples to CSV")
    parser.add_argument("csv_file", help="output CSV file")
    args = parser.parse_args()

    dev = find_imu_device()
    print(f"Using device: {dev.name} ({dev.path})", file=sys.stderr)

    with open(args.csv_file, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(FIELDS)
        try:
            for counter, dt, accel, gyro in read_samples(dev):
                writer.writerow((counter, dt, *accel, *gyro))
        except KeyboardInterrupt:
            pass

    print(f"Saved to {args.csv_file}", file=sys.stderr)


if __name__ == "__main__":
    main()
