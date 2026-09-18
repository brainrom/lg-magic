# LG Magic Remote (MR20) Linux Kernel Driver and Tools
**Language:** **English🇬🇧** [Русский🇷🇺](README.ru.md)

![LG Magic Remote](images/lg_magic_remote.png)

## Overview

This project provides a comprehensive Linux kernel driver and toolset for the LG Magic Remote (MR20 and similar models). The driver enables full functionality of the remote including button mapping, gyroscopic airmouse control, and IMU data access. The package includes both a kernel module and Python utilities for calibration, testing, and visualization.

## Project Structure

### Kernel Module Files

- **kernel/dkms.conf** - DKMS configuration file for automated kernel module building
- **kernel/lg_magic_main.c** - Main kernel driver implementation
- **kernel/lg_magic_airmouse.h** - Header file for airmouse calibration structures
- **kernel/lg_magic_airmouse.c** - Airmouse calibration and filtering implementation
- **kernel/Makefile** - Build system for the kernel module
- **51-lgimu.rules** - Udev rule for non-root raw IMU access. Place to `/etc/udev/rules.d/` if needed.

### Python Tools

- **scripts/capture_imu.py** - Raw IMU capture for calibration (requires only python-evdev)
- **scripts/imu_reader.py** - Shared evdev reader for capture and display
- **scripts/lg_magic.py** - HIDRAW-level packet analyzer and debug tool. Initial tool, kept for historical reasons
- **scripts/calibrate.py** - IMU calibration utility (accelerometer and gyroscope)
- **scripts/convert_calib.py** - Converts JSON or legacy float calibration to the fixed-point format used by the kernel module
- **scripts/test_convert_calib.py** - Tests for the calibration converter
- **scripts/display_imu.py** - Real-time IMU data visualization and airmouse emulation

## Kernel Module Features

### Device Support
- Supports LG Magic Remote (HID Bluetooth device 000f:3412)
- Creates two input devices:
  - `LG Magic Remote` - Standard HID events (buttons, wheel, airmouse)
  - `LG Magic Remote IMU` - Raw IMU data (accelerometer + gyroscope)

### Button Mapping
Comprehensive button support including:
- Number keys (0-9), navigation buttons (UP/DOWN/LEFT/RIGHT)
- Media controls (PLAY, PAUSE, STOP, VOLUME, MUTE)
- Color buttons (RED, GREEN, YELLOW, BLUE)
- Special function buttons (HOME, BACK, SETTINGS, GUIDE, INFO, LIST)
- Regional and model specific keys the remote can emit (TEXT, SUBTITLE,
  FAVORITES, ASPECT_RATIO, ZOOM, DATA, LANGUAGE, RADIO, RECENT, app shortcuts)

The four in-band control codes `0x800A`, `0x800D` (voice stream start and stop)
and `0x803E`, `0x803F` (motion start and stop) are not keys and are filtered
out instead of being reported.

#### POWER, and why `0x8000` is CHANNELUP

Code `0x8000` is `KEY_CHANNELUP` in every LG TV key map from 2017 to 2023, and
the MR18/MR19/MR20 key catalogue holds it exactly once. The remote's POWER
button is an infrared-first key: it carries LG NEC address `0x20DF`, command
`0x08`, and its catalogue record has no Bluetooth code at all. Measurements on
an MR20 nevertheless show POWER transmitting `0x8000` as well, which the
firmware can only explain through the remote's SRAM key handler, so the two
buttons are indistinguishable on the Bluetooth wire.

The driver therefore reports `0x8000` as `KEY_CHANNELUP`, with standby left on
the secondary power key `0x8099` (`KEY_SLEEP`). Set `power_key=1` to report
`KEY_POWER` instead if a stray channel change is the greater nuisance. A host
with an infrared receiver can always tell the two apart, because only POWER
emits NEC command `0x08`.

### Airmouse Functionality
**Needs calibration before usage**
- Gyroscope-based pointer control
- Configurable sensitivity and threshold
- Low-pass filtering for smooth movement
- Automatic mode switching between navigation and pointer control

The airmouse feature is implemented in two ways:
- **Kernel Module (Production)**: Built-in airmouse processing with minimal latency, running entirely in kernel space
- **Python + Uinput (Debug)**: Raw IMU data processing in userspace via `display_imu.py --mouse` for testing and calibration validation

### IMU Data Access
- Raw accelerometer and gyroscope data via evdev
- 6-axis motion data (3-axis accel + 3-axis gyro)
- Hardware counter for timing synchronization

## Building and Installation

### Prerequisites
- Linux kernel headers
- DKMS (Dynamic Kernel Module Support)
- Build essentials (make, gcc)

### Manual Build
```bash
make
sudo insmod lg_magic.ko
```

### DKMS Installation
```bash
sudo mkdir /usr/src/lg-magic-1.0
sudo cp * /usr/src/lg-magic-1.0/
sudo dkms add -m lg-magic -v 1.0
sudo dkms build -m lg-magic -v 1.0
sudo dkms install -m lg-magic -v 1.0
```

### Module Parameters
The driver supports several runtime parameters:

```bash
# Load with custom parameters
sudo modprobe lg_magic airmouse=1 airmouse_threshold=300 imu_evdev=1 \
              power_key=0 debug=2

# Or set via sysfs after loading
echo 1 > /sys/module/lg_magic/parameters/airmouse
echo 500 > /sys/module/lg_magic/parameters/airmouse_threshold
echo 2 > /sys/module/lg_magic/parameters/debug
```

**Parameters:**
- `airmouse` (0/1): Enable/disable airmouse functionality
- `airmouse_threshold` (int): Gyro threshold for enabling airmouse (default: 300)
- `imu_evdev` (0/1): Expose raw IMU data as separate input device
- `power_key` (0/1): Report code `0x8000` as `KEY_POWER` instead of
  `KEY_CHANNELUP` (default: 0)
- `debug` (0-2): Debug message level (0=quiet, 1=normal, 2=verbose)

## Calibration System

### Calibration File Format
The driver loads calibration data from binary files via Linux Firmware subsystem:
- `lg_magic_calib_XX_XX_XX_XX_XX_XX.bin` - MAC address-specific individual calibration
- `lg_magic_calib.bin` - Fallback calibration

The kernel module only accepts the versioned, little-endian fixed-point format
written by `scripts/convert_calib.py`. All values are signed 16.16 fixed-point
numbers stored in 32 bits; the driver never parses floating-point data.

If you have a blob created by an older version of this project (8 native-endian
IEEE-754 `float` values), convert it once:

```bash
python3 scripts/convert_calib.py legacy_float.bin lg_magic_calib.bin --from-legacy
# Add --legacy-endian big if the old blob was created on a big-endian machine.
```

The converter validates the ranges and rejects NaN/Inf values.

### Creating Calibration Files

1. **Collect IMU samples:**
```bash
python3 scripts/capture_imu.py samples.csv
```

2. **Calculate calibration values:**
```bash
# Calibrate accelerometer (slowly rotate across all axes while collecting)
python3 scripts/calibrate.py --accel samples.csv calib_accel.json

# Calibrate gyroscope (keep remote stationary while collecting)
python3 scripts/calibrate.py --gyro samples.csv calib_gyro.json

# Combine Gyro/Accel JSONs
Combine gyro/accel sections. Adjust gyro scale. Out of scope of this project, recommended value about 0.07

```

3. **Convert to binary format:**
```bash
python3 scripts/convert_calib.py calib.json lg_magic_calib.bin --alpha 0.2 --mouse_k 0.5
```

Run the converter tests with:

```bash
python3 scripts/test_convert_calib.py
```

The fixed-point math can be tested on the host as well:

```bash
make -C tests check
```

### Calibration Parameters
- `alpha`: Low-pass filter coefficient (0.0-1.0)
- `mouse_k`: Airmouse sensitivity multiplier
- `gyro_bias`: Gyroscope zero-offset values
- `gyro_scale`: Gyroscope scaling factors

## Python Tools Usage

### lg_magic.py - Packet Analyzer
```bash
# Monitor HIDRAW packets
python3 lg_magic.py
```

### capture_imu.py - Raw Capture
```bash
# Raw IMU samples for calibration (only python-evdev is required)
python3 scripts/capture_imu.py samples.csv
```

### display_imu.py - IMU Visualization
```bash
# Raw IMU data display
python3 scripts/display_imu.py

# AHRS
python3 scripts/display_imu.py --calib calib.json --ahrs

# 3D orientation cube
python3 scripts/display_imu.py --calib calib.json --cube

# Uinput airmouse
python3 scripts/display_imu.py --calib calib.json --mouse
```

### Airmouse debug
The Python tools can create a virtual mouse device using uinput:

```bash
# Enable uinput module
sudo modprobe uinput

# Run airmouse with calibration
python3 scripts/display_imu.py --calib calib.json --mouse
```

## Technical Details

### HID Protocol Structure

The remote uses report ID `0xFD`, 20 bytes in total: 1 byte report ID plus 19
bytes of payload. The payload structure is:

| Offset | Size | Description | Format |
|--------|------|-------------|---------|
| 0 | 1 | Report ID (0xFD) | uint8_t |
| 1-2 | 2 | Packet counter, two separate bytes on the TV side | little-endian uint16 |
| 3 | 1 | Bits 1:0 motion state (1 valid, 2 stopped), bits 7:2 a level field | uint8_t |
| 4 | 1 | Status, meaning not established | uint8_t |
| 5-6 | 2 | Gyro X | big-endian int16 |
| 7-8 | 2 | Gyro Y | big-endian int16 |
| 9-10 | 2 | Gyro Z | big-endian int16 |
| 11-12 | 2 | Accel X | big-endian int16 |
| 13-14 | 2 | Accel Y | big-endian int16 |
| 15-16 | 2 | Accel Z | big-endian int16 |
| 17-18 | 2 | Button code | big-endian uint16 |
| 19 | 1 | Wheel delta | int8 |

While motion is valid the remote suppresses discrete button-down frames: every
`0xFD` frame simply repeats the code of the key currently held, so the driver
edge-detects on bytes 17-18 rather than trusting byte 3.

### IMU Data Processing
- **Sampling rate**: ~50Hz (20ms intervals)
- **Data format**: Big-endian signed 16-bit values
- **Coordinate system**: Shown on image

### Airmouse Algorithm
1. Gyro data is bias-corrected and scaled
2. Low-pass filtering reduces high-frequency noise
3. Angular velocity is converted to pointer movement
4. Threshold detection switches between navigation and pointer modes. Pressing navigation buttons return to button mode.

## Filesystem Locations

- **Module**: `/lib/modules/$(uname -r)/kernel/drivers/input/misc/lg_magic.ko`
- **Calibration**: `/lib/firmware/lg_magic_calib.bin`
- **DKMS source**: `/usr/src/lg-magic-1.0/`

## Compatibility

- **Tested with**: LG Magic Remote MR20
- **Kernel versions**: 4.15+ (tested on 6.11)
- **Python**: 3.6+
- **Dependencies**: python-evdev for raw capture; numpy, scipy, pyqtgraph for display and calibration

## Contributing

Please report issues and submit pull requests for:
- Additional device support
- Improved calibration algorithms
- Bug fixes and performance improvements

## License

GPL v2 - Same as Linux kernel

Copyright © 2025 [Ilya Chelyadin]. This project is not affiliated with LG Electronics.
