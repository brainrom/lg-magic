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
- accelerometer reference block at floats 20..22 and per-axis coefficients at
  floats 54/56/57;
- gyroscope matrix at floats 24..32.

The report period drives the cursor gain, gravity propagation and auto-bias.
The matrices provide the device-frame rotation; the gyro columns also give the
per-axis gyro scales. The accelerometer tail gives the scale:

```
counts/g = g * reference / coefficient
```

If the tail is missing or implausible, the scale is unknown and the magnitude
gate is skipped. Matrices must decompose into a proper rotation and per-axis
scales; invalid payloads are rejected. A validated table is cached by MAC and
re-parsed on reconnect.

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
- Scale the accelerometer by its per-axis counts/g (when known), apply the
  accelerometer matrix and normalize; the sensor reports specific force, so
  this is the up vector.
- Track gravity with the gyro and correct it with the accelerometer. Ignore a
  sample when its scaled magnitude is far from 1g. While still, replace the
  estimate with the accelerometer; otherwise correct toward it. A new estimate
  learns fast until a still sample confirms it, then tracks slowly, so a bad
  startup sample is repaired without putting the remote down.
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
| 20..22 | 89..100 | accelerometer reference block |
| 24..32 | 105..140 | gyroscope matrix |
| 54 | 225..228 | z acceleration per reference block |
| 56..57 | 233..240 | x/y acceleration per reference block |

The tail mapping was identified from 1g and 8g captures; the remaining tail
fields are unused.

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

`make -C tests check` covers SCD parsing (including the per-axis
accelerometer scale), matrix/blob validation, the projection and its
roll/tilt invariants, gravity seeding and recovery, the pre-gravity fallback,
and auto-bias. The conversion tooling is tested in
[scripts.md](scripts.md#tests).
