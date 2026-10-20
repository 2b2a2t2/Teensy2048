# Teensy2048 — Backlog

Single source of truth for bugs, tuning work and feature ideas.
Rules of engagement live in `AGENTS.md` (read it before touching this file).

## 🔧 Active — tuning & bug hunt

Current phase: tuning + bug hunting on the hardware. **No feature work
from the Backlog while this section has open items** (unless overridden).

_(empty — freeze lifted; Backlog is fair game)_

## 🚿 Hygiene (quick fixes, anytime)

- [x] Add MIT `LICENSE` (README promised it) — 2026-10-12
- [x] Untrack `.DS_Store` + add to `.gitignore` — 2026-10-12

## 🧊 Backlog — features (frozen until Active is empty)

### SEQMODE (sequencer, `Keyboard.cpp` / `Bindings.cpp` / `Events.cpp`)

- [ ] **Pattern copy/paste** — copy a pattern (or selection) and paste it to another track/pattern slot. Step table lives in `Keyboard.cpp` (`STEP_COUNT`, `STEP_MAX`, per-channel step arrays).
- [ ] **Shift / transpose pattern** — rotate steps left/right and transpose notes, applicable to the whole pattern or to individual steps. Same storage as above; mind per-step attributes (TIE, velocity, `seqOct`).
- [ ] **Euclidean sequencing** — Euclidean rhythm as a step-writing tool (e.g. GNR8-adjacent or a dedicated pad gesture). `bjorklund.h` + `Gen.cpp` already provide the Bjorklund/Euclidean placement primitive to reuse.

### ENCMODE (encoder/CC mode, `Events.cpp` / `Bindings.cpp` / `Display.cpp`)

- [ ] **Knob motion recording** — record encoder movements over time and play them back as CC automation. Reuse the ENCMODE EEPROM save block (`Events.cpp:1227`) for persistence.

### Chord progression (`Chords.cpp` / `Key4.cpp`)

- [ ] **Per-chord length** — adjust how long each chord in the progression is held (individual step length, not uniform).
- [ ] **Insert chord** — insert a chord at a position within the progression, shifting the rest.

### Scale / Key

- [ ] **Record scale or key change** — capture scale/key changes as they happen (automation over time). Extend with a per-bar key/scale map (16 slots): playback resolves KEY/SCALE per bar (FScale lookup in `Chords.cpp`, send path in `SeqClock.cpp`), enabling generator-driven modulation (pivot chord then re-key). Scale tables in `Chords.cpp` (`kScalePat`, `kScaleName`), KeyLock/key state in `Chords.h`/`Key4.cpp`.

### Composition engine (`Key4.cpp` / `Gen.cpp` / `SeqClock.cpp`)

- [ ] **LEAD generator** — new MONO track type parallel to BASS: melody against the progression via the existing FOLLOW/MAP machinery, new MAP options chord-tone (`ct`) + guide-tone (`gd`, 3rds/7ths), contour parameter (arch/descend/random walk). `Gen.cpp`, FOLLOW path `SeqClock.cpp`, `Bindings.cpp`.
- [ ] **POLY comping rhythm** — rhythmic stabs for POLY chord tracks derived from Euclid/bjorklund.h instead of one block chord per step (`SeqClock.cpp` POLY playback).
- [ ] **Chord anticipation** — chord changes on the "& of 4" instead of the downbeat (`SeqClock.cpp` advance path).
- [ ] **FOLLOW approach tones** — new per-step modifier (semitone/scale approach into the next chord tone), reusing the PROB/BRW machinery (`SeqClock.cpp` FOLLOW map + per-step attributes in `Keyboard.cpp`).
- [ ] **Motif capture** — snapshot a 4-bar phrase as a motif; transpose/repeat per track with a "repeat every N bars" flag (new `Motif.h/cpp`; today: 16-step loop + one-level undo).
- [ ] **Energy-arc automation** — per-bar envelope automating the KEYMODE4 TENS/MOTION/SURP knobs over the pattern length (build/drop).
- [ ] **Per-genre substitution tables** — replace fixed `subSw[7]` (`Key4.cpp:689`) with per-genre tables like the transition data, gated by SURP + function state; add tritone sub and modal interchange (borrow from parallel minor) as first-class substitutions.
- [ ] **HARM: bass/arp → chord progression (inverse FOLLOW)** — new generation mode inferring chords from a monophonic bassline (Mode A) or an ARP step pattern (Mode B: direct smallest-chord-contains, or Viterbi inference for sparse arps). Candidates scored by a new bass-consonance term in `Key4.cpp` + existing genre/MOOD/MOTION/CADENCE/SURP terms; Viterbi/beam pass over steps (shared machinery with the beam-search parking item); bulk-write via the M3 apply/undo path, skipping locked steps; header `harm N steps` mirroring `gen N steps`. Edge cases: non-chord-tone lookahead, hold-on-repeated-bass toggle, rests carry previous chord, octave fold before inference. Pairs with FOLLOW/LEAD: bass → HARM → chords → FOLLOW → LEAD.

### General — display & workflow (`Display.cpp`)

- [ ] **Split main screen: encoder part vs button part** — divide the main screen into two zones (encoder grid, control-button grid). When an encoder is tweaked or a button is used, temporarily enlarge that part and show the encoder's full name + value, or the accessed button function, then fall back. Today: 4x4 encoder grid y16–40 + button rows y54/62 (`Display.cpp:38`), transient feedback via `headerShow()` (`Display.cpp:502`). Related: on-device legend/help overlay — big temporary legend of the current mode/variant/overlay bindings, same enlarge-then-fall-back idea.

## 💡 Parking lot (raw ideas, unsorted)

- Host-side unit tests for the pure logic (chords/genre/bjorklund) — prerequisite before any `Events.cpp` dispatch refactor. Then migrate dispatch incrementally (one binding group at a time, e.g. ENCMODE CC banks first; verify with the `logSets()` serial dump + on-hardware testing per group) instead of a big-bang BindingResolver/DSL rewrite
- README split — extract user manual (Workflows) and developer docs (architecture, event flow, extending) into `docs/`, keep `README.md` as concise overview + quick start; do it when the README next grows past a pain threshold
- Multi-level undo (3–4 snapshot ring buffer instead of the one-level M3-hold)
- Optional SD-card save for patterns/progressions (Teensy 4.1 slot; RAM-only by design today)
- `Config.h` — consolidate scattered constants (`Keyboard.h`, `Chords.h`, inline magic numbers)
- Beam-search 4-bar phrase proposal (score whole spans; "propose a phrase" long-press)
- FOLLOW variations: inversion-aware NEAR, BASS rhythmic independence (HAND-style empty-step resolution), octave/register contour, tiered fallback (buffered feeder chord → pending → last-known), second-follower chain with recursion guard
- Harmonic rhythm control — chord-change rate independent of step rate

## ✅ Done (ticked items; prune occasionally)

- [x] **KEYMODE4 live regeneration** — GENRE/MOOD/TENS/MOTION/CAD/CPX/ERA/SURP turns now re-generate every unlocked step in place (`k4Regenerate()` in `Key4.cpp`), so the progression playing in the sequencer follows the knobs in real time; the first count of a tweak burst claims the undo slot (M3-hold restores the pre-tweak progression); locked steps untouched. EMB knobs (SUB/INV/PAS/COL) left as M3/F1-only — they don't re-rank the proposals; 2026-10-20
- [x] **KEYMODE4 header: drop the focus prefix** — progression now reads `V vi > I IV` (the focused step is already marked on the sequencer pads); `k4Announce()` in `Key4.cpp` + README; 2026-10-20
- [x] **Fix placeholder encoder labels** — `Bindings.cpp` `encLabels[]`: real idle legends for every set (SEQ1/2/4 = OCT/GATE/VEL/PROB with `--` for page-bound knobs, KEY1 = OCT/SCALE/KEY, KEY3 = OCT/SCALE, dead `ENC_SEQ_STEP_HELD` row = `--`); Display overrides still repaint per page/radio, and idle knobs (label `--`/`---`) now show a **blank** value instead of the raw encoder position or `---` (`Display.cpp`); `ctrlLegend()` reviewed — already covers all modes/submodes; 2026-10-20
- [x] **SEQMODE radio toggle bug** — M1 (edit) / M2 (ARP/GNR8 page) / F1–F4 (targets) are now one radio on MONO arp+bass tracks: opening an M2 page drops the edit selection (M1 dims, not white), arming an F target or M1 drops the ARP page (GNR8 already had exclusivity; the M1 white LED was the remaining bass-track bug) — `Events.cpp` M1/M2/F handlers + `Modes.cpp` M1 LED; 2026-10-20
- [x] **Key4 scoring cluster** — phrase/pair cadence terms (ii–V, V–I at bar ends + authentic/plagal landing in the pattern's last bar, CADENCE knob), voice-leading term (common tones + stepwise motion rewarded, parallel fifths dented), secondary-dominant proposal class (SURP-gated V/x roll: forced maj+m7, resolution target pulled on the next pass, chain/apply both track it) — all in `Key4.cpp`, 2026-10-20
- [x] **ENCMODE CC pages (F1–F4 banks × M3 groups)** — ENC1–4 = CC17–20 fixed; ENC5–8 = F1–F4 radio banks in two M3 page groups (CC1–16 / CC21–36); F row dim purple (g1) / dim medium sea green (g2), armed white, defaults F1+g1 on entry; M3 (PAGE) = group toggle dim MSG / purple; knob EEPROM layout v3 (36 CCs × 16 ch, older saves ignored). Was "CC pages" in Backlog.
- [x] (move finished backlog/active items here)
