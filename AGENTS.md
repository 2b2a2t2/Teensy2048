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
3. **Preferred: arduino-cli** — the IDE's own tooling, non-interactive, works headless.
   ```sh
   CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
   "$CLI" compile --fqbn "teensy:avr:teensy41:usb=midi,speed=600" \
     --libraries /var/folders/dy/bnnskbx953j3xwngcqkb1c040000gn/T/opencode/t2048libs \
     --build-path /var/folders/dy/bnnskbx953j3xwngcqkb1c040000gn/T/opencode/t2048build \
     Teensy2048.ino
   "$CLI" upload --fqbn "teensy:avr:teensy41:usb=midi,speed=600" \
     --input-dir /var/folders/dy/bnnskbx953j3xwngcqkb1c040000gn/T/opencode/t2048build \
     --port /dev/ttyUSB0 Teensy2048.ino
   ```
   The `<lib-symlink-dir>` holds **symlinks to FastLED + Encoder only** — the rest
   (U8g2, uClock, Adafruit_MPR121) resolve from the sketchbook `~/Documents/Arduino/libraries`.
   USB Type MIDI enumerates as "Teensy MIDI" (no serial tty); `--port /dev/ttyUSB0` is a
   **placeholder** — the tool finds the board by USB VID/PID. The upload recipe expects
   `Teensy2048.ino.hex` in `<build-dir>`, which is what compile produces.
4. There is no automated test suite — verification is compiling plus on-hardware
   behavior. After edits, at minimum confirm the sketch still compiles.

## Conventions

- Modules are flat `.h/.cpp` pairs in the repo root; responsibilities are
  listed in the README "Module Overview" table.
- Mode/binding changes: `Bindings.cpp` table + `Events.cpp` dispatch + encoder
  labels in `Display.cpp` (see README "Extending / Customizing").
- EEPROM layout changes must stay backward-tolerant (blank `0xFF` = defaults),
  see the save blocks in `Keyboard.cpp` and `Events.cpp`.
- Keep `README.md` in sync with user-visible changes.
