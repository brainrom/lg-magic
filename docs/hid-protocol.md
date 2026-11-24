# HID protocol

This is developer reference for the remote's raw reports and the vendor
command channel. Normal driver use does not require it.

## Motion and button report `0xFD`

The driver accepts only 20-byte `0xFD` reports, including the report ID:

| Offset | Size | Description | Format |
|---|---|---|---|
| 0 | 1 | Report ID (`0xFD`) | uint8 |
| 1-2 | 2 | Packet counter | little-endian uint16 |
| 3 | 1 | Level/battery in bits 7:2; bits 1:0 are not interpreted | uint8 |
| 4 | 1 | Status; meaning not established, not used by the driver | uint8 |
| 5-6 | 2 | Gyro X | big-endian int16 |
| 7-8 | 2 | Gyro Y | big-endian int16 |
| 9-10 | 2 | Gyro Z | big-endian int16 |
| 11-12 | 2 | Accel X | big-endian int16 |
| 13-14 | 2 | Accel Y | big-endian int16 |
| 15-16 | 2 | Accel Z | big-endian int16 |
| 17-18 | 2 | Button code | big-endian uint16 |
| 19 | 1 | Wheel delta | int8 |

The driver exposes `data[3] >> 2` as the `battery_level` attribute. Byte 4 is
not used.

The button field at bytes 17-18 is a big-endian key code. The driver compares
it with the table below and reports the mapped Linux key; a code that is not
in the table produces no key event. A change of the button code is treated as
a release of the previous key followed by a press of the new one.

The wheel delta at byte 19 is signed. In button mode it is reported as
`KEY_UP` or `KEY_DOWN`; in pointer mode it is reported as `REL_WHEEL`. The
wheel click is code `0x8044`.

The button field also carries stream markers such as `0x800A`/`0x800D`
(voice start/stop) and `0x803E` (motion status). They are not keys and are not
in the key table, so no key event is reported for them.

The remote declares no output report. It declares `0xF9` as a 30-byte input
report and a 200-byte feature report (the vendor command channel), `0xFD` as a
30-byte input report (20 bytes are sent), and `0xFE` as a 124-byte input report
for voice: report ID, a status byte, a big-endian 16-bit event code in the same
code space as the button field, then 120 bytes of audio as two 60-byte blocks.

## Button map

| Code | Linux key | Meaning |
|---|---|---|
| `0x8000` | `KEY_CHANNELUP` | Channel up; see POWER below |
| `0x8001` | `KEY_CHANNELDOWN` | Channel down |
| `0x8002` | `KEY_VOLUMEUP` | Volume up |
| `0x8003` | `KEY_VOLUMEDOWN` | Volume down |
| `0x8006` | `KEY_RIGHT` | Right |
| `0x8007` | `KEY_LEFT` | Left |
| `0x8009` | `KEY_MUTE` | Mute |
| `0x800B` | `KEY_SWITCHVIDEOMODE` | Input/source |
| `0x8010`-`0x8019` | `KEY_0`-`KEY_9` | Digits |
| `0x8024` | `KEY_PROG4` | YouTube |
| `0x8028` | `KEY_BACK` | Back |
| `0x8029` | `KEY_FAVORITES` | Heart/favourites |
| `0x8031` | `KEY_PROG3` | Disney+ |
| `0x8040` | `KEY_UP` | Up |
| `0x8041` | `KEY_DOWN` | Down |
| `0x8043` | `KEY_SETUP` | Settings |
| `0x8044` | `KEY_ENTER` / `BTN_LEFT` | Wheel click; `BTN_LEFT` in pointer mode |
| `0x8045` | `KEY_MENU` | Menu (`...`) |
| `0x8048` | `KEY_CHANNEL` | LG Channels |
| `0x804C` | `KEY_LIST` | List on MR19 |
| `0x8053` | `KEY_LIST` | List on MR20 |
| `0x8056` | `KEY_PROG1` | Netflix; shares the physical button with IVI |
| `0x805C` | `KEY_PROG2` | Amazon Prime |
| `0x805D` | `KEY_PROG1` | IVI; shares the physical button with Netflix |
| `0x8061` | `KEY_BLUE` | Blue |
| `0x8063` | `KEY_YELLOW` | Yellow |
| `0x8071` | `KEY_GREEN` | Green |
| `0x8072` | `KEY_RED` | Red |
| `0x807B` | `KEY_NEWS` | News |
| `0x807C` | `KEY_HOME` | Home |
| `0x8081` | `KEY_VIDEO` | Movies |
| `0x808B` | `KEY_VOICECOMMAND` | Microphone |
| `0x8098` | `KEY_CONTEXT_MENU` | STB menu |
| `0x8099` | `KEY_SLEEP` | Standby/second power |
| `0x80A1` | `KEY_SWITCHVIDEOMODE` | Input/source |
| `0x80AB` | `KEY_PROGRAM` | Guide |
| `0x80B0` | `KEY_PLAY` | Play |
| `0x80BA` | `KEY_PAUSE` | Pause |

The two input/source codes and the two List codes are model-specific names for
the same physical buttons. The streaming shortcuts are mapped by physical
position rather than by service name, which is why IVI and Netflix share
`KEY_PROG1`.

## POWER and `0x8000`

The POWER button is infrared-first: it has no Bluetooth key code of its own.
Some remotes nevertheless send `0x8000`, which is the channel-up code, so
POWER and channel-up cannot be distinguished on the Bluetooth wire. The driver
maps `0x8000` to `KEY_CHANNELUP` and does not expose `KEY_POWER`. The
secondary power/standby button reports `0x8099` and is mapped to `KEY_SLEEP`.

A host with an infrared receiver can distinguish the two: only POWER emits the
LG NEC address `0x20DF`, command `0x08`.

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
