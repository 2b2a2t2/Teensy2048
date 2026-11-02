# Changelog

User-visible changes, newest first. The firmware has **no tagged releases** yet, so the
de-facto version marker is the size of the saved-settings block (see
[Compatibility](#compatibility)).

## Unreleased

### Keyboard

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
| 2026-09-28 | `scales update` | scale table and the swing style tables |
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
| 260 bytes | earliest saved layout |
| … 388 / 404 / 420 | intermediate additions (defaults on older saves) |
| 436 | COMP `HITS` per track |
| 567 | the eight SKANK knobs |
| 616 | per-channel mode memory (KEY variant, F bank + page group) |
| 649 | LEAD contour, CT blend, GD depth |
| 923 | DRUM page state: genre, VAR, lane lines, per-lane FILL/MOT |
| 1330 | swing style, amount, `EXT IN`, `SRC` per track |
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
