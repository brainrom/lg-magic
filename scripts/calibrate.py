#!/usr/bin/env python3
import numpy as np
import csv
import json
import argparse
import sys

# -----------------------------
# Load samples CSV
# CSV format: counter, dt, ax, ay, az, gx, gy, gz... (other columns ignored)
def load_imu_csv(filename):
    a = []
    g = []
    with open(filename, newline='') as f:
        reader = csv.reader(f)
        for row in reader:
            if len(row) < 8:
                continue
            try:
                a.append([float(row[2]), float(row[3]), float(row[4])])
                g.append([float(row[5]), float(row[6]), float(row[7])])
            except ValueError:
                continue  # skip invalid lines
    return np.array(a), np.array(g)

def save_calibration_json(filename, bias=[], matrix=[], gyro_bias=[0,0,0]):
    bias_list = [float(f"{v:.6f}") for v in bias]
    matrix_list = [[float(f"{v:.6f}") for v in row] for row in matrix]
    calib_dict = {
        "accel": {
        "bias": bias_list,
        "matrix": matrix_list
        },
        # Default values
        "gyro" : {
            "bias" : gyro_bias,
            "scale" : [1,1,1]
        }
    }
    with open(filename, "w") as f:
        json.dump(calib_dict, f, indent=4)
    print(f"Calibration saved to {filename}")

# ---------------------------------------
# Define residuals for least squares
# x = [b_x, b_y, b_z, M00, M01, M02, M10, M11, M12, M20, M21, M22]
def residuals(x, accel):
    b = x[0:3]                  # bias
    M = x[3:].reshape(3, 3)     # 3x3 scale/misalignment
    res = []
    for a_raw in accel:
        a_corr = M @ (a_raw - b)
        res.append(np.linalg.norm(a_corr) - 9.80665)  # want |a_corr| = 1g
    return res

# -----------------------------
def calibrate_accel(accel_samples):
    from scipy.optimize import least_squares
    if len(accel_samples) < 6:
        raise ValueError("Need at least 6 samples in different orientations")

    # Initial guess: bias=mean, M=identity
    b0 = np.mean(accel_samples, axis=0)
    M0 = np.eye(3).flatten()
    x0 = np.hstack([b0, M0])

    # Solve
    result = least_squares(residuals, x0, args=(accel_samples,))
    b_calib = result.x[0:3]
    M_calib = result.x[3:].reshape(3, 3)
    return b_calib, M_calib



def calibrate_gyro_bias(gyro_samples):
    if gyro_samples.ndim != 2 or gyro_samples.shape[1] != 3:
        raise ValueError("gyro_samples must be (N, 3) array")

    bias = np.mean(gyro_samples, axis=0)
    return bias.tolist()

def main():
    parser = argparse.ArgumentParser(description="Calculate calibration values from raw IMU samples")
    parser.add_argument("csv_file", help="Path to input CSV file")
    parser.add_argument("output_file", help="Path to output JSON file")

    parser.add_argument("--accel", help="Calculate accelerometer ellipsoidal bias/scale", action='store_true')
    parser.add_argument("--gyro", help="Calculate gyroscope bias from static samples", action='store_true')

    args = parser.parse_args()

    try:
        accel_samples, gyro_samples = load_imu_csv(args.csv_file)
        if args.accel:
            b_calib, M_calib = calibrate_accel(accel_samples)
            save_calibration_json(args.output_file, b_calib, M_calib)
        elif args.gyro:
            g_bias = calibrate_gyro_bias(gyro_samples)
            save_calibration_json(args.output_file, gyro_bias=g_bias)
        else:
            print("You need to select --accel or --gyro")
    except Exception as e:
        print(f"Error reading CSV: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
