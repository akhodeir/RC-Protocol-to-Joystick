# rc-joystick

Turn a Raspberry Pi Pico (RP2040) into a USB HID joystick that reads an RC
receiver's serial output (iBUS **or** SBUS) and presents itself to a PC/Mac
as a standard game controller — perfect for drone / plane simulators like
**Liftoff**, **Velocidrone**, **DRL Sim**, or **FPV FreeRider**.

The firmware auto-detects iBUS vs SBUS at boot from the same GP1 pin, and
even re-detects live if you swap receivers without unplugging the Pico. Any
receiver speaking either protocol works:

- FlySky FS-iA6B, FS-iA10B, FS-iA8B, X6B, X8B, etc. (iBUS or SBUS)
- Radiolink R7FG, R9DS, R12DS (SBUS)
- FrSky X4R, X8R, R-XSR, X6R (SBUS)
- Any generic receiver with an iBUS or SBUS port

## Features

- **Runtime protocol auto-detect** — one UF2, no build-time flag; boots into
  whichever protocol is on the wire, and re-detects after prolonged failsafe
  so you can swap iBUS ↔ SBUS receivers without power-cycling
- **SBUS with no external inverter** — signal polarity flipped at the GPIO
  pad via the RP2040's built-in `INOVER` field; a plain hardware UART reads
  the (normally inverted) SBUS signal directly
- 8 axes, 16-bit signed each (full ±32767 range → smooth throttle/pitch curves)
- 32 buttons (aux switches CH9+ mapped to buttons 0+ by default)
- Multi-trigger **failsafe**: axes center, throttle to minimum, buttons
  cleared on frame timeout (500 ms), channel freeze (5.5 s), or SBUS
  FAILSAFE/FRAMELOST flag
- **5-state status LED** on GP25 — visually distinguishes booting, waiting,
  wrong-wiring, failsafe, and active states
- USB HID compliant — no drivers needed on Windows, macOS, or Linux

## Hardware

**Board:** YD-RP2040 (USB-C, on-board LED on GP25). A standard Raspberry Pi
Pico works too.

**Receiver:** any FlySky / Radiolink / FrSky / OrangeRx unit with an iBUS or
SBUS output. Tested with FS-iA6B (iBUS) and FS-iA10B (iBUS + SBUS) paired
with an FS-i6X transmitter.

## Wiring

Same wiring for iBUS **or** SBUS — the firmware auto-detects which one your
receiver is speaking.

```
Receiver (iBUS or SBUS port)          YD-RP2040 (or Pi Pico)
─────────────────────────────────────────────────────────────
Signal wire      ─────────────────→   GP1  (UART0 RX, pin 2)
+5 V (VCC)       ─────────────────→   VBUS (pin 40)
GND              ─────────────────→   GND  (pin 3 or 38)

USB-C  ───────────────────────────→   Host PC / Mac
```

Both protocols are 3.3 V-level on the data wire — no level shifter needed.
SBUS is inverted; the firmware flips polarity at the GPIO pad, so **no
external hardware inverter is required either**. Do NOT connect the
receiver's 5 V rail to the Pico 3V3 pin.

## Status LED (GP25)

All pulse durations are sized so you can visually count the blinks.

| Pattern | Meaning |
|---|---|
| Fast even blink (250 on / 250 off) | Booting / USB not yet enumerated |
| Slow even blink (1000 on / 1000 off) | USB up, no bytes on the wire yet — receiver not connected or unpowered |
| **3 pulses (250/250) + 1500 pause** | Bytes arriving on the wire but no valid iBUS/SBUS frames — wrong pin, wrong protocol, or wrong baud |
| **4 pulses (250/250) + 1500 pause** | Signal lost — failsafe engaged (transmitter off, out of range, or channels frozen) |
| Nearly solid (2500 on / 300 off) | Healthy: valid frames arriving with moving channel values |

### How autodetect works

At power-up the firmware alternates between iBUS (115200 8N1, non-inverted)
and SBUS (100000 8E2, inverted at the GPIO pad) on the same UART, spending
~300 ms on each attempt. As soon as one produces a valid frame it locks in
and the LED transitions from `WAITING` (or `WRONG_WIRING`) to `ACTIVE`.

### Runtime re-detect

If the LED sits in the 4-pulse `FAILSAFE` pattern for more than 10 seconds,
the firmware silently tears down the current protocol and re-runs
detection. This lets you swap iBUS ↔ SBUS receivers on the same GP1 pin
without power-cycling — see the Failsafe section below for the full flow.

### Failsafe

**Triggers** — failsafe engages the moment any of these fire:

- **Frame timeout** — no valid frame for 500 ms (either protocol)
- **Channel freeze** — all channels bit-identical for 5.5 s (iBUS has no
  explicit signal-lost flag; many receivers keep sending frames with held
  values when the TX drops, so we heuristically detect frozen values)
- **SBUS flag** — the FAILSAFE or FRAMELOST bit is set in the SBUS frame
  (nearly instant, no timeout needed)

The 5.5 s freeze threshold lets you hold sticks steady in real flight
without falsely tripping failsafe. Tune via `CHANNEL_FREEZE_MS` in `src/main.c`.

**HID output during failsafe** — the device does NOT disappear from the
host. The firmware keeps sending reports at 200 Hz, but every report has
this fixed payload:

| HID field | Value | Meaning to the sim |
|---|---|---|
| X (Roll)         | 0        | Stick centered |
| Y (Pitch)        | 0        | Stick centered |
| **Z (Throttle)** | **-32767** | **Throttle bottomed** |
| Rx (Yaw)         | 0        | Stick centered |
| Ry / Rz / Slider / Dial | 0 | Aux at neutral |
| Buttons 0–31     | all 0    | All switches released |

Raw wire bytes: `00 00 00 00 01 80 00 00 00 00 00 00 00 00 00 00 00 00 00 00`
(Z = `0x8001` little-endian = -32767).

This matches real-world FPV convention: **drop, don't wander** — safer
than "hold last stick position", which could cause a runaway. In Liftoff
the drone will level out and fall.

**Recovery** — as soon as valid frames with changing channel values start
arriving again, `rc_active` flips back to true and real stick data resumes
on the very next 5 ms send tick.

**Runtime re-detect** — if failsafe persists for > 10 s, the firmware
silently re-runs protocol detection (tears down the current UART setup and
alternates between iBUS and SBUS again). This lets you swap iBUS ↔ SBUS
receivers on the same GP1 pin without power-cycling: unplug the old
receiver, wait ~15 seconds, plug in the new one — the LED goes back to
`ACTIVE` on whichever protocol the new receiver speaks.

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
cmake ..
make -j$(sysctl -n hw.ncpu)
```

Output: `build/rc_joystick.uf2` (~46 KB).

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

1. Plug in the flashed Pico. The on-board LED (GP25) should blink at the
   fast BOOT cadence during USB enumeration, then slow to the 1 s WAITING
   blink once macOS mounts the HID device — no receiver connected yet.

2. For descriptor-level debugging (Chrome / Edge only), open
   **https://nondebug.github.io/webhid-explorer/** and select the RC-Joystick
   device — you'll see the parsed HID descriptor tree and raw report bytes.

3. For a sim-styled visualiser tailored to this project, open
   `tools/webhid/index.html` in Chrome or Edge — labelled bars for each axis,
   button LEDs, live report rate, and a 10-second CSV recording button.

4. Bind the receiver and transmitter (procedure varies by receiver — most
   FlySky receivers: hold the button while powering it, then start bind mode
   on the transmitter). Once bound, the on-board LED enters the healthy
   nearly-solid `ACTIVE` pattern. If the TX drops for > 500 ms or channels
   freeze for > 5.5 s, the LED switches to the 4-pulse `FAILSAFE` pattern
   and the report enters failsafe (throttle to minimum).

## Channel to axis mapping

| RC channel | HID axis    | Typical use      |
|-----------|-------------|------------------|
| CH1       | X           | Roll / Aileron   |
| CH2       | Y           | Pitch / Elevator |
| CH3       | Z           | Throttle         |
| CH4       | Rx          | Yaw / Rudder     |
| CH5       | Ry          | Aux              |
| CH6       | Rz          | Aux              |
| CH7       | Slider      | Aux              |
| CH8       | Dial        | Aux              |
| CH9+      | Buttons 0+  | Switches (threshold at protocol midpoint) |

iBUS exposes 14 channels; SBUS exposes 16. Extras above CH8 appear as
buttons 0 through 5 (iBUS) or 0 through 7 (SBUS).

## Project layout

```
rc-joystick/
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── tusb_config.h
├── README.md
├── src/
│   ├── main.c                  Main loop, autodetect, scaling, failsafe, LED
│   ├── usb_descriptors.c/.h    HID descriptor + TinyUSB callbacks
│   ├── ibus.c/.h               iBUS UART parser (115200 8N1)
│   └── sbus.c/.h               SBUS UART parser (100000 8E2 inverted via pad)
└── tools/
    └── webhid/
        └── index.html          Custom WebHID tester (Chrome / Edge)
```

## Notable implementation details

- **Pad-level SBUS inversion** — the RP2040 has an `INOVER` field in
  `IO_BANK0_GPIOxx_CTRL` that inverts the peripheral input signal before it
  reaches the UART. `gpio_set_inover(pin, GPIO_OVERRIDE_INVERT)` enables it.
  **Critical:** `gpio_set_function()` writes the whole CTRL register and
  zeroes `INOVER`, so the function must be set *before* the inversion — see
  `sbus_init()` for the correct ordering.
- **Runtime channel-freeze detection** — iBUS has no explicit signal-lost
  flag, so we watch for all channels being bit-identical for > 5.5 s.
  Potentiometer jitter under a real transmitter always drifts at least one
  count within that window; only "TX off with receiver holding" produces a
  perfectly frozen frame.
- **Same-UART protocol swap** — both parsers share `uart0` on GP1. Switching
  protocols requires only `uart_deinit` + `uart_init` at a different baud
  rate plus flipping the pad inversion — the whole operation completes in
  under 1 ms.

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

- Done: iBUS + SBUS + auto-detect + runtime re-detect + HID + LED + multi-trigger failsafe + WebHID tester
- Later: Persistent per-user axis inversion / mid-point trims via flash
- Later: Optional CDC serial channel for live debugging

## License

MIT — see `LICENSE`.
