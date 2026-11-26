#!/usr/bin/env python3
"""Capture raw IMU samples to CSV, with optional in-process marks.

The first eight columns are the ones calibrate.py expects:

    counter,dt,ax,ay,az,gx,gy,gz,seq,host_time

Type a label and press Enter while recording to write it to
<base>.marks.csv at the current sample.  Ctrl-C stops the capture.
"""

import argparse
import csv
import queue
import sys
import threading
import time
from pathlib import Path

from imu_reader import find_imu_device, read_samples

FIELDS = ["counter", "dt", "ax", "ay", "az", "gx", "gy", "gz", "seq",
          "host_time"]
MARK_FIELDS = ["seq", "counter", "host_time", "label"]


def marks_path(csv_file):
    return Path(csv_file).with_suffix(".marks.csv")


def read_labels(labels):
    try:
        for line in sys.stdin:
            label = line.strip()
            if label:
                labels.put(label)
    except (EOFError, KeyboardInterrupt):
        pass


def main():
    parser = argparse.ArgumentParser(
        description="Capture raw IMU samples to CSV")
    parser.add_argument("csv_file", help="output CSV file")
    parser.add_argument(
        "--no-mark", action="store_true",
        help="do not read segment labels from stdin")
    args = parser.parse_args()

    dev = find_imu_device()
    print(f"Using device: {dev.name} ({dev.path})", file=sys.stderr)

    labels = queue.Queue()
    if not args.no_mark:
        threading.Thread(target=read_labels, args=(labels,),
                         daemon=True).start()
        print("Type a label and press Enter to mark; Ctrl-C stops.",
              file=sys.stderr)

    marks_file = None
    if not args.no_mark:
        marks_file = open(marks_path(args.csv_file), "w", newline="")
        csv.writer(marks_file).writerow(MARK_FIELDS)

    start = time.monotonic()
    seq = 0

    try:
        with open(args.csv_file, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(FIELDS)

            for counter, dt, accel, gyro in read_samples(dev):
                now = time.monotonic() - start
                writer.writerow((counter, dt, *accel, *gyro, seq,
                                 f"{now:.6f}"))

                if marks_file is not None:
                    while True:
                        try:
                            label = labels.get_nowait()
                        except queue.Empty:
                            break
                        csv.writer(marks_file).writerow(
                            (seq, counter, f"{now:.6f}", label))
                        marks_file.flush()

                seq += 1
    except KeyboardInterrupt:
        pass
    finally:
        if marks_file is not None:
            marks_file.close()

    print(f"Saved to {args.csv_file}", file=sys.stderr)


if __name__ == "__main__":
    main()
