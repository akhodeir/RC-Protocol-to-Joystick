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
- WS2812 status LED (on GP23 of the YD-RP2040) with five distinct states
- USB HID compliant — no drivers needed on Windows, macOS, or Linux

## Hardware

**Board:** YD-RP2040 (USB-C, on-board WS2812 on GP23). A standard Raspberry Pi
Pico works too; you just won't have the status LED unless you wire one externally.

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

## LED status legend (WS2812 on GP23)

| Color                 | Meaning                                        |
|-----------------------|------------------------------------------------|
| Dim blue, pulsing     | Booting / USB not yet enumerated               |
| Dim white             | USB ready, waiting for first iBUS frame        |
| Dim green             | Normal operation — frames arriving             |
| Bright green flash    | All four primary sticks within ±2 % of center  |
| Solid red             | Failsafe: no valid frame for > 500 ms          |

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

If your board's WS2812 lives on a different pin (some clones use GP16 or GP12),
override at configure time:

```bash
cmake .. -DPROTOCOL=ibus -DWS2812_PIN=16
```

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

1. Plug in the flashed Pico. The RGB LED should pulse blue briefly, then go
   dim white once macOS enumerates the HID device.

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
   on the transmitter). Once bound, the RGB LED turns green in normal
   operation and briefly flashes bright green whenever all four primary
   sticks return to center — handy for trim checks.

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
│   ├── main.c                  Main loop, scaling, failsafe, LED state
│   ├── usb_descriptors.c/.h    HID descriptor + TinyUSB callbacks
│   ├── ibus.c/.h               iBUS UART frame parser
│   ├── rgb_led.c/.h            WS2812 status LED driver
│   └── ws2812.pio              PIO program for WS2812 timing
└── tools/
    └── webhid/
        └── index.html          Custom WebHID tester (Chrome / Edge)
```

## References & prior art

Two open-source projects were studied while building this one. Neither is a
dependency — they're worth a look if you're extending this project or want
to see how others have solved adjacent problems:

- **[wiredopposite/OGX-Mini](https://github.com/wiredopposite/OGX-Mini)** —
  RP2040 firmware that emulates USB gamepads for Xbox / PlayStation / Switch
  consoles. Excellent reference for TinyUSB device-driver patterns and HID
  descriptor construction on the RP2040.
- **[danylog/rpi-pico-fs-ia6](https://github.com/danylog/rpi-pico-fs-ia6)** —
  Arduino-pico project that reads a FlySky FS-iA6 receiver via **PWM**
  (6 separate signal wires) and exposes a joystick. Useful reference for
  channel-to-axis mapping conventions expected by flight simulators.

## Roadmap

- Done: iBUS + HID joystick + WS2812 status + failsafe + WebHID tester
- Next: SBUS support (PIO inverted UART) — enables FrSky, Radiolink, and the
  FS-iA10B's alternate output
- Later: Persistent per-user axis inversion / mid-point trims via flash
- Later: Optional CDC serial channel for live debugging

## License

MIT — see `LICENSE`.
