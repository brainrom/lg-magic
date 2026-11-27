# Air mouse reference

The driver provides air-mouse control from the remote's SCD (Sensor
Calibration Data), with an optional per-unit calibration blob for precision.

## Tuning

Per-device `airmouse`, `airmouse_threshold`, `sensitivity_x` and
`sensitivity_y` are described in the main README's
[Configuration](../README.md#configuration) section. `motion` is a read-only
diagnostic attribute; module parameters are probe-time defaults.

## Zero-config SCD path

The driver requests the SCD with `F9 17` and uses:

- report period at float 0 (`0.01` on tested units);
- accelerometer matrix at floats 2..10;
- gyroscope matrix at floats 24..32.

The report period drives the cursor gain, gravity propagation and auto-bias.
The gyro matrix maps raw counts to angular velocity in the device frame; the
accelerometer matrix maps raw counts to the up direction. The gyro matrix is
also decomposed into a rotation (the remap) and per-axis scales; the remap
rotates the pointing axis.

Each matrix must decompose into a proper rotation and per-axis scales; invalid
payloads are rejected. A validated table is cached by MAC and re-parsed on
reconnect.

The SCD has no per-unit gyro bias; the driver learns it while the remote is
still. A valid calibration blob disables auto-bias and keeps its measured bias.

## Optional per-unit calibration

### Blob format

Version 1 is a 40-byte little-endian fixed-point record: magic `LGC2`,
version 1, size 40, `gyro_bias[3]` in raw counts and `gyro_scale[3]` in
deg/s per count. Two legacy fields (`alpha`, `mouse_k`) are ignored. Only
`gyro_bias` and `gyro_scale` are physical. A zero or negative scale makes the
blob invalid; a zero bias is valid and disables auto-bias.

Firmware lookup order:

1. `lg_magic_calib_<AA_BB_CC_DD_EE_FF>.bin`;
2. `lg_magic_calib.bin`;
3. SCD only.

### Capturing a calibration

Load the module with `imu_evdev=1` and use the tools in
[scripts.md](scripts.md#calibration). Install the blob under `/lib/firmware/`.
A valid blob overrides the SCD gyro scale/bias for that unit.

## Algorithm

All report-path calculations use Q16.16 fixed point.

- Correct the gyro by bias and map it to the device frame.
- Apply the accelerometer matrix and normalize; the sensor reports specific
  force, so this is the up vector.
- Track gravity with the gyro and correct it with the accelerometer. Ignore a
  sample when its magnitude is far from 1g. While still, replace the estimate
  with the accelerometer; otherwise correct toward it. A new estimate learns
  fast until a still sample confirms it, then tracks slowly, so a bad startup
  sample is repaired without putting the remote down.
- Build the cursor basis from the current up vector and the fixed pointing
  axis `p`. Motion is `omega x p` projected on that basis, so a roll around
  `p` cancels. No stored screen-forward or initial yaw is used.
- Before gravity is available, move the pointer on the raw `z`/`x` axes
  (remapped into the device frame) for immediate feedback.
- Shake enters pointer mode; a mapped button returns to button mode. In button
  mode the wheel is UP/DOWN; in pointer mode it is a normal wheel and the wheel
  button is `BTN_LEFT`.

## SCD fields used

The SCD payload is 260 bytes. The float array starts at byte 9; float k is at
`9 + 4k`.

| Floats | Bytes | Use |
|---|---|---|
| 0 | 9..12 | report period |
| 2..10 | 17..52 | accelerometer matrix |
| 24..32 | 105..140 | gyroscope matrix |

The remaining SCD fields are not used by the driver.

## Troubleshooting

Check `dmesg | grep lg_magic` for SCD and calibration messages. `airmouse`,
`motion` and the tuning attributes are under
`/sys/bus/hid/devices/*/airmouse`.

If the pointer does not move, confirm `airmouse=1` and shake the remote; with
no SCD and no blob the pointer is disabled while buttons and the wheel keep
working.

If it drifts, let the remote lie still so auto-bias can settle. If the axes
look wrong, hold it still for a moment so gravity is replaced by the
accelerometer; otherwise the sensor frame or pointing axis is wrong.

## Testing

`make -C tests check` covers SCD parsing, matrix/blob validation, the
projection and its roll/tilt invariants, gravity seeding and recovery, the
pre-gravity fallback, and auto-bias. The conversion tooling is tested in
[scripts.md](scripts.md#tests).
