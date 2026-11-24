# LG Magic Remote (MR20) Linux Kernel Driver

**Language:** **English🇬🇧** [Русский🇷🇺](README.ru.md)

![LG Magic Remote](images/lg_magic_remote.png)

## Overview

A Linux kernel driver for the LG Magic Remote (MR20 and similar models). It
provides button mapping, pointer control and raw IMU access, with an optional
voice-input subproject.

## Features

### Buttons

Number keys, navigation, media controls, colour buttons and function keys. In
button mode the wheel acts as UP/DOWN.

### Air mouse

The air mouse works without additional calibration: the driver uses the
remote's SCD (Sensor Calibration Data) and estimates the gyro bias at runtime.
Shaking the remote enables pointer mode. In pointer mode the wheel reports
normal scroll events and the wheel button is `BTN_LEFT`. Pressing any button
other than the wheel button returns to button mode.

Sensitivity and the shake threshold can be tuned per device. An optional
per-unit calibration blob is available for higher precision. See
[docs/airmouse.md](docs/airmouse.md).

### IMU data

Raw data from the inertial measurement unit (IMU) — accelerometer and
gyroscope — is exposed as a separate input device when the module is loaded
with `imu_evdev=1`.

### Voice input

Optional microphone support: a privileged service, a WirePlumber/PipeWire
source and an optional FIFO. See [voice/README.md](voice/README.md) for
installation and architecture.

## Installation

### Prerequisites

- Linux kernel headers
- DKMS (for the DKMS installation)
- Build tools (`make`, `gcc`)

### Manual build

```bash
make -C kernel
sudo insmod kernel/lg_magic.ko
```

### DKMS

```bash
sudo mkdir /usr/src/lg-magic-1.0
sudo cp * /usr/src/lg-magic-1.0/
sudo dkms add -m lg-magic -v 1.0
sudo dkms build -m lg-magic -v 1.0
sudo dkms install -m lg-magic -v 1.0
```

## Configuration

### Module parameters

These are **probe-time defaults**. They are copied into each device when it
probes; use the per-device sysfs attributes for runtime tuning.

```bash
sudo modprobe lg_magic airmouse=1 airmouse_threshold=300 \
    sensitivity_x=600 sensitivity_y=600 imu_evdev=1 debug=2
```

| Parameter | Units | Default | Meaning |
|---|---|---|---|
| `airmouse` | 0/1 | 1 | Enable pointer mode at probe |
| `airmouse_threshold` | deg/s | 300 | Shake threshold that enters pointer mode |
| `sensitivity_x` | px/rad | 600 | Horizontal cursor sensitivity |
| `sensitivity_y` | px/rad | 600 | Vertical cursor sensitivity |
| `imu_evdev` | 0/1 | 0 | Expose raw IMU data as an input device |
| `debug` | 0-2 | 1 | Kernel module debug output level |

### Sysfs

The attributes are per HID device. Locate them with:

```bash
ls /sys/bus/hid/devices/*/airmouse
```

| Attribute | Access | Units | Meaning |
|---|---|---|---|
| `airmouse` | rw | 0/1 | Enable pointer mode |
| `airmouse_threshold` | rw | deg/s | Shake threshold |
| `sensitivity_x` | rw | px/rad | Horizontal cursor sensitivity |
| `sensitivity_y` | rw | px/rad | Vertical cursor sensitivity |

Diagnostic attributes (`fw_version`, `fw_major`, `fw_zone`, `battery_level`,
`scd`, `motion`) are available in the same directory.

## Project structure

- `kernel/` — kernel module sources and DKMS configuration
- `scripts/` — optional host-side utilities, see [docs/scripts.md](docs/scripts.md)
- `docs/` — reference documentation
- `voice/` — optional microphone subproject, see [voice/README.md](voice/README.md)
- `images/` — README assets
- `51-lgimu.rules` — udev rule for non-root IMU access

## Documentation

- [docs/airmouse.md](docs/airmouse.md) — SCD path, optional calibration, algorithm, tuning
- [docs/hid-protocol.md](docs/hid-protocol.md) — raw reports, vendor commands, unsupported features
- [docs/scripts.md](docs/scripts.md) — capture, calibration, visualization and tests
- [voice/README.md](voice/README.md) — microphone subproject

## Compatibility

- **Tested with:** LG Magic Remote MR20
- **Kernel:** 5.10+ (tested on 6.11 and 7.3)
- **Python:** 3.9+ for the optional host-side tooling

## Contributing

Bug reports, additional device support and fixes are welcome.

## License

GPL v2 — same as the Linux kernel.

Copyright © 2025 Ilya Chelyadin and contributors. This project is not affiliated with LG Electronics.
