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

**Work in the local checkout** (the folder GitHub Desktop syncs — on this machine
`~/Desktop/Teensy2048`, branch `main`), not in a throwaway copy: the Teensy 4.1 is
plugged into this machine, so edits can be compiled and flashed straight away with
no sync-and-copy step. Never edit only a scratch copy — the user's own folder is
the source of truth.

1. Teensyduino (Arduino IDE or VS Code + PlatformIO), libraries: FastLED,
   U8g2, Encoder, Adafruit_MPR121, uClock. Open `Teensy2048.ino`, select
   **Teensy 4.1**, **USB Type: MIDI**.
2. **Preferred: `tools/fw.sh`** — wraps arduino-cli (from the Arduino IDE bundle)
   and does the bookkeeping the raw command needs:
   ```sh
   ./tools/fw.sh build     # compile only
   ./tools/fw.sh upload    # compile, then flash the attached board
   ```
   It finds `arduino-cli` (`PATH` → Arduino IDE bundle → `$ARDUINO_CLI`), collects the
   library roots that actually provide the five required libs (the sketchbook
   `~/Documents/Arduino/libraries` plus any `$SKETCHBOOK/*/libraries` that holds
   FastLED/Encoder — on this machine `~/Documents/Arduino/Arduino maison/libraries`),
   builds into `${TMPDIR:-/tmp}/t2048build`, and resolves the **Teensy port**
   (`usb:…` from `arduino-cli board list`) because USB Type MIDI exposes no serial tty.
   Overrides: `ARDUINO_CLI`, `T2048_LIBS` (colon-separated), `T2048_BUILD`, `T2048_PORT`.
3. Raw equivalent, if the script is unavailable:
   ```sh
   CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
   "$CLI" compile --fqbn "teensy:avr:teensy41:usb=midi,speed=600" \
     --libraries "$HOME/Documents/Arduino/libraries,$HOME/Documents/Arduino/Arduino maison/libraries" \
     --build-path /tmp/t2048build Teensy2048.ino
   "$CLI" upload --fqbn "teensy:avr:teensy41:usb=midi,speed=600" \
     --input-dir /tmp/t2048build \
     --port "$("$CLI" board list | awk '$2=="teensy"{print $1;exit}')" Teensy2048.ino
   ```
   Currently-running firmware that was built **without** MIDI-only USB still shows a
   `/dev/cu.usbmodem*` serial tty; after flashing this sketch that tty disappears and the
   board only appears under **Teensy Ports** — that is success, not a disconnect.
4. There is no automated test suite — verification is compiling plus on-hardware
   behavior. After edits, at minimum confirm the sketch still compiles
   (`./tools/fw.sh build`).

## Conventions

- Modules are flat `.h/.cpp` pairs in the repo root; responsibilities are
  listed in the README "Module Overview" table.
- Mode/binding changes: `Bindings.cpp` table + `Events.cpp` dispatch + encoder
  labels in `Display.cpp` (see README "Extending / Customizing").
- EEPROM layout changes must stay backward-tolerant (blank `0xFF` = defaults),
  see the save blocks in `Keyboard.cpp` and `Events.cpp`.
- Keep `README.md` in sync with user-visible changes.
