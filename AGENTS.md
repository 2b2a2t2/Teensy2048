# Teensy2048 — Agent rules

Firmware for a Teensy 4.1 touch/encoder performance controller (pads, encoders,
OLED, WS2812 LEDs, USB-MIDI). Architecture, modes and workflows are documented
in `README.md` — treat it as the reference and keep it updated when behavior
changes.

## Backlog workflow (TODO.md)

`TODO.md` is the single source of truth for bugs, tuning work and feature
ideas. It persists across sessions — never rely on chat memory for tasks.

- **Before feature or bug work**: read `TODO.md`.
- **User reports a bug / tuning issue**: append it to `Active — tuning & bug hunt`.
- **User mentions an idea** (even in passing): append it to `Backlog` or
  `Parking lot` immediately — do not let it drop.
- **Item finished**: tick the checkbox and move it to `Done`.
- **Freeze rule**: do not start work from `Backlog` while `Active` has open
  items, unless the user explicitly overrides.

## Build & flash

1. Teensyduino (Arduino IDE or VS Code + PlatformIO), libraries: FastLED,
   U8g2, Encoder, Adafruit_MPR121, uClock.
2. Open `Teensy2048.ino`, select **Teensy 4.1**, **USB Type: MIDI**, compile & upload.
3. There is no automated test suite — verification is compiling plus on-hardware
   behavior. After edits, at minimum confirm the sketch still compiles.

## Conventions

- Modules are flat `.h/.cpp` pairs in the repo root; responsibilities are
  listed in the README "Module Overview" table.
- Mode/binding changes: `Bindings.cpp` table + `Events.cpp` dispatch + encoder
  labels in `Display.cpp` (see README "Extending / Customizing").
- EEPROM layout changes must stay backward-tolerant (blank `0xFF` = defaults),
  see the save blocks in `Keyboard.cpp` and `Events.cpp`.
- Keep `README.md` in sync with user-visible changes.
