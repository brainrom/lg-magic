#!/usr/bin/env python3
import argparse
import json
import math
import threading

import numpy as np

from imu_reader import T_UNIT, find_imu_device, read_samples

R_ALIGN = np.array([
    [ 0,  -1,  0],
    [ -1,  0,  0],
    [ 0,  0, -1],
])


class LPF:
    def __init__(self, alpha=0.2):
        self.alpha = alpha
        self.prev = np.zeros(3)

    def filter(self, gyro):
        self.prev = self.alpha * gyro + (1 - self.alpha) * self.prev
        return self.prev


def load_calibration(filename):
    with open(filename) as f:
        calib = json.load(f)
    calib["accel"]["matrix"] = np.array(calib["accel"]["matrix"])
    calib["accel"]["bias"] = np.array(calib["accel"]["bias"])
    calib["gyro"]["bias"] = np.array(calib["gyro"]["bias"])
    calib["gyro"]["scale"] = np.array(calib["gyro"]["scale"])
    return calib


def apply_calibration(accel, gyro, calib):
    a = np.array(accel, dtype=float)
    g = np.array(gyro, dtype=float)
    a_corr = calib["accel"]["matrix"].dot(a - calib["accel"]["bias"])
    g_corr = (g - calib["gyro"]["bias"]) * calib["gyro"]["scale"]
    return R_ALIGN @ a_corr, R_ALIGN @ g_corr


def accel_to_quat(a):
    ax, ay, az = a / np.linalg.norm(a)
    roll = math.atan2(ay, az)
    pitch = math.atan2(-ax, math.sqrt(ay * ay + az * az))
    cy, sy = math.cos(0.0), math.sin(0.0)
    cp, sp = math.cos(pitch / 2), math.sin(pitch / 2)
    cr, sr = math.cos(roll / 2), math.sin(roll / 2)
    return np.array([
        cr * cp * cy + sr * sp * sy,
        sr * cp * cy - cr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
    ])


def read_events(args):
    dev = find_imu_device()
    print(f"Using device: {dev.name} ({dev.path})")

    calib = load_calibration(args.calib) if args.calib else None
    gyro_filter = LPF()
    q = None
    madgwick = None

    if calib and args.mouse:
        import uinput_mouse
    if calib and args.cube:
        import draw_cube
    if calib and (args.ahrs or args.cube):
        from ahrs.filters import Madgwick
        if args.ahrs:
            from ahrs.common.orientation import q2euler
        madgwick = Madgwick(sampleperiod=T_UNIT, beta=0.1)

    print("Press Ctrl+C to stop...")
    for _counter, dt, accel, gyro in read_samples(dev):
        if not calib:
            print(f"dt={dt if dt else 0:.5f}s | Accel={accel} | Gyro={gyro}")
            continue

        a_corr, g_corr = apply_calibration(accel, gyro, calib)
        g_corr *= math.pi / 180

        if args.mouse:
            gyro_filt = gyro_filter.filter(g_corr)
            print(f"dt={dt if dt else 0:.5f}s | gyro_filt={gyro_filt}")
            uinput_mouse.imu_to_mouse_from_rads(
                -gyro_filt[2], -gyro_filt[1], s_x=30.0, s_y=30.0
            )

        if args.ahrs or args.cube:
            if q is None:
                q = accel_to_quat(a_corr)
            q = madgwick.updateIMU(q, gyr=g_corr, acc=a_corr)
            if args.cube:
                draw_cube.update(q)
            if args.ahrs:
                euler = np.degrees(q2euler(q))
                print(f"Roll={euler[0]:+.2f}  Pitch={euler[1]:+.2f}  Yaw={euler[2]:+.2f}")
                continue

        print(f"dt={dt if dt else 0:.5f}s | Accel={a_corr} | Gyro={g_corr}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--calib", help="calibration JSON file")
    parser.add_argument("--ahrs", action="store_true", help="Display Euler angles")
    parser.add_argument("--cube", action="store_true", help="Display spinning cube")
    parser.add_argument("--mouse", action="store_true", help="AirMouse via uinput")
    args = parser.parse_args()

    if args.cube:
        import draw_cube
        t = threading.Thread(target=read_events, kwargs={"args": args}, daemon=True)
        t.start()
        draw_cube.start()
    else:
        read_events(args)


if __name__ == "__main__":
    main()
