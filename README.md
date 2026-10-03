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
| `Chords.h/cpp` | KEYMODE2 chord engine: quality, extensions, voicing, latch, KeyLock |

## Mode System

Three mode families, each with 4 variants (1–4). Variant 3 is **momentary hold** (returns to previous on release). Variant 4 is **blinking** (double-click to enter).

| Mode Button | Family | Variants |
|-------------|--------|----------|
| **ENC** (LED 32) | Encoder/CC mode | 1: shared SEQ controls, 2: ENC2, 3: ENC3 (momentary), 4: ENC4 |
| **SEQ** (LED 33) | Sequencer mode | 1–4: SEQ1–SEQ4 |
| **KEY** (LED 34) | Keyboard mode | 1–4: KEY1–KEY4 |

**KEYMODE2** = chord mode (diatonic auto-chords, quality/extension buttons, latch, KeyLock).  
Chord behavior is **inherited** by SEQMODE/ENCMODE via `keyBehaviorVariant()`.

### Binding Matrix (Bindings.cpp)

Each mode/variant combination selects an `EncSet`, `CtrlSet`, `StepSet`, `KeySet` that defines what the 8 encoders, 9 control buttons, 16 step pads, and 12 keyboard pads do.

**Example (SEQMODE3):**
- ENC1: Octave shift of edit sequence
- ENC2: (unused)
- ENC3: (unused)
- ENC4: SYNC INT/EXT
- ENC5: Input channel (1–16)
- ENC6: Track type (DRUM/MONO/POLY)
- ENC7: Mono type (ROOT/BASS/ARP) — MONO only
- ENC8: Tempo (40–240 BPM)

## Workflows

### 1. Keyboard Playing (Any Mode)
- **12 chromatic pads** (C–B) send MIDI notes on `keyChannel` (default ch2)
- **ENC1** (KEYMODE): Octave –2..+8 (4 counts = 1 octave, clamped)
- **ENC2** (KEYMODE): Scale select (39 scales, Chromatic = no filter)
- **KEYMODE2**: Chord engine active
  - M1/M2/M3/NEXT = quality (Maj/Min/Dim/Sus) — momentary
  - F1–F4 = extensions (6, m7, M7, 9) — toggle
  - ENC3 = Root, ENC4–7 = Voicing/Bass/Spread/Density, ENC8 = Latch
  - **PREV (hold)** = momentary encoder overlay (Root/Scale/Latch/KeyLock/Perf)

### 2. Sequencer (SEQMODE)

**Playhead** advances on MIDI clock (24 PPQN external or internal master at tempo). 16 steps per sequence, 16 sequences (one per MIDI channel 1–16).

#### Step Recording
- **Press unassigned step** → silently records current *pending chord* into that step
- **Hold step** → assign mode: play keyboard keys to **replace** that step's chord
- **Click active step** → disables it
- **Pending chord sources**: keyboard (when its channel matches), external MIDI on sequence's input channel

#### Live Recording (SEQMODE1/2)
- **REC** arms one record pass over the edit track's pattern from its **next 1st bar** (or the looped bar's head when a bar loop is armed); the pass covers the whole play range and then switches itself off (`rec done`)
- **LED**: dim dark red = idle, **bright red** = armed / recording (touch = white)
- The keyboard channel must match the edit track — otherwise arming is refused (`kbd ch mismatch`); changing the keyboard channel mid-record disarms
- Each gesture writes the step under the playhead: **POLY/MONO replace** (fresh gesture clears step + attributes, held notes build the chord), **DRUM overdubs** (notes are added, never removed, duplicates skipped)
- Press **REC again** = cancel (`rec off`); **transport stop** interrupts the pass and re-arms it for the next start

#### Bar Loop (SEQMODE3)
- **Press C/D/E/F bar button** (on step pads in SEQMODE3): first press selects that 16-step edit window; **press again on the SELECTED bar toggles its loop**
- **Loop activation**: playhead **teleports immediately** to the looped bar and starts looping it (no waiting to "walk" there)
- **Loop disable**: playhead resumes at the global position it *would have reached* without the loop (shadow position continues advancing silently during the loop)
- Each track has independent loop state and pattern length (polymeter)

#### Sequence Configuration (SEQMODE3)
- **ENC1**: Playback octave shift (±5 octaves from stored notes, clamped to –2..+8 convention)
- **ENC4**: SYNC **INT** (Teensy master, streams Clock+Start) / **EXT** (follow USB-MIDI clock)
- **ENC5**: Input channel (1–16) — notes arriving here feed pending chord; changing backfills from that track
- **ENC6**: Track type — **DRUM** / **MONO** / **POLY**
  - Ch1 defaults DRUM, ch2–16 default POLY
- **ENC7**: MONO transform — **ROOT** (chord root), **BASS** (lowest note), **ARP** (ascending arpeggio)
- **ENC8**: Tempo 40–240 BPM (INT sync only)

#### Track Behaviors
| Track | Playback | Step Recording |
|-------|----------|----------------|
| **DRUM** | Each step holds drum parts (voices C1–D#2). Playhead sends all active parts on channel. | Press step = toggle selected part (M1 hold = part-select overlay) |
| **MONO** | One note per step: root/lowest/arp of stored chord + sequence octave shift | Records pending chord; MONO also follows **input track's timeline** (last active step at/before position) |
| **POLY** | Full chord per step + sequence octave shift | Records pending chord; if input≠own, prefers **held gesture** (keys/latched chord/ext notes) over timeline |

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
- **SYNC=INT**: Teensy generates MIDI Clock (0xF8) + Start (0xFA) → DAW follows
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
- Control button labels: row1 PREV NEXT M1 M2 M3, row2 M1 M2 M3 M4

Paged I2C write (1 page per `displayUpdate()` call, ~4 ms @ 400 kHz) to avoid blocking MIDI pump.

## LED Feedback

- **Mode LEDs** (32/33/34): Purple/Blue/Green dim; selected = white/pulse/blink per variant
- **Step pads**: Green playhead; dim green = active step; dim blue = empty; yellow channel overlay (variant 3); orange drum part overlay
- **Keyboard pads**: Dark blue (KEY1/3), hue-mapped (KEY4), blue-violet (KEY2 chord mode); in-scale only lit; out-of-scale = dark; sounding chord keys tint cyan; playback on keyboard channel blinks dim green
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