<!-- section moved from the old README; edit freely -->
_Docs home: [Teensy2048](../README.md) · [Cheat sheet](../cheatsheet.md) · [Glossary](../glossary.md)_

# ENCABLETONMODE — the Live bridge (NEXT)

A **latched overlay inside ENCMODE**: tap **NEXT** and the pads become Ableton Live's clip
slots, the eight knobs become the selected device's parameters, and the OLED names whatever
Live has under each knob. Tap **NEXT** again and everything is exactly as it was.

It is an **overlay, not a fourth mode family** — the three families (ENC/SEQ/KEY) keep their
variants. In plain ENCMODE, NEXT has no other job, so in this family it is the bridge.

The bridge needs a companion **remote script** (`Remote Scripts/Teensy2048/`) running on the
same port: the firmware sends the script what you press, the script sends back Live's state.
**Nothing else changes** — ENCMODE's CC 17–20 / CC 1–36 map, its dials and every MIDI-learn
map you made stay byte-identical, because the bridge knobs use a **new CC block, 64–71**.

## Opening and closing

| | |
|---|---|
| **NEXT** tap | `Live Mode On` / `Live Mode Off`; lights the pad row from Live at once |
| leave ENCMODE, or change variant | the overlay closes (it is RAM only, like every page) |
| script not answering | header `Live Link Off`; pads do nothing, knobs still send CC 64–71 |

## Controls while the overlay is up

| Control | Does |
|---|---|
| **Pads** | the selected track's 16 **clip slots**, rows 1–16 |
| **PREV** (hold) | pads become the **16 tracks** — press one to select that track in Live *and* bind the encoders to it; release → clips again |
| **M3** tap | swaps **F1–F4** to the clip-action set (`Clip Actions` / `Bank Actions`) |
| **F1 / F2** | parameter bank ← / → (`Bank 2/5` in the header) · *M3 set:* **fill this track** into the highlighted slot / **fill all 16 tracks** into that scene row |
| **F3 / F4** | device ← / → on the track · *M3 set:* **stop this track's clips** / **stop all clips** |
| **ENC1–ENC8** | CC **64–71** on the encoder channel, = the eight parameters of the current bank |
| **M1 / M2** | unchanged (drum-voice / channel-mute overlays still take the row while held) |

`ENCABLETONMODE` keeps the locked channel picker of plain ENCMODE (variant 4, and the
momentary hold) — picking a channel there also tells Live which track to select.

## The pads

| Pad shows | Meaning |
|---|---|
| **bright green** | clip playing |
| **blinking green** | clip triggered, waiting for its launch quantisation |
| **dim green** | slot has a clip |
| **dark blue** | empty slot |
| **dim red** | the track is armed (every empty slot of an armed track) |
| **yellow** (PREV held) | the track the encoders are bound to |

| Gesture | Does |
|---|---|
| press an **empty** slot | creates the clip and fills it from that track's Teensy pattern |
| press a **filled** slot | launches it (Live's launch quantisation applies) |
| press a **playing** slot | stops it |
| **double**-press a filled slot | re-fills it (notes replaced, clip kept) |
| press a track (PREV held) | selects that track in Live |

## What lands in the clip

| | |
|---|---|
| Length | `pattern length × 0.25` beats (one Teensy step = a 16th), loop = the whole clip |
| Pitch | the stored notes **+ 12 × the track's octave shift + its transpose** — what the sequencer would sound |
| Start | `step × 0.25` |
| Duration | `0.25 × gate %`, a **TIE** step rings on through the following empty steps, **GLIDE** adds 0.02 beat of overlap |
| Velocity | the track's velocity, its **accent velocity** on an accented step |
| Probability | a **PROB** step carries its probability as the note's `probability` (Live rolls it) |

KLOCK/scale snapping is **not** applied — Live has its own scale devices. A track with
**FOLLOW/STRUM/GNR8** dumps the steps it *stored*, not the notes its engine derives at
playback: the engine's output is not a pattern, so it cannot be written down.

A **scene fill** (M3 + F2) writes all 16 tracks into `tracks[n].clip_slots[row]`, where
`row` is Live's highlighted slot row.

## The knobs

The eight knobs auto-map to **`device.parameters`** of the selected device, one per knob,
in pages of eight — **F1/F2** move the page (`Bank 2/5`). The label is Live's parameter name
(short), the value line is Live's own **display value** (`Cutoff 84` in the header as you
turn). Every knob write is rate-limited to one per parameter per 20 ms.

- `value = min + (turned value / 127) × (max − min)`, so a dial always spans the parameter's
  full range whatever its units;
- a **quantised** parameter snaps to its own steps and the display value comes back from
  Live, so the OLED never shows a value Live did not take;
- a track with **no device selected** shows its **mixer** instead: Vol · Pan · six sends;
- since the CC (64–71) is the thing that moves the parameter, it is still **MIDI-learnable**
  on the same port if you would rather learn it into something else.

## The remote script

`Remote Scripts/Teensy2048/` (see its `README.md`): copy the folder into your User Library's
`Remote Scripts`, then Settings → Tempo & MIDI → Control Surface **Teensy2048** with the
Teensy port as **Input and Output**. `Remote Scripts/Tap/` is untouched — the two scripts
live in different control-surface slots.

## Limits

- A remote script is **not** sample-accurate and this bridge carries state, not clock: expect
  ~8–16 Hz of traffic (track names, clip states, parameter pages, pattern dumps). Notes go
  out from the firmware as always — the bridge never touches the sequencer's timing.
- The pattern dump is **chunked** (≤ 32 bytes a message) because a full 64-step poly pattern
  is far more than one SysEx, and the OLED write costs ~25 ms.
- Overlay state is **RAM only**; after a power cycle press NEXT and the script re-latches on
  the next handshake.
- A port that also carries Ableton's **EXT clock** keeps working — clock and SysEx are
  independent messages.
- Clip fills do not touch EEPROM, and Live's own undo does not cover them: re-fill a slot to
  replace its notes.
