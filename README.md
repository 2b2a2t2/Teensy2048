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
| `Key4.h/cpp` + `genre_data.h` | KEYMODE4 genre engine: 18-genre transition tables, ranked chord proposals, write-to-focus, EMB embellish menu (passing chords + substitution; data generated into `genre_data.h`, method in `GENRE_DATA.md`) |

## Mode System

Three mode families, each with 4 variants (1–4). Variant 3 is **momentary hold** (returns to previous on release). Variant 4 is **blinking** (double-click to enter).

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
- ENC3: FOLLOW ON/OFF
- ENC4: Pattern speed (1/16 … 1 … 4, default 1)
- ENC5: Input channel (1–16)
- ENC6: Track type (DRUM/MONO/POLY)
- ENC7: Mono type (ROOT/BASS/ARP/STRUM) — MONO only; Poly type (STEP/STRUM/HAND) — POLY only
- ENC8: Tempo (40–240 BPM)

## Workflows

### 1. Keyboard Playing (Any Mode)
- **12 chromatic pads** (C–B) send MIDI notes on `keyChannel` (default ch2)
- **ENC1** (KEYMODE): Octave –2..+8 (4 counts = 1 octave, clamped)
- **ENC2** (KEYMODE): Scale select (39 scales, Chromatic = no filter)
- **KEYMODE2**: Chord engine active
  - M1/M2/M3/NEXT = quality (Maj/Min/Dim/Sus) — momentary, beats **KeyLock**'s diatonic triad (root still snapped to the scale)
  - F1–F4 = extensions (6, m7, M7, 9) — toggle
  - **LEDs**: quality pads idle in the KEY button's dim green, extensions the same green with less red and blue (a touch darker); touch lights them white
  - ENC3 = Root, **ENC4 = FScale (Force to scale)**, ENC5–7 = Bass/Spread/Density, ENC8 = Latch
    - **FScale ON**: every chord tone is snapped to the nearest pitch of the current scale before it is sent (ties flatten first), so no out-of-scale note ever reaches MIDI — duplicates collapse to one voice; Chromatic passes everything through; OFF = free voicings. The snap also applies **at playback**: with FScale ON the sequencer forces each stored chord step into the current scale as it is sent (stored notes stay raw), so **changing the scale recolors recorded progressions live**; OFF plays the recording exactly as stored
  - **PREV (hold)** = momentary encoder overlay (Root/Scale/Latch/KeyLock/Perf)

- **KEYMODE4** (double-click KEY): genre-driven chord proposals
  - The engine scores all 12 pitch classes for the slot **before the focus** (transition matrix × MOOD × MOTION × CADENCE × SURP) and lays the whole scored degree set out on the keyboard: **hue-mapped by key as originally designed** (C = blue … B = red), **rank 0 = the next chord from the highest probability = hottest**, brightness stepping down the list so the colors tell how probable each degree is after the last chord; borrowed degrees open with **TENS ≥ 34** and light **orange** (closed ones stay dark); every knob is 0–100 (boot 50), 1 count = 1 unit. Every knob/genre turn re-ranks the propositions **dynamically** and repaints the keyboard live
  - The **header displays the progression**: `f3 V vi > I IV` = focus · the walk so far · `>` · the top-4 proposals. The **first chord of the walk is the chord played in KEYMODE2** when you enter (its kept notes seed the chain), or the stored step before the focus after a focus move
  - **ENC row**: ENC1 = **GENRE** (18, clamps — the knob does not wrap), ENC2–8 = MOOD/TENS/MOTION/CAD/CPX/ERA/SURP; **PREV (hold)** = overlay with ENC1 Octave / ENC2 Scale / ENC3 Key / **ENC4 FScale** (force-to-scale ON/OFF — ON makes playback of recorded steps follow the scale, same switch as KEYMODE2), and **PREV (click)** = focus −1; **NEXT (tap)** = the sticky **EMB menu** (below) — while it is open the row becomes **ENC1 EMB / ENC2 PLACE**, PREV hold still borrows it for Octave/Scale/Key/FScale and hands it back on release
  - **Press a proposed key** = audition the chord (unscored keys are silent) and **chain the progression**: the next proposals follow the chord you just heard, so repeated presses walk a sequence by ear — even in AUDITION with nothing written. The chain re-anchors on the stored steps whenever the focus moves; on release the header shows where the chain goes next. With **WRITE** on (boots in **AUDITION**) it also lands on the **focused step** and the focus auto-advances (**M2** = AUTO, boots off)
  - **Controls**: **NEXT tap** = the **EMB menu** (toggle, below) · **NEXT hold** = focus +1 · **M1** = WRITE ⇄ AUDITION (**dim red** = AUDITION, **bright red** = WRITE) · **M2** = AUTO-ADVANCE · **M3 tap** = apply the generator to every unlocked step, **M3 hold** = undo (one-level, restores channel + attributes) · **F1** = VARIATION (reseed + generate) · **F2** = lock the focused step (apply skips it) · **F3** = VOICE-LEAD (inversions follow the bass line) · **F4 held** = audition only (no write, no advance). After M3/F1/undo the header shows the **written progression** as numerals (`I V vi IV +8` = first four chords + how many follow)
  - **EMB menu (NEXT tap)** — what M3/F1 generate beyond the plain progression (the menu is KEYMODE4-only and closes when you leave the mode; locked steps are never touched):
    - **ENC1 EMB** (wraps): **OFF** = the plain generator as before · **PASS** = every odd step becomes a **passing chord** — a triad on the scale degree **one below the following main chord**, its quality/extension/inversion re-scored through the same genre+knob proposal logic (so it belongs to the grammar and voice-leads); when that degree is the previous main chord itself, the step simply keeps its main chord · **P+S** = PASS plus **substitution**: every main chord rolls once against the **SURP** knob and, if it hits, is swapped through a fixed table — **I→iii, ii→IV, iii→I, IV→ii, V→♭VII, vi→IV, ♭VII→V** (SURP 0 = never, 100 = always)
    - **ENC2 PLACE** (CW = **DIST**, CCW = **ODD**): **ODD** = every odd step passes when valid · **DIST** = only when the bracketing main chords stand **at least two scale degrees apart** — tighter neighbours keep their main chord (a near-touching "passing" chord would only wobble)
    - The header after M3 counts every written step including the passing ones (`… +16` for a full pattern)
  - **Step pads**: press = move the focus (**white** in AUDITION, **red** in WRITE; locked = amber), then the usual trigger/assign behavior
  - **PLAY** works here too: start/stop the transport without leaving KEYMODE4
  - Nothing persists across a reboot (RAM only, by design); the progression grammar follows the current scale (7-note scales exactly, otherwise major degrees from the root)

#### HAND Strum Keyboard (POLY track, TYPE = HAND)
- With the keyboard channel on a **POLY** sequence whose **TYPE = HAND** (ENC7 in SEQMODE3), the keyboard becomes a **strum zone** (Le Strum style): the track stays **silent** — each played step **buffers its chord** (post-FX / force-to-scale = what *would* have sounded) instead of sounding it, and the **7 white keys are the strings** (C…B, low to high) that strum that buffer. Pluck one string or sweep across; the strings read the **buffered chord first** (the progression under the playhead), then the pending chord (manual build / input), then the engine snapshot. Default voicing = **inversion spread**: the chord's sorted tones land **root position on E F G**, the two lower inversion notes fill **C D** (the 2nd and 3rd tones an octave down), and the upper ones fill **A B** (A = the 4th chord tone when there is one, else the 2nd tone an octave up; B = the 3rd tone an octave up) — e.g. C major strums `52 55 60 64 67 76 79` (E→C in first inversion, its 3rd in the bass). Degenerate chords leave strings without a tone **dark** (a lone root lights only E; no chord yet = silent but lit)
- **Black keys = silent option toggles** (tap flips the bit, header shows `<name> on/off`, nothing is written to the pattern; RAM only, booted with SUS + SUSCOMMON on): **C♯ = GUITAR**, **D♯ = GTR2**, **F♯ = ADDS**, **G♯ = SUS**, **A♯ = SUSCOMMON**
  - **GUITAR**: real 6-string shapes (C/C7/A/Am/A7/D/Dm/D7/E/Em/E7/G, ported from Le Strum) for MAJ/MIN/DOM7 chords — keys C…A carry the shape, **B is the spare string** (dark = the bassless C/A/D shapes have no note there, like a muted guitar string); maj7 / dim / aug chords fall back to the inversion spread. **GTR2** with GUITAR: the shape's notes an octave up fill D…B (C keeps the shape's lowest note); alone, GTR2 raises the inversion spread's top half (F…B) an octave
  - **SUS on**: strummed notes **ring past the step boundary until the buffered chord changes**; **SUS off** = they cut at the step boundary (per-step length). **SUSCOMMON on**: notes common to the old and new voicing **survive** that chord change; off = clean swap (all old notes cut). Stop / mute / erase / TYPE change / keyboard channel change always cut everything
  - **ADDS master + F-row**: M2 (SEQMODE 1/2/4, POLY track with TYPE STRUM/HAND) opens the temporary **EXT row** — **F1 = sus4**, **F2 = add6**, **F3 = add9** (independent toggles, combinable), **F4 = clear** (legend `sus4 · add6 · add9 · off`, header `EXT`; F4 lights while anything is armed). The colors reshape the string voicing while the ADDS master (F♯) is on; M2 again or leaving the context closes the row
- **White keys still BUILD** (KEYMODE2 quality/extensions, KEYMODE4 proposals, pending-chord feed — silent, chord engine muted); plucks bypass the scale filter and feed neither pending nor the block recorder (the gesture does). The black keys toggle instead of building while the strum owns the keyboard
- **Recording**: with REC on, a pluck appends the note to the gesture with its **real timing** — the first touch = onset 0, every later note = ms elapsed (u16, up to ~65 s, never squeezed). All notes of one gesture (**first touch → all keys up**) anchor to the step where it **started**, even if the playhead moved on; playback schedules the onsets from that origin step across the following steps/bars. A block-written step (assign mode, all timings zero) falls back to an **even ≈ 8–40 ms spread**. The block-record path is suppressed in HAND — only plucks record, so a strum never doubles up as a block chord
- **Playback**: a HAND step **buffers** its chord and stays silent — pick **TYPE = STRUM** (track default or F2 per-step override) when you want the auto-rake. An explicitly **recorded gesture** (stored timings) still **auto-replays** note by note as performed; rests and empty steps leave the buffer — and the ringing strings — untouched. HAND **FOLLOW**: an empty own step still resolves the feeder's current chord (a blank pattern = the live chord at all times); written steps gate the rhythm as usual. Steps with no override follow the **track** TYPE (the live gate is track-level; per-step TYPE shapes playback only)
- **LEDs**: the white keys show the **pink → seagreen gradient** (hue 247 → 107, left to right, dimmed); strings with no chord or a guitar gap dim to almost nothing; the **black keys show their option state** (violet = on, dim dark violet = off). The gradient and the option keys win over the KEYMODE2 chord tint, the KEYMODE4 proposal rainbow and the playback blink while HAND is live (touch still flashes white first)

### 2. Sequencer (SEQMODE)

**Playhead** advances on MIDI clock (24 PPQN external or internal master at tempo). 16 steps per sequence, 16 sequences (one per MIDI channel 1–16).

#### Step Recording
- **Press unassigned step** → silently records current *pending chord* into that step
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
- **ENC4**: Pattern **speed** of the edit sequence — step rate relative to the master clock: **1/16, 1/8, 1/4, 1/2, 1 (default), 2, 4**
  - Per track (polymeter + polyrhythm): a slow track skips master ticks until its accumulator fills, a fast one plays 2–4 sub-steps inside one tick; the gate time scales with the track's own step length
  - Saved/restored with SEQMODE4+REC. Clock **SYNC** moved to **ENCMODE3+PLAY**
- **ENC5**: Input channel (1–16) — notes arriving here feed pending chord; changing backfills from that track
- **ENC6**: Track type — **DRUM** / **MONO** / **POLY**
  - Ch1 defaults DRUM, ch2–16 default POLY
- **ENC7**: MONO transform — **ROOT** (chord root), **BASS** (lowest note), **ARP** (ascending arpeggio), **STRUM** (the whole chord raked inside the step)
  - Selecting **ARP** lights **M2 purple** in SEQMODE 1/2/4 — click it for the [ARP options](#arp-options-seqmode-124-mono-track-with-type--arp)
  - Selecting **BASS** lights **M2 purple** in SEQMODE 1/2/4 — click it for the [GNR8 generator](#gnr8-generative-page-seqmode-124-mono-track-with-type--bass)
  - **STRUM** sends the stored chord's notes ascending, one after another across the step (stagger ≈ 8–40 ms each, scaling with the step), every note-on cutting the one before it and the last held to the gate — a quick strum instead of one block chord. TIE, GLIDE, PROB and the gate behave as on the other types; TYPE is per track and saved with SEQMODE4+REC
- **ENC7 (POLY track)**: playback TYPE of the whole track — **STEP** (block chord per step, as before), **STRUM** (auto-strum: every step rakes ascending at a fixed ≈ 8–40 ms stagger scaling with the step), **HAND** (see [HAND keyboard](#hand-strum-keyboard-poly-track-type--hand) — steps buffer their chord silently, the keyboard strums the buffer). Clamp 0..2 (no wrap), per track, RAM only — a stale value normalizes to STEP at boot
- **ENC8**: Tempo 40–240 BPM (INT sync only)
- **Step-pad channel selector**: while SEQ is held into SEQMODE3 the 16 steps are the edit-channel selector (dim yellow, selected channel bright yellow). **Any encoder movement dismisses it** — the pads immediately show the normal sequencer again (playhead, recorded steps, pattern length) and stay that way until SEQ is held into SEQMODE3 once more
  - **Choosing a channel always returns to step edit**: the edit radio switches back on with **M1 (STEPS)** selected on the newly selected channel, whatever attribute it had on the previous one

#### Track Behaviors
| Track | Playback | Step Recording |
|-------|----------|----------------|
| **DRUM** | Each step holds drum parts (voices C1–D#2). Playhead sends all active parts on channel. | Press step = toggle selected part (M1 hold = part-select overlay) |
| **MONO** | One note per step: root/lowest/arp of stored chord + sequence octave shift | Records pending chord; MONO also follows **input track's timeline** (last active step at/before position) |
| **POLY** | Full chord per step + sequence octave shift + per-step FX (sus/ext/color, see Step Edit) | Records pending chord; if input≠own, prefers **held gesture** (keys/latched chord/ext notes) over timeline |

#### Step Edit Radio (SEQMODE 1/2/4)
- **M1 (edit)**: off = pads record steps, on = pads edit attributes on ACTIVE steps (PROB arms the gate; ENC8 sets the **track** probability that gate re-reads live every hit)
  - **MONO**: click cycles OFF → STEPS → OFF; **F1–F4** pick TIE / GLIDE / ACCENT / PROB; ENC5/6 shape gate/velocity — turning the radio **on** closes the GNR8 page if it is open (mutually exclusive with M2 GNR8)  - **DRUM/POLY**: quick M1 tap toggles edit (on reuses ACCENT/PROB, default ACCENT); **F3 = ACCENT, F4 = PROB** (click selected again = off); M1 **hold** on DRUM still opens the part-select; ENC8 = track probability
- **POLY step FX (F1 = `FX`)**: on a POLY track with the radio on, **F1** switches the target to **FX** (the control legend reads `FX`, the ENC7 slot reads `FX` and shows the current type; F1 again closes the radio). **ENC7** cycles the selected type with wrap: **sus2 · sus4 · 6 · m7 · M7 · 9 · color** — the header names every turn. Tap a step pad to toggle the selected FX on that step; the yellow attribute pulse shows only the steps carrying the **currently selected** type, and **F1 blinks while the track holds any FX**. **sus2/sus4 are mutually exclusive** (arming one clears the other); the extension types can stack with them. FX live only with the pattern (RAM, by design — patterns are never stored)
  - **Playback (POLY only — MONO/DRUM steps are never transformed)**: **sus2** replaces the chord's third with a second, **sus4** with a fourth; **6 / m7 / M7 / 9** add the extension on top of the stored triad; **color** appends a quiet ghost note a **half step below the root** (half velocity, half delay, sent raw — a chromatic color tone that bypasses the scale snap). FX are applied at send time — the stored notes stay untouched
- **POLY step TYPE (F2 = `TYPE`)**: on a POLY track with the radio on, **F2** switches the target to **TYPE** (legend `TYPE`, ENC7 slot reads `TYPE` + the armed value; F2 again closes the radio). **ENC7** cycles the armed value with wrap: **STEP · STRUM · HAND**. Tap a step pad to **stamp** that override onto the step; **tap again = clear** back to inherit-the-track (`… type inherit`). The yellow pulse shows the steps carrying an override and **F2 blinks** while any step of the track has one. The override wins over the track default (ENC7 in SEQMODE3) for that step's playback only
  - **step TYPE playback**: **STEP** = the plain block chord (as always); **STRUM** = auto-strum — that step rakes ascending at ≈ 8–40 ms stagger scaling with the step; **HAND** = the step buffers silently for the strum — an explicitly recorded gesture (stored timings) still replays as performed (see below), regardless of the track default
- **LED**: M1 bright white while no attribute is selected, selected F-pad white (on POLY all four F-pads are radio pads: FX / TYPE / ACC / PROB — DRUM and MONO follow their own subsets), all radio pads carry a dim dark-blue background (F-pads blink green while the playhead reads an attributed step)
- **Accent velocity (per track, boot 101, 1–127)**: an accented step sends this instead of a hard-wired 127 — applies to MONO, POLY and the DRUM step/per-voice accent alike (the other notes keep their own velocity)
  - **Hold F3** in SEQMODE 1/2/4: the ENC6 row switches **VEL → ACC** and ENC6 edits the edit track's accent velocity (header shows `ACCENT VEL`); release restores ENC6 to VEL
- **Probability (per track, boot 100, 0–100%)**: ENC8 edits the edit track's own playback probability. Every step carrying **PROB** re-reads it on each advance, so a turn changes the odds from the very next hit — no re-toggling the attribute. On DRUM the mask still picks *which* parts are gated, only the % is per track
  - Saved/restored with SEQMODE4+REC
  - F1–F4 in SEQMODE are radio keys only — they no longer trigger chord extensions there (DRUM/POLY with the radio off used to leak into the KEY behaviour); the one extra job is POLY's **F1 = FX target** and **F2 = TYPE target** above

#### ARP Options (SEQMODE 1/2/4, MONO track with TYPE = ARP)
- **M2** becomes an **ARP** key on such a track: click toggles the ARP encoder page (header shows `ARP` while it is open, the control legend reads `ARP`)
  - **Auto-off**: changing channel, TRACK away from MONO, TYPE away from ARP, or leaving SEQMODE always closes the page
- **LED**: M2 burns **purple** whenever the edit track is a MONO arp — dim while the page is closed, bright while it is open (touch still lights it white)
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

#### GNR8 Generative Page (SEQMODE 1/2/4, MONO track with TYPE = BASS)
- **M2** becomes a **GNR8** key on such a track: click toggles the GNR8 encoder page (header shows `GNR8` while it is open, the control legend reads `GNR8`) — **opening it turns the M1 edit radio OFF** (M1 and the GNR8 page are mutually exclusive: pressing M1 to start edit closes the page instead)
  - **Auto-off**: changing channel, TRACK away from MONO, TYPE away from BASS, or leaving SEQMODE always closes the page (ROOT / ARP / STRUM and every other track type never take it)
- **LEDs**: M2 burns **purple** whenever the edit track is MONO+BASS — dim while the page is closed, breathing while it is open (touch still lights it white). M3 runs a **slow hue cycle** on that track: it is the generator button
- **M3 tap = generate**, **M3 hold = undo** (one-level: restores the pattern as it was before the last generate — or before a rotation burst — plus its attributes, mirroring KEYMODE4's apply/undo pair). The header reports `gen N steps`
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

- **F row while the page is open = option radios** (legend `OPTS · OCT · BRW · PROB`): a click **arms** an option — the **whole encoder row becomes its sub-page**. **F1 = the full options page** (ENC1 TONES, ENC2 RANGE, ENC3 LOW, ENC4 ALGO, ENC5–8 `---`). **F2/F3/F4 = the per-step modifier pages**: **tap a step pad first** (armed pages only — the picked pad burns **full violet**, the header reads `step 5 oct +1`), then dial it live: **ENC2 = OCT** (F2, −2…+2 octaves), **ENC3 = BRW** (F3, `off` · `1`–`8` = borrow that tone of the incoming chord), **ENC4 = PROB** (F4, 0–100% = chance the step's OCT/BRW modifier actually applies on a pass, default **100**). **F2 also owns ENC3 = MAP** (needs no step): how FOLLOW re-resolves the generated notes — `near` · `root` · `deg` · `ivl` (see the FOLLOW mapping bullet below). With nothing picked, a dial flashes `pick step` (the plain page and F1/none-armed taps never record — attribute edits and the chain run with the page **closed**). Dials use **4 counts = 1 detent = 1 value, clamped at both ends — no wrap**. Click the **armed** F again to release (the plain page comes back, header `GNR8`), click a **different** F to switch directly (the picked step stays). The armed pad burns **bright violet**, the other three stay dim; arming announces `GNR8 opts` (F1) or `step oct` / `step brw` / `step prob` (`step 5 …` once a step is picked), and every dial change re-announces the value (`LOW C1`, `map deg`, `step 5 brw 3`, …). The radio **and the picked step die with the page** — closing it (M2, channel, TRACK, TYPE or mode change) resets to the plain row. **PREV-hold rotate keeps priority** while a radio is armed (the sub-page returns on release)

| Key | Option | Values (default first) |
|-----|--------|------------------------|
| **F1** | **OPTS** (full page) | ENC1 **TONES**: how many scale degrees the notes may use — **all · 5 · 4 · 3** (kept spread evenly over the KEY + SCALE) · ENC2 **RANGE**: octaves above LOW — **1 · 2 · 3 · 4 · 5** (steers the next generate *and* folds the existing line live — reversible, stored notes untouched; see FOLLOW mapping) · ENC3 **LOW**: bottom of the range — **C0 · C1 · C2** (MIDI 24 / 36 / 48) · ENC4 **ALGO**: **CLASSIC** (random per step) · **EUCLID** (Bjorklund, FILL rounded to a pulse count) |
| **F2** | **STEP OCT + MAP** | tap a step, then ENC2 = its octave transposition **−2 … 0 … +2** (default 0); ENC3 = **MAP** (global, persisted): **near** · **root** · **deg** · **ivl** — how FOLLOW maps generated notes onto the chord |
| **F3** | **Borrow** | tap a step, then ENC3 = borrowed tone of the incoming chord: **off** · **1** … **8** (root, 2nd tone, … of the chord the feeder plays) |
| **F4** | **Mod prob** | tap a step, then ENC4 = chance the step's OCT+BRW modifier applies on a pass: 0–100%, default **100** |

- **Generate (M3 tap)**: rolls every step of the edit sequence's **own pattern length** — empty steps are cleared, filled ones get one note snapped into the **current KEY + SCALE** (nearest in-scale pitch, so the generator follows key changes), inside LOW..LOW+RANGE−1, with **ACC**/**GLIDE** rolled per note. **TIE** follows Aciduino's rule: it rolls on the **rests**, holding the previous note over itself (consecutive rolls chain the hold), never on the note steps
  - Takes the undo snapshot first (M3 hold restores), seeds `random()` once per boot, RAM only — patterns are still never stored in EEPROM
- **FOLLOW mapping (what F2/F3/F4 do at playback)**: with **FOLLOW ON** (SEQMODE3 ENC3, a MONO/POLY listener reading another track's timeline) a BASS track no longer collapses onto the feeder chord's lowest note — each step's **own generated pitch** is re-resolved against the **incoming chord** (every tone, heard through the listener's octave shift) and the **note window**, then the step's **OCT** and **BRW** modifiers apply when their **PROB** roll hits (defaults = no effect: OCT 0, BRW off, PROB 100). The mode lives on **F2/ENC3** (global, saved in the SEQ config):
  - **near** (default): the chord tone **nearest the generated pitch inside the window** — keeps the line's contour and follows the chord under it
  - **root**: always the chord's **lowest tone**, octave-shaped to the generated pitch (the old one-note behavior, now octave-tracking)
  - **deg**: the generated pitch's **scale degree** (from KEY + SCALE; off-scale pitches snap to the nearest degree) indexes the chord tones ascending — degree N = chord tone N+1, wrapping over the chord
  - **ivl**: the chord's lowest tone **plus the pitch's semitone offset from the KEY root** — may deliberately leave the chord (chromatic color)
  - **BRW** (any mode): replaces the mapped tone with the borrowed chord tone, octave-shaped back into the window. Every mapping result stays inside the window; **OCT** applies afterward and may leave it (clamped to 0..127). **FOLLOW OFF** ignores MAP/BRW (the line plays as generated, plus OCT); ROOT/ARP take OCT but not MAP/BRW (STRUM's rake has neither). The generator itself is unchanged — with a static chord, FOLLOW ON sounds like OFF, and the line only moves when the chord does
  - **RANGE reshapes the line live (reversibly)**: every sounding pitch of the BASS track is **folded by whole octaves into the current LOW..RANGE window** at playback — RANGE 2→1 **lowers the upper-octave notes** into the new octave immediately (the line really becomes 1 octave wide), and RANGE 1→2 **raises the original pitches back**, exactly. The fold is a pure playback transform: **stored steps are never rewritten**, so the round-trip is lossless, and the per-step **OCT** shifts ride *on top of* the folded base (an OCT step can still sound outside the window by design)
- **Rotate (PREV held)**: while the page is open, hold **PREV** to swap the encoder row to the rotate overlay (`ROT` on ENC1, the other seven read `---`). **ENC1** rotates the whole pattern **4 counts = 1 step** (CW = content delays one step, wrapping at the pattern length — notes, attributes, FX, strum timings, drum masks **and the per-step OCT/BRW/PROB values** all move together). The header shows the rotation position (`rot N`); a continuous turn is one undo burst (M3 hold restores the pre-rotation pattern)
- **TIE span fill (hold-arm gesture)**: plain taps **always just toggle** the selected attribute — on a BASS track too, TIE / GLIDE / ACCENT / PROB behave exactly as everywhere else (GLIDE/ACCENT/PROB are per-step only and never span). **Attribute taps and chains run with the GNR8 page closed**: opening it turns the edit radio off, and with the page open the step pads belong to the F2/F3/F4 dial selection (F1 or no radio armed = inert taps) — so close the page first and make sure **M1 + TIE** are armed (or re-arm them after closing it). **Hold a step** to arm the span start: the touch-down toggle of that same press is reverted first, so holding never changes the flag, and the header announces `chain from N`. The **next tap on another step** stamps TIE across every step from start up to (not including) the end, wrapping inside the pattern length — `N steps tie` in the header; the span rings the start note over its rests. Tap the armed start itself = cancel (`chain off`); a forgotten arm **expires after 5 seconds**. Changing target / mode / channel / TRACK / TYPE disarms
- Saved/restored with SEQMODE4+REC (the eight GNR8 parameters **and the F2 MAP mode** live in the SEQ EEPROM block; a config saved by an older build still loads and keeps the defaults — including MAP = `near`). The per-step **OCT / BRW / PROB** values are **RAM only** like every other step field: they live and die with the step (generate and erase reset them, rotate moves them, M3 undo restores them)

**Bar Loop (per track, SEQMODE3):**
- Loop arms on the **selected edit window bar** (0–3, each 16 steps)
- **On activation**: playhead jumps immediately to loop start (teleports)
- **During loop**: green playhead tracks within looped bar; shadow position advances globally
- **On disable**: playhead resumes at shadow position (catches up)
- Independent per track → polymeter loops possible

#### Cross-Sequence Sync
When a track's step chords are written, all MONO/POLY sequences **listening to that track's channel** re-derive their active steps from the source timeline (single-level, non-recursive). This enables "follow the leader" arrangements.

### 3. Encoder/CC Mode (ENCMODE)
- **ENC1–ENC8** send **CC 1–8** on `encChannel` (default ch1, selectable via step-pad overlay in variant 3)
- Values persist across mode changes (0–127, clamped, no wrap)

### 4. Transport
- **PLAY** button (SEQMODE): Start (or Stop if INT transport running)
- **PLAY** button (**ENCMODE3**): toggle clock **INT/EXT** — it never starts the transport there (frees SEQMODE3 ENC4)
- **SYNC=INT**: Teensy generates MIDI Clock (0xF8) + Start (0xFA) → DAW follows (**default**)
- **SYNC=EXT**: Follows incoming USB-MIDI Clock/Start/Stop/Continue

### 5. DRUM Part Sequencing
- 16 parts = MIDI notes 36 (C1) … 51 (D#2) — General MIDI drum map
- **Hold M1** (SEQMODE, not variant 3, DRUM track) → orange part-select overlay on steps
- **Press step in overlay** → selects part
- **Normal step view** → toggles selected part on/off at that step

### 6. MIDI I/O
- **USB-MIDI Device**: Notes, CC, Clock, Start/Stop/Continue
- **Keyboard output channel**: Default ch2 (ch1 reserved for DRUM)
- **Sequencer output**: Each sequence plays on its own MIDI channel (1–16)
- **External MIDI In**: `extNoteOn/Off` feeds pending chords of sequences whose input channel matches

## Display

**Header** (y 0–15): Mode name (left), temporary messages (right, 2s timeout)  
**Main screen** (y 16–63):
- Encoder grid 4×2: labels (top) + values (bottom)
- Control button labels: row1 PREV NEXT M1 M2 M3, row2 M1 M2 M3 M4 (KEYMODE4 labels NEXT as **EMB** — the sticky EMB menu)

Paged I2C write (1 page per `displayUpdate()` call, ~4 ms @ 400 kHz) to avoid blocking MIDI pump.

## LED Feedback

- **Mode LEDs** (32/33/34): Purple/Blue/Green dim; selected = white/pulse/blink per variant
- **Step pads**: Green playhead; dim green = active step; dim blue = empty; yellow channel overlay (variant 3); orange drum part overlay; KEYMODE4 adds white = focus in AUDITION / red = focus in WRITE, amber = locked step (bright = both)
- **Keyboard pads**: Dark blue (KEY1/3), blue-violet (KEY2 chord mode), degree map in KEYMODE4 (hue-mapped C = blue … B = red, rank 0 hottest, dimming down the rank, orange = borrowed, dark = not scored); in-scale only lit (KEY1/2/3); out-of-scale = dark; sounding chord keys tint cyan; playback on keyboard channel blinks dim green; **HAND strum zone** (POLY TYPE = HAND): white keys = pink → seagreen string gradient (hue 247 → 107, silent strings almost dark), black keys = option toggles (violet on / dim dark violet off)
- **Touch**: White while touched (wins over mode colors)

## Building & Flashing

1. Install Teensyduino (Arduino IDE or VS Code + PlatformIO)
2. Install libraries: FastLED, U8g2, Encoder, Adafruit_MPR121, uClock
3. Open `Teensy2048.ino`
4. Select **Teensy 4.1**, **USB Type: MIDI**
5. Compile & upload

## Key Constants (Keyboard.h / Chords.h)

```cpp
// Keyboard
KEY_OCT_MIN  = -2
KEY_OCT_MAX  = 8
KEY_OCT_DEF  = 3      // C3 = 60
KEY_VEL      = 100
KEY_COUNTS_OCT = 4    // encoder counts per octave
STEP_COUNT   = 16
STEP_MAX     = 64     // 4 bars × 16 steps max per track
CHORD_MAX    = 8      // notes per chord
SEQ_CH_COUNT = 16     // sequences = MIDI channels 1..16

// Chord Engine
CHORD_VEL    = 100
CHORD_N      = 8      // max simultaneous notes
CHORD_ENC    = 4      // encoder counts per param step
KEY_SCALE_COUNT = 39  // scale table size

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
}
```

`seqDiagNote()` timestamps each section; worst-case >3 ms reported at 1 Hz.

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