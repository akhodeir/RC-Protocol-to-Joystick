# rc-joystick

Turn a Raspberry Pi Pico (RP2040) into a USB HID joystick that reads an RC
receiver's serial output (iBUS today, SBUS coming) and presents itself to a
PC/Mac as a standard game controller — perfect for drone / plane simulators
like **Liftoff**, **Velocidrone**, **DRL Sim**, or **FPV FreeRider**.

The firmware is protocol-based, not transmitter-specific. Any receiver that
speaks **iBUS** or **SBUS** works:

- FlySky FS-iA6B, FS-iA10B, FS-iA8B, X6B, X8B, etc.
- Radiolink R7FG, R9DS, R12DS
- FrSky X4R, X8R, R-XSR, X6R (via SBUS — coming in a follow-up phase)
- Any generic receiver with an iBUS or SBUS port

**Current scope:** iBUS only. SBUS support is planned as a follow-up.

## Features

- 8 axes, 16-bit signed each (full ±32767 range → smooth throttle/pitch curves)
- 32 buttons (aux switches CH9–CH14 mapped to buttons 0–5 by default)
- Failsafe: axes center, throttle to minimum, buttons cleared if no valid frame
  is received for 500 ms
- On-board LED (GP25) shows status via blink cadence
- USB HID compliant — no drivers needed on Windows, macOS, or Linux

## Hardware

**Board:** YD-RP2040 (USB-C, on-board LED on GP25). A standard Raspberry Pi
Pico works too.

**Receiver:** any FlySky / Radiolink / FrSky / OrangeRx unit with an iBUS output.
Tested with FS-iA6B and FS-iA10B paired with an FS-i6X transmitter.

## Wiring

```
Receiver (iBUS port)                  YD-RP2040 (or Pi Pico)
─────────────────────────────────────────────────────────────
iBUS signal wire ─────────────────→   GP1  (UART0 RX, pin 2)
+5 V (VCC)       ─────────────────→   VBUS (pin 40)
GND              ─────────────────→   GND  (pin 3 or 38)

USB-C  ───────────────────────────→   Host PC / Mac
```

The iBUS data line is 3.3 V-level — no level shifter needed. Do NOT connect the
receiver's 5 V rail to the Pico 3V3 pin; it will fail to boot or brown out.

## Status LED (GP25)

| Pattern | Meaning |
|---|---|
| Fast even blink (100/100 ms) | Booting / USB not yet enumerated |
| Slow even blink (500/500 ms) | USB up, no bytes seen on the iBUS wire yet — receiver probably not connected or unpowered |
| **3 rapid pulses + long pause** | Bytes arriving on the wire but no valid iBUS frames — check wiring, protocol, and receiver output mode (iBUS vs SBUS vs PPM) |
| **4 rapid pulses + long pause** | Signal lost — either the receiver stopped sending frames, or channel values froze for > 1 s (transmitter turned off with receiver in "hold" mode) |
| Mostly on with brief 100 ms dip every 2 s | Healthy: valid iBUS frames arriving with real (moving) channel values |

### Note on iBUS failsafe

Unlike SBUS (which has an explicit "signal lost" flag in every frame), iBUS
has no way to signal that the transmitter dropped out. Many receivers
(including the FS-iA6B) keep sending iBUS frames with **held** channel
values when they lose the transmitter. To detect this, the firmware watches
for all 14 channels being bit-identical for > 1 s — real stick input always
jitters a little, so perfectly frozen values across the board mean the TX
is off. This detection can be tuned via `CHANNEL_FREEZE_MS` in `src/main.c`.

## Build

Prerequisites (macOS via Homebrew):

```bash
brew install cmake arm-none-eabi-gcc picotool
```

Environment (in `~/.zshrc`):

```bash
export PICO_SDK_PATH="$HOME/pico-sdk"     # or wherever you cloned it
```

Configure and build:

```bash
git clone https://github.com/<you>/rc-joystick.git
cd rc-joystick
cp "$PICO_SDK_PATH/external/pico_sdk_import.cmake" .

mkdir build && cd build
cmake .. -DPROTOCOL=ibus
make -j$(sysctl -n hw.ncpu)
```

Output: `build/rc_joystick.uf2` (~43 KB).

## Flash

**BOOTSEL method** (works on any RP2040 board):
1. Hold the BOOT button on the Pico
2. Plug in the USB-C cable
3. Release BOOT — the Pico mounts as an `RPI-RP2` drive
4. Drag `build/rc_joystick.uf2` onto the drive; it will reboot into the new firmware

**picotool method** (Pico already running our firmware):
```bash
picotool load build/rc_joystick.uf2 -f && picotool reboot
```

## Verify

1. Plug in the flashed Pico. The on-board LED (GP25) should blink rapidly
   (100 ms) while USB enumerates, then slow to 500 ms once macOS mounts the
   HID device — that's the "USB up, no iBUS wire activity yet" state.

2. Open **https://gamepad-tester.com/** in Chrome, Firefox, or Safari, then
   move any stick / press any switch to wake the page's Gamepad API listener.
   All 8 axes and up to 6 buttons should respond.

3. For descriptor-level debugging (Chrome / Edge only), open
   **https://nondebug.github.io/webhid-explorer/** and select the RC-Joystick
   device — you'll see the parsed HID descriptor tree and raw report bytes.

4. For a sim-styled visualiser tailored to this project, open
   `tools/webhid/index.html` in Chrome or Edge.

5. Bind the receiver and transmitter (procedure varies by receiver — most
   FlySky receivers: hold the button while powering it, then start bind mode
   on the transmitter). Once bound, the on-board LED enters the healthy
   "mostly on with a brief dip every 2 s" pattern. If iBUS frames stop for
   more than 500 ms the LED switches to the 2-pulse failsafe pattern and the
   report enters failsafe.

## Channel to axis mapping

| RC channel | HID axis   | Typical use     |
|-----------|-------------|-----------------|
| CH1       | X           | Roll / Aileron  |
| CH2       | Y           | Pitch / Elevator|
| CH3       | Z           | Throttle        |
| CH4       | Rx          | Yaw / Rudder    |
| CH5       | Ry          | Aux             |
| CH6       | Rz          | Aux             |
| CH7       | Slider      | Aux             |
| CH8       | Dial        | Aux             |
| CH9–CH14  | Buttons 0–5 | Switches (threshold at raw 1500) |

## Project layout

```
rc-joystick/
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── tusb_config.h
├── README.md
├── src/
│   ├── main.c                  Main loop, scaling, failsafe, status LED
│   ├── usb_descriptors.c/.h    HID descriptor + TinyUSB callbacks
│   └── ibus.c/.h               iBUS UART frame parser
└── tools/
    └── webhid/
        └── index.html          Custom WebHID tester (Chrome / Edge)
```

## References & prior art

Two open-source projects were studied while building this one, plus one
web tool that proved essential during descriptor debugging. Neither project
is a dependency — they're worth a look if you're extending this project or
want to see how others have solved adjacent problems:

- **[nondebug/webhid-explorer](https://nondebug.github.io/webhid-explorer/)** — 
  Live at the linked URL (source: `github.com/nondebug/webhid-explorer`).
  Browser-based WebHID debugger by François Beaufort. Parses the HID report
  descriptor tree, dumps raw input reports as hex, decodes fields per-report.
  **Invaluable for verifying a custom HID descriptor is well-formed** — was
  the single most useful external tool while bringing up this firmware.
- **[wiredopposite/OGX-Mini](https://github.com/wiredopposite/OGX-Mini)** —
  RP2040 firmware that emulates USB gamepads for Xbox / PlayStation / Switch
  consoles. Excellent reference for TinyUSB device-driver patterns and HID
  descriptor construction on the RP2040.
- **[danylog/rpi-pico-fs-ia6](https://github.com/danylog/rpi-pico-fs-ia6)** —
  Arduino-pico project that reads a FlySky FS-iA6 receiver via **PWM**
  (6 separate signal wires) and exposes a joystick. Useful reference for
  channel-to-axis mapping conventions expected by flight simulators.

## Roadmap

- Done: iBUS + HID joystick + on-board LED status + failsafe + WebHID tester
- Next: SBUS support (PIO inverted UART) — enables FrSky, Radiolink, and the
  FS-iA10B's alternate output
- Later: Persistent per-user axis inversion / mid-point trims via flash
- Later: Optional CDC serial channel for live debugging

## License

MIT — see `LICENSE`.
