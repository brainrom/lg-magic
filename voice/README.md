# LG Magic Remote voice service

This is an optional subproject for the LG Magic Remote microphone. It is
independent of the kernel driver's input and IMU code and only needs the
vendor voice reports that the driver passes through.

The microphone is a vendor HID stream, not a standard ALSA or HFP device. The
kernel module only reports the microphone button as `KEY_VOICECOMMAND` and
accepts the `0xFE` voice reports; it does not decode audio.

## Architecture

```text
microphone button
       |
       v
lgmagicd (hidraw, arm, mSBC decode)
       |
       | D-Bus Acquire() -> read-only PCM pipe
       v
libpipewire-module-lgmagic.so
       |
       | pw_stream, S16LE 16 kHz mono
       v
WirePlumber client context -> PipeWire -> applications
```

- `lgmagicd` is the only process that opens the remote's hidraw node and the
  only process that can arm the microphone.
- The unprivileged WirePlumber module receives a read-only PCM pipe over
  D-Bus and publishes it as `lg-magic-voice`. It never sees hidraw.
- An optional FIFO at `/run/lgmagic/voice.pcm` can expose the same PCM through
  `libpipewire-module-pipe-tunnel` as `lg-magic-voice-fifo`.
- There is no separate producer service.

## Files

- **lgmagicd.c** - privileged microphone daemon
- **lg-magic-voice-module.c** - PipeWire source module loaded by WirePlumber
- **60-lg-magic-voice.conf** - WirePlumber drop-in for the module
- **lgmagicd.service.in** - systemd system unit template
- **lgmagicd-fifo.conf.in** - optional systemd drop-in that re-enables the FIFO
- **50-lg-magic-voice.conf** - optional PipeWire FIFO drop-in
- **lgmagic.conf** - sysusers definition for the `lgmagic` user and group
- **org.lgmagic.conf** - D-Bus system bus policy
- **52-lgmagic-hidraw.rules** - udev rule for the remote's hidraw node
- **Makefile** - build and install the daemon, module and optional FIFO

## Build and install

```bash
make -C voice
sudo make -C voice install
sudo systemd-sysusers
sudo udevadm control --reload && sudo udevadm trigger
sudo systemctl daemon-reload
sudo systemctl reload dbus
sudo systemctl enable --now lgmagicd
systemctl --user restart wireplumber
```

`make install` places the module in the PipeWire module directory reported by
`pkg-config --variable=moduledir libpipewire-0.3` (normally
`/usr/lib/pipewire-0.3`) and installs the WirePlumber drop-in in
`/etc/wireplumber/wireplumber.conf.d/`.

`systemctl reload dbus` is usually an alias for the D-Bus broker. On systems
running `dbus-broker`, the unit may be named `dbus-broker.service`; restarting
that unit (or rebooting) also picks up the new policy.

The source is deliberately low priority (`priority.session = 0`) so it does
not become the default microphone. Select it explicitly, or make it default:

```bash
wpctl status
pw-record --target lg-magic-voice --rate 16000 --channels 1 --format s16 out.wav
# optional: wpctl set-default <id-of-lg-magic-voice>
```

The module needs WirePlumber 0.5 or newer with `support.client-context`.

## D-Bus control plane

```text
Bus name:  org.lgmagic
Object:    /org/lgmagic/Voice1
Interface: org.lgmagic.Voice1

Methods:
  Acquire() -> h        read-only PCM pipe, S16LE 16 kHz mono
  Release()

Properties:
  IsArmed (b)           microphone is armed
  IsStreaming (b)       decoded PCM is flowing

Signals:
  Armed()
  Disarmed(s reason)    silence | timeout | no audio |
                        disconnect | shutdown
```

A physical microphone-button press arms the microphone: `lgmagicd` sends
`VOICE_START`, sets `IsArmed`, and emits `Armed`. The phrase ends on the
silence timeout, the maximum duration, loss of audio, device disconnect or
daemon shutdown; `Disarmed` reports the corresponding reason.

`IsStreaming` is independent of the arm state: it becomes true when decoded
PCM actually starts flowing and false when the phrase is disarmed.

`Acquire()` returns a **blocking** read-only pipe; the consumer may switch its
own end to non-blocking. The transport is **exclusive**: the first caller
becomes the owner, a second `Acquire()` gets `AccessDenied`, and `Release()` is
accepted only from the owner. The pipe is valid while the owner's D-Bus
connection is alive: `NameOwnerChanged` closes it when the unique name
disappears.

`org.lgmagic.conf` is installed into `/etc/dbus-1/system.d/`; only
`root`/`lgmagic` can own `org.lgmagic`, and session processes may call it. The
caller's **effective UID (EUID)** must belong to an active logind user
(`GetUser(euid) -> State=active`), i.e. it should look like a session client
that may legitimately need the microphone. It is not a check for a specific
consumer unit or executable: PipeWire and WirePlumber run as user services and
are not attached to a logind session by PID, so a per-PID session check would
reject the WirePlumber module. No group, ACL or device permissions are
involved on this path, and the session never sees hidraw.

`State=active` includes remote/SSH sessions and users active on another seat,
so this is a coarse gate, not a seat-aware authorization check.

The exclusive transport is the security-relevant part, matching BlueZ's
`MediaTransport1` owner: a local process that reaches the service first can
take the PCM stream, but it then owns it exclusively and the legitimate
WirePlumber module cannot; the user notices that PipeWire has no microphone.
A service impersonation/MITM is not possible because only `root`/`lgmagic` can
own the D-Bus name. A same-UID malicious process can still use the session's
PipeWire (or modify its config) if it has that access; pinning a consumer by
systemd unit or executable would add fragility without changing that boundary,
so it is intentionally not part of the model.

## WirePlumber module

The module is a normal PipeWire module (`libpipewire-module-lgmagic.so`), but
WirePlumber loads it as a `pw-module-client` component. That puts it in the
client context on its own thread, next to the other media objects, with no
separate user service. It acquires the transport when it loads, retries if
`lgmagicd` is restarted or the user session is not active yet, and watches
`NameOwnerChanged` for daemon restarts. On a clean unload it calls `Release()`;
if the process disappears, the daemon releases the transport through
`NameOwnerChanged`.

If the PipeWire stream is destroyed or its PipeWire connection is lost, the
module stays loaded: it drops the dead stream, reconnects the core if needed,
and recreates the `lg-magic-voice` source. The D-Bus PCM transport stays
attached, so `lgmagicd` does not need to be restarted.

Applications do not call `Acquire()` themselves. The module publishes the
PipeWire source; applications use PipeWire, and PipeWire/WirePlumber/portal
policy decides which applications may record it.

## Tuning

`lgmagicd` has no configuration file. Pass options in a systemd drop-in
(`systemctl edit lgmagicd`) or on the command line:

- `-s, --silence N` - end the phrase after N ms without voice (default 1500);
- `-M, --max N` - maximum capture duration in ms (default 30000); it must not
  be smaller than the silence timeout;
- `-V, --vad N` - RMS threshold for voice activity (default 500, about
  -36 dBFS for 16-bit samples). `0` treats every block as voice, so only the
  maximum duration ends the phrase;
- `-d, --device /dev/hidrawN` - use only this node instead of any hidraw node
  whose HID parent is driven by `lgmagic`;
- `-p, --pipe PATH` / `--no-fifo` - use or disable the optional PCM FIFO;
- `--lock PATH` - lock file (default `/run/lgmagic/lgmagicd.lock`);
- `-v, --verbose` - state transitions and per-code diagnostics.

For example:

```ini
[Service]
ExecStart=
ExecStart=/usr/local/bin/lgmagicd --no-fifo -s 2000 -M 20000
```

## Optional FIFO fallback

`install-fifo` installs both the PipeWire drop-in and a systemd drop-in that
re-enables `-p /run/lgmagic/voice.pcm`:

```bash
sudo make -C voice install-fifo
sudo usermod -aG lgmagic "$USER"
sudo systemctl daemon-reload
sudo systemctl restart lgmagicd
```

Log out and back in after adding the group, then restart PipeWire:

```bash
pw-record --target lg-magic-voice-fifo --rate 16000 --channels 1 --format s16 out.wav
```

`module-pipe-tunnel` is marked `nofail`: if the session user cannot open the
FIFO, PipeWire keeps running without the FIFO source and logs the module
failure. Check the PipeWire journal if `lg-magic-voice-fifo` does not appear.

The FIFO path is a deliberate compromise for simple installations. It is
**not** read-only: `module-pipe-tunnel` opens the FIFO `O_RDWR`, so members of
the `lgmagic` group can both read and write it. Treat the group as a trust
boundary; use the D-Bus path if that matters. The core unit already runs with
`--no-fifo`; `install-fifo` adds
`/usr/local/lib/systemd/system/lgmagicd.service.d/50-lg-magic-fifo.conf` to
re-enable it. `lgmagicd --no-fifo` can also be used for manual runs.

## Arm events

A service built specifically for this microphone can follow the arm state
without becoming the PCM consumer:

```bash
gdbus monitor --system --dest org.lgmagic --object-path /org/lgmagic/Voice1
```

- `Armed()` is emitted after a successful `VOICE_START`;
- `Disarmed(s reason)` is emitted when the phrase ends;
- `IsArmed` is true for the whole armed interval;
- `IsStreaming` becomes true when decoded PCM actually starts flowing.

These signals are advisory and do not grant access to the PCM transport. A
service that starts while a phrase is already in progress can read `IsArmed`
and `IsStreaming` to recover the current state. The signals are not a
substitute for the audio stream; they only report when the physical remote is
capturing.

## Testing

```bash
wpctl status
pw-record --target lg-magic-voice --rate 16000 --channels 1 --format s16 out.wav
```

Press the remote's voice button while `pw-record` is running. Before the
button is pressed the source is silent, which is expected: the daemon only
starts the microphone on a physical button press.

To watch the arm events:

```bash
gdbus monitor --system --dest org.lgmagic --object-path /org/lgmagic/Voice1
```

For intermittent arming problems, run the daemon manually with `-v` and press
the microphone button several times:

```bash
sudo systemctl stop lgmagicd
sudo -u lgmagic /usr/local/bin/lgmagicd --no-fifo \
    --lock /tmp/lgmagicd-debug.lock -v
```

Verbose lines are prefixed with a monotonic millisecond timestamp. The
relevant lines are:

- `microphone button press` - the press report reached the daemon;
- `armed` - the microphone is armed;
- `command 03` / `command 04` / `command 05` - the daemon sent
  `VOICE_START` / `VOICE_STOP` / `VOICE_RESTART`;
- `motion button XXXX` - motion-report button code changed;
- `disarmed: <reason>` - the phrase ended;
- `read: Input/output error` - the remote/dongle dropped the HID link; the
  daemon will reopen it.

If a failed press has no `microphone button press` line, the press report was
lost before the daemon saw it (for example, the remote was not connected yet).
If it has the line but no `armed`, the `VOICE_START` command failed.

Observed motion-report codes include:

- `808b` - MIC press;
- `800a` - follows `command 03` by a few milliseconds (`VOICE_START`
  acknowledgement);
- `800d` - follows `command 04` by a few milliseconds (`VOICE_STOP`
  acknowledgement);
- `803e` - unknown status code, appears after the stop acknowledgement;
- `0000` - automatic release/neutral report, also arrives when the button is
  held. It is not an end-of-phrase event and is not used for disarming.

These are motion-report codes, not voice-report header codes; the `0xFE`
voice-report code word has been observed as zero. The microphone is not
push-to-talk: the remote sends its release automatically, and `silence` is
the normal end of a phrase. The daemon therefore disarms on silence/timeout,
not on a release report.

After the test:

```bash
sudo systemctl start lgmagicd
```

## systemd hardening

The daemon runs as `lgmagic` with an empty capability set, a
`@system-service` syscall filter, a device policy limited to `char-hidraw`, a
read-only system filesystem and a private `/tmp`. The service only needs to
write `/run/lgmagic` and the hidraw node.

## Filesystem locations

- **Daemon**: `/usr/local/bin/lgmagicd`
- **Unit**: `/usr/local/lib/systemd/system/lgmagicd.service`
- **sysusers entry**: `/usr/lib/sysusers.d/lgmagic.conf`
- **D-Bus policy**: `/etc/dbus-1/system.d/org.lgmagic.conf`
- **Hidraw udev rule**: `/etc/udev/rules.d/52-lgmagic-hidraw.rules`
- **PipeWire module**: `/usr/lib/pipewire-0.3/libpipewire-module-lgmagic.so`
  (the actual directory comes from
  `pkg-config --variable=moduledir libpipewire-0.3`)
- **WirePlumber drop-in**: `/etc/wireplumber/wireplumber.conf.d/60-lg-magic-voice.conf`
- **PCM FIFO** (optional): `/run/lgmagic/voice.pcm`
- **FIFO systemd drop-in** (optional): `/usr/local/lib/systemd/system/lgmagicd.service.d/50-lg-magic-fifo.conf`
- **FIFO PipeWire drop-in** (optional): `/etc/pipewire/pipewire.conf.d/50-lg-magic-voice.conf`

## Dependencies

- libsbc, libudev, libsystemd (sd-bus), libpipewire-0.3
- WirePlumber 0.5+ with `support.client-context`
- PipeWire with `libpipewire-module-pipe-tunnel` for the optional FIFO
- Build tools: a C compiler, `make` and `pkg-config`. Development package
  names vary by distribution: `libsbc-dev`, `libudev-dev`, `libsystemd-dev`
  and `libpipewire-0.3-dev` on Debian/Ubuntu; `sbc`, `systemd-libs` and
  `libpipewire` on Arch.
