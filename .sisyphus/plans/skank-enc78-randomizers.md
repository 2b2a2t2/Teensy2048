# SKANK page: replace LEN/SPD with TES% / NOT% randomizers

User request: ENC7/ENC8 of the M2 SKANK page drop LEN/SPD (already on SEQMODE3)
and become two per-hit randomizers.

## Semantics (confirmed with user)

- **ENC7 TES%** (`skankPrm idx 6`): per hit, with this probability the
  inversion is a **fully random roll (0..3)** instead of the anchored
  TES/MOT/SHP walk.  A per-step TES lock pins the anchor knob but does not
  block the roll (locks pin knob values, not probability rolls).
- **ENC8 NOT%** (`skankPrm idx 7`): per hit, with this probability **one
  tone of the voicing is replaced by a random in-scale tone** at the same
  register (stab size unchanged; applied after invert+thin, before
  finalize — sort/dedup still runs).
- Lock row re-map: **ENC1–7 lock TES..SUB + TES%**, **ENC8 = shared
  TRANS**; NOT% has no per-step lock.
- Page legends: `TES%` / `NOT%` on ENC7/8; lock row ENC7 label `TES%`.
  Headers: `tes 45%` / `not 45%` (page), `step N tes 45% inherit` (lock).

## Touches

### `Skank.h` / `Skank.cpp`
- `deriveSkankChord(...)` gains `encTesRand, encNotRand` (0..100) — 8 knobs.
- After the walk computes `inv`: `if (encTesRand && random(100) < encTesRand) inv = random(4);`
- New `skankMutate(Chord&)`: `k = random(count)`; pick random in-scale pc
  (0..11 filtered by `keyIsInScale`); replace `notes[k]` with nearest pitch
  carrying that pc (same octave, shift −11..+12 clamped); runs after
  `skankInvert`+`skankThin`, before `skankFinalize`.
- `skankPrmDef(idx)` unchanged (only idx 1 → 50).
- Header comments: ENC7/8 re-described, lock row text (ENC1–7 + TRANS).

### `Keyboard.cpp` / `Keyboard.h`
- `skankPrmM[16][6]` → `[16][8]`, init rows `{0,50,0,0,0,0,0,0}`;
  `skankRes[6]` → `[8]` (reset loop `i<8`); accessors `idx < 6` → `< 8`.
- `stepSkankM[..][..][6]` → `[..][..][7]`; `stepSkankLockAt/Set` bound `idx < 7`;
  `stepAttClear` wipe loop `i<7`; `stepShiftPattern` rotRow width `7`;
  `skankDerive` lock loop `i<7`, pass `p[6]` into derive.
- EEPROM: `SEQ_EE_LEN` 532 → **564**; new `SEQ_EE_LEN_SKANK2 = 532` in the
  cascade (564→532→436→420→404→388→260).
  - Save: `for p<8: b[435 + i*8 + p] = skankPrmOf(i, p)`.
  - Load: `extS = len >= 532` reads the **old 6-knob stride** (`435 + i*6 + p`,
    `p<6`); `extS2 = len >= 564` additionally reads knobs 6/7 from
    `435 + i*8 + p`.  Older 532-byte saves keep their knobs, new two = 0.

### `Events.cpp`
- Page dispatch: `ei < 8` → `skankPrmAdjust`; headers — `ei==2` shp name,
  `ei==4/5` ext/sub `%`, `ei==6/7` `tes N%` / `not N%`, else plain.
  Delete the ENC7 `seqPatLenAdjust`/`refreshAllLeds` and ENC8
  `seqSpeedAdjust` branches.
- Lock dispatch: `ei < 7` = locks (`nm[7] = {tes,mot,shp,thn,ext,sub,tes}`,
  `%` format on `ei==6`); `ei == 7` stays TRANS.
- `Bindings.cpp`: page labels `... SUB TES% NOT%`; lock labels
  `... SUB TES% TRANS` (was `--`).

### `Display.cpp`
- Page value chain: `4+c==6/7` → `"%u%%" skankPrm(6/7)`; drop the
  `seqPatLen()` / `seqSpeedName()` branches.
- Lock row: row-2 labels `{EXT, SUB, TES%, TRANS}` (was `---` in slot 3);
  lock value chain `c==2` now shows the TES% lock (inherit/`NN%`) instead
  of idle; row-1 chain unchanged (knobs 0–5).

### `Modes.cpp` — no change (orange M2 + violet step pulse already keyed to page/overlay).

### Docs
- `README.md` SKANK section: ENC7/8 bullet rewrite (randomizers, no LEN/SPD),
  lock-row bullet (ENC1–7), EEPROM 564-byte note.
- `TODO.md` Done entry: amend the SKANK entry (knob list + EEPROM size).

## Verify
- `arduino-cli compile` (Teensy 4.1, usb=midi) → 0 errors, then **upload**
  to the connected board (last port `/dev/tty.usbmodem*`).
- Hardware smoke: ENC7/8 show `TES%`/`NOT%`; TES% up = inversion jitter;
  NOT% up = voicing color drifts; hold step → ENC7 locks `tes%`,
  ENC8 still TRANS; SEQMODE4+REC save/reload round-trips 8 knobs
  (old 532-byte save loads first 6, new two = 0).
