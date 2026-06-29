# lcd-ipconfig

[![Build](https://github.com/lucduguaysita/MatrixOrbitalApp/actions/workflows/build.yml/badge.svg)](https://github.com/lucduguaysita/MatrixOrbitalApp/actions/workflows/build.yml)

Interactive IP-address configuration UI for the Matrix Orbital BGK19264A-7T
(GLK19264A-7T-1U-USB) 192×64 graphic LCD, targeting **NetworkManager** on Linux
(developed/tested on AlmaLinux).

It runs as a **resident, hot-pluggable poller**: it waits for the display, and
each time the panel is connected it lights the LEDs, lets an operator pick an
ethernet interface and edit its IPv4 settings on the 7-key pad, then applies the
change via `nmcli`. Unplugging the display returns it to waiting.

## Features

- Auto-detects the USB serial device (no hardcoded port) and survives
  connect/disconnect on the fly.
- Discovers ethernet interfaces at runtime; if more than one, you pick on the
  keypad. The current IP is shown next to each interface.
- Preloads the interface's **live** IP / netmask / gateway so you edit from the
  current values.
- Writes a NetworkManager keyfile and reloads it; optional **apply + reboot**.
- LED status: top/bottom solid green, **centre LED heartbeat** (green↔amber,
  ~1 s) to show an active connection; all red on quit.
- Unlock key sequence (casual tamper deterrent), skippable.
- Logs to **syslog** and tees to the console.
- Built-in keycode and LED/GPO testers for bring-up on new hardware.

## Hardware

- Display: BGK19264A-7T via USB. The FTDI-bridge variant enumerates as
  `/dev/ttyUSB0`; native-CDC units appear as `/dev/ttyACM0`. The program
  auto-detects, preferring the stable `/dev/serial/by-id/...` symlink.
- Baud rate **matters** even over USB — this unit is configured for **19200**
  (the MO factory default). A mismatch shows garbage characters.
- Keypad: 7-key tactile pad.

### Keypad codes (this unit)

These were captured from the actual hardware with the built-in tester
(`./lcd_ipconfig keys`) and **differ from the generic MO defaults**:

| Key    | Code |
|--------|------|
| Up     | 0x42 |
| Down   | 0x48 |
| Right  | 0x43 |
| Left   | 0x44 |
| Enter  | 0x45 |
| Back   | 0x41 |
| Cancel | 0x47 |

If keys misbehave on a different unit, run `./lcd_ipconfig keys`, press each
key, note the codes, and update the `KEY_*` defines near the top of
`lcd_ipconfig.c`.

## Prerequisites (AlmaLinux)

Everything below ships with a default AlmaLinux install, but check before you
build/run.

**Check what's present:**

```bash
# build tools
rpm -q gcc make

# NetworkManager + nmcli (the app applies config via nmcli)
rpm -q NetworkManager && command -v nmcli

# USB-serial kernel driver (auto-loads when the display is plugged in)
lsmod | grep -E 'ftdi_sio|cdc_acm'
```

**Install whatever is missing:**

```bash
sudo dnf install -y gcc make NetworkManager
```

`nmcli` is provided by the `NetworkManager` package. Make sure the service is
running (it manages the connection the app edits):

```bash
systemctl is-active NetworkManager || sudo systemctl enable --now NetworkManager
```

**USB-serial kernel modules.** The driver loads automatically on plug-in
(`ftdi_sio` for `/dev/ttyUSB*`, `cdc_acm` for `/dev/ttyACM*`). If `lsmod` shows
nothing after connecting the display:

```bash
sudo modprobe ftdi_sio      # FTDI bridge  -> /dev/ttyUSB*
sudo modprobe cdc_acm       # native CDC   -> /dev/ttyACM*
dmesg | tail                # confirm the device attached and which node it got
```

These modules are part of the stock kernel; `modinfo ftdi_sio` confirms it's
available. To load `ftdi_sio` automatically at boot (rarely needed):

```bash
echo ftdi_sio | sudo tee /etc/modules-load.d/ftdi_sio.conf
```

## Build

```
make
```

Requires `gcc` and glibc, no external dependencies. (The source defines
`_DEFAULT_SOURCE` so the Linux socket-ioctl types resolve under the strict
`-std=c11 -D_POSIX_C_SOURCE=200809L` flags.)

## Run

```
sudo ./lcd_ipconfig [port|auto] [baud] [keys] [leds] [nolock]
```

Arguments are matched by content, so order does not matter:

- **port** — serial device (e.g. `/dev/ttyUSB1`), or `auto` (default) to
  continuously auto-detect the display as it is plugged in.
- **baud** — `9600 | 19200 | 38400 | 57600 | 115200` (default **19200**).
- **keys** — run the keycode tester instead of the config UI.
- **leds** — run the LED/GPO tester instead of the config UI.
- **nolock** — skip the unlock key sequence (see below).

Run as **root**: it writes under `/etc/NetworkManager/` and runs `nmcli`.

## Unlock sequence

On every connect the panel shows a **Locked** screen and waits for the unlock
key sequence:

> **UP, UP, DOWN, ENTER**

Entering it correctly opens the configuration UI. **Any wrong key** releases the
display (LEDs red, "Reconnect USB to restart") and ends the session — you must
unplug/replug to try again, which naturally rate-limits guessing.

This is a casual tamper deterrent, **not real security** (the sequence is short
and shoulder-surfable; physical access to the host bypasses it entirely).
Disable it with the `nolock` argument, or change the sequence by editing
`g_passcode[]` in `lcd_ipconfig.c`. To run unlocked as a service, add `nolock`
to the `ExecStart` line in the systemd unit.

## Usage

1. **Locked screen** — enter UP, UP, DOWN, ENTER to unlock.
2. **Interface selection** (shown only if more than one ethernet interface) —
   UP/DOWN to highlight, ENTER to select, BACK to quit. The current IP is shown
   next to each name.
3. **Edit IP / Netmask / Gateway** in sequence, starting from the live values:
   - UP/DOWN: change current octet by ±1
   - LEFT/RIGHT: change by ±10
   - ENTER: confirm octet / advance field
   - BACK: previous octet/field
   - CANCEL: back to interface selection
4. **Confirm screen**:
   - ENTER: apply (write keyfile, `nmcli connection reload` + `up`)
   - RIGHT: apply **and reboot** (LEDs go red, then `systemctl reboot`)
   - CANCEL: abort, back to interface selection
5. **Quit (BACK on the first screen)** releases the display and shows a
   "Reconnect USB to restart" prompt; the program stays resident.

### How the network change is applied

The chosen interface name is used as the connection profile id. The program
writes `/etc/NetworkManager/system-connections/<iface>.nmconnection`
(`method=manual`, the entered address/prefix/gateway, `dns=8.8.8.8`), `chmod`s
it to `600`, then runs `nmcli connection reload` and `nmcli connection up
<iface>`. This assumes the NM profile id matches the interface name; verify with
`nmcli connection show`.

## Diagnostics

- **`./lcd_ipconfig keys`** — shows the hex code of each key pressed (LCD +
  syslog), for capturing a new unit's keypad mapping.
- **`./lcd_ipconfig leds`** — cycles "all GPOs off" then each GPO 1–6 lit alone,
  for mapping the keypad LEDs. Note: on this module an LED element is lit while
  its GPO is **off**, so all-GPOs-off shows **amber**; a true off needs both of
  an LED's GPOs on.

## Logging

Messages go to syslog (tag `lcd_ipconfig`, facility `daemon`) and are teed to
the console:

```
journalctl -t lcd_ipconfig -f          # systemd journal
grep lcd_ipconfig /var/log/messages    # rsyslog text log
```

## Install (systemd service)

```
sudo make install
```

Installs the binary to `/usr/local/bin`, installs and enables the
`lcd-ipconfig.service` unit (runs as root, `Restart=on-failure`).

### Manual install

Equivalent steps if you prefer to do it by hand:

```
# binary
sudo install -m 755 lcd_ipconfig /usr/local/bin/lcd_ipconfig

# systemd unit -> copy lcd-ipconfig.service into /etc/systemd/system/
sudo install -m 644 lcd-ipconfig.service /etc/systemd/system/lcd-ipconfig.service

# enable + start
sudo systemctl daemon-reload
sudo systemctl enable --now lcd-ipconfig.service
```

The unit file belongs in **`/etc/systemd/system/lcd-ipconfig.service`**. After
editing it (e.g. to add `nolock` to `ExecStart`), run `sudo systemctl
daemon-reload && sudo systemctl restart lcd-ipconfig`. Start immediately:

```
sudo systemctl start lcd-ipconfig
```

### Check the service is loaded and active

```bash
# one-word answers
systemctl is-enabled lcd-ipconfig    # enabled  -> starts at boot
systemctl is-active  lcd-ipconfig    # active   -> running now

# full view: "Loaded:" line shows the unit path + enabled state,
# "Active:" line shows running/since, followed by recent log lines
systemctl status lcd-ipconfig

# confirm systemd actually sees the unit file (loaded into its catalog)
systemctl list-unit-files | grep lcd-ipconfig

# service log
journalctl -u lcd-ipconfig -n 20 --no-pager
```

In `systemctl status`, a healthy service reads roughly:

```
Loaded: loaded (/etc/systemd/system/lcd-ipconfig.service; enabled; preset: disabled)
Active: active (running) since ...
```

If `status` says **`Unit lcd-ipconfig.service could not be found`**, the unit
file isn't installed (or you skipped `daemon-reload`) — recopy it to
`/etc/systemd/system/` and run `sudo systemctl daemon-reload`. If it shows
`Loaded: ... disabled`, run `sudo systemctl enable lcd-ipconfig` so it starts at
boot.

## Troubleshooting

**Display not found**
```
ls -l /dev/serial/by-id/ /dev/ttyUSB* /dev/ttyACM*
```
Confirm the device enumerates and you have access (run as root / `dialout`).

**Garbage characters on the display** — baud mismatch. This unit expects 19200;
try `sudo ./lcd_ipconfig /dev/ttyUSBx 19200` (or 57600/115200 on other units).

**Keys not responding / wrong direction** — capture real codes with
`./lcd_ipconfig keys` and update the `KEY_*` defines.

**Network not applying**
```
journalctl -t lcd_ipconfig          # app log
nmcli connection show               # confirm the profile id == interface name
```
Must run as root.

## Uninstall

```
sudo make uninstall
```
