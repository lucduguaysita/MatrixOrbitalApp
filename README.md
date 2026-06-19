# lcd-ipconfig

Interactive IP address configuration UI for the Matrix Orbital BGK19264A-7T
(GLK19264A-7T-1U-USB) 192x64 graphic LCD, targeting systemd-networkd on Linux.

## Hardware

- Display: BGK19264A-7T via mini-USB (enumerates as /dev/ttyACM0)
- Keypad: 7-key tactile pad (Up/Down/Left/Right/Enter/Back/Cancel)

Key mapping assumes Matrix Orbital factory default keycodes:

| Key    | Code |
|--------|------|
| Up     | 0x41 |
| Down   | 0x42 |
| Right  | 0x43 |
| Left   | 0x44 |
| Enter  | 0x45 |
| Back   | 0x46 |
| Cancel | 0x47 |

If your keycodes differ, check them with MOGD# (Matrix Orbital utility) and
update the KEY_* defines at the top of src/lcd_ipconfig.c.

## Configuration

Before building, review the top of src/lcd_ipconfig.c:

```c
#define SERIAL_PORT     "/dev/ttyACM0"
#define NETWORK_FILE    "/etc/systemd/network/10-static.network"
#define INTERFACE_NAME  "eth0"
```

Change INTERFACE_NAME to match your NIC (ip link to list).
Change SERIAL_PORT if the device enumerates differently (check dmesg after
plugging in the display).

## Build

```
make
```

Requires: gcc, glibc. No external dependencies.

## Install

```
sudo make install
```

This copies the binary to /usr/local/bin, installs the systemd unit, enables
it, and starts it on next boot. To start immediately:

```
sudo systemctl start lcd-ipconfig
```

## Usage

1. Idle screen shows on boot with the configured interface name.
2. Press ENTER to begin editing.
3. Three fields are edited in sequence: IP Address, Netmask, Gateway.
   - UP/DOWN:  change current octet by +/- 1
   - LEFT/RIGHT: change by +/- 10
   - ENTER: confirm current octet, move to next
   - BACK: go back to previous octet (or previous field)
   - CANCEL: abort and return to idle screen
4. After all three fields, a confirmation screen shows the full config.
   - ENTER: write /etc/systemd/network/10-static.network and restart
             systemd-networkd
   - CANCEL: abort, return to idle

## Troubleshooting

**Display not found:**
  dmesg | grep tty   -- look for ttyACM0 or ttyUSB0

**Keys not responding:**
  Use MOGD# on a Windows machine to verify the key codes sent by your unit.
  Matrix Orbital factory defaults are listed above; some units ship with
  different mappings.

**Network not applying:**
  journalctl -u lcd-ipconfig   -- app log
  journalctl -u systemd-networkd  -- networkd log
  The binary must run as root (enforced by the service unit).

**Contrast/backlight:**
  Adjust lcd_contrast(128) and lcd_backlight_on() calls in main() if
  the display is hard to read.

## Uninstall

```
sudo make uninstall
```
