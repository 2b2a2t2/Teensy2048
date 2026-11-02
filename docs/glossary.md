# Glossary

The vocabulary the rest of the docs assume. Terms are grouped, not alphabetical, because
the groups are what make the instrument legible.

## Structure

**Mode family** — one of the three top-level behaviours: `ENC` (knobs as CC), `SEQ`
(sequencer), `KEY` (keyboard). Chosen with the MODE pad.

**Variant** — one of four sub-behaviours inside a family. `SEQ 3` is "sequencer, variant 3".
Variants are independent: each family numbers its own 1–4.

**Momentary variant** — variant 3 everywhere. You enter it by holding the mode pad and
leave it by releasing, which restores the previous base **and** variant. It is a look, not
a place.

**Locked variant** — `ENC 4`, the channel picker. It stays until you leave it with a click.

**Track** — one of 16 sequencer lanes, each bound to its own MIDI channel (1–16). A track
has a type, a length, a pattern and its own settings.

**Sequence** — the 16-step pattern a track plays. "Track" is the lane, "sequence" is its
content; the docs say "track" when they mean the whole lane.

**Track type** — `DRUM`, `MONO` or `POLY`. MONO and POLY carry a sub-type (the "transform"
or "type" knob): MONO is `ROOT` / `BASS` / `ARP` / `STRUM` / `LEAD`, POLY is `STEP` /
`STRUM` / `HAND` / `HARM` / `SKANK`.

**Pattern length (`LEN`)** — how many steps the track actually plays, up to 64. Different
lengths per track are the point: that is how polimeter and polyrhythm happen.

## Binding machinery

**Binding set** — the lookup that says what the encoders, controls, step pads and keys do
right now. Four groups: encoder, control, step, key. Defined in `Bindings.cpp`.

**Overlay** — a temporary binding set pushed over the base one for one group, cleared on
release. There is exactly **one overlay slot per group**, so an overlay cannot stack on an
overlay.

**Page** — an encoder page owned by a track type, opened with **M2** and identified by the
pad's hue: GNR8 on a bass track, LEAD on a lead track, COMP on POLY/STEP, and so on.

**Radio** — a set of options where exactly one is armed and arming one takes over the
whole row. The step-edit radio (TIE / GLIDE / ACCENT / PROB) and the F row are radios.

**Arm** — a click that makes an F key or an attribute current until it is clicked off.
Armed keys burn bright.

**Sub-page** — what the encoder row becomes while an F is armed. F1 on the ARP page is the
ORDER sub-page, F4 is OPTIONS.

## Sound and harmony

**Pending chord** — the notes currently held (or arriving over MIDI) that the instrument
treats as one chord: what the pads play, what gets recorded, what followers read.

**Feeder / follower** — a *feeder* is a track whose notes another track reads; the reader
is the *follower*. Set with `SEQ 3 → INCH` (input channel) and `FOLLOW` on. One level only:
a follower is never itself read.

**FOLLOW** — the listener switch. With it on, the track re-derives its notes from the
feeder's timeline at playback instead of playing its stored steps.

**Stored vs derived** — a *stored* step is what the pattern holds; a *derived* note is
computed at send time (LEAD's MAP resolve, SKANK's walk, HARM's live follow). Derived
notes never rewrite the pattern.

**FScale** — force-to-scale: inbound notes are snapped into the current KEY + SCALE
instead of being played raw.

**Latch** — `KEY 2 → ENC8`: the chord keeps sounding after the fingers lift, until the next
chord replaces it.

**Latch vs lock** — a *latch* holds a sound; a *lock* pins a value. SKANK's per-step locks
override that step's knobs; KEY 3's ENCOD/KBOARD **locks** tie the encoder channel and the
keyboard channel to the sequencer's.

**Borrow (`BRW`)** — replace a mapped note with a tone borrowed from the incoming chord,
instead of using the generated pitch.

**Guide tone** — the 3rd and 7th of a chord: the two notes that carry its colour. `GD`
expands that pool.

**Tessitura (`TES`)** — the inversion a walking voicing is anchored to.

**Comp** — re-strike a step's chord several times inside the step (`HITS`, 0–8).

**Accent** — a per-step velocity lift, stamped by the ACCENT target and by the drum page's
`MOT` mask. Accent velocity is a global setting.

## Time and persistence

**Swing ratio** — `50 + (AMT/100) × (max − 50)`, where `max` comes from the selected style
table (MPC60, MPC3K, SP1200, LINN, TR808, TR909, SHUFFLE, TRIPLET).

**EXT IN** — `OFF` / `ON` / `THRU` / `LIVE`: whether external notes are ignored, played,
forwarded or recorded live.

**SRC** — `USB` / `DIN` / `BOTH`: which ports a track accepts external notes from.

**Detent** — one click of an encoder = **4 counts**. Wide dials use 1 count = 1 unit, so a
detent is 4 units (4 % on a 0–100 % dial).

**EEPROM block** — the saved settings region, one block per subsystem, each with a byte
size that grows when fields are added. An older block loads with defaults for the new
fields; that cascade is the compatibility promise.

**`SEQ 4 + REC` / `ENC 4 + REC`** — the save gesture. There is no autosave of settings.

## Display

**Header band** — the top 16 px of the OLED, 128 px / **21 characters**, mode on the left,
transient message on the right.

**Announce** — a transient 2.5 s message naming the parameter and its value in words.

**Legend** — the control-button label row under the encoder grid. ENCMODE 1/2/4 draw none;
the dials take the band.
