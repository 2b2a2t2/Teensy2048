# Teensy2048 — Backlog

Single source of truth for bugs, tuning work and feature ideas.
Rules of engagement live in `AGENTS.md` (read it before touching this file).

## 🔧 Active — tuning & bug hunt

Current phase: tuning + bug hunting on the hardware. **No feature work
from the Backlog while this section has open items** (unless overridden).

- [ ] **SEQMODE: radio toggle bug** — selecting F1,F2,F3,F4 should dimm M1, M2 depending on the one selected (LED logic: `Modes.cpp:326-403`)
- [ ] **Review label names in all modes and submodes** — `ctrlLegend()` + encoder labels (`Display.cpp:46`, `encLabel`); wrong/inconsistent legends anywhere

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

- [ ] **Record scale or key change** — capture scale/key changes as they happen (automation over time). Scale tables in `Chords.cpp` (`kScalePat`, `kScaleName`), KeyLock/key state in `Chords.h`/`Key4.cpp`.

### General — display & workflow (`Display.cpp`)

- [ ] **Split main screen: encoder part vs button part** — divide the main screen into two zones (encoder grid, control-button grid). When an encoder is tweaked or a button is used, temporarily enlarge that part and show the encoder's full name + value, or the accessed button function, then fall back. Today: 4x4 encoder grid y16–40 + button rows y54/62 (`Display.cpp:38`), transient feedback via `headerShow()` (`Display.cpp:502`).

## 💡 Parking lot (raw ideas, unsorted)

- (drop half-formed ideas here; promote to Backlog when they firm up)

## ✅ Done (ticked items; prune occasionally)

- [x] **ENCMODE CC pages (F1–F4 banks × M3 groups)** — ENC1–4 = CC17–20 fixed; ENC5–8 = F1–F4 radio banks in two M3 page groups (CC1–16 / CC21–36); F row dim purple (g1) / dim medium sea green (g2), armed white, defaults F1+g1 on entry; M3 (PAGE) = group toggle dim MSG / purple; knob EEPROM layout v3 (36 CCs × 16 ch, older saves ignored). Was "CC pages" in Backlog.
- [x] (move finished backlog/active items here)
