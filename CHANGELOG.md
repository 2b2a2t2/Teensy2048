# Changelog

User-visible changes, newest first. The firmware has **no tagged releases** yet, so the
de-facto version marker is the size of the saved-settings block (see
[Compatibility](#compatibility)).

## Unreleased

### Sequencer

- **ARP: the MODE knob now carries all nine walk orders** - `OUTIN` / `INOUT` /
  `CONV` / `DIV` existed only on the F1 `ORDER` knob, so with `ORDER = MODE` (the
  default) they were unreachable and the MODE knob stopped at `RANDOM2`. The enum
  grew past `RANDOM2` (`SEQ_ARP_OUTIN` .. `SEQ_ARP_DIV`), the knob and the value
  column follow it, and an older save's mode 0..4 keeps its exact sound. The ORDER
  knob's list is unchanged, so `RANDOM` / `RANDOM2` stay on MODE and `PLAY` still
  follows MODE for its shape.

- **SEQ 3: hold a bar pad for that bar's `OCT` / `TRANS`** - the keyboard's C D E F row
  already picks the 16-step edit window (and loops it on a second press); now the pad
  also arms an encoder row for **that bar**: `ENC1` = `OCT` (-2..+2), `ENC2` = `TRANS`
  (-12..+12 st), header `Bar 3 Transpose +5`. The stored chords are untouched - the
  shift folds into the track's own octave/`TRANS` at playback, so a `KLOCK` track
  snaps the transposed bar back into scale instead of stepping outside it. DRUM tracks
  refuse both (`Bar 2 Drum Track`): their steps are drum-map ids, not pitches. One bar
  at a time (the last pad pressed); the row drops when the last C-F pad lets go.

### Keyboard

- **KEY 4 phrase templates are per-bar, so `AABB` / `ABAB` / `AABA` differ** - the
  restating templates used to copy the first half verbatim into the second, which made
  all three produce the same `AA` pattern and left only `ABAC` distinguishable. They
  are a four-letter map over bars now: a bar whose letter has already played replays
  the first bar that carried it, so `AABB` runs one cell for bars 1-2 and a repeated
  second cell for 3-4, `ABAB` alternates, `AABA` returns to the opening cell for its
  last bar, and `ABAC` repeats just bar 3 from bar 1. The replay goes through the
  normal write path, so the chain advances with it and the next new bar answers the
  repeated material; each bar's planned landing and any locked step are never
  overwritten, which keeps the cadence arc intact.

- **KEY 4: `PHRASE` promoted to the encoder row, `ERA` moved to the EMB page** - the phrase
  template decides whether the pattern has an arc at all, so it no longer sits two gestures
  deep (`NEXT` tap, then `ENC3`): it takes `ENC7`, the slot `ERA` used, and reads out by name
  (`FREE`/`AABB`/...). `ERA` keeps its value and now lives at the EMB page's `ENC3`, where a
  turn regenerates the unlocked steps immediately instead of waiting for the next row turn.
- **KEY 4 phrase templates** - `FREE` / `AABB` / `ABAB` / `ABAC` / `AABA` (EMB page
  `ENC3`) give the progression generator a **goal**: the cadence the knob selected is
  planned for the last bar, every earlier bar end is derived **backward** from it, and
  the forward pass scores those landings x8. `FREE` (the boot default) keeps the old
  causal chain. After M3/F1 the header says which layer decided the phrase end
  (`Plan V-vi PLAN|CAD|GENR`) - arc-level only, the scoring terms are not attributed.
- **Named readouts for two KEY 4 knobs** - `MOOD` reads DARK/COOL/MIXED/WARM/LIGHT and
  `CAD` its cadence pair (`V-I`/`IV-I`/`V-vi`/`ii-V`). The `CADENCE` knob is now a
  **four-band selector** (HALF `ii-V` under 25, PLAGAL `IV-I` under 50, AUTHENTIC `V-I`
  under 75, DECEPTIVE `V-vi` above): a seeded weighted roll left the type unchanged
  while the knob moved, which read as a dead knob. Its continuous value still scales the
  cadence terms.

- **KEY 3 `ENCOD` / `KBOARD` boot `locked`** — the encoder channel and the keyboard
  channel follow the SEQ edit channel (the hub) from power-up, instead of each keeping
  its own channel until you turn the link on. Turn the knob CCW to go back to `free`.
  A save written before this change holds no explicit choice, so it boots locked too;
  the first turn of the knob records yours.

### Documentation

- `README.md` split into a one-page landing page and a `docs/` tree — the old page was
  103,568 characters on 599 lines with no table of contents. Prose was moved verbatim; see
  [ADR 0008](docs/adr/0008-docs-split.md).
- New pages: [start here](docs/start-here.md), [glossary](docs/glossary.md),
  [cheat sheet](docs/cheatsheet.md), [panel map](docs/panel-map.md),
  [concepts](docs/concepts.md), [feature status](docs/status.md),
  [recipes](docs/recipes/index.md), [FAQ](docs/faq.md),
  [limitations](docs/limitations.md), [style guide](docs/style-guide.md),
  [bindings reference](docs/reference/bindings.md) and eight [ADRs](docs/adr/README.md).
- `AGENTS.md` now points at `docs/` as the reference instead of `README.md`.

No firmware behaviour changed in this entry.

## Feature timeline

The first commit whose diff mentions each surface, with the commit subject quoted as it was
written. Subjects often describe the broad change rather than the feature, so read the date
as "this is when it appeared", not as a release note.

| Date | Commit subject | Surface |
|---|---|---|
| 2026-09-25 | `first up` | 48-pad MPR121 scan, OLED, sequencer skeleton, USB-MIDI |
| 2026-09-28 | `scales update` | scale table and the chord engine (`Chords.cpp`) |
| 2026-11-01 | `Swing, pattern edit` | per-track swing style and amount |
| 2026-09-30 | `mono channel implementation` | per-channel step edit; the channel variants (`SEQ 3`/`4`, `ENC 3`/`4`) |
| 2026-10-03 | `64 steps` | 64-step patterns, bar loop, follow |
| 2026-10-06 | `chord progressions` | KEY 4 progression engine, 18 genre tables |
| 2026-10-07 | `Strumming` | strummed chords and the HAND strum zone |
| 2026-10-09 | `bassline generator` | GNR8 page (MONO/BASS) |
| 2026-10-20 | `minor bugs` | HARM harmonizer |
| 2026-10-21 | `MONO / HARM engine` | LEAD generator page |
| 2026-10-22 | `skank track engine` | SKANK walking voicings and COMP stabs |
| 2026-10-25 | `drum patterns` | 18 preset DRUM styles |
| 2026-10-26 | `more arp options` | ARP sub-pages; encoder dial grid; 21-character header lint |
| 2026-11-01 | `live input` | `EXT IN = LIVE`, `SRC = DIN/BOTH` |

## Compatibility

Saved settings are **backward-tolerant**: a block written by an older build loads, with the
fields it did not have left at their defaults. Fields are appended, never inserted. The
block size tells you which build wrote it.

| SEQ block | Added |
|---|---|
| 260 | earliest saved layout |
| 388 | GNR8 page parameters |
| 404 | LEAD contour |
| 420 | COMP `HITS` per track |
| 436 | TRANS offset |
| 532 | SKANK knobs `TES`…`SUB` |
| 564 | SKANK randomizers `TES%` / `NOT%` |
| 567 | KEY 3 `ENCOD` / `KBOARD` link flags |
| 616 | per-channel mode memory (KEY variant, F bank + page group) |
| 649 | LEAD CT blend, GD depth |
| 665 | per-track `Klock` (7-rung speed ladder) |
| 666 | 9-rung speed ladder marker |
| 923 | DRUM page state: genre, VAR, lane lines, per-lane FILL/MOT |
| 940 | DRUM kit map |
| 1262 | ARP sub-page parameters |
| 1296 | per-track swing style and amount |
| 1330 | per-track `EXT IN` and `SRC` |
| 1380 | **pending** — live-record options on the M1 step-edit row (`TODO.md`) |

`ENC 4 + REC` is the exception: an older block is **ignored** and defaults stay, because a
half-loaded CC map is worse than a clean one.

## Release-note template

```markdown
## <date> — <what a player can now do>

### Play
- The three or four things that are new or different at the panel, in player language.

### Compatibility
- SEQ block: <old> → <new> bytes. An older save loads with <defaults>.
- Any gesture that changed, and what it was before.

### Known issues
- Open items from TODO.md that a player can hit, with the workaround.
```

Lead with the behaviour, not the files. Never claim a fix that was only compiled.
