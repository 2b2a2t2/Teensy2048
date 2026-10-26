# Teensy2048

A 48-pad MIDI controller built on Teensy 4.1 with 4× MPR121 capacitive touch controllers, 42 WS2812 LEDs, an SSD1306 OLED display, and 8 rotary encoders. Designed for live performance and sequencing with multiple operation modes.

## Hardware

### Core
- **Teensy 4.1** (600 MHz ARM Cortex-M7)
- **4× MPR121** I2C capacitive touch controllers (48 touch pads total)
- **42× WS2812 (NeoPixel)** LEDs for pad feedback
- **SSD1306 128×64** OLED display (I2C, hardware @ 400 kHz)
- **8× Rotary Encoders** (quadrature, no detents)

### Pin Configuration

| Function | Pins | Notes |
|----------|------|-------|
| **MPR121 #1** | I2C 0x5A, IRQ pin 5 | 12 pads |
| **MPR121 #2** | I2C 0x5B, IRQ pin 2 | 12 pads |
| **MPR121 #3** | I2C 0x5C, IRQ pin 4 | 12 pads |
| **MPR121 #4** | I2C 0x5D, IRQ pin 3 | 12 pads |
| **WS2812 Data** | Pin 6 | 42 LEDs |
| **OLED (I2C)** | SDA/SCL (pins 18/19) | Shared Wire bus @ 400 kHz |
| **Encoder 1** | 40 (A), 39 (B) | |
| **Encoder 2** | 36 (A), 35 (B) | |
| **Encoder 3** | 34 (A), 33 (B) | |
| **Encoder 4** | 31 (A), 32 (B) | |
| **Encoder 5** | 38 (A), 37 (B) | |
| **Encoder 6** | 26 (A), 25 (B) | |
| **Encoder 7** | 27 (A), 28 (B) | |
| **Encoder 8** | 29 (A), 30 (B) | |

### Pad/LED Mapping (48 pads → 42 LEDs)

The hardware has 48 touch pads but only 42 LEDs. The mapping (`padToLedList` in Buttons.cpp) routes pads to LEDs:

| Pad Range | Function | LED Indices |
|-----------|----------|-------------|
| 0–15 | Step pads (16) | 0–15 |
| 16–24 | Control buttons (9) | 16–24 |
| 25–31 | White keys (7) | 25–31 |
| 32–34 | Mode buttons (3) | 32–34 |
| 35–36 | Transport (2) | 35–36 |
| 37–41 | Black keys (5) | 37–41 |
| 42–47 | (No LEDs) | — |

**Keyboard chromatic order** (groupKeyboard): C, C#, D, D#, E, F, F#, G, G#, A, A#, B  
→ LEDs: 25, 37, 26, 38, 27, 28, 39, 29, 40, 30, 41, 31

## Software Architecture

### Libraries Used
- **FastLED** — WS2812 LED control
- **U8g2lib** — SSD1306 OLED driver
- **Encoder** (PJRC) — Quadrature encoder reading
- **Adafruit_MPR121** — Capacitive touch controllers
- **uClock** — MIDI clock/transport (24 PPQN external, 96 PPQN internal)
- **Teensyduino USB MIDI** — USB-MIDI device (note on/off, CC, clock, start/stop/continue)

### Module Overview

| File | Responsibility |
|------|----------------|
| `Teensy2048.ino` | Setup, main loop, timing diagnostics |
| `Buttons.h/cpp` | Touch handling, debounce, LED color hook, pad groups |
| `TouchChips.h/cpp` | MPR121 init, IRQ-driven polling |
| `Display.h/cpp` | OLED rendering (header + main screen), paged I2C write |
| `Encoders.h/cpp` | 8 encoders, delta queue, position tracking |
| `Modes.h/cpp` | Mode state machine (ENC/SEQ/KEY, variants 1–4), LED animation |
| `Bindings.h/cpp` | Mode→control mapping table, momentary overlays |
| `Events.h/cpp` | Event loop: button + encoder dispatch per binding |
| `SeqClock.h/cpp` | uClock integration, playhead, sync (INT/EXT), tempo, track types |
| `Keyboard.h/cpp` | Keyboard pads → MIDI notes, octave, channel, pending chords |
| `Chords.h/cpp` | KEYMODE2 chord engine: quality, extensions, FScale, latch, KeyLock |
| `Gen.h/cpp` + `bjorklund.h` | GNR8 generative page (SEQMODE, MONO+BASS): FILL/ACC/GLIDE/TIE knobs, option sub-pages (TONES/RANGE/LOW/ALGO), per-step OCT/BRW/PROB modifiers, FOLLOW note mapping (near/root/deg/ivl), Bjorklund/Euclidean placement. LEAD page (SEQMODE, MONO+LEAD): contour melody generator (ARCH/DESC/WALK, live reshape + per-contour cache) with CT/GD follow-mapping knobs, re-resolved onto the feeder chord via FOLLOW + MAP ct/gd |
| `Key4.h/cpp` + `genre_data.h` | KEYMODE4 genre engine: 18-genre transition tables, ranked chord proposals, write-to-focus, EMB embellish menu (passing chords + substitution; data generated into `genre_data.h`, method in `GENRE_DATA.md`) |
| `Skank.h/cpp` | SKANK walking-voicing derive (SEQMODE, POLY track with TYPE = SKANK): `deriveSkankChord()` — anchor inversion (TES) walked by MOT/LIN·PP·RND, thinning, in-scale extension roll, genre-matrix bar-end substitution (via `Key4.cpp` `k4BorrowSample()`); page knobs + per-step locks live in `Keyboard.cpp` |

## Mode System

Three mode families, each with 4 variants (1–4). Variant 3 is **momentary hold**. Variant 4 is **blinking** (double-click to enter). On release of the held button, **ENCMODE3 always exits into ENCMODE** and **SEQMODE3 always exits into SEQMODE** — a hold started inside that family restores that variant, a hold started anywhere else lands on that family's variant 1; only **KEYMODE3** restores the previous base/variant wherever it came from.

**Per-channel mode memory**: each **keyboard channel** remembers the KEY variant last latched on it — switching the keyboard channel (its own variant-3 overlay, or a locked channel-link change) recalls that channel's variant live (KEYMODE2 for one track, KEYMODE4 for another) and the next KEYMODE entry lands on it. Each **encoder channel** likewise remembers its last **F bank + M3 page group** — switching the encoder channel or re-entering ENCMODE recalls that channel's page instead of resetting to F1. Both memories **persist with SEQMODE4+REC** (the 616-byte SEQ EEPROM block; older saves load with all channels at the defaults — KEYMODE1, F1, group 1).

| Mode Button | Family | Variants |
|-------------|--------|----------|
| **ENC** (LED 32) | Encoder/CC mode | 1: shared SEQ controls, 2: ENC2, 3: ENC3 (momentary), 4: ENC4 |
| **SEQ** (LED 33) | Sequencer mode | 1–4: SEQ1–SEQ4 |
| **KEY** (LED 34) | Keyboard mode | 1–4: KEY1–KEY4 |

**KEYMODE2** = chord mode (diatonic auto-chords, quality/extension buttons, latch, KeyLock).  
Chord behavior is **inherited** by SEQMODE/ENCMODE via `keyBehaviorVariant()`.

**KEYMODE4** = genre mode (double-click KEY): an 18-genre progression engine ranks the best next chords on the keyboard, pressing one auditions it and writes it to the focused step (see Workflows).

### Binding Matrix (Bindings.cpp)

Each mode/variant combination selects an `EncSet`, `CtrlSet`, `StepSet`, `KeySet` that defines what the 8 encoders, 9 control buttons, 16 step pads, and 12 keyboard pads do.

**Example (SEQMODE3):**
- ENC1: Octave shift of edit sequence
- ENC2: Pattern length (0–64 steps)
- ENC3: FOLLOW OFF/ON/FREEZE (three-state; FREEZE holds the last incoming chord)
- ENC4: Pattern speed (1/64 … 1 … 4, default 1)
- ENC5: Input channel (1–16)
- ENC6: Track type (DRUM/MONO/POLY)
- ENC7: Mono type (ROOT/BASS/ARP/STRUM/LEAD) — MONO only; Poly type (STEP/STRUM/HAND/HARM/SKANK) — POLY only
- ENC8: Tempo (40–240 BPM)

## Workflows

### 1. Keyboard Playing (Any Mode)
- **12 chromatic pads** (C–B) send MIDI notes on `keyChannel` (default ch2; a KEYMODE3 **KBOARD lock** links it to the SEQ edit channel — see below)
- **ENC1** (KEYMODE): Octave –2..+8 (4 counts = 1 octave, clamped)
- **ENC2** (KEYMODE): Scale select (39 scales, Chromatic = no filter)
- **KEYMODE2**: Chord engine active
  - M1/M2/M3/NEXT = quality (Maj/Min/Dim/Sus) — momentary, beats **KeyLock**'s diatonic triad (root still snapped to the scale); **with KeyLock ON, a held quality button wins over the scale's triad for that press**
  - F1–F4 = extensions (6, m7, M7, 9) — toggle
  - **LEDs**: quality/extension pads idle in the KEY button's dim green (extensions slightly darker: less red/blue); touch lights them white
  - ENC3 = Root, **ENC4 = FScale (Force to scale)**, ENC5–7 = Bass/Spread/Density, ENC8 = Latch
    - **FScale ON**: every chord tone is snapped to the nearest pitch of the current scale before it is sent (ties flatten first), so no out-of-scale note ever reaches MIDI — duplicates collapse to one voice; Chromatic passes everything through; OFF = free voicings. The snap also applies **at playback**: with FScale ON the sequencer forces each stored chord step into the current scale as it is sent (stored notes stay raw), so **changing the scale recolors recorded progressions live**; OFF plays the recording exactly as stored
  - **PREV (hold)** = momentary encoder overlay (Root/Scale/Latch/KeyLock/Perf)

- **KEYMODE4** (double-click KEY): genre-driven chord proposals
  - The engine scores all 12 pitch classes for the slot **before the focus** (transition matrix × MOOD × MOTION × CADENCE × SURP, plus a **voice-leading term** that rewards common tones and stepwise motion and dents parallel fifths/octaves, **phrase/pair cadence terms** — ii–V and V–I at bar ends, the authentic/plagal landing in the pattern's last bar — and a **secondary-dominant class**: SURP-gated rolls turn a proposal into V/x (maj triad + m7, e.g. `II` = V/V) whose resolution target the next scoring pass pulls toward) and lays the whole scored degree set out on the keyboard: **hue-mapped by key as originally designed** (C = blue … B = red), **rank 0 = the next chord from the highest probability = hottest**, brightness stepping down the list so the colors tell how probable each degree is after the last chord; borrowed degrees open with **TENS ≥ 34** and light **orange** (closed ones stay dark); every knob is 0–100 (boot 50), 1 count = 1 unit. Every knob/genre turn re-ranks the proposals **dynamically**, repaints the keyboard live **and re-generates every unlocked step in place** — the progression playing in the sequencer follows the knobs in real time (locked steps are never touched; the first count of a tweak burst claims the undo slot, so M3-hold restores the pre-tweak progression)
  - The **header displays the progression and keeps it there**: `V vi > I IV` = the walk so far · `>` · the top-4 proposals — persistent (it re-ranks live with every knob/scale/root/focus change); transient messages such as the **scale mood** or write results (`I V vi IV +8`) flash over it, then the progression returns. The focused step needs no header marker — the sequencer pads already show it. The **first chord of the walk is the chord played in KEYMODE2** when you enter (its kept notes seed the chain), or the stored step before the focus after a focus move
  - **ENC row**: ENC1 = **GENRE** (18, clamps — the knob does not wrap), ENC2–8 = MOOD/TENS/MOTION/CAD/CPX/ERA/SURP; **PREV (hold)** = overlay with ENC1 Octave / ENC2 Scale (flashes the scale mood over the progression) / ENC3 Key / **ENC4 FScale** (force-to-scale ON/OFF — ON makes playback of recorded steps follow the scale, same switch as KEYMODE2), and **PREV (click)** = focus −1; **NEXT (tap)** = the sticky **EMB menu** (below) — while it is open the row becomes **ENC1 EMB / ENC2 PLACE**, PREV hold still borrows it for Octave/Scale/Key/FScale and hands it back on release
  - **Press a proposed key** = audition the chord (unscored keys are silent) and **chain the progression**: the next proposals follow the chord you just heard, so repeated presses walk a sequence by ear — even in AUDITION with nothing written. The chain re-anchors on the stored steps whenever the focus moves; on release the header shows where the chain goes next. With **WRITE** on (boots in **AUDITION**) it also lands on the **focused step** and the focus auto-advances (**M2** = AUTO, boots off)
  - **Controls**: **NEXT tap** = the **EMB menu** (toggle, below) · **NEXT hold** = focus +1 · **M1** = WRITE ⇄ AUDITION (**dim red** = AUDITION, **bright red** = WRITE) · **M2** = AUTO-ADVANCE · **M3 tap** = apply the generator to every unlocked step, **M3 hold** = undo (one-level, restores channel + attributes) · **F1** = VARIATION (reseed + generate) · **F2** = lock the focused step (apply skips it) · **F3** = VOICE-LEAD (inversions follow the bass line) · **F4 held** = audition only (no write, no advance). After M3/F1/undo the header shows the **written progression** as numerals (`I V vi IV +8` = first four chords + how many follow)
  - **EMB menu (NEXT tap)** — what M3/F1 generate beyond the plain progression (the menu is KEYMODE4-only and closes when you leave the mode; locked steps are never touched):
    - **ENC1 EMB** (wraps): **OFF** = the plain generator as before · **PASS** = every odd step becomes a **passing chord** — a triad on the scale degree **one below the following main chord**, its quality/extension/inversion re-scored through the same genre+knob proposal logic (so it belongs to the grammar and voice-leads); when that degree is the previous main chord itself, the step simply keeps its main chord · **P+S** = PASS plus **substitution**: every main chord rolls once against the **SURP** knob and, if it hits, is swapped through a fixed table — **I→iii, ii→IV, iii→I, IV→ii, V→♭VII, vi→IV, ♭VII→V** (SURP 0 = never, 100 = always)
    - **ENC2 PLACE** (CW = **DIST**, CCW = **ODD**): **ODD** = every odd step passes when valid · **DIST** = only when the bracketing main chords stand **at least two scale degrees apart** — tighter neighbours keep their main chord (a near-touching "passing" chord would only wobble)
    - The header after M3 counts every written step including the passing ones (`… +16` for a full pattern)
  - **Step pads**: press = move the focus (**white** in AUDITION, **red** in WRITE; locked = amber); a **tap** disables the step only while **WRITE** is on (**AUDITION** keeps it — nothing is written there); a **hold** overlays the encoder row onto that step — **ENC1 OCT** (per-step playback octave −2…+2, stored chord untouched), **ENC2 DEG** (shift every tone one scale degree in the key, intervals keep their shape), **ENC3 INV** (cycle the bass up / top down an octave, refuses to land on an existing tone), **ENC4 VOICE** (spread/close the upper voices one octave around the bass). Release hands the row back (or to the open EMB menu); the header echoes each turn (`Step 5 Octave +1`, `Step 5 Degree vi`, `Step 5 Inversion 1st`, `Step 5 Voicing 2 oct`). A hold in **WRITE** still arms the usual assign gesture on top
  - **PLAY** works here too: start/stop the transport without leaving KEYMODE4
  - Nothing persists across a reboot (RAM only, by design); the progression grammar follows the current scale (7-note scales exactly, otherwise major degrees from the root)

  - **KEYMODE3**: the **channel-picker variant** — the step pads are the variant-3 **channel overlay** (yellow): a pad press sets the keyboard output channel. **ENC1 = OCT**, **ENC2 = SCALE** (same as every variant), and **ENC3 = ENCOD** / **ENC4 = KBOARD** are the **channel-link switches** (two-state `free`/`locked`, **no wrap** — CW saturates at `locked`, CCW at `free`, a turn never flips past the end; header echoes each change):
  - **free** (boot default): each mode keeps its own channel exactly as always — the ENCMODE3 encoder CCs, the KEYMODE3 keyboard and the SEQMODE3 edit focus change independently through their own overlays
  - **locked**: that family's channel is **linked to the SEQ edit channel** (the hub). Any channel picked through any of the three variant-3 overlays then moves **all locked families at once** — e.g. with both locked, a pad on the keyboard's overlay sets the keyboard channel, the SEQ edit channel and the encoder channel together; same from the ENCMODE3 or SEQMODE3 overlay. Locked families boot onto the hub channel. The flags persist in the (567-byte) SEQ EEPROM block; older saves boot with both free

#### HAND Strum Keyboard (POLY track, TYPE = HAND)
- With the keyboard channel on a **POLY** sequence whose **TYPE = HAND** (ENC7 in SEQMODE3), the keyboard becomes a **strum zone** (Le Strum style): the track stays **silent** — each played step **buffers its chord** (post-FX / force-to-scale = what *would* have sounded) instead of sounding it, and the **7 white keys are the strings** (C…B, low to high) that strum that buffer. Pluck one string or sweep across; the strings read the **buffered chord first** (the progression under the playhead), then the pending chord (manual build / input), then the engine snapshot. Default voicing = **inversion spread**: the chord's sorted tones land **root position on E F G**, the two lower inversion notes fill **C D** (the 2nd and 3rd tones an octave down), and the upper ones fill **A B** (A = the 4th chord tone when there is one, else the 2nd tone an octave up; B = the 3rd tone an octave up) — e.g. C major strums `52 55 60 64 67 76 79` (E→C in first inversion, its 3rd in the bass). Degenerate chords leave strings without a tone **dark** (a lone root lights only E; no chord yet = silent but lit)
- **Black keys = silent option toggles** (tap flips the bit, header shows `<name> on/off`, nothing is written to the pattern; RAM only, booted with SUS + SUSCOMMON on): **C♯ = GUITAR**, **D♯ = GTR2**, **F♯ = ADDS**, **G♯ = SUS**, **A♯ = SUSCOMMON**
  - **GUITAR**: real 6-string shapes (C/C7/A/Am/A7/D/Dm/D7/E/Em/E7/G, ported from Le Strum) for MAJ/MIN/DOM7 chords — keys C…A carry the shape, **B is the spare string** (dark = the bassless C/A/D shapes have no note there, like a muted guitar string); maj7 / dim / aug chords fall back to the inversion spread. **GTR2** with GUITAR: the shape's notes an octave up fill D…B (C keeps the shape's lowest note); alone, GTR2 raises the inversion spread's top half (F…B) an octave
  - **SUS on**: strummed notes **ring past the step boundary until the buffered chord changes**; **SUS off** = they cut at the step boundary (per-step length). **SUSCOMMON on**: notes common to the old and new voicing **survive** that chord change; off = clean swap (all old notes cut). Stop / mute / erase / TYPE change / keyboard channel change always cut everything
  - **ADDS master + F-row**: M2 (SEQMODE 1/2/4, POLY track with TYPE STRUM/HAND) opens the temporary **EXT row** — **F1 = sus4**, **F2 = add6**, **F3 = add9** (independent toggles, combinable), **F4 = clear** (legend `sus4 · add6 · add9 · off`, header `STRUM ADDS`; F4 lights while anything is armed). The row wears the **STRUM/HAND lavender** (the same hue as the M2 pad that opened it, bright for the armed entries). The colors reshape the string voicing while the ADDS master (F♯) is on; M2 again or leaving the context closes the row
- **White keys still BUILD** (KEYMODE2 quality/extensions, KEYMODE4 proposals, pending-chord feed — silent, chord engine muted); plucks bypass the scale filter and feed neither pending nor the block recorder (the gesture does). The black keys toggle instead of building while the strum owns the keyboard
- **Recording**: with REC on, a pluck appends the note to the gesture with its **real timing** — the first touch = onset 0, every later note = ms elapsed (u16, up to ~65 s, never squeezed). All notes of one gesture (**first touch → all keys up**) anchor to the step where it **started**, even if the playhead moved on; playback schedules the onsets from that origin step across the following steps/bars. A block-written step (assign mode, all timings zero) falls back to an **even ≈ 8–40 ms spread**. The block-record path is suppressed in HAND — only plucks record, so a strum never doubles up as a block chord
- **Playback**: a HAND step **buffers** its chord and stays silent — pick **TYPE = STRUM** (track default or F2 per-step override) when you want the auto-rake. An explicitly **recorded gesture** (stored timings) still **auto-replays** note by note as performed; rests and empty steps leave the buffer — and the ringing strings — untouched. HAND **FOLLOW**: an empty own step still resolves the feeder's current chord (a blank pattern = the live chord at all times); written steps gate the rhythm as usual. Steps with no override follow the **track** TYPE (the live gate is track-level; per-step TYPE shapes playback only)
- **LEDs**: the white keys show the **pink → seagreen gradient** (hue 247 → 107, left to right, dimmed); strings with no chord or a guitar gap dim to almost nothing; the **black keys show their option state** (violet = on, dim dark violet = off). The gradient and the option keys win over the KEYMODE2 chord tint, the KEYMODE4 proposal rainbow and the playback blink while HAND is live (touch still flashes white first)

### 2. Sequencer (SEQMODE)

**Encoder feel**: one detent is **4 counts**. Knobs whose value is a wide range — every **0–100%** dial (GNR8 FILL/ACC/GLIDE/TIE, the LEAD CT blend, the ARP sub-page percentages, GATE, PROB, the eight SKANK knobs, DRUM FILL/MOT and the per-lane FILL/MOT) — go through `encDrain()` in `Encoders.cpp` at **1 count = 1 unit**, the same rate as the ENCMODE **CC knobs**, so **one detent = 4% and a full 0–100 sweep is about one turn**; a spin (the library hands a fast turn over as a bigger delta per poll) is worth up to **4×** that. Small enums (MODE, SPAN, TRACK, GENRE …) stay 1 step per detent, and the CC knobs themselves stay 1 count = 1 value.

**Playhead** advances on MIDI clock (24 PPQN external or internal master at tempo). 16 steps per sequence, 16 sequences (one per MIDI channel 1–16).

#### Step Recording
- **Press unassigned step** → silently records current *pending chord* into that step
- **Refused press** → when there is nothing to record the header says why instead of leaving the pad dark: **`Feeder track empty`** (the track reads its input track on ENC5 and that track has not been fed yet — the usual case on a follower) or **`No notes to record`** (nothing held/pending and no timeline to mirror)
- **Hold step** → assign mode: play keyboard keys to **replace** that step's chord
- **Click active step** → disables it
- **Pending chord sources**: keyboard (when its channel matches), external MIDI on sequence's input channel

#### Live Recording (SEQMODE1/2, KEYMODE1/2/4)
- **REC** arms one record pass over the edit track's pattern from its **next 1st bar** (or the looped bar's head when a bar loop is armed); the pass covers the whole play range and then switches itself off (`rec done`)
- **LED**: dim dark red = idle, **bright red** = armed / recording (touch = white)
- The keyboard channel must match the edit track — otherwise arming is refused (`kbd ch mismatch`); changing the keyboard channel mid-record disarms
- Each gesture writes the step under the playhead: **POLY/MONO replace** (fresh gesture clears step + attributes, held notes build the chord), **DRUM overdubs** (notes are added, never removed, duplicates skipped)
  - **POLY gestures are anchored**: all notes of one gesture (first touch → all keys up) land on the step where it **started** — on a HAND step each pluck carries its real onset (ms since the first touch, u16), so a long strum replays exactly as performed across later steps/bars
- Press **REC again** = cancel (`rec off`); **transport stop** interrupts the pass and re-arms it for the next start
- **PLAY starts in SEQMODE or KEYMODE4** — start the transport there first (or feed an external clock), then switch to KEYMODE to arm and play

#### Bar Loop (SEQMODE3)
- **Press C/D/E/F bar button** (on step pads in SEQMODE3): first press selects that 16-step edit window; **press again on the SELECTED bar toggles its loop**
- **Loop activation**: playhead **teleports immediately** to the looped bar and starts looping it (no waiting to "walk" there)
- **Loop disable**: playhead resumes at the global position it *would have reached* without the loop (shadow position continues advancing silently during the loop)
- Each track has independent loop state and pattern length (polymeter)

#### Sequence Configuration (SEQMODE3)
- **ENC1**: Playback octave shift (±5 octaves from stored notes, clamped to –2..+8 convention)
- **ENC2**: Pattern length of the edit sequence (**0–64** steps, boot default 16; 0 = silent track, per-track RAM only)
  - **Steps past the length go dark and inert** — no record, no toggle, no attribute edit, so a shortened pattern can't hide stale content. An armed **bar loop** still covers its own 16 steps, so those stay lit and editable
- **ENC3**: **FOLLOW** — three-state `OFF · ON · FREEZE` (4 counts = 1 state, **clamped at both ends — no wrap**). **OFF** = the track plays its own stored line. **ON** = its active steps still gate the rhythm, but the notes re-resolve live from the feeding track's current bar (the input channel on ENC5). **FREEZE** = keep the chord **that was sounding when you turned the knob**: `seqFollowModeSet` latches the feeder's current chord/note right then (`seqFollowFreezeCapture`) and **replays it unchanged** even as the feeder walks on — its own steps still gate *when* it plays, only *what* it plays is frozen. Leaving FREEZE (to ON or OFF) drops the latch, so every entry recaptures; if the feeder has nothing sounding at that instant the latch stays empty and the follower captures on its first frozen active step instead (also the boot path, since a restored FREEZE track starts unlatched). FREEZE counts as following for every gate that checks FOLLOW, so HARM/SKANK/arp-style listeners behave identically except that the resolved chord is held. A follower copies its feeder's timeline into a step, so while the **feeder is empty** a step press has nothing to copy and is refused with `Feeder track empty` in the header — feed the input track (or play a chord and press again) and the step lights. Survives SEQMODE4+REC (a 0/1/2 mode per track in the SEQ EEPROM block; older ON/OFF saves still load)
- **ENC4**: Pattern **speed** of the edit sequence — step rate relative to the master clock: **1/64, 1/32, 1/16, 1/8, 1/4, 1/2, 1 (default), 2, 4** (the two slowest rungs are one step per 32 / 64 master ticks — a step can span several bars)
  - Per track (polymeter + polyrhythm): a slow track skips master ticks until its accumulator fills, a fast one plays 2–4 sub-steps inside one tick; the gate time scales with the track's own step length
  - Saved/restored with SEQMODE4+REC. Clock **SYNC** moved to **ENCMODE3+PLAY**
- **ENC5**: Input channel (1–16) — notes arriving here feed pending chord; changing backfills from that track
- **ENC6**: Track type — **DRUM** / **MONO** / **POLY**
  - Ch1 defaults DRUM, ch2–16 default POLY
  - A **DRUM** track lights **M2 indigo** in SEQMODE 1/2/4 — click it for the [DRUM pattern page](#drum-pattern-page-seqmode-124-drum-track). Every page track has its **own M2 hue** (DRUM indigo · ARP pink · BASS blue · LEAD sea green · STRUM/HAND lavender · HARM aqua · COMP pale sky · SKANK spring green), so the pad name needs no reading — see [page pad colors](#encoder-page-pad-colors)
- **ENC7**: MONO transform — **ROOT** (chord root), **BASS** (lowest note), **ARP** (ascending arpeggio), **STRUM** (the whole chord raked inside the step), **LEAD** (the generated melody line: its own steps gate the rhythm, the notes re-resolve onto the feeder chord via FOLLOW + MAP `ct`/`gd`)
  - Selecting **ARP** lights **M2 pink** in SEQMODE 1/2/4 — click it for the [ARP options](#arp-options-seqmode-124-mono-track-with-type--arp)
  - Selecting **BASS** lights **M2 blue** in SEQMODE 1/2/4 — click it for the [GNR8 generator](#gnr8-generative-page-seqmode-124-mono-track-with-type--bass)
  - Selecting **LEAD** lights **M2 sea green** in SEQMODE 1/2/4 — click it for the [LEAD generator](#lead-generative-page-seqmode-124-mono-track-with-type--lead)
  - **STRUM** sends the stored chord's notes ascending, one after another across the step (stagger ≈ 8–40 ms each, scaling with the step), every note-on cutting the one before it and the last held to the gate — a quick strum instead of one block chord. TIE, GLIDE, PROB and the gate behave as on the other types; TYPE is per track and saved with SEQMODE4+REC
- **ENC7 (POLY track)**: playback TYPE of the whole track — **STEP** (block chord per step — optionally rhythmically comped, see the [COMP page](#comp-comping-page-seqmode-124-poly-track-with-type--step)), **STRUM** (auto-strum: every step rakes ascending at a fixed ≈ 8–40 ms stagger scaling with the step), **HAND** (see [HAND keyboard](#hand-strum-keyboard-poly-track-type--hand) — steps buffer their chord silently, the keyboard strums the buffer), **HARM** (see [HARM page](#harm-generative-page-seqmode-124-poly-track-with-type--harm) — the track is a harmonizer: M3 bakes a chord progression inferred from the FOLLOW feeder's bassline; plays like STEP), **SKANK** (see [SKANK page](#skank-walking-voicing-page-seqmode-124-poly-track-with-type--skank) — the track derives each step's voicing live from the source chord: a walking inversion/extension/substitution engine). No wrap; value 4 is reserved and never surfaces (the MONO type byte is shared — a stale LEAD reads as STEP), per track, RAM only — a stale value normalizes to STEP at boot
- **ENC8**: Tempo 40–240 BPM (INT sync only)
- **Step-pad channel selector**: while SEQ is held into SEQMODE3 the 16 steps are the edit-channel selector (dim yellow, selected channel bright yellow). **Any encoder movement dismisses it** — the pads immediately show the normal sequencer again (playhead, recorded steps, pattern length) and stay that way until SEQ is held into SEQMODE3 once more
  - **Choosing a channel always returns to step edit**: the edit radio switches back on with **M1 (STEPS)** selected on the newly selected channel, whatever attribute it had on the previous one

#### Track Behaviors
| Track | Playback | Step Recording |
|-------|----------|----------------|
| **DRUM** | Each step holds drum parts (voices C1–D#2). Playhead sends all active parts on channel. | Press step = toggle selected part (M1 hold = part-select overlay) |
| **MONO** | One note per step: root/lowest/arp of stored chord + sequence octave shift | Records pending chord; MONO also follows **input track's timeline** (last active step at/before position) |
| **POLY** | Full chord per step + sequence octave shift + per-step FX (sus/ext/color) + **PROB gate** + **ACCENT** velocity | Records pending chord; if input≠own, prefers **held gesture** (keys/latched chord/ext notes) over timeline |

**Attribute Support by Track:**
| Attribute | MONO | DRUM | POLY |
|-----------|------|------|------|
| TIE | ✓ | — | — |
| GLIDE | ✓ | — | — |
| ACCENT | ✓ | ✓ (per-voice) | ✓ (per-step, uses track accent velocity) |
| PROB | ✓ | ✓ (per-voice mask) | ✓ (per-step, re-reads track probability live) |

#### Step Edit Radio (SEQMODE 1/2/4)
- **M1 (edit)**: off = pads record steps, on = pads edit attributes on ACTIVE steps (PROB arms the gate; ENC8 sets the **track** probability that gate re-reads live every hit)
  - **MONO**: click cycles OFF → STEPS → OFF; **F1–F4** pick TIE / GLIDE / ACCENT / PROB; ENC5/6 shape gate/velocity — turning the radio **on** closes the GNR8 page if it is open (mutually exclusive with M2 GNR8)
  - **DRUM/POLY**: quick M1 tap toggles edit (on reuses ACCENT/PROB, default ACCENT); **F3 = ACCENT, F4 = PROB** (click selected again = off); M1 **hold** on DRUM still opens the part-select; ENC8 = track probability
- **POLY step FX (F1 = `FX`)**: on a POLY track with the radio on, **F1** switches the target to **FX** (the control legend reads `FX`, the ENC7 slot reads `FX` and shows the current type; F1 again closes the radio). **ENC7** cycles the selected type with wrap: **sus2 · sus4 · 6 · m7 · M7 · 9 · color** — the header names every turn. Tap a step pad to toggle the selected FX on that step; the yellow attribute pulse shows only the steps carrying the **currently selected** type, and **F1 blinks while the track holds any FX**. **sus2/sus4 are mutually exclusive** (arming one clears the other); the extension types can stack with them. FX live only with the pattern (RAM, by design — patterns are never stored)
  - **Playback (POLY only — MONO/DRUM steps are never transformed)**: **sus2** replaces the chord's third with a second, **sus4** with a fourth; **6 / m7 / M7 / 9** add the extension on top of the stored triad; **color** appends a quiet ghost note a **half step below the root** (half velocity, half delay, sent raw — a chromatic color tone that bypasses the scale snap). FX are applied at send time — the stored notes stay untouched
- **POLY step TYPE (F2 = `TYPE`)**: on a POLY track with the radio on, **F2** switches the target to **TYPE** (legend `TYPE`, ENC7 slot reads `TYPE` + the armed value; F2 again closes the radio). **ENC7** cycles the armed value with wrap: **STEP · STRUM · HAND**. Tap a step pad to **stamp** that override onto the step; **tap again = clear** back to inherit-the-track (`… type inherit`). The yellow pulse shows the steps carrying an override and **F2 blinks** while any step of the track has one. The override wins over the track default (ENC7 in SEQMODE3) for that step's playback only
  - **step TYPE playback**: **STEP** = the plain block chord (as always); **STRUM** = auto-strum — that step rakes ascending at ≈ 8–40 ms stagger scaling with the step; **HAND** = the step buffers silently for the strum — an explicitly recorded gesture (stored timings) still replays as performed (see below), regardless of the track default
- **LED**: M1 bright white while no attribute is selected, selected F-pad white (on POLY all four F-pads are radio pads: FX / TYPE / ACC / PROB — DRUM and MONO follow their own subsets), all radio pads carry a dim dark-blue background (F-pads blink green while the playhead reads an attributed step)
- **Accent velocity (per track, boot 101, 1–127)**: an accented step sends this instead of a hard-wired 127 — applies to MONO, POLY and the DRUM step/per-voice accent alike (the other notes keep their own velocity)
  - **Hold F3** in SEQMODE 1/2/4: the ENC6 row switches **VEL → ACC** and ENC6 edits the edit track's accent velocity (header shows `Accent Velocity`); release restores ENC6 to VEL
- **DRUM ENC5 = `MAPPING`** (SEQMODE 1/2/4 on a DRUM track, step-edit radio on or off): the GATE slot means nothing for drums, so it becomes the **kit map** switch — **`CUSTOM`** (the default, see [DRUM Part Sequencing](#5-drum-part-sequencing): C1 bass drum, C#1 hand clap, D1/D#1 hats, E1 snare, F1–G1 congas, G#1–A#1 toms, C2 cow bell, C#2 claves, D2 maracas, D#2 cymbal) or **`GM`** (General MIDI percussion: 36 kick, 37 rim, 38 snare, 39 clap, 40 e.snare, 41/43 floor toms, 42/44/46 hats, 45/47/48/50 toms, 49 crash, 51 ride). Two-state, **no wrap** (CW = `GM`, CCW = `CUSTOM`, saturating like KLOCK/ENCOD); the header echoes `Kit Map CUSTOM` / `Kit Map GM` and the OLED shows `MAP` with the current value. Switching **re-bakes the track's steps onto the new map** (one undo burst), so a preset pattern lands on the right sounds of either kit. Per track, saved with SEQMODE4+REC. With the **M2 DRUM page open ENC5 is that page's KICK knob** — close the page (or use a non-DRUM edit channel) to reach `MAPPING`
- **Probability (per track, boot 100, 0–100%)**: ENC8 edits the edit track's own playback probability. Every step carrying **PROB** re-reads it on each advance, so a turn changes the odds from the very next hit — no re-toggling the attribute. On DRUM the mask still picks *which* parts are gated, only the % is per track
  - Saved/restored with SEQMODE4+REC
  - F1–F4 in SEQMODE are radio keys only — they no longer trigger chord extensions there (DRUM/POLY with the radio off used to leak into the KEY behaviour); the one extra job is POLY's **F1 = FX target** and **F2 = TYPE target** above

#### Shift / Transpose (SEQMODE 1/2/4)
- **PREV (hold) = SHIFT/TRANS encoder row** — on **any** SEQMODE variant (GNR8, ARP, LEAD, HARM, COMP and the plain track view alike; the previous GNR8-only rotate row now lives here):
  - **ENC1 = SHIFT** — rotates the whole pattern **4 counts = 1 step** (CW = content delays one step, wrapping at the pattern length — notes, attributes, FX, strum timings, drum masks **and the per-step OCT/BRW/PROB/TRANS values** all move together). The header shows the rotation position (`Pattern Start N`); a continuous turn is one undo burst (M3 hold restores the pre-rotation pattern). **SHIFT is a real data move** (inherently destructive to the pattern) and works on every track type including DRUM.
  - **ENC2 = TRANS** — the edit track's **semitone transpose** of its playback, **1 count = 1 st**, clamped **−12…+12**, no wrap; header `Transpose +k` / `Transpose −k`. TRANS is **non-destructive**: the stored steps never change, the playback offset is applied after the fold/octave shaping (the same property ENC1's octave shift has). **DRUM tracks ignore it** (the turn does nothing). Generators bake *raw* notes (the offset then moves the whole result), comp stabs / HARM live-follow / FOLLOW-resolved notes pick it up at send time, and it survives SEQMODE4+REC (per-track field of the extended SEQ EEPROM block, backward-tolerant like the rest).
  - **ENC3 = KLOCK** — per-track **scale-lock** for the TRANS offset, a **two-state switch with no wrap** exactly like the KEYMODE3 `ENCOD`/`KBOARD` switches: **CW = `Scale Lock on`**, **CCW = `Scale Lock off`**, so a continuous turn saturates at the end instead of flipping back and forth on every count (the header/value column only repaint when the flag actually changes). When **ON** the transposed playback note is snapped back to the **nearest in-scale pitch** (down first, then up), so the −12…+12 semitone knob stays **diatonic** under the current KEY + SCALE. Non-destructive like TRANS (stored steps untouched; the snap is a playback-time transform), applied *after* the octave + TRANS offset on every playback path (POLY, MONO incl. STRUM, and FOLLOW-resolved notes). Chromatic (scale `Chrom`) is a no-op. **DRUM tracks never lock** (their "notes" are drum-map ids). Turning the scale recolors the transposed line live. Survives SEQMODE4+REC (per-track flag of the extended SEQ EEPROM block).
- **Hold a step pad = per-step TRANS overlay** (non-DRUM tracks): the encoder row swaps to `TRANS` on **ENC1** and the header announces `Step N Transpose +k` — **1 count = 1 st**, same −12…+12 clamp. It is a **playback-only modifier like the step's OCT** (the stored chord stays a clean root-shape for the generators), **never probability-rolled**, moves with **SHIFT** (rotate), resets with generate/erase, and needs no undo (release just closes the overlay; the step keeps its offset until the next step refresh). Assign mode keeps working while the step is held; release closes the overlay (a PREV pressed mid-hold keeps its own row). Per-step values are **RAM only** like every other step field.
- Saved/restored with SEQMODE4+REC: the per-track **TRANS** offset and **Klock** flag are fields of the extended (665-byte) SEQ EEPROM block (now 666 bytes with the speed-ladder revision marker); older (665/649/616/567/564/532/436/420/404/388/260-byte) saves still load — TRANS = 0, Klock = off, and pre-extension speed indices are lifted two rungs so a saved **1/16** still boots as 1/16.

#### ARP Options (SEQMODE 1/2/4, MONO track with TYPE = ARP)
- **M2** becomes an **ARP** key on such a track: click toggles the ARP encoder page (the left slot reads `ARP` while it is open — `ARP ORDER` … `ARP OPTIONS` once a sub-page is armed — and the control legend reads `ARP`)
  - **Auto-off**: changing channel, TRACK away from MONO, TYPE away from ARP, or leaving SEQMODE always closes the page
- **LED**: M2 burns **pink** whenever the edit track is a MONO arp — dim while the page is closed, bright while it is open (touch still lights it white)
- **Encoder page** (the four knobs the page owns; everything else keeps its binding):

| Encoder | Name | Values |
|---------|------|--------|
| ENC1 | OCT | playback octave shift of the sequence (unchanged) |
| ENC2 | **SPAN** | 1–4 octaves, default **1** |
| ENC3 | **SKIP** | ON/OFF, default **OFF** |
| ENC4 | **MODE** | **UP / DOWN / UPDOWN / RANDOM / RANDOM2**, default **UP** |
| ENC5 | GATE | step gate % (unchanged) |
| ENC6 | VEL | default velocity (unchanged) |
| ENC7 | **INST** | **OFF / HI / LOW / 3F1B / 4F2B**, default **OFF** |
| ENC8 | PROB | probability of the edit track, 0–100% (default 100), per track and live |

- **SPAN**: the chord is extended over that many octaves (up to 3 more on top of it, octave 1 = the chord itself), notes folded back into the MIDI range when they run high
- **MODE**: the walk order over the spanned chord
  - **UP** / **DOWN**: bottom→top or top→bottom of the whole span
  - **UPDOWN**: up through every octave and back down without repeating either turning point (period 2×span−2)
  - **RANDOM**: the chord order is shuffled once per octave and stays that way until the chord changes
  - **RANDOM2**: same, but re-shuffled every full pass of the sequence
- **INST**: repeats notes inside the sequence (extends its length)
  - **OFF**: none · **HI**: the highest note goes in front of every other note · **LOW**: the lowest does the same · **3F1B**: 3 steps forward, 1 back · **4F2B**: 4 steps forward, 2 back
- **SKIP**: on a rest (empty step) **or** a probability miss the arp position **advances anyway** instead of pausing, so the sequence keeps walking over dead steps
- Both the **playhead track** and the **live keyboard voice** (KEYMODE MONO+ARP) use these settings; the keyboard channel's track owns the live one
- Saved/restored with SEQMODE4+REC

**F1–F4 = sub-pages** (legend `ORD · MOT · GEN · OPT`): arming one turns the **whole encoder row** into that page's knobs (header `ARP ORDER`, `ARP MOTION`, `ARP GENERATIVE`, `ARP OPTIONS` — the full name of the page, not a code; the armed F burns bright). Slots a page does not own keep the plain binding above, so GATE/VEL/PROB (and OCT on F3, INST on F1/F2/F4) stay reachable from inside every page — and GATE/VEL/PROB stay **live** there, because the page always turns the M1 edit radio off and so carries its own path for those three encoders. Clicking the armed F again goes back to the plain row; the armed page dies with the page (M2, channel, TRACK, TYPE or mode change). While the ARP page owns the F row the SEQMODE step-edit targets and the **F3-hold ACCENT VEL** overlay stay closed — close the page first, the same trade GNR8 makes. **Every knob stops at both ends — no wrap** (an enum that wrapped would drop a shape you just dialled in on one click), and the percentage knobs (WEIGHT, MUTATE, MORPH, RATE, DEPTH, MEMORY, the OPT amounts) move 4% per detent like every other 0–100% dial (see *Encoder feel*).

| Page | ENC1–4 | ENC5–8 |
|------|--------|--------|
| **F1 ORD** | **ORDER** (`MODE` = follow the MODE knob, else UP/DOWN/UPDN/OUTIN/INOUT/CONV/DIV/PLAY) · **STEP** (1/1·2/1·3/1·2/2·3/2·4/2 skip pattern over that walk) · **PIVOT** (ROOT/5TH/TOP/LOW/CTR) · **WEIGHT** (0–100% pull onto the pivot tone) | GATE · VEL · INST · PROB |
| **F2 MOT** | **MUTATE** (0–100% chance a step becomes another chord tone in register) · **INSERT** (`FOLLOW` = the INST knob, else OFF/HI/LOW/3F1B/4F2B/RAND) · **CYCLE** (1/2/4/8/16 bars between RANDOM2 reshuffles) · **MORPH** (0–100% of steps played in the plain walk) | GATE · VEL · INST · PROB |
| **F3 GEN** | OCT · **ENGINE** (OFF/MARKOV/WALK) · **RATE** · **DEPTH** · **MEMORY** | **BIAS** (NONE/HIGH/LOW/ROOT/PIVOT/SPARSE/DENSE) · **SEED** (0–63, turning re-rolls) · PROB |
| **F4 OPT** | the armed engine's own four, labelled per engine: MARKOV = **JUMP/SMOOTH/STAY/REST**, WALK = **STEP/REFLECT/DRIFT/REST** | **TARGET** (ORDER/PITCH/BOTH) · **CLOCK** (STEP/PASS/BAR/PHRASE) |

- **ORDER**: `MODE` keeps the MODE knob's walk; **OUTIN** starts at the top and bottom and walks inward, **INOUT** the reverse, **CONV**/**DIV** converge on / diverge from **PIVOT** by distance (WEIGHT biases the pick toward the pivot tone), **PLAY** keeps the chord in the order the keys arrived instead of sorted
- **ENGINE**: the walk is regenerated from (chord, params, SEED, cycle) on every call, so the playhead and the live voice always agree and nothing has to be stored or undone. **TARGET** chooses what it rewrites — the walk order, the note's octave, or both; **CLOCK** picks the reroll point (per step / pass / bar / phrase) and **RATE** stretches it, **DEPTH** bounds the leap, **MEMORY** holds position (100 = frozen), **SEED** re-rolls. **MARKOV** favours small steps (SMOOTH damps them, STAY repeats, JUMP widens the leap range), **WALK** moves a fixed step (REFLECT turns at the edges, DRIFT steers); **BIAS** pushes the result high/low, toward the chord root or PIVOT, or sparser/denser
- All 20 F-page values are per track and live (a turn is audible on the next step); they persist in the extended SEQ EEPROM block, and an older save loads with the plain-page defaults (ORDER `MODE`, INSERT `FOLLOW`, ENGINE `OFF`, RATE 50, DEPTH 30, MEMORY 50, TARGET `ORDER`, CLOCK `PASS`) so the track sounds exactly as before until you turn something

#### GNR8 Generative Page (SEQMODE 1/2/4, MONO track with TYPE = BASS)
- **M2** becomes a **GNR8** key on such a track: click toggles the GNR8 encoder page (the left slot reads `GENERATOR` while it is open, the control legend reads `GNR8`) — **opening it turns the M1 edit radio OFF** (M1 and the GNR8 page are mutually exclusive: pressing M1 to start edit closes the page instead)
  - **Auto-off**: changing channel, TRACK away from MONO, TYPE away from BASS, or leaving SEQMODE always closes the page (ROOT / ARP / STRUM and every other track type never take it)
- **LEDs**: M2 burns **blue** whenever the edit track is MONO+BASS — dim while the page is closed, breathing while it is open (touch still lights it white). M3 runs a **slow hue cycle** on that track: it is the generator button
- **M3 tap = generate**, **M3 hold = undo** (one-level: restores the pattern as it was before the last generate — or before a rotation burst — plus its attributes, mirroring KEYMODE4's apply/undo pair). The header reports `Generator N Steps`
- **Encoder page** (the four knobs the page owns; everything else keeps its binding):

| Encoder | Name | Values |
|---------|------|--------|
| ENC1 | OCT | playback octave shift of the sequence (unchanged) |
| ENC2 | **FILL** | step density, 0–100%, default **50** |
| ENC3 | **ACC** | accent probability per note, 0–100%, default **30** |
| ENC4 | **GLIDE** | glide probability per note, 0–100%, default **20** |
| ENC5 | GATE | step gate % (unchanged) |
| ENC6 | VEL | default velocity (unchanged) |
| ENC7 | **TIE** | chance a note holds over the **following rest**, 0–100%, default **10** |
| ENC8 | PROB | probability of the edit track (unchanged) |

While an **F radio is armed** the whole row belongs to its sub-page instead (**F1**: ENC1–4 = TONES · RNG · LOW · ALGO; **F2**: ENC2 = the tapped step's OCT + ENC3 = MAP; **F3**: ENC3 = the step's BRW; **F4**: ENC4 = the step's PROB — every other encoder reads `---`); releasing the radio restores the bindings above

- **F row while the page is open = option radios** (legend `OPTS · OCT · BRW · PROB`): a click **arms** an option — the **whole encoder row becomes its sub-page**. **F1 = the full options page** (ENC1 TONES, ENC2 RANGE, ENC3 LOW, ENC4 ALGO, ENC5–8 `---`). **F2/F3/F4 = the per-step modifier pages**: **tap a step pad first** (armed pages only — the picked pad burns **full violet**, the header reads `Step 5 Octave +1`), then dial it live: **ENC2 = OCT** (F2, −2…+2 octaves), **ENC3 = BRW** (F3, `off` · `1`–`8` = borrow that tone of the incoming chord), **ENC4 = PROB** (F4, 0–100% = chance the step's OCT/BRW modifier actually applies on a pass, default **100**). **F2 also owns ENC3 = MAP** (needs no step): how FOLLOW re-resolves the generated notes — `near` · `root` · `deg` · `ivl` (see the FOLLOW mapping bullet below). With nothing picked, a dial flashes `Pick a Step` (the plain page and F1/none-armed taps never record — attribute edits and the chain run with the page **closed**). Dials use **4 counts = 1 detent = 1 value, clamped at both ends — no wrap**. Click the **armed** F again to release (the plain page comes back, header `Generator`), click a **different** F to switch directly (the picked step stays). The armed pad burns **bright in the track hue**, the other three stay dim; arming announces `Generator Options` (F1) or `Pick a Step` (the mod pages wait for one), and every dial change re-announces the value (`Low C1`, `Kit Map deg`, `Step 5 Borrow 3`, `Fill 40%`, …). The radio **and the picked step die with the page** — closing it (M2, channel, TRACK, TYPE or mode change) resets to the plain row. **PREV-hold rotate keeps priority** while a radio is armed (the sub-page returns on release)

| Key | Option | Values (default first) |
|-----|--------|------------------------|
| **F1** | **OPTS** (full page) | ENC1 **TONES**: how many scale degrees the notes may use — **all · 5 · 4 · 3** (kept spread evenly over the KEY + SCALE) · ENC2 **RANGE**: octaves above LOW — **1 · 2 · 3 · 4 · 5** (steers the next generate *and* folds the existing line live — reversible, stored notes untouched; see FOLLOW mapping) · ENC3 **LOW**: bottom of the range — **C0 · C1 · C2** (MIDI 24 / 36 / 48) · ENC4 **ALGO**: **CLASSIC** (random per step) · **EUCLID** (Bjorklund, FILL rounded to a pulse count) |
| **F2** | **STEP OCT + MAP** | tap a step, then ENC2 = its octave transposition **−2 … 0 … +2** (default 0); ENC3 = **MAP** (global, persisted): **near** · **root** · **deg** · **ivl** · **ct** · **gd** — how FOLLOW maps generated notes onto the chord |
| **F3** | **Borrow** | tap a step, then ENC3 = borrowed tone of the incoming chord: **off** · **1** … **8** (root, 2nd tone, … of the chord the feeder plays) |
| **F4** | **Mod prob** | tap a step, then ENC4 = chance the step's OCT+BRW modifier applies on a pass: 0–100%, default **100** |

- **Generate (M3 tap)**: rolls every step of the edit sequence's **own pattern length** — empty steps are cleared, filled ones get one note snapped into the **current KEY + SCALE** (nearest in-scale pitch, so the generator follows key changes), inside LOW..LOW+RANGE−1, with **ACC**/**GLIDE** rolled per note. **TIE** follows Aciduino's rule: it rolls on the **rests**, holding the previous note over itself (consecutive rolls chain the hold), never on the note steps
  - Takes the undo snapshot first (M3 hold restores), seeds `random()` once per boot, RAM only — patterns are still never stored in EEPROM
- **FOLLOW mapping (what F2/F3/F4 do at playback)**: with **FOLLOW ON** (SEQMODE3 ENC3, a MONO/POLY listener reading another track's timeline) a BASS track no longer collapses onto the feeder chord's lowest note — each step's **own generated pitch** is re-resolved against the **incoming chord** (every tone, heard through the listener's octave shift) and the **note window**, then the step's **OCT** and **BRW** modifiers apply when their **PROB** roll hits (defaults = no effect: OCT 0, BRW off, PROB 100). The mode lives on **F2/ENC3** (global, saved in the SEQ config):
  - **near** (default): the chord tone **nearest the generated pitch inside the window** — keeps the line's contour and follows the chord under it
  - **root**: always the chord's **lowest tone**, octave-shaped to the generated pitch (the old one-note behavior, now octave-tracking)
  - **deg**: the generated pitch's **scale degree** (from KEY + SCALE; off-scale pitches snap to the nearest degree) indexes the chord tones ascending — degree N = chord tone N+1, wrapping over the chord
  - **ivl**: the chord's lowest tone **plus the pitch's semitone offset from the KEY root** — may deliberately leave the chord (chromatic color)
  - **ct**: chord **TONE** whose **octave follows the generated contour note** — every chord tone is shaped to sit nearest the pitch, the closest one wins, so the melodic register rides the contour across the chord (the LEAD-friendly mode)
  - **gd**: nearest **guide tone** — only the chord's **3rd and 7th** (its 2nd and 4th tone) are candidates; a chord without them falls back to **near**. Strips the line down to the two notes that carry the harmony's color
  - **BRW** (any mode): replaces the mapped tone with the borrowed chord tone, octave-shaped back into the window. Every mapping result stays inside the window; **OCT** applies afterward and may leave it (clamped to 0..127). **FOLLOW OFF** ignores MAP/BRW (the line plays as generated, plus OCT); ROOT/ARP take OCT but not MAP/BRW (STRUM's rake has neither). The generator itself is unchanged — with a static chord, FOLLOW ON sounds like OFF, and the line only moves when the chord does
  - **RANGE reshapes the line live (reversibly)**: every sounding pitch of the BASS track is **folded by whole octaves into the current LOW..RANGE window** at playback — RANGE 2→1 **lowers the upper-octave notes** into the new octave immediately (the line really becomes 1 octave wide), and RANGE 1→2 **raises the original pitches back**, exactly. The fold is a pure playback transform: **stored steps are never rewritten**, so the round-trip is lossless, and the per-step **OCT** shifts ride *on top of* the folded base (an OCT step can still sound outside the window by design). The fold runs **before** SEQMODE **ENC1**'s octave shift, so ENC1 keeps transposing the whole folded line — a shift applied after the fold would just be folded back into the window
- **Rotate (PREV held)**: while the page is open, hold **PREV** to swap the encoder row to the shared **SHIFT/TRANS row** (see [Shift / Transpose](#shift--transpose-seqmode-124)): **ENC1 = SHIFT** rotates the whole pattern **4 counts = 1 step** (the header shows the rotation position `Pattern Start N`; a continuous turn is one undo burst — M3 hold restores the pre-rotation pattern), **ENC2 = TRANS** transposes the track's playback in semitones (the other six read `---`)
- **TIE span fill (hold-arm gesture)**: plain taps **always just toggle** the selected attribute — on a BASS track too, TIE / GLIDE / ACCENT / PROB behave exactly as everywhere else (GLIDE/ACCENT/PROB are per-step only and never span). **Attribute taps and chains run with the GNR8 page closed**: opening it turns the edit radio off, and with the page open the step pads belong to the F2/F3/F4 dial selection (F1 or no radio armed = inert taps) — so close the page first and make sure **M1 + TIE** are armed (or re-arm them after closing it). **Hold a step** to arm the span start: the touch-down toggle of that same press is reverted first, so holding never changes the flag, and the header announces `Chain From Step N`. The **next tap on another step** stamps TIE across every step from start up to (not including) the end, wrapping inside the pattern length — `Tie N Steps` in the header; the span rings the start note over its rests. Tap the armed start itself = cancel (`Chain Cleared`); a forgotten arm **expires after 5 seconds**. Changing target / mode / channel / TRACK / TYPE disarms
- Saved/restored with SEQMODE4+REC (the eight GNR8 parameters **and the F2 MAP mode** live in the SEQ EEPROM block; a config saved by an older build still loads and keeps the defaults — including MAP = `near`). The per-step **OCT / BRW / PROB** values are **RAM only** like every other step field: they live and die with the step (generate and erase reset them, rotate moves them, M3 undo restores them)

#### LEAD Generative Page (SEQMODE 1/2/4, MONO track with TYPE = LEAD)
- **M2** becomes a **LEAD** key on such a track: click toggles the LEAD encoder page (the click announces `Lead`, the control legend reads `LEAD`) — **opening it turns the M1 edit radio OFF** (M1 and the LEAD page are mutually exclusive, exactly like GNR8)
  - **Auto-off**: changing channel, TRACK away from MONO, TYPE away from LEAD, or leaving SEQMODE always closes the page (ROOT / BASS / ARP / STRUM and every other track type never take it)
- **LEDs**: M2 burns **sea green** whenever the edit track is MONO+LEAD — dim while the page is closed, breathing while it is open (touch still lights it white). M3 runs the **same slow hue cycle** as the BASS generator: it is the generator button (sea green on M2 tells the two generator tracks apart)
- **M3 tap = generate the melody**, **M3 hold = undo** (one-level step snapshot, exactly like GNR8). The header reports `Lead N Steps`
- **Encoder page** (the page owns ENC2–7; ENC1 octave and ENC8 prob keep their bindings — GATE/VEL live in the M1 edit menu):

| Encoder | Name | Values |
|---------|------|--------|
| ENC1 | OCT | playback octave shift of the sequence (unchanged) |
| ENC2 | **CONTOUR** | melodic shape — **ARCH** (rise to a mid-pattern peak, fall back) · **DESC** (step down across the pattern, wraps) · **WALK** (random ±1/±2 scale-step walk, reflects at the ends), default **ARCH**. **Live**: turning it reshapes the stored sequence immediately — no GEN press needed (see the live-flip bullet below) |
| ENC3 | **LOW** | bottom of the note range — **C0 · C1 · C2** (MIDI 24 / 36 / 48), shared with GNR8 |
| ENC4 | **RANGE** | octaves above LOW — **1 · 2 · 3 · 4 · 5**, shared with GNR8 |
| ENC5 | **CT** | chord-tone vs scale blend for FOLLOW + MAP **ct** — per-note chance to keep the skeleton's in-scale pitch instead of snapping to the feeder chord, 0–100%, default **0** (pure ct; see the CT/GD bullet) |
| ENC6 | **GD** | guide-tone set depth for FOLLOW + MAP **gd** — **3/7** · **+9** · **+13** · **EXT**, default **3/7** (see the CT/GD bullet) |
| ENC7 | **DENS** | step density (the shared **FILL** slot), 0–100%, default **50** |
| ENC8 | PROB | probability of the edit track (unchanged) |

- **Generate (M3 tap)**: rolls every step of the edit sequence's **own pattern length** — the rhythm is the shared **DENSITY** gate (CLASSIC random per step, or **EUCLID** via the shared ALGO slot), the pitch **walks the contour** over the in-scale ladder of the LOW..RANGE window (ARCH/DESC/WALK above), every note snapped into the **current KEY + SCALE**. Notes are **stored** as a skeleton — with FOLLOW OFF the LEAD track just plays that stored contour
- **CONTOUR is live and non-destructive (A/B/C per contour)**: turning ENC2 flips the stored sequence between contour versions **in place** — no M3 press. Each track keeps a **per-contour cache** (session RAM): the pattern being left is saved under its contour, and the target contour's **last version is restored**. The **first visit** to a contour **deterministically reshapes the existing notes** onto it (same pitch material, reordered — ARCH = mountain fold, DESC = descending, WALK = a fixed zigzag; no randomness; an empty pattern falls back to a fresh generation). So ARCH → DESC → ARCH returns the exact original ARCH melody, and each contour stays revisitable; M3 GEN still fresh-rolls with the current contour. Undo (M3 hold) returns to the pre-flip pattern (one snapshot per flip burst, like the rotate knob)
- **FOLLOW + MAP (what LEAD is for)**: with **FOLLOW ON** (SEQMODE3 ENC3, the LEAD listener pointing at a chord track) each stored skeleton note is re-resolved onto the **feeder's chord** via the **MAP mode** — the LEAD-friendly modes are **ct** (chord tone, octave follows the contour) and **gd** (guide tone = 3rd/7th, extended by the GD knob). Turn MAP on the GNR8 F2 page (or a LEAD track can share it — MAP is global) and the stored melody re-harmonizes live under the changing progression; the stored steps are never rewritten. Two LEAD-page knobs tune how those modes resolve (bite only when their MAP mode is active):
  - **CT (ENC5) — chord-tone vs scale blend**: per-note chance to keep the skeleton's own in-scale pitch (its scale-degree identity) instead of snapping to the chord tone. 0% = pure ct (flattens the line to the feeder's tones); 100% = pure skeleton (FOLLOW-OFF pitch, rhythm still follows). Mid values interleave scale color with the chord so the line stops locking to the triad.
  - **GD (ENC6) — guide-tone set depth**: expands the guide pool. **3/7** = 3rd + 7th (a plain triad feeder has no 7th, so this collapses to the single 3rd); **+9** / **+13** add scale-snapped 9th/13th extensions derived from the chord root; **EXT** also adds the 5th. More notes available → gd stops collapsing to 1–2.
- **RANGE / LOW reshape the line live (reversibly)**: exactly like BASS, every sounding pitch is **folded by whole octaves into the current LOW..RANGE window** at playback (stored steps untouched, round-trip lossless), then ENC1's octave shift applies
- Saved/restored with SEQMODE4+REC: the **contour**, the **CT blend** and the **GD depth** are fields of the extended **(649-byte)** SEQ EEPROM block (cascade 649→616→567→…); a config saved by an older build still loads and keeps the LEAD defaults (contour = ARCH, CT = 0%, GD = 3/7). LOW/RANGE/DENSITY ride the shared GNR8 slots. Patterns themselves are still RAM only

#### COMP Comping Page (SEQMODE 1/2/4, POLY track with TYPE = STEP)
- **COMP = rhythmic chord stabs**: on a POLY track with **TYPE = STEP**, a per-track **HITS** setting (0–8) re-strikes each step's chord several times inside the step on the **Euclid on-positions of 8 slots** (the same `Bjorklund` primitive the GNR8/HARM FILL uses): **0 or 1 = one block chord per step** (the behavior as before), **2–8 = the chord retriggers** on the Euclid pattern (`3` ≈ `x..x..x.`, `5` ≈ `x.x.x.x.`). Each stab **cuts the previous one** cleanly (no pile-up) and the last stab rings to the step boundary, like a block chord. Stab times scale with the **step duration** (a slower pattern comps slower) and follow everything the step already does: FOLLOW-resolved chords, accent velocity, FScale, probability (a step that misses doesn't comp), and mute (the stabs keep firing internally, only the USB output is gated). A rest, a pattern-length change or transport stop drops an un-sent stab tail (same convention as the strum queue). **STRUM/HAND/HARM steps, MONO and DRUM tracks are untouched** — STRUM/HAND already have their own gesture rhythm, HARM keeps its block triads (the FILL gate owns its density)
- **M2 opens the COMP encoder page** on such a track (the click announces `Comping`, M2 breathes pale sky blue — the conditional COMP pad, lit only while this page is open): **ENC1 = HITS** (0–8, 4 counts = 1, clamped, no wrap; header `Comping Hits N` / `Comping off`; **live** — a turn is audible on the next step). **ENC2–4 = `---`** (reserved: pattern rotation, per-stab gate). Opening the page drops the M1 edit radio (one radio, like ARP/GNR8/LEAD/HARM), and a channel / TRACK / TYPE / mode change closes it; M2 again closes the page
- Saved/restored with SEQMODE4+REC: **HITS** is a per-track field in the extended (436-byte) SEQ EEPROM block; a config saved by an older (420/404/388/260-byte) build still loads and keeps the default (HITS = off). Patterns themselves are still RAM only

#### HARM Generative Page (SEQMODE 1/2/4, POLY track with TYPE = HARM)
- **HARM = inverse FOLLOW**: a POLY track whose **TYPE = HARM** (ENC7 in SEQMODE3) is a **harmonizer**. It plays its stored chords like STEP, but **M3** bakes a whole **chord progression inferred from another track's bassline** into its steps — the missing link in **bass → HARM → chords → FOLLOW → LEAD**. Point the HARM track's **input channel** (SEQMODE ENC5) at a **MONO bass (or arp) track**; M3 reads that feeder's stored notes and writes root-position triads into the HARM track's own steps (the feeder is only read, never touched)
- **M2 opens the HARM encoder page** (aqua, breathing while open — like the blue BASS / sea-green LEAD pages): **ENC1 = GENRE** (the harmonic grammar the inference follows — the same 18-genre transition tables that drive KEYMODE4; the header names the picked genre, e.g. `Genre House`), **ENC2 = FILL** (the HARM track's own FILL% — the same per-track roll gate the generators use, in the same slot the GNR8 page gives its fill knob: **CLASSIC** rolls each step independently, **EUCLID** (the track's ALGO on the GNR8 F1 page) spreads the pulses evenly — the bake lands chords on the rolled steps only and the rest stay **rests**, so a baked pattern keeps rests like a bassline instead of filling every step), **ENC3 = SRC** (`BASS` = each feeder step's **lowest note** is a bass line, weighted **root > fifth > third**; `ARP` = every feeder note is an arp tone, any chord tone fits equally, averaged over the step), **ENC4 = HOLD** (bass mode: force a **repeated bass note** to keep the same chord instead of re-voicing, independent of the MOTION knob). ENC5–8 keep their normal bindings; **M3 = bake** (rolls the rhythm from FILL — every press re-rolls which steps land, FILL 100 = fill everything), **M3 hold = undo** (one-level step snapshot, mirroring GNR8/LEAD). The header reports `Harmonizer N Steps`. Opening the page drops the M1 edit radio (one radio), and a channel / TRACK / TYPE / mode change closes it
- **How the inference works**: a **Viterbi** pass over the 7 scale degrees where the **emission = the source fit** (bass-consonance or arp chord-tone fit; a non-chord tone falls off with the distance to the nearest chord tone, so a passing note still resolves to the nearest-fitting chord) and the **transition = the KEYMODE4 genre term** (genre × MOOD × MOTION × CADENCE × SURP + voice-leading + cadence), so the feeder drives the harmony while the genre grammar smooths the path. Each step becomes a **plain root-position triad**; a **rest** in the feeder carries the previous chord, so the harmony holds through a gap. The knobs keep their KEYMODE4 values (shared grammar — setting GENRE here also sets KEYMODE4's)
- **Dynamic re-bake (the harmony follows a changing feeder)**: whenever the feeder's **stored content changes** — a step edit or tap-off, a record, a GNR8 produce, a KEYMODE4 knob re-generate, a follower sync, or the feeder's **pattern length** — every HARM track pointing at that channel with **FOLLOW ON** re-bakes itself **before the next draw**, with no M3 press (in a chain: A changes → B mirrors A → the HARM track feeding from B re-derives). It is the same inference as M3, but **deferred** — a whole generate burst costs one Viterbi pass — it claims **no undo snapshot** (M3-hold still lands on your last manual gesture), it **keeps the rhythmic signature** (only already-active steps are refreshed — disabled steps stay disabled, their content untouched; the automatic re-bake never re-rolls FILL — M3's roll remains the gesture that (re)populates the pattern), and steps that already hold the inferred chord are **left untouched**, so step attributes survive. Conventions match any listener: erasing the feeder leaves the baked chords alone (M3 re-derives manually), and changing the HARM track's own source setup (input channel / FOLLOW / TYPE) takes effect at the next M3 bake or the next feeder change
- **Live follow (FOLLOW ON)**: playback re-derives each active step's chord from the feeder's **actually-sounding note** — post-probability, post-fold/arp-walk (`k4HarmLiveChord()` in `Key4.cpp`: the played note's nearest scale degree becomes the same plain root-position triad the bake writes, out-of-scale notes snap like FScale) — so per-step probability rolls and arp walks **re-harmonize every pass** instead of replaying the baked chords. A feeder silent for one pass (prob miss / rest) **carries the previous chord** (the rest-carries convention); a cold start falls back to the baked step chord. **Mute is a USB-output gate only** — a muted track keeps *playing* internally (follow chains advance, the arp walks, `seqNotes` fills; only the wire traffic is suppressed at the send), so a muted feeder — even one that is itself a follower — keeps feeding the live follow exactly as it did before the mute, and unmute resumes its output on the next step. Muting still cuts whatever was already ringing (no stuck notes). **FOLLOW OFF plays the stored steps unchanged**, the pads always show the baked pattern, and the step's own gate/prob/vel/FScale/FX still apply — only the note content is live (the LEAD stored-skeleton / live-resolve convention; the playhead's note-on section runs in two passes so followers always read the feeder's fresh note)
- **LEDs**: **M2 = aqua** (breathing while the HARM page is open, dim aqua idle), **M3 = slow hue cycle** (the generate button, same as the BASS/LEAD generators). Pairs with FOLLOW/LEAD: **bass → HARM → chords → FOLLOW → LEAD**

#### SKANK Walking-Voicing Page (SEQMODE 1/2/4, POLY track with TYPE = SKANK)
- **SKANK = send-time voicing walk**: a POLY+SKANK track gates its rhythm from its stored steps like STEP, but the notes played are **derived live on every active step** (`deriveSkankChord()` in `Skank.cpp`) from the **source chord** — the **FOLLOW feeder's chord** (its actually-sounding notes when FOLLOW points at another track, post-probability like the HARM live path; its stored step when the feeder is quiet) or the track's **own stored step** otherwise — and the derived voicing runs through the normal playback pipeline (step FX, FScale, octave/TRANS, gate, COMP, mute). Stored steps are **never rewritten** by the walk (the same send-time convention as LEAD's live resolve; the playhead's two-pass note-on section means the walk always reads the feeder's fresh notes)
- **M2 opens the SKANK encoder page** (the click announces `Skank`, M2 breathes spring green — not the COMP page's pale sky blue): **ENC1 = TES** (tessitura — the walk's anchor inversion, 0–100; 0/25/50/75 = 1st/2nd/3rd/root position of the walk, 100 wraps home), **ENC2 = MOT** (motion — how far each chord slot walks away from the anchor, 0–100 with **50 = parked** at TES; below/above 50 walk in opposite directions), **ENC3 = SHP** (shape of the walk: `LIN` rises through the bar, `PP` ping-pongs 0·1·2·3·2·1 across the four chord slots of the bar, `RND` rolls a fresh inversion per hit), **ENC4 = THN** (thinning — drop the lowest tones progressively: 0 = full voicing, 50 = drop the lowest, 100 = keep the top two only), **ENC5 = EXT** (extension % — per hit, roll an in-scale color tone above the voicing: 6th / maj7 / 9th, snapped by the FScale key rule), **ENC6 = SUB** (substitution % — at the **last chord of every bar**, roll a passing chord sampled from the current KEYMODE4 **genre's transition matrix** (`k4BorrowSample()` in `Key4.cpp`, the same 18-genre tables): the voicing becomes that degree's root-position diatonic triad at the sounding bass), **ENC7 = TES%** (tessitura randomizer — per hit, with this probability the inversion is a **fully random roll (0–3)** instead of the anchored TES/MOT/SHP walk), **ENC8 = NOT%** (note randomizer — per hit, with this probability **one tone of the voicing mutates** to a random in-scale tone at the same register, stab size unchanged). All eight knobs are live (4 counts = 1 unit, clamped 0–100, no wrap; header names every turn). Opening the page drops the M1 edit radio (one radio, like ARP/GNR8/LEAD/HARM/COMP), and a channel / TRACK / TYPE / mode change closes it; M2 again closes the page
- **Hold a step** (non-DRUM, POLY+SKANK) arms the **per-step LOCK row** instead of the plain TRANS row (header `Step N Locks`): **ENC1–ENC7 override that step's TES/MOT/SHP/THN/EXT/SUB/TES%** — the OLED reads `inherit` while the page knob applies; turning from `inherit` grabs the track's current value and nudges it, turning below 0 clears the lock back to `inherit` (0 is a legal locked value and stays distinct), **ENC8 = that step's TRANS** (shared with the plain held-step row). NOT% stays page-level (no lock slot). A **TES lock parks MOT at 50** on that step (the lock sets the inversion alone; it pins the anchor knob, not the TES% roll). Locks are step content — they travel with a pattern **rotate** and die with the step's attributes (`stepAttClear`) — and stay **RAM only**, like every step field
- **Comp rides the walk**: HITS stabs re-strike the **derived** voicing (`compOn` includes SKANK), so comping and the walk share one note source; a step's TYPE override (STRUM/HAND) also rides the derived chord
- **Feeds followers live**: any MONO/POLY follower (arp, bass, lead, strum, STEP/STRUM/HAND) with **FOLLOW ON** pointing its input at a SKANK track plays the feeder's **sounding (walked)** notes — the walk exists only at the feeder's send time, so stored-step follow would be blind to it. The feeder's last sounding notes are **carried per listener** through the gate-release gaps (the same hold convention as HARM's live follow; cold start = the stored step), and the playhead runs the live followers in a **third pass after the skank tracks**, so the arp reads this tick's walk regardless of channel order (other feeder types keep the stored-step convention; HARM keeps its own bass-line inference)
- **Saved/restored with SEQMODE4+REC**: the eight page knobs of all 16 tracks persist in the extended (**567-byte**) SEQ EEPROM block — older (564/532/436/420/404/388/260-byte) saves load with their original six knobs and the randomizers at their defaults (MOT = 50, the rest 0). Stored patterns stay RAM only
- **LEDs**: **M2 = spring green** (dim while the page is closed, breathing while it is open; touch still lights it white); the held step breathes **violet** while its lock row is up (playhead colors still win)

#### DRUM Pattern Page (SEQMODE 1/2/4, DRUM track)
- **PRESET DRUM STYLES, independent of the progression**: a DRUM track's **M2** (indigo pad, breathing while the page is open) opens a pattern page that bakes one of **18 authored one-bar styles** into the track's steps — **HOUSE · TECHNO · BREAKS · HIPHOP · TRAP · JUNGLE · FUNK · ROCK · JAZZ · LATIN · AFRO · ELECTRO · DNB · GARAGE · PUNK · SKA · DISCO · REGGAE**. The drum style is its own list: it does **not** follow the KEYMODE4 progression genre, so a jazz progression can run over a trap beat. The styles are idiomatic, not just different kick grids — **JAZZ** is the swing ride (quarters plus the skip note on the last 16th of beats 2 and 4, which reads as the swung "a" on a straight grid) with a sparse feathered kick and **comping** snare instead of a 2/4 backbeat; **DNB** is the two-step (kick 1 and 3&, snare 2 and 4, tom chops), **GARAGE** swings its 8ths, **PUNK** is straight 8ths at full tilt, **SKA** puts the hats and the cowbell on the up-beats, **DISCO** is four-on-the-floor with claps and congas, **REGGAE** is a one-drop (kick + snare only on 3) over a skank
- **The lanes follow the track's kit map** — `CUSTOM` (default) or `GM`, switched on **SEQMODE ENC5 = `MAP`** while the page is closed; changing it re-bakes the pattern. Lane voicing on `CUSTOM` (see [DRUM Part Sequencing](#5-drum-part-sequencing)), each lane reaching **seven** voices (`a`..`g`, ghosts `A`..`G`): **KICK** = C1 bass drum · **SNARE** = E1 snare / C#1 hand clap / C#2 claves · **HAT** = D1 closed hat / D#1 open hat / D2 maracas / D#2 cymbal · **PERC** = G1 hi conga / F#1 mid conga / **D#1 open hat** (the bar-start accent — the D#2 cymbal rang over the whole bar on this kit, so it is not used by the patterns) / F1 low conga / A#1 hi tom / A1 mid tom / **C2 cow bell**. On `GM` the same accent is the 46 open hat and the perc lane runs 45/47/48/50 toms + 49 crash + 51 ride. The bar-end VAR fill adds E1 snares and an A#1→G#1 tom run
- **Base row (page open, no F set armed)**: **ENC1 = GENRE**, **ENC2 = VAR** (`off` / `fill4` / `fill8` — a rolled snare+tom run over the last beat or the last half bar), **ENC3 = FILL %** (ghost-layer density — every line carries core hits plus rolled **ghost** hits, e.g. the hat lanes are an 8th-note backbone at FILL 0 that fill in to full 16ths at 100), **ENC4 = MOT %** (per-hit accent chance), **ENC5–8 = KICK / SNARE / HAT / PERC** line selectors (`A` · `B` · `OFF` — every lane carries two authored lines, so the four lanes recombine into one pattern). The two lines of a lane are always **different music**, not a copy with extra ghosts: the percussion lane in particular plays real conga / tom / cowbell lines on both (3+ hits each, in every style), so `A`→`B` and `OFF` change what you hear even at FILL 0
- **F row = four knob sets** (click to arm, click the armed one again for the base row): **F1 GROOVE** (genre / VAR / FILL / MOT on ENC1–4), **F2 LINES** (the four lane selectors on ENC1–4), **F3 FILL%** and **F4 MOT%** (the per-lane values on ENC1–4). The master FILL / MOT knobs write all four lanes at once, the per-lane knobs then trim them individually
- **Live**: every knob turn rebuilds the pattern in place (4 counts = 1 detent, clamped, no wrap) — a genre or lane flip is audible on the next bar, no M3 press needed. **M3 tap = re-roll** (fresh FILL ghosts, MOT accents and bar-end fill from the same settings), **M3 hold = undo**. One undo snapshot per knob burst, so a continuous turn undoes as a single gesture
- **MOT rides the accent mask**: the per-hit accent chance stamps the same **per-voice drum accent mask** the playhead reads (and the ACCENT step target edits — now **16-bit**, so hat/perc voices above note 43 accent too), so the track's accent velocity shapes the dynamics
- Step content is written like the other generators: over the track's **own pattern length**, the authored 16-step bar repeating when the track is longer. Opening the page changes nothing — the first knob turn or M3 does
- Saved/restored with SEQMODE4+REC: genre, VAR, the four lane lines and the per-lane FILL / MOT of all 16 tracks persist in the extended **(923-byte)** SEQ EEPROM block; a save from an older build (666 → 260 bytes) still loads and keeps the page defaults (HOUSE, line A, FILL 60, MOT 20). Patterns themselves stay RAM only
- **LEDs**: **M2 = indigo** (dim while the page is closed, breathing while it is open; touch still lights it white); while the page is open the **F row lights indigo** — bright for the armed set, dim for the rest (base row = all four dim)

#### Cross-Sequence Sync
When a track's step chords are written, all MONO/POLY sequences **listening to that track's channel** re-derive their active steps from the source timeline (single-level, non-recursive). This enables "follow the leader" arrangements. A POLY track with **TYPE = HARM** never mirrors — it **re-bakes** instead (see the HARM Generative Page above).

### 3. Encoder/CC Mode (ENCMODE)
- **ENC1–ENC4** send **CC 17–20** on `encChannel` (default ch1, selectable via step-pad overlay in variant 3; a KEYMODE3 **ENCOD lock** links it to the SEQ edit channel — see Keyboard Playing)
- **ENC5–ENC8** send the current **F-bank** block inside the active **M3 page group**: group 1 (M3 off) = **F1 = CC 1–4**, **F2 = CC 5–8**, **F3 = CC 9–12**, **F4 = CC 13–16**; group 2 (M3 on) = **F1 = CC 21–24**, **F2 = CC 25–28**, **F3 = CC 29–32**, **F4 = CC 33–36**
- **F1–F4** = CC-bank radio (bright white = armed; always one armed — **remembered per encoder channel and persisted with SEQMODE4+REC**: switching the encoder channel, or re-entering ENCMODE, recalls that channel's last bank + page group). Idle color follows the page group: dim purple in group 1, dim medium sea green in group 2. In ENCMODE the F row leaves the inherited KEYMODE2 chord-extension job
- **M3 (PAGE)** = page-group toggle: dim medium sea green = group 1 (CC 1–16), purple = group 2 (CC 21–36); off on first entry (also leaves the inherited KEYMODE2 chord-quality job)
- Values persist across mode changes (0–127, clamped, no wrap); DAW feedback on CC 1–36 syncs the matching knobs
- Knob positions are stored **per CC number × channel** (36×16): switching the encoder channel loads that channel's own values instead of inheriting the previous channel's (no jump)
- **Screen (ENCMODE 1/2/4)**: the eight knobs are drawn as **270° dials** in a 4×4 grid — **dial, CC number, dial, CC number** — so the value sits *inside* its ring and the CC number underneath it. A dial is a **radius-8 ring drawn as a single-pixel arc** (`u8g2.drawArc`, 90° gap at 6 o'clock: 0 at the lower-left end, 127 at the lower-right) with a **4-px pointer that reaches one pixel outside the rim** at the current value, and the 0–127 value in the middle. The CC number under each dial is set in the **3×3 caption font** — small enough to stay secondary to the value, and short enough (3 px) to leave the row above each ring free for the pointer. The second row's CC numbers follow the **F bank + M3 page group** (`CC1`–`CC16` / `CC21`–`CC36`), which is what the old bottom legend used to hint at: **the control-button legend is gone in this mode** so the second dial row gets the room. **ENCMODE3 keeps the plain label/value screen** (its pads are the channel overlay, not the knobs)
- **ENCMODE4 + REC** saves all 36×16 knob values + channel to EEPROM (an older save is ignored, defaults stay)

### 4. Transport
- **PLAY** button (SEQMODE): Start (or Stop if INT transport running)
- **PLAY** button (**ENCMODE3**): toggle clock **INT/EXT** — it never starts the transport there (frees SEQMODE3 ENC4)
- **SYNC=INT**: Teensy generates MIDI Clock (0xF8) + Start (0xFA) → DAW follows (**default**)
- **SYNC=EXT**: Follows incoming USB-MIDI Clock/Start/Stop/Continue

### 5. DRUM Part Sequencing
- 16 parts = MIDI notes 36 (C1) … 51 (D#2) — **this kit's map** (device note naming, MIDI 24 = C0):
  **C1** bass drum · **C#1** hand clap · **D1** closed hat · **D#1** open hat · **E1** snare · **F1** low conga · **F#1** mid conga · **G1** hi conga · **G#1** low tom · **A1** mid tom · **A#1** hi tom · **B1** (unused) · **C2** cow bell · **C#2** claves · **D2** maracas · **D#2** cymbal
  - **Two maps**: `CUSTOM` (above, the default) and `GM` (the General MIDI percussion map) — a DRUM track picks one on **SEQMODE ENC5 = `MAP`** (variants 1/2/4); the drum page's lanes are voiced from the selected map, so a preset pattern plays the right sounds on either kit. Per track, saved with SEQMODE4+REC
- **Hold M1** (SEQMODE, not variant 3, DRUM track) → orange part-select overlay on steps
- **Press step in overlay** → selects part
- **Normal step view** → toggles selected part on/off at that step
- **M2** on the same track opens the [DRUM pattern page](#drum-pattern-page-seqmode-124-drum-track) — 18 preset styles with live FILL/MOT and per-lane line selectors

### 6. MIDI I/O
- **USB-MIDI Device**: Notes, CC, Clock, Start/Stop/Continue
- **Keyboard output channel**: Default ch2 (ch1 reserved for DRUM)
- **Sequencer output**: Each sequence plays on its own MIDI channel (1–16)
- **External MIDI In**: `extNoteOn/Off` feeds pending chords of sequences whose input channel matches

## Display

**Header** (y 0–15): the mode in whole words on the left (`ENCODER 1`, `SEQUENCER 3`, `KEYBOARD 2`; variant 3 reads `CHANNEL SELECT` while its pad overlay is up) and one **temporary message** on the right (2.5 s). Every message names the parameter that just moved and its value in full words — `Pattern Length 16`, `Accent Velocity 90`, `Kit Map GM` — never the 4-character main-screen code (`LEN`, `ACC`, `PROB`). A **scale change** announces only its mood (`Scale Happy`, `Scale Sad`), because the knob's own value line already shows which scale is selected. An open page takes the left slot (`ARP ORDER`, `GENERATOR`, `DRUM PATTERN`, `STRUM ADDS`, `PROBABILITY`). The band is 128 px (21 characters); a message too long for it is pinned to the left edge so its head stays readable instead of running off-screen. KEYMODE4 also keeps a **persistent left header** (the progression `V vi > I IV`) that transients overlay and then uncover again  
**Main screen** (y 16–63):
- Encoder grid 4×2: labels (top) + values (bottom) — **ENCMODE 1/2/4 replace both text rows** with the dial grid above (value inside the ring, CC number under it)
- Control button labels: row1 PREV NEXT M1 M2 M3, row2 M1 M2 M3 M4 (KEYMODE4 labels NEXT as **EMB** — the sticky EMB menu; ENCMODE3 labels row2 **F1–F4** — the CC-bank radio, and ENC5–8 values show the live CC numbers per bank). **ENCMODE 1/2/4 draw no legend at all** — the dials take the whole band

Paged I2C write (1 page per `displayUpdate()` call, ~4 ms @ 400 kHz) to avoid blocking MIDI pump.

## LED Feedback

- **Mode LEDs** (32/33/34): Purple/Blue/Green dim; selected = white/pulse/blink per variant
- **Step pads**: Green playhead; dim green = active step; dim blue = empty; yellow channel overlay (variant 3); orange drum part overlay; KEYMODE4 adds white = focus in AUDITION / red = focus in WRITE, amber = locked step (bright = both)
- **Keyboard pads**: Dark blue (KEY1/3), blue-violet (KEY2 chord mode), degree map in KEYMODE4 (hue-mapped C = blue … B = red, rank 0 hottest, dimming down the rank, orange = borrowed, dark = not scored); in-scale only lit (KEY1/2/3); out-of-scale = dark; sounding chord keys tint cyan; playback on keyboard channel blinks dim green; **HAND strum zone** (POLY TYPE = HAND): white keys = pink → seagreen string gradient (hue 247 → 107, silent strings almost dark), black keys = option toggles (violet on / dim dark violet off)
- **Touch**: White while touched (wins over mode colors)

### Encoder page pad colors

SEQMODE **M2** opens an encoder page on a track that has one, and the pad carries that **track type's own hue** — dim while the page is closed, breathing while it is open — so the pad says which page it holds without the header. The hues are spread deliberately:

| Track type | M2 pad | Page behind it |
|---|---|---|
| **DRUM** | indigo `120,0,255` | [DRUM pattern page](#drum-pattern-page-seqmode-124-drum-track) |
| **MONO + ARP** | pink `255,0,170` | [ARP options](#arp-options-seqmode-124-mono-track-with-type--arp) |
| **MONO + BASS** | blue `0,70,200` | [GNR8 generator](#gnr8-generative-page-seqmode-124-mono-track-with-type--bass) |
| **MONO + LEAD** | sea green `46,120,80` | [LEAD generator](#lead-generative-page-seqmode-124-mono-track-with-type--lead) |
| **POLY + STRUM/HAND** | lavender `205,135,255` | the strum ADDS row |
| **POLY + HARM** | aqua `0,215,215` | [HARM page](#harm-generative-page-seqmode-124-poly-track-with-type--harm) |
| **POLY + STEP** | pale sky `135,195,255` | [COMP page](#comp-comping-page-seqmode-124-poly-track-with-type--step) — only lit while the page is open |
| **POLY + SKANK** | spring green `0,205,125` | [SKANK page](#skank-walking-voicing-page-seqmode-124-poly-track-with-type--skank) |
| MONO + BASS/LEAD, POLY + HARM | **M3** slow hue cycle | the generator button (tap = generate, hold = undo) |

**Red, orange and yellow are reserved** — REC, the drum / encoder MUTE overlays, the variant-3 channel selector and the armed-attribute step pulse already use them — and the step-edit attribute row keeps its **blue** (TIE / GLIDE / ACCENT / PROB). Every track hue therefore lives in the cool arc from spring green through cyan and blue to indigo and pink; two **pale tints** (STRUM lavender, COMP pale sky) buy their separation by lightness instead of hue.

When an M2 page is open, **its F row wears the same hue as the pad** that opened it (ARP sub-pages, GNR8 option radios, the strum ADDS row, the drum knob sets) — bright for the armed entry, dim for the rest. F pads that a page does not own keep their step-edit blue.

Idle brightness is 64/255 (breathing 64→200); the closest pair in CIELAB at that level is **14.4 ΔE** (HARM aqua vs SKANK spring green), against 0.0 for the duplicates and 8.9 for indigo vs blue before the split.

## Building & Flashing

1. Install Teensyduino (Arduino IDE or VS Code + PlatformIO)
2. Install libraries: FastLED, U8g2, Encoder, Adafruit_MPR121, uClock
3. Open `Teensy2048.ino`
4. Select **Teensy 4.1**, **USB Type: MIDI**
5. Compile & upload

**arduino-cli** — no IDE, no copy step, works on the checked-out tree:

```sh
./tools/fw.sh build     # compile only
./tools/fw.sh upload    # compile, then flash the attached Teensy 4.1
```

`tools/fw.sh` locates `arduino-cli` (PATH → Arduino IDE bundle → `$ARDUINO_CLI`),
collects the library roots that hold the five required libraries (sketchbook
`~/Documents/Arduino/libraries` plus any `$SKETCHBOOK/*/libraries` providing
FastLED/Encoder), builds into `${TMPDIR:-/tmp}/t2048build`, and flashes via the
**Teensy port** reported by `arduino-cli board list`. Overrides: `ARDUINO_CLI`,
`T2048_LIBS` (colon-separated roots), `T2048_BUILD`, `T2048_PORT`.

Because USB Type is MIDI the board has no serial tty: it appears only under
"Teensy Ports" (`usb:…`), and a `/dev/cu.usbmodem*` port left over from an older
non-MIDI build disappears after flashing — that is success, not a disconnect.

`tools/header_lint.py` guards the OLED header text: it fails on any
`headerShow()`/`headerAnnounce()` message wider than the 21-character band and on
the shorthand codes the strip must not use again (`PROB`, `mot`, `klock`, `GNR8`,
…), and it lists the snprintf-built messages whose worst case would overflow.
Run it after touching a message (exit status 1 = findings).

## Key Constants (Keyboard.cpp / Chords.h / TouchChips.h)

```cpp
// Keyboard (Keyboard.cpp)
KEY_OCT_MIN  = -2
KEY_OCT_MAX  = 8
KEY_OCT_DEF  = 3      // C3 = 60
KEY_VEL      = 100
KEY_COUNTS_OCT = 4    // encoder counts per octave
STEP_COUNT   = 16
STEP_MAX     = 64     // 4 bars × 16 steps max per track
CHORD_MAX    = 8      // notes per chord
SEQ_CH_COUNT = 16     // sequences = MIDI channels 1..16

// Chord Engine (Chords.cpp / Chords.h)
CHORD_VEL    = 100
CHORD_N      = 8      // max simultaneous notes
CHORD_ENC    = 4      // encoder counts per param step
KEY_SCALE_COUNT = 39  // scale table size

// Touch (TouchChips.h)
TOUCH_THRESH = 50     // press threshold
TOUCH_REL    = 30     // release threshold (hysteresis)

// Sequencer (Keyboard.cpp)
static uint8_t chStep[16]          // playhead per track (0..63)
static uint8_t chStepShadow[16]    // global position (ignores loops)
static bool    loopWasOn[16]       // previous loop state (edge detect)
```

## Event Flow (Main Loop)

```cpp
loop() {
  pollSeqClock();        // MIDI clock pump + playhead advance (highest priority)
  pollTouchChips();      // Read MPR121 IRQs → handleTouches()
  btnUpdate();           // Debounce, hold/double detection
  pollEncoders();        // Read encoder deltas → queue
  pollModeLeds();        // Animation (pulse/blink/playback blink)

  handleButtonEvents();  // Dispatch per current binding (mode+variant+overlays)
  handleEncoderEvents(); // Dispatch per current binding

  pollSeqClock();        // Drain ticks queued during event handling
  displayUpdate();       // Paged OLED write (non-blocking)

  // Serial commands: "recal" / "diag" (see Serial Diagnostics)
}
```

`seqDiagNote()` timestamps each section; worst-case >3 ms reported at 1 Hz.

## Serial Diagnostics

Open Serial Monitor @ 115200 and type commands:

| Command | Action |
|---------|--------|
| `recal` + Enter | Force full recalibration of all 4 MPR121 chips (stops, re-autoconfigs, restarts, prints baselines) |
| `diag` + Enter | Print current baseline + filtered data for all 48 electrodes |

Boot prints baseline values for all electrodes after the 500ms autoconfig delay. Electrodes with `0` or very low baselines failed calibration.

## Extending / Customizing

- **Add a scale**: Edit `kScaleName`, `kScaleMood`, `kScalePat` in `Chords.cpp`
- **Add a chord quality**: Extend `kQualityRow`, `kQualityName`, `kTriad` in `Chords.cpp`
- **Change KEYMODE4 genres/tables**: regenerate `genre_data.h` from `tools/genre_data/` (method + sources documented in `GENRE_DATA.md`); `Key4.cpp` is its only consumer
- **New mode variant**: Add row to `table` in `Bindings.cpp`, define encoder labels, update `Events.cpp` dispatch
- **New control binding**: Add `EncSet`/`CtrlSet`/`StepSet`/`KeySet` enums, extend binding struct

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Green playhead freezes | MIDI pump starved (long `displayUpdate`/`Serial` block) | Keep `pollSeqClock()` frequent; paged OLED write already helps |
| Stuck notes on mode change | Channel change without note-off | `keySetChannel()`/`chordCut()`/`chordRestore()` handle this |
| MPR121 not found | I2C address conflict / wiring | Check ADDR pins (0x5A–0x5D), 4.7 kΩ pull-ups on SDA/SCL |
| Encoder jitter | Mechanical bounce / no debounce | Encoder library handles quadrature; add RC filter if needed |
| Loop doesn't start immediately | Old code waited for playhead to reach bar | Updated: loop now teleports on activation (see `seqStepAdvance`) |
| Wrong bar after loop disable | Playhead didn't track global position | Shadow position (`chStepShadow`) now advances silently during loop |

## License

MIT — see LICENSE file (or add one).