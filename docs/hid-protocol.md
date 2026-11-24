# HID protocol

This is developer reference for the remote's raw reports and the vendor
command channel. Normal driver use does not require it.

## Motion and button report `0xFD`

The driver receives 20 bytes, including the report ID:

| Offset | Size | Description | Format |
|---|---|---|---|
| 0 | 1 | Report ID (`0xFD`) | uint8 |
| 1-2 | 2 | Packet counter | little-endian uint16 |
| 3-4 | 2 | Constant value (`0xFD00`) | little-endian uint16 |
| 5-6 | 2 | Gyro X | big-endian int16 |
| 7-8 | 2 | Gyro Y | big-endian int16 |
| 9-10 | 2 | Gyro Z | big-endian int16 |
| 11-12 | 2 | Accel X | big-endian int16 |
| 13-14 | 2 | Accel Y | big-endian int16 |
| 15-16 | 2 | Accel Z | big-endian int16 |
| 17-18 | 2 | Button code | big-endian uint16 |
| 19 | 1 | Wheel delta | int8 |

The remote declares no output report. It declares `0xF9` as a 30-byte input
report and a 200-byte feature report (the vendor command channel), `0xFD` as a
30-byte input report (20 bytes are sent), and `0xFE` as a 124-byte input report
for voice: report ID, a status byte, a big-endian 16-bit event code in the same
code space as the button field, then 120 bytes of audio as two 60-byte blocks.

## Vendor command channel `0xF9`

The vendor channel carries a one-byte opcode after the report ID, sent as a
two-byte `SET_REPORT` on the feature report. An output report is taken by the
transport and dropped by the remote, which declares none.

Opcodes used by this project:

- `0x01` motion active;
- `0x02` motion sleep;
- `0x03`/`0x04`/`0x05` voice start, stop and restart;
- `0x11` read firmware version;
- `0x17` read the SCD (Sensor Calibration Data).

Motion is driven by the air mouse state: the driver sends `0x01` when pointer
mode is enabled and `0x02` when it is disabled. An MR22 streams motion even
without the command.

A command issued at the end of probe is answered about half the time, so the
driver repeats the startup commands when the remote's first report shows the
link carrying traffic. SCD requests are retried a few times.

Writing to the vendor GATT bank `d0ff` is different and must be avoided:
writing `0x01` to characteristic `ffd1` reboots the remote into its DFU
bootloader.

## Voice report `0xFE`

Voice is a vendor HID stream, not ALSA or HFP. The voice subproject handles the
microphone in userspace; see [../voice/README.md](../voice/README.md).

## Not implemented

Documented remote behaviour this project deliberately leaves alone:

- the infrared transmitter and the UEI code-set commands (`0xF9` opcodes
  `0x50`–`0x74`), which drive an external set-top box;
- `0xF9 0x19` remote information and `0xF9 0x90` liquid detection;
- over-the-air firmware update over the `d0ff` GATT bank, which is one blind
  write away from stranding the remote in its bootloader;
- the Broadcom generation (MR14–MR16A), which is BR/EDR, identifies as
  `0a5c:8502` and uses the incompatible 22-byte report `0xFA`.
