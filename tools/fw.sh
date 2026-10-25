#!/usr/bin/env bash
# tools/fw.sh — build / flash Teensy2048 with arduino-cli.
#
#   ./tools/fw.sh build     compile only
#   ./tools/fw.sh upload    compile, then flash the attached Teensy 4.1
#
# Overrides (env):
#   ARDUINO_CLI   path to arduino-cli
#   T2048_LIBS    extra library root dirs, colon separated
#   T2048_BUILD   build directory (default ${TMPDIR:-/tmp}/t2048build)
#   T2048_PORT    upload port (default: auto-detected Teensy port)
#
# USB type is MIDI, so the board exposes no serial tty: the port is resolved
# from `arduino-cli board list` (Teensy Ports, USB VID/PID) like the IDE does.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH="$ROOT/Teensy2048.ino"
FQBN="teensy:avr:teensy41:usb=midi,speed=600"
BUILD_DIR="${T2048_BUILD:-${TMPDIR:-/tmp}/t2048build}"
SKETCHBOOK="${SKETCHBOOK:-$HOME/Documents/Arduino}"
REQUIRED_LIBS=(FastLED U8g2 Encoder Adafruit_MPR121 uClock)

die() { echo "fw: $*" >&2; exit 1; }

# --- arduino-cli ------------------------------------------------------------
find_cli() {
  if [[ -n "${ARDUINO_CLI:-}" ]]; then echo "$ARDUINO_CLI"; return; fi
  if command -v arduino-cli >/dev/null 2>&1; then command -v arduino-cli; return; fi
  local ide="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
  [[ -x "$ide" ]] && { echo "$ide"; return; }
  die "arduino-cli not found (install it, or set ARDUINO_CLI=...)"
}
CLI="$(find_cli)"

# --- libraries --------------------------------------------------------------
# FastLED + Encoder usually live in a secondary sketchbook folder; a dir is
# used only if it actually provides one of the libs the sketch needs.
lib_dirs=()
add_dir() { [[ -d "$1" ]] && lib_dirs+=("$1"); }
add_dir "$SKETCHBOOK/libraries"
IFS=':' read -r -a extra <<< "${T2048_LIBS:-}"
for d in "${extra[@]:-}"; do [[ -n "$d" ]] && add_dir "$d"; done
for d in "$SKETCHBOOK"/*/libraries; do
  [[ -d "$d" ]] || continue
  for lib in "${REQUIRED_LIBS[@]}"; do
    [[ -d "$d/$lib" ]] && { add_dir "$d"; break; }
  done
done

[[ ${#lib_dirs[@]} -gt 0 ]] || die "no library dirs found in $SKETCHBOOK"
for lib in "${REQUIRED_LIBS[@]}"; do
  found=0
  for d in "${lib_dirs[@]}"; do [[ -d "$d/$lib" ]] && { found=1; break; }; done
  [[ $found -eq 1 ]] || echo "fw: warning: library '$lib' not found" >&2
done
LIBS="$(IFS=,; echo "${lib_dirs[*]}")"

# --- port -------------------------------------------------------------------
detect_port() {
  if [[ -n "${T2048_PORT:-}" ]]; then echo "$T2048_PORT"; return; fi
  local port
  port="$("$CLI" board list 2>/dev/null | awk '$2 == "teensy" { print $1; exit }')"
  if [[ -z "$port" ]]; then
    port="$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1 || true)"
  fi
  echo "$port"
}

# --- actions ----------------------------------------------------------------
build() {
  echo "fw: building into $BUILD_DIR"
  "$CLI" compile --fqbn "$FQBN" --libraries "$LIBS" \
    --build-path "$BUILD_DIR" "$SKETCH"
}

upload() {
  local port; port="$(detect_port)"
  if [[ -n "$port" ]]; then
    echo "fw: flashing $port"
    "$CLI" upload --fqbn "$FQBN" --input-dir "$BUILD_DIR" \
      --port "$port" "$SKETCH"
  else
    echo "fw: no Teensy port found — letting arduino-cli pick" >&2
    "$CLI" upload --fqbn "$FQBN" --input-dir "$BUILD_DIR" "$SKETCH"
  fi
}

case "${1:-build}" in
  build)  build ;;
  upload|flash) build && upload ;;
  *) echo "usage: $0 [build|upload]" >&2; exit 2 ;;
esac
