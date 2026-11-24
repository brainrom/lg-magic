# Air mouse reference

The driver uses the remote's SCD (Sensor Calibration Data) and provides pointer
control without a calibration file. A per-unit calibration blob is available as
an optional precision upgrade.

## Tuning

Per-device `airmouse`, `airmouse_threshold`, `sensitivity_x` and
`sensitivity_y` are described in the main README's
[Configuration](../README.md#configuration) section. `motion` is a read-only
diagnostic attribute. Module parameters are probe-time defaults.

## Zero-config SCD path

The driver requests the SCD with `F9 17` and parses the two matrices it uses:

- accelerometer matrix at floats 2..10 (bytes 17..52);
- gyroscope matrix at floats 24..32 (bytes 105..140).

The request is skipped when a validated table is already cached for the
remote's MAC address.

Each matrix is a scale times an axis remap. The driver applies the matrix as it
is; it does not require a specific rotation. Validation only checks that the
matrix can be decomposed into a proper rotation (`det +1`) and per-axis scales.
Reflections, singular matrices and malformed payloads are rejected.

The fixed pointing axis is rotated by the gyro remap. The accelerometer matrix
independently maps the accelerometer into the same device frame; the two
matrices may therefore use different rotations, for example when the gyro and
accelerometer are separate chips with non-aligned sensor frames.

The SCD has no per-unit gyro bias. On the SCD-only path the driver updates the
bias while the remote is still:

```
bias += beta * (raw - bias)
```

with a conservative per-report `beta`; the update is skipped when the angular
speed is above the still threshold. A valid calibration blob disables
auto-bias and keeps its measured bias fixed.

A validated SCD table is cached in memory per MAC address, including the raw
payload. On reconnect the driver reuses it and does not request the SCD again.
Only a complete 260-byte table with two valid matrices is cached; a truncated
or malformed response cannot poison the cache.

## Optional per-unit calibration

### Blob format

Version 1 is a 40-byte little-endian fixed-point record:

- magic `LGC2`, version 1, size 40;
- `gyro_bias[3]` in raw counts;
- `gyro_scale[3]` in deg/s per count on disk (the driver converts to rad/s);
- `alpha` and `mouse_k`, legacy fields that the driver ignores and reads only
  to log a one-time deprecation warning.

Only `gyro_bias` and `gyro_scale` are physical. A zero or negative scale makes
the blob invalid; the driver logs a warning and falls back to SCD. A blob with
a zero bias is still a valid file and disables auto-bias, so measure the bias
before installing it.

Firmware lookup order:

1. `lg_magic_calib_<AA_BB_CC_DD_EE_FF>.bin`;
2. `lg_magic_calib.bin`;
3. SCD only.

### Capturing a calibration

Load the module with `imu_evdev=1` and use the capture/calibration tools
described in [scripts.md](scripts.md#calibration). Install the resulting blob
under `/lib/firmware/` as above. A valid blob overrides the SCD gyro scale/bias
for that unit.

## Algorithm

All report-path calculations use Q16.16 fixed point.

1. Correct the raw gyro by the bias in counts.
2. Apply the gyro matrix: angular velocity in rad/s, same frame as the remote.
3. Apply the accelerometer matrix and normalize the result into a gravity
   direction.
4. Low-pass filter gravity and project angular velocity onto gravity-defined
   axes:

   ```
   yaw   = omega . gravity
   h     = normalize(pointing x gravity)
   pitch = omega . h
   ```

   A roll about the pointing axis produces no cursor motion.
5. Apply a small dead zone. Shake detection compares the raw angular speed
   with `airmouse_threshold`.
6. Shake enters pointer mode. A mapped button press returns to button mode. In
   button mode the wheel is UP/DOWN; in pointer mode it is a normal wheel and
   the wheel button is `BTN_LEFT`.
7. Cursor pixels are `-sensitivity * rate * dt`, with X and Y independent.

## SCD fields used

The SCD payload is 260 bytes. The float array starts at byte 9, not after the
5-byte magic; float *k* occupies bytes `9 + 4k .. 12 + 4k`.

| Floats | Bytes | Use |
|---|---|---|
| `2..10` | `17..52` | accelerometer scale/remap matrix |
| `24..32` | `105..140` | gyroscope scale/remap matrix |

The remaining SCD fields are not used by the driver and are not documented
here.

## Troubleshooting

Check the kernel log:

```bash
dmesg | grep lg_magic
```

The driver logs when the SCD is complete, when the matrices are accepted, and
when a table is unusable or no SCD response arrives.

Find the device attributes and inspect the air mouse state:

```bash
ls /sys/bus/hid/devices/*/airmouse
cat /sys/bus/hid/devices/*/airmouse
cat /sys/bus/hid/devices/*/motion
```

If the pointer does not move:

- confirm `airmouse` is `1`;
- shake the remote to enter pointer mode;
- if there is no SCD and no blob, the pointer is intentionally disabled while
  buttons and the wheel keep working;
- check `dmesg` for a malformed SCD or a missing firmware file.

If the cursor drifts on the SCD-only path, let the remote lie still for a few
seconds so auto-bias can settle. With a blob, auto-bias is off by design;
re-measure the bias or remove the blob. Axis inversion is not a supported
option: an inversion means the calibration or the physical assumption is
wrong.

## Testing

```bash
make -C tests check
```

The C tests cover the SCD offsets against a real 260-byte payload, matrix
validation, the blob format and units, the gravity projection, cursor signs
and auto-bias convergence. The conversion tooling has its own tests; see
[scripts.md](scripts.md#tests).
