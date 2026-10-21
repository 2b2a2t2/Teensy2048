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
- [ ] **Turing Machine arpeggio** — shift-register + mutation-probability generator available as an option inside existing MONO/POLY engines (ARP, BASS, LEAD, POLY): an 8–16 step loop that slowly mutates; mutation knob controls drift rate. Adds as a GNR8 algo variant or generator page option.
- [ ] **L-System melody generator** — fractal rewrite rules (A→AB, B→A) mapped to pitch/velocity; produces self-similar melodic structures. Available as a generator option inside MONO/POLY engines (GNR8 algo variant or generator page).
- [ ] **Markov order-2 chord transition** — extend Key4 genre scoring to use previous 2 chords (bigram → next) instead of unigram; captures cadential function (ii–V–I as unit). Transition tables per genre; Key4.cpp scoring.

### ENCMODE (encoder/CC mode, `Events.cpp` / `Bindings.cpp` / `Display.cpp`)

- [ ] **Knob motion recording** — record encoder movements over time and play them back as CC automation. Reuse the ENCMODE EEPROM save block (`Events.cpp:1227`) for persistence.

### Chord progression (`Chords.cpp` / `Key4.cpp`)

- [ ] **Per-chord length** — adjust how long each chord in the progression is held (individual step length, not uniform).
- [ ] **Insert chord** — insert a chord at a position within the progression, shifting the rest.

### Scale / Key

- [ ] **Record scale or key change** — capture scale/key changes as they happen (automation over time). Extend with a per-bar key/scale map (16 slots): playback resolves KEY/SCALE per bar (FScale lookup in `Chords.cpp`, send path in `SeqClock.cpp`), enabling generator-driven modulation (pivot chord then re-key). Scale tables in `Chords.cpp` (`kScalePat`, `kScaleName`), KeyLock/key state in `Chords.h`/`Key4.cpp`.

### Composition engine (`Key4.cpp` / `Gen.cpp` / `SeqClock.cpp`)

- [ ] **POLY comping rhythm** — rhythmic stabs for POLY chord tracks derived from Euclid/bjorklund.h instead of one block chord per step (`SeqClock.cpp` POLY playback).
- [ ] **Chord anticipation** — chord changes on the "& of 4" instead of the downbeat (`SeqClock.cpp` advance path).
- [ ] **FOLLOW approach tones** — new per-step modifier (semitone/scale approach into the next chord tone), reusing the PROB/BRW machinery (`SeqClock.cpp` FOLLOW map + per-step attributes in `Keyboard.cpp`).
- [ ] **Motif capture** — snapshot a 4-bar phrase as a motif; transpose/repeat per track with a "repeat every N bars" flag (new `Motif.h/cpp`; today: 16-step loop + one-level undo).
- [ ] **Energy-arc automation** — per-bar envelope automating the KEYMODE4 TENS/MOTION/SURP knobs over the pattern length (build/drop).
- [ ] **Per-genre substitution tables** — replace fixed `subSw[7]` (`Key4.cpp:689`) with per-genre tables like the transition data, gated by SURP + function state; add tritone sub and modal interchange (borrow from parallel minor) as first-class substitutions.

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

- [x] **HARM: bass/arp → chord progression (inverse FOLLOW)** — now a **POLY track TYPE** (SEQ_POLY_HARM, ENC7; like BASS is a MONO type), not a KEYMODE4 gesture. A POLY+HARM track plays its stored chords like STEP, but its **M2 HARM page** (M3) bakes a chord progression inferred from the **FOLLOW feeder's bassline/arp** into its steps (`k4Harm()` in `Key4.cpp` reads the feeder via new `seqFeederChannel()`/`stepChordAtCh()` in `Keyboard.cpp`). Viterbi over the 7 scale degrees: emission = source fit (Mode A **bass** = root>5th>3rd bass-consonance; Mode B **ARP** = any chord tone fit, averaged; non-chord tones fall off with distance), transition = the existing `scorePc` genre grammar; root-position triads baked into the HARM track (feeder only read), rest carries previous chord. **HARM page**: ENC1 = GENRE, ENC2 = SRC (bass/arp), ENC3 = HOLD (force hold on repeated bass); M3 bake / M3-hold undo, `harm N steps` header; teal M2 + hue-cycle M3 LEDs. Removed the old KEYMODE4 F1-hold HARM + EMB ENC3/ENC4. Pairs with FOLLOW/LEAD: bass → HARM → chords → FOLLOW → LEAD. **Follow-isolation fix (2026-10-21)**: the HARM track's input channel is the *generation* source only — it must never be treated as a follow/mirror relationship, else a bass feeder's raw single notes clobber the baked triads. Excluded POLY+HARM from the two mirror paths in `Keyboard.cpp`: **playback** (`follow`, ~L2306 — otherwise a follow-on HARM track replays the bass's single notes) and **`stepSyncFrom`** (~L264 — otherwise a later write to the bass re-derives the HARM steps into single notes). **FOLLOW now gates the SOURCE (user's choice 2026-10-21)**: `harmInferPath` (Key4.cpp) picks the feeder as `seqFollow() ? input channel : self`, so **FOLLOW ON = M3 bake + step activation read the input channel (bass/arp)**; **FOLLOW OFF = HARM ignores the input** (activation falls back to the pending chord; bake re-harmonizes its own notes). Step activation with no held gesture records the per-step inferred chord via new `k4HarmStepChord()` (same Viterbi as the bake, Key4.cpp + Key4.h), only when FOLLOW is on — never the feeder's raw single note. **Dynamic auto re-bake (2026-10-21, user's A → B → HARM chain)**: all content writes funnel through `stepSyncFrom`, which now *marks* — via new `harmFeederChanged()` — every POLY+HARM track feeding from the changed channel (FOLLOW on; never through the track's own channel, so manual step edits and M3 bakes stand), with a **cascade mark** after a mirrored listener copy (A writes → B mirrors → HARM feeding B marked) and a mark from `seqPatLenAdjust` when a feeder's pattern length moves; `stepChordClick()` got its missing `stepSyncFrom` (tap-off is a content change too). The main loop runs the marks via new `harmRebakePoll()` (`Teensy2048.ino`, before `displayUpdate`) → `k4HarmRebake()` (Key4.cpp, same inference, **no snapshot / no K4-context rebuild**, unchanged steps skipped via new `stepChordSame()` so attributes survive and a generate burst costs one Viterbi pass). **Rhythm preservation fix (2026-10-21, user bug report)**: the first version wrote every step `0..L-1` and re-activated disabled ones — `k4HarmBody` now skips `!stepIsActive(st)` in the automatic branch, so a feeder change refreshes **only already-active steps** (disabled steps keep their state and content; M3 stays the full-populate gesture for bootstrapping). Source-config changes (input/FOLLOW/TYPE) stay M3 gestures; erasing the feeder keeps the baked chords (existing no-empty-propagation convention); 2026-10-20
- [x] **LEAD generator** — new MONO TYPE = **LEAD** parallel to BASS: M3 tap generates a **contour melody** (`ARCH`/`DESC`/`WALK` over the in-scale ladder of the LOW..RANGE window, shared FILL% density gate + optional Euclid, `seqLeadProduce()` in `Gen.cpp`), M3 hold undoes; playback re-resolves the stored skeleton onto the feeder chord via **FOLLOW + new MAP modes** `ct` (chord tone, octave follows the contour) and `gd` (guide tone = 3rd/7th only); new **M2 LEAD page** (ENC2 contour / ENC3 LOW / ENC4 RANGE / ENC7 density, radio-exclusive with M1 like ARP/GNR8); contour persisted in the extended (404-byte, backward-tolerant) SEQ EEPROM block — `SeqClock.h/cpp`, `Gen.h/cpp`, `Keyboard.cpp`, `Events.cpp`, `Display.cpp`, `Modes.cpp`; 2026-10-20
- [x] **KEYMODE4 live regeneration** — GENRE/MOOD/TENS/MOTION/CAD/CPX/ERA/SURP turns now re-generate every unlocked step in place (`k4Regenerate()` in `Key4.cpp`), so the progression playing in the sequencer follows the knobs in real time; the first count of a tweak burst claims the undo slot (M3-hold restores the pre-tweak progression); locked steps untouched. EMB knobs (SUB/INV/PAS/COL) left as M3/F1-only — they don't re-rank the proposals; 2026-10-20
- [x] **KEYMODE4 header: drop the focus prefix** — progression now reads `V vi > I IV` (the focused step is already marked on the sequencer pads); `k4Announce()` in `Key4.cpp` + README; 2026-10-20
- [x] **Fix placeholder encoder labels** — `Bindings.cpp` `encLabels[]`: real idle legends for every set (SEQ1/2/4 = OCT/GATE/VEL/PROB with `--` for page-bound knobs, KEY1 = OCT/SCALE/KEY, KEY3 = OCT/SCALE, dead `ENC_SEQ_STEP_HELD` row = `--`); Display overrides still repaint per page/radio, and idle knobs (label `--`/`---`) now show a **blank** value instead of the raw encoder position or `---` (`Display.cpp`); `ctrlLegend()` reviewed — already covers all modes/submodes; 2026-10-20
- [x] **SEQMODE radio toggle bug** — M1 (edit) / M2 (ARP/GNR8 page) / F1–F4 (targets) are now one radio on MONO arp+bass tracks: opening an M2 page drops the edit selection (M1 dims, not white), arming an F target or M1 drops the ARP page (GNR8 already had exclusivity; the M1 white LED was the remaining bass-track bug) — `Events.cpp` M1/M2/F handlers + `Modes.cpp` M1 LED; 2026-10-20
- [x] **Key4 scoring cluster** — phrase/pair cadence terms (ii–V, V–I at bar ends + authentic/plagal landing in the pattern's last bar, CADENCE knob), voice-leading term (common tones + stepwise motion rewarded, parallel fifths dented), secondary-dominant proposal class (SURP-gated V/x roll: forced maj+m7, resolution target pulled on the next pass, chain/apply both track it) — all in `Key4.cpp`, 2026-10-20
- [x] **ENCMODE CC pages (F1–F4 banks × M3 groups)** — ENC1–4 = CC17–20 fixed; ENC5–8 = F1–F4 radio banks in two M3 page groups (CC1–16 / CC21–36); F row dim purple (g1) / dim medium sea green (g2), armed white, defaults F1+g1 on entry; M3 (PAGE) = group toggle dim MSG / purple; knob EEPROM layout v3 (36 CCs × 16 ch, older saves ignored). Was "CC pages" in Backlog.
- [x] (move finished backlog/active items here)
