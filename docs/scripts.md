# Host-side scripts

These are optional utilities. The kernel driver, including the air mouse, works
without them.

## Dependencies

Core capture/visualization dependencies are in `requirements.txt`. Optional
AHRS and 3D-cube dependencies are in `requirements-viz.txt`.

## Raw IMU capture

The IMU input device is registered only when the module is loaded with
`imu_evdev=1`:

```bash
sudo modprobe -r lg_magic
sudo modprobe lg_magic imu_evdev=1
python3 scripts/capture_imu.py samples.csv
```

`51-lgimu.rules` grants non-root access to the IMU event device. Install it
under `/etc/udev/rules.d/` if needed.

## Calibration

`calibrate.py` extracts gyro bias/scale and accelerometer data from a captured
CSV. `convert_calib.py` turns the gyro part into the kernel calibration blob.
The blob and the optional per-unit calibration workflow are described in
[docs/airmouse.md](airmouse.md#optional-per-unit-calibration).

```bash
python3 scripts/calibrate.py --gyro still.csv calib_gyro.json
python3 scripts/calibrate.py --accel samples.csv calib_accel.json
python3 scripts/convert_calib.py calib.json lg_magic_calib.bin
```

`gyro.scale` is deg/s per count by default; `--scale-unit rad` converts JSON
input. The converter rejects non-finite values, out-of-range values and
non-positive scales, and accepts `--from-legacy` for the old eight-float blob.
`--alpha`/`--mouse_k` are accepted for compatibility but written as zero.

## Visualization

`display_imu.py` prints raw IMU data, and with `--calib` can show AHRS Euler
angles or a 3D orientation cube:

```bash
python3 scripts/display_imu.py
python3 scripts/display_imu.py --calib calib.json --ahrs
python3 scripts/display_imu.py --calib calib.json --cube
```

The `--mouse` option and `uinput_mouse.py` are a historical uinput experiment.
They are not a product or debug path; the kernel driver is the only air mouse
implementation. `python-uinput` is not a core dependency.

## Packet analyzer

`lg_magic.py` is the original HIDRAW-level packet analyzer, kept for
historical debugging.

## Tests

```bash
python3 scripts/test_convert_calib.py
make -C tests check
```

The first covers the calibration blob format and units; the second covers the
kernel air mouse math and both parsers.
