# Teensy2048 — Ableton Live remote script

Companion control-surface script for **ENCABLETONMODE** (the NEXT overlay in the firmware's
ENCMODE). It mirrors track selection, turns Teensy patterns into clip notes, fires/stops
clips, and maps the eight knobs onto the selected device's parameters.

The Tap script is untouched and unrelated: this one lives in its own control-surface slot.

## Install

1. Copy this whole folder to
   `<User Library>/Remote Scripts/Teensy2048/` (keep `__init__.py` and `Teensy2048.py`).
2. Live → Settings → **Link, Tempo & MIDI** → an empty Control Surface slot:
   - **Control Surface**: `Teensy2048`
   - **Input**: the Teensy port (e.g. `Teensy MIDI`)
   - **Output**: the same port
3. Leave the port's **Track** switch on; turn **Sync** on only if the firmware follows
   Ableton's clock (SYNC = EXT).
4. On the Teensy: **ENC** → tap **NEXT**. The header shows `Live Mode On` and the pad row
   lights from Live.

## What it does

| Teensy | Live |
|---|---|
| NEXT | starts/stops the OLED/pad bridge (the script answers the firmware's handshake either way) |
| pads | fire / stop / create-and-fill the selected track's 16 clip slots |
| double-press | re-fill the slot from the pattern |
| PREV + pad | select track `n` (also on the firmware side) |
| M3 then F1/F2 | fill this track into the highlighted slot / all 16 tracks into that scene row |
| M3 then F3/F4 | stop this track's clips / stop all clips |
| F1/F2 | parameter bank ←/→ |
| F3/F4 | previous/next device on the track |
| ENC1–8 | the eight parameters of the current bank (CC 64–71 echoed as SysEx) |

Mixer page: a track with no device selected exposes Vol · Pan · Sends instead.

## Protocol

`F0 7D 37 32 34 38 <cmd> [7-bit args] F7` (manufacturer 0x7D, tag "7248"), ≤ 32 bytes per
message. Frame delimiters are optional on receive. Commands and payloads are documented in
`docs/modes/link.md` and `Link.h`; the clip frame is
`nNotes, notes…, attr, prob, velocity` per step with `attr` bits TIE 1 · GLIDE 2 · ACCENT 4 ·
PROB 8.

## Notes / limits

- Remote-script MIDI is not sample-accurate: state only, ~8–16 Hz, never clock.
- Clip length is `patlen × 0.25` beats; notes are `stored + 12 × octave + transpose`, with
  TIE/GLIDE/ACCENT/PROB mapped onto `MidiNoteSpecification` (probability included).
- `remove_notes_extended` is used to clear a slot and `set_notes` is the fallback for older
  Live versions.
- Nothing here writes the firmware's EEPROM, and nothing in `Remote Scripts/Tap/` is read.
