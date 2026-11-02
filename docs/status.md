# Feature status

What is safe to build a set around, and what is still moving.

**Status** means:

| Status | Meaning |
|---|---|
| **Stable** | Complete end-to-end, persisted where persistence is promised, no open tuning item, no reserved-but-inert slot in the way |
| **Beta** | Works and is documented, but a known limitation or an open tuning item remains — check the Notes column before depending on it |
| **Experimental** | Shipped and usable, but the shape may change (default off, reserved slots, grammar still being tuned) |
| **Absent by design** | Not a gap — a deliberate non-goal, see [Limitations](limitations.md) |

**Since** is the date of the first commit that introduced the feature in this repository
(`git log -S`), not a release number: the firmware has no tagged releases yet, and the
de-facto version marker is the EEPROM block size (see [Architecture](architecture.md)).
A blank means the feature predates the initial import or cannot be attributed to one commit.

## Keyboard

| Feature | Mode | Status | Since | Notes |
|---|---|---|---|---|
| Chromatic pads, in-scale LEDs, octave/scale/key | KEY 1 | Stable | 2026-09-25 | MIDI 24 = C0; pads outside the scale stay dark |
| Chord engine, bass, spread, density, latch | KEY 2 | Stable | 2026-09-26 | 8-note maximum |
| Force-to-scale on inbound notes | KEY 2 | Stable | 2026-09-28 | `FScale` |
| Channel select + ENCOD/KBOARD links | KEY 3 | Stable | 2026-09-30 | Momentary; restores the previous base and variant |
| Progression engine, 18 genres | KEY 4 | **Beta** | 2026-10-06 | The progression itself is not persisted — it is rebuilt from the knobs; phrase templates and the cadence band selector are new — compiled, not yet auditioned on hardware |

## Sequencer

| Feature | Mode | Status | Since | Notes |
|---|---|---|---|---|
| 16 tracks, one per MIDI channel, 16 steps each | SEQ 1 | Stable | 2026-09-25 | Track length up to 64 (4 bars) |
| Polimeter / polyrhythm per track | SEQ 3 | Stable | 2026-09-30 | Length is per track, so tracks drift out of phase on purpose |
| Step edit radio (TIE / GLIDE / ACCENT / PROB), shift/transpose | SEQ 1 | Stable | 2026-09-30 | Attributes are step content and move with rotate |
| Live record | SEQ 1/2 | **Beta** | 2026-10-03 | Open tuning item: live-record options on the M1 step-edit row (see `TODO.md`) |
| Bar loop, teleports on activation | SEQ 1 | Stable | 2026-10-03 | Global position kept in `chStepShadow` |
| Swing, 8 style tables | SEQ 4 | Stable | 2026-09-28 | Effective ratio = 50 + (AMT/100) × (max − 50) |
| External input, USB / DIN / BOTH | SEQ 4 | **Beta** | 2026-11-01 | DIN carries **notes only**; clock stays USB so two sources cannot double-advance |
| Follow (a track reading another's timeline) | SEQ 3 | Stable | 2026-10-03 | Single level, non-recursive |
| Per-channel mode memory, `SEQ 4 + REC` | SEQ 4 | Stable | 2026-09-30 | 16 channels × base/variant |

## Generators and pages

| Feature | Mode | Status | Since | Notes |
|---|---|---|---|---|
| ARP + four sub-pages (order, motion, generative, options) | SEQ, MONO/ARP | Stable | 2026-10-26 | 20 per-track values, persisted |
| GNR8 bassline generator | SEQ, MONO/BASS | Stable | 2026-10-09 | Per-step OCT/BRW/PROB are RAM only |
| LEAD melody generator with live contour flip | SEQ, MONO/LEAD | Stable | 2026-10-21 | Per-contour cache is session RAM |
| COMP chord stabs | SEQ, POLY/STEP | Stable | 2026-10-26 | ENC2–4 reserved |
| HARM progression inference (Viterbi over genres) | SEQ, POLY/HARM | Stable | 2026-10-20 | Re-bakes when the feeder changes |
| SKANK walking voicings | SEQ, POLY/SKANK | Stable | 2026-10-22 | Eight knobs + per-step locks (RAM only) |
| HAND strum zone | KEY, POLY/HAND | Stable | 2026-10-06 | Pink→seagreen string gradient, black keys are option toggles |
| 18 preset drum styles | SEQ, DRUM | Stable | 2026-10-25 | Style list is independent of the progression genre |

## Encoder, I/O, persistence

| Feature | Mode | Status | Since | Notes |
|---|---|---|---|---|
| CC 17–20 + four F-banks × two page groups | ENC 1 | Stable | 2026-09-30 | Knob positions stored per CC × channel (36×16) |
| Dial grid screen (270° arcs, CC captions) | ENC 1/2/4 | Stable | 2026-10-26 | ENC 3 keeps the plain label/value screen |
| Locked channel picker | ENC 4 | Stable | 2026-09-30 | 16 pads = 16 encoder channels |
| `ENC 4 + REC` (36×16 values + channel) | ENC 4 | Stable | 2026-09-30 | An older save is ignored |
| USB-MIDI device, notes/CC/clock | all | Stable | 2026-09-25 | USB Type **MIDI** — no serial tty |
| 5-pin DIN input (Serial2) | SEQ 4 | **Beta** | 2026-11-01 | Notes only, no clock/transport |
| SEQMODE4 + REC save/restore | SEQ 4 | Stable | 2026-09-30 | Block grows per feature; older saves load with defaults |
| OLED header lint | tooling | Stable | 2026-10-26 | `tools/header_lint.py`, 21-character band |

## What "still moving" looks like

Two things move independently of this table:

- **Open tuning work** lives in `TODO.md` under *Active — tuning & bug hunt*. A feature
  with an open item there is Beta by definition, even if it feels finished.
- **EEPROM compatibility** is a promise, not a status: every block change must load an
  older save with defaults. If a save is ignored entirely (ENCMODE4), that is called out
  in the table above.
