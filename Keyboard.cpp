#include "Keyboard.h"
#include "SeqClock.h"   // seqTrack() - step view follows the TRACK type
#include "Modes.h"      // keyBehaviorVariant() - KEYMODE2 = chord mode
#include "Chords.h"     // chord engine + scale filter state
#include "Events.h"     // prob clipboard accessors (SEQ save/load)
#include <EEPROM.h>

#define KEY_OCT_MIN   (-2)
#define KEY_OCT_MAX    8
#define KEY_OCT_DEF    3      // C3 = 60 (middle C)
#define KEY_VEL      100
#define KEY_COUNTS_OCT 4      // encoder counts per octave step

#define STEP_COUNT     16     // step pads = one edit window (one bar)
#define STEP_MAX       64     // steps stored per track (4 bars of 16)
#define CHORD_MAX      8      // notes stored per chord
#define SEQ_CH_COUNT   16     // one sequence per MIDI channel (1..16)

static int8_t  octave = KEY_OCT_DEF;
static int16_t octRes = 0;              // leftover encoder counts
static uint8_t keyCh = 2;               // keyboard MIDI channel (1..16); boot
                                        // default ch2 (ch1 is a DRUM track)
static bool    keyActive[12] = {false}; // key currently held
static bool    keySounding[12] = {false}; // key has its OWN note out (single mode)
static uint8_t keyNote[12] = {0};       // note sounding on that key

// live keyboard voice through the keyboard channel's MONO track (engine
// further down): while engaged the raw notes are suppressed and the mono
// voice sounds instead (ROOT/BASS follow the held gesture, ARP is driven
// by the playhead); polled every loop and on every gesture
static bool monoEng = false;            // mono voice owns the keyboard sound
static bool monoOn = false;             // the live voice (or arp note) is out
static uint8_t monoNote = 0;
static uint8_t monoLastT = 255;         // TYPE the voice was built for
static void monoApply();
static void monoVoiceOff();

// pending chord per sequence: fed by external notes on that sequence's
// input channel (EVERY sequence listening to the channel is fed, so ch2's
// default input and ch3's input=2 both get the same notes) and by keyboard
// plays while the keyboard channel (KEY3) selects that sequence.  Kept
// after release; recorded when a step of that sequence is activated.
static uint8_t pending[SEQ_CH_COUNT][CHORD_MAX];
static uint8_t pendingLen[SEQ_CH_COUNT] = {0};

// external notes currently held, per sequence (gesture bookkeeping)
static uint8_t extHeld[SEQ_CH_COUNT][CHORD_MAX];
static uint8_t extHeldLen[SEQ_CH_COUNT] = {0};

// per-sequence per-step chord history: what the pending chord was when the
// playhead last visited each step (written on every feed at the current
// playhead position, carried forward on step advance).  MONO step activation
// records this history ("actual or previous chord"): play B while the
// playhead is at step 1 and G while it is at step 9 -> steps 1-8 get B,
// steps 9-16 get G, regardless of the latest chord in the buffer
static uint8_t histChord[SEQ_CH_COUNT][STEP_MAX][CHORD_MAX];
static uint8_t histLen[SEQ_CH_COUNT][STEP_MAX];   // statics are zero-init

// per-track playhead position (0..63): every track advances on its own
// pattern length / bar loop, so lengths may differ (polymeter)
static uint8_t chStep[SEQ_CH_COUNT] = {0};

// record sequence s's pending chord at playhead position p
static void histAt(uint8_t s, uint8_t p) {
  if (p >= STEP_MAX || pendingLen[s] == 0) return;
  histLen[s][p] = pendingLen[s];
  for (uint8_t i = 0; i < pendingLen[s] && i < CHORD_MAX; i++)
    histChord[s][p][i] = pending[s][i];
}

// feed hooks: keep the history in sync wherever a pending chord is written
static void histFeed(uint8_t s) { histAt(s, chStep[s]); }

// sequencer edit focus: 0-based channel index (MIDI channel = seqEditCh + 1);
// the playhead plays EVERY channel's sequence at once, this only selects
// which channel the step pads display / edit
static uint8_t seqEditCh = 1;           // boot default = ch2 (step sequence 2
                                        // selected, matching the keyboard)

// input (listen) channel per sequence: notes arriving there feed the record
// workflow; default sequence N listens to channel N
static uint8_t seqInCh[SEQ_CH_COUNT] = {1, 2, 3, 4, 5, 6, 7, 8,
                                        9, 10, 11, 12, 13, 14, 15, 16};
static int16_t inRes = 0;                // leftover encoder counts (ENC5)

// octave shift of each sequence's playback (ENC1 in SEQMODE): +1 = one
// octave up, applied by the playhead only - recorded notes stay raw so the
// shift is changeable live.  Clamped to the -2..8 octave convention
static int8_t  seqOct[SEQ_CH_COUNT] = {0};
static int16_t octSRes = 0;              // leftover encoder counts (ENC1)
static int16_t gateSRes = 0;             // leftover encoder counts (ENC5 gate)
static int16_t velSRes = 0;              // leftover encoder counts (ENC6 vel)

// per-channel per-step stored chord; len 0 = unassigned (plays pending[])
static uint8_t stepChord[SEQ_CH_COUNT][STEP_MAX][CHORD_MAX];
static uint8_t stepChordLen[SEQ_CH_COUNT][STEP_MAX];

// step held for assignment; first key press clears the stored chord
// (assignCh = edit channel captured when the hold started)
static int8_t  assignStep = -1;
static uint8_t assignCh = 0;
static bool    assignStarted = false;

// an ACTIVE step was pressed during the current tap (candidate for disable;
// confirmed only on click, so a hold-to-edit does not clear the step)
static bool tapCandidate[STEP_MAX] = {false};

// --- step-edit attributes (SEQMODE radio on a MONO track) -------------------
// per-step flags (zero-init = none); probability stored as 100-value so
// zero-init means 100% (no boot init needed).  Track note shaping:
// seqVel = default velocity (boot 100, accent step = 127), seqGate = gate
// time in % of one step (100 = hold until the next step = today's behavior)
static uint8_t stepAtt[SEQ_CH_COUNT][STEP_MAX];
static uint8_t stepProbInv[SEQ_CH_COUNT][STEP_MAX];   // 100 - prob %
// DRUM tracks split accent/probability PER VOICE: mask bits say which of
// the 16 parts (notes 36..51) is accented / probability-gated in a step,
// the values hold each part's own 100-prob.  Zero-init: no accents, no
// probability gates (mask bit = attribute present, so 100% still reads as
// "armed" while always playing)
static uint8_t drumAccent[SEQ_CH_COUNT][STEP_MAX];
static uint8_t drumProbOn[SEQ_CH_COUNT][STEP_MAX];
static uint8_t drumProbInv[SEQ_CH_COUNT][STEP_MAX][16];
static uint8_t seqVel[SEQ_CH_COUNT] = {90,90,90,90,90,90,90,90,
                                       90,90,90,90,90,90,90,90};
static uint8_t seqGate[SEQ_CH_COUNT] = {100,100,100,100,100,100,100,100,
                                        100,100,100,100,100,100,100,100};
static uint32_t gateOffAt[SEQ_CH_COUNT] = {0};   // 0 = no pending note-off
static uint8_t advPrev[SEQ_CH_COUNT] = {          // per-track step of the
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // previous advance
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // (TIE/GLIDE carry)
static uint8_t fBlinkMask = 0;   // edit-channel attrs read by the playhead

// a step's attributes live and die with its content (both the MONO flags
// and the DRUM per-voice accent/probability masks)
static void stepAttClear(uint8_t ch, uint8_t st) {
  stepAtt[ch][st] = 0;
  stepProbInv[ch][st] = 0;       // back to 100%
  drumAccent[ch][st] = 0;
  drumProbOn[ch][st] = 0;
  for (uint8_t v = 0; v < 16; v++) drumProbInv[ch][st][v] = 0;
}

// source step on track x for listener position n: the last active step at
// or before n within x's pattern (cyclic over its length, so wrapping past
// the end uses the previous loop's chord), so "B at step 1, G at step 9"
// partitions the timeline exactly.  Returns -1 when no active step exists
static int8_t srcStepBack(uint8_t x, uint8_t n) {
  if (x >= SEQ_CH_COUNT || n >= STEP_MAX) return -1;
  uint8_t L = seqPatLenOf(x);
  if (L == 0) return -1;
  if (n >= L) n = (uint8_t)(n % L);        // listener past the end -> mapped
  for (uint8_t k = 0; k < L; k++) {
    uint8_t idx = (uint8_t)((n + L - k) % L);
    if (stepChordLen[x][idx] > 0) return (int8_t)idx;
  }
  return -1;
}

// clamp a sequence's octave shift so every stored note + 12*shift stays in
// 0..127 (the -2..8 octave convention spans notes 0..127): range derived
// from the stored min/max note - notes at C3 (60) give -5..+5, exactly the
// convention example; empty sequence -> keyboard octave as reference
static void seqOctClamp(uint8_t s) {
  int mn = -1, mx = -1;
  for (uint8_t st = 0; st < STEP_MAX; st++) {
    uint8_t *ch = stepChord[s][st];
    for (uint8_t i = 0; i < stepChordLen[s][st]; i++) {
      if (mn < 0 || ch[i] < mn) mn = ch[i];
      if (mx < 0 || ch[i] > mx) mx = ch[i];
    }
  }
  if (mn < 0) mn = mx = 12 * keyOctave() + 24;   // empty: keyboard's C
  int lo = -(mn / 12);                           // mn + 12*lo >= 0
  int hi = (127 - mx) / 12;                      // mx + 12*hi <= 127
  if (lo > hi) lo = hi = 0;
  if (seqOct[s] < lo) seqOct[s] = (int8_t)lo;
  if (seqOct[s] > hi) seqOct[s] = (int8_t)hi;
}

// timeline adaptation: when track x's step chords are written, every MONO
// or POLY sequence listening to x's channel (input channel = x+1) re-derives
// its ACTIVE steps from x's timeline - each takes the last chord at or before
// its position, so ch3 follows "the last played chord in the sequence",
// never the global last-played buffer while x has chords.  Content only:
// the listener's active/inactive state is untouched ("keeping the same
// active steps") and no-source steps keep their content.  Single level:
// copies do not re-trigger, so input-channel cycles cannot recurse
static void stepSyncFrom(uint8_t x) {
  if (x >= SEQ_CH_COUNT) return;
  for (uint8_t y = 0; y < SEQ_CH_COUNT; y++) {
    if (y == x || seqInCh[y] != x + 1) continue;
    uint8_t ltr = seqTrackOf(y);
    if (ltr != SEQ_TRACK_MONO && ltr != SEQ_TRACK_POLY) continue;  // DRUM never follows
    bool changed = false;
    for (uint8_t n = 0; n < STEP_MAX; n++) {
      if (stepChordLen[y][n] == 0) continue;   // never creates active steps
      int8_t idx = srcStepBack(x, n);
      if (idx < 0) continue;                   // no source: keep as-is
      for (uint8_t i = 0; i < CHORD_MAX; i++)
        stepChord[y][n][i] = stepChord[x][idx][i];
      stepChordLen[y][n] = stepChordLen[x][idx];
      changed = true;
    }
    if (changed) seqOctClamp(y);               // content changed -> re-clamp
  }
}

// external MIDI state replaced by per-sequence pending buffers (above)

// DRUM track: selected part (drum voice) for per-voice step sequencing;
// overlay pad N -> note 36+N (pad0 = C1 = 36, the GM kick, .. pad15 = D#2)
static uint8_t drumPart = 36;
static bool    drumPartSel = false;     // M1 pressed = part-select overlay

static uint8_t noteFor(uint8_t k, int8_t oct) {
  long n = 12L * oct + 24L + k;         // k = semitone (groupKeyboard is chromatic)
  if (n < 0) n = 0;
  if (n > 127) n = 127;
  return (uint8_t)n;
}

// keyboard gesture = snapshot of all currently held keys into the pending
// chord of every sequence the keyboard channel (KEY3) reaches: its own
// sequence N (keyboard channel N feeds sequence N) plus every sequence
// whose input channel listens to N (input-channel propagation, so a
// keyboard chord recorded on ch2 also feeds ch3 when ch3 input = ch2).
// In KEYMODE2 the snapshot is the chord engine's sounding notes instead
// (chords feed the sequencer like single notes)
static void updatePending() {
  bool chordMode = (keyBehaviorVariant() == 2);
  const uint8_t *cn = chordNotes();
  uint8_t ccount = chordCount();
  for (uint8_t s = 0; s < SEQ_CH_COUNT; s++) {
    if (s + 1 != keyCh && seqInCh[s] != keyCh) continue;
    pendingLen[s] = 0;
    if (chordMode) {
      for (uint8_t i = 0; i < ccount && pendingLen[s] < CHORD_MAX; i++)
        pending[s][pendingLen[s]++] = cn[i];
    } else {
      for (uint8_t i = 0; i < 12 && pendingLen[s] < CHORD_MAX; i++)
        if (keyActive[i]) pending[s][pendingLen[s]++] = keyNote[i];
    }
    histFeed(s);
  }
  monoApply();   // the held gesture changed: re-voice the MONO track
}

void keyPendingRefresh() { updatePending(); }

static void setOctave(int8_t o) {
  if (o < KEY_OCT_MIN) o = KEY_OCT_MIN;
  if (o > KEY_OCT_MAX) o = KEY_OCT_MAX;
  if (o == octave) return;
  int16_t delta = 12 * (o - octave);
  octave = o;
  // KEYMODE2: the chord engine owns its notes - shift and re-send them
  if (keyBehaviorVariant() == 2) {
    chordShift(delta);
    return;
  }
  // re-trigger every sounding key so no note is stranded in the old octave;
  // keys held under the mono voice stay silent (their keyNote follows so
  // the voice and the pending chord track the octave change)
  for (uint8_t k = 0; k < 12; k++) {
    if (keySounding[k]) {
      usbMIDI.sendNoteOff(keyNote[k], 0, keyCh);
      keyNote[k] = noteFor(k, octave);
      usbMIDI.sendNoteOn(keyNote[k], KEY_VEL, keyCh);
    } else if (keyActive[k]) {
      keyNote[k] = noteFor(k, octave);
    }
  }
  monoApply();   // ROOT/BASS voice re-voices with the octave change
}

// holding a step = assign mode: every note of this gesture replaces that
// step's stored chord (first note starts the new chord)
static void assignAppend(const uint8_t *notes, uint8_t n) {
  if (assignStep < 0 || n == 0) return;
  if (!assignStarted) {
    stepChordLen[assignCh][assignStep] = 0;
    stepAttClear(assignCh, assignStep);
    assignStarted = true;
  }
  // the live gesture lands pre-compensated for the sequence's octave shift,
  // so the step sounds exactly the note pressed; incoming (ext) and
  // tap-recorded content stays raw and is transposed at playback as usual
  int pre = -12 * (int)seqOct[assignCh];
  for (uint8_t i = 0; i < n && stepChordLen[assignCh][assignStep] < CHORD_MAX; i++) {
    int v = (int)notes[i] + pre;
    if (v < 0) v = 0;
    else if (v > 127) v = 127;
    stepChord[assignCh][assignStep][stepChordLen[assignCh][assignStep]++] = (uint8_t)v;
  }
  seqOctClamp(assignCh);
  stepSyncFrom(assignCh);
}

int keyPadPress(uint8_t idx) {
  if (idx >= 12) return -1;
  monoApply();   // reconcile with the keyboard channel's track first
  // KEYMODE2: the chord engine owns the gesture (quality/diatonic chords,
  // latch, KeyLock) - it sends its own notes and feeds the pending chord
  // (muted while the mono voice is engaged; the state still builds)
  if (keyBehaviorVariant() == 2) {
    keyNote[idx] = noteFor(idx, octave);
    keyActive[idx] = true;
    keySounding[idx] = false;   // the chord engine, not us, sends the sound
    int r = chordPadPress(idx, keyNote[idx]);
    assignAppend(chordNotes(), chordCount());
    return r;
  }
  // scale filter: an out-of-scale key is silent and untracked (KeyLock
  // bypasses it); Chromatic (the default) has every key in scale = no change
  if (keyScale() != 0 && !chordKeyLock() && !keyIsInScale(idx)) return -1;
  keyNote[idx] = noteFor(idx, octave);
  // MONO track engaged: no raw note - the root/bass voice or the playhead
  // arp sounds instead (updatePending below re-voices it)
  if (!monoEng) {
    usbMIDI.sendNoteOn(keyNote[idx], KEY_VEL, keyCh);
    keySounding[idx] = true;
  }
  keyActive[idx] = true;
  updatePending();
  assignAppend(&keyNote[idx], 1);
  return monoEng ? -1 : keyNote[idx];
}

int keyPadRelease(uint8_t idx) {
  if (idx >= 12 || !keyActive[idx]) return -1;
  keyActive[idx] = false;
  // this key's own single note (only ever sent outside chord mode): covers
  // a mode change while the key was held - never a stuck note
  if (keySounding[idx]) {
    usbMIDI.sendNoteOff(keyNote[idx], 0, keyCh);
    keySounding[idx] = false;
  }
  if (keyBehaviorVariant() == 2) chordPadRelease(idx);
  // all keys up while assigning = that chord is finished; the next press
  // starts a fresh replacement instead of appending
  if (assignStep >= 0) {
    bool any = false;
    for (uint8_t k = 0; k < 12; k++) any = any || keyActive[k];
    if (!any && extHeldLen[assignCh] == 0) assignStarted = false;
  }
  monoApply();   // the held gesture shrank: re-voice / release the voice
  return keyNote[idx];
}

void keyOctaveAdjust(int16_t counts) {
  octRes += counts;
  int8_t step = 0;
  while (octRes >= KEY_COUNTS_OCT) { octRes -= KEY_COUNTS_OCT; step++; }
  while (octRes <= -KEY_COUNTS_OCT) { octRes += KEY_COUNTS_OCT; step--; }
  if (step) setOctave((int8_t)(octave + step));
}

int keyOctave() { return octave; }

// chord currently HELD for sequence s - the live gesture, never the
// kept-after-release pending buffer: the keyboard when its channel reaches
// s (held keys in single mode; sounding chord incl. latch in KEYMODE2),
// else external notes still held on s's input channel.  Returns 0 when
// nothing is held (or the held chord is empty), *out then undefined
static uint8_t liveHeldChord(uint8_t s, const uint8_t **out) {
  if (keyCh == s + 1 || seqInCh[s] == keyCh) {
    if (keyBehaviorVariant() == 2) {
      if (chordSounding()) {
        *out = chordNotes();
        return chordCount();
      }
    } else {
      static uint8_t live[CHORD_MAX];   // static: pointer handed to caller
      uint8_t n = 0;
      for (uint8_t k = 0; k < 12 && n < CHORD_MAX; k++)
        if (keyActive[k]) live[n++] = keyNote[k];
      if (n > 0) { *out = live; return n; }
    }
  }
  if (extHeldLen[s] > 0) { *out = extHeld[s]; return extHeldLen[s]; }
  return 0;
}

int stepChordPress(uint8_t step) {
  if (step >= STEP_MAX) return -1;
  // pressing an ACTIVE step makes no sound yet: if the tap completes as a
  // click it is disabled; if it turns into a hold it means "edit", not off
  if (stepChordLen[seqEditCh][step] > 0) {
    tapCandidate[step] = true;
    return -1;
  }
  tapCandidate[step] = false;
  // pressing an unassigned step RECORDS a chord into it - silently: the
  // chord is only sent when the playhead (green dot) reads the step during
  // playback.  POLY records the latest pending chord.  MONO records, in
  // order of priority: the input track's timeline chord (last recorded
  // step at or before this position - "the last played chord in the
  // sequence", B at step 1 / G at step 9 partitions 1-8 / 9-16), then the
  // step history (live-played while ch2 has no recorded chords), then the
  // latest pending chord.  POLY fed from another channel (input != own)
  // mirrors that chain but a gesture still HELD (keyboard keys, latched
  // chord, external notes) wins over the timeline, so holding a chord and
  // pressing the step inserts it
  const uint8_t *src = pending[seqEditCh];
  uint8_t n = pendingLen[seqEditCh];
  if (seqTrack() == SEQ_TRACK_MONO) {
    uint8_t x = seqInCh[seqEditCh] - 1;          // track this seq listens to
    int8_t idx = (x < SEQ_CH_COUNT && x != seqEditCh) ? srcStepBack(x, step) : -1;
    if (idx >= 0) {
      src = stepChord[x][idx];
      n = stepChordLen[x][idx];
    } else if (histLen[seqEditCh][step] > 0) {
      src = histChord[seqEditCh][step];
      n = histLen[seqEditCh][step];
    }
  } else if (seqTrack() == SEQ_TRACK_POLY &&
             seqInCh[seqEditCh] != seqEditCh + 1) {
    const uint8_t *held;
    uint8_t hn = liveHeldChord(seqEditCh, &held);
    if (hn > 0) {
      src = held;                     // held/latched chord insert
      n = hn;
    } else {
      uint8_t x = seqInCh[seqEditCh] - 1;        // the INPUT track
      int8_t idx = srcStepBack(x, step);
      if (idx >= 0) {
        src = stepChord[x][idx];
        n = stepChordLen[x][idx];
      } else if (histLen[seqEditCh][step] > 0) {
        src = histChord[seqEditCh][step];
        n = histLen[seqEditCh][step];
      }
      // else: input track empty - keep pending (last gesture), like MONO
    }
  }
  if (n == 0) return 0;
  for (uint8_t i = 0; i < n && i < CHORD_MAX; i++)
    stepChord[seqEditCh][step][i] = src[i];
  stepChordLen[seqEditCh][step] = n;
  stepAttClear(seqEditCh, step);   // fresh content starts attribute-free
  seqOctClamp(seqEditCh);
  stepSyncFrom(seqEditCh);
  return n;
}

void stepAssignSet(int8_t step) {
  // a hold means "edit", never "disable": drop the tap candidate
  if (step >= 0 && step < STEP_MAX) {
    tapCandidate[step] = false;
    assignCh = seqEditCh;    // the channel being edited when the hold began
  }
  assignStep = step;
  assignStarted = false;
}

// tap on an active step completed (EV_CLICKED): disable it now
int stepChordClick(uint8_t step) {
  if (step >= STEP_MAX || !tapCandidate[step]) return -1;
  tapCandidate[step] = false;
  if (stepChordLen[seqEditCh][step] == 0) return -1;
  stepChordLen[seqEditCh][step] = 0;
  stepAttClear(seqEditCh, step);   // a dead step carries no tie/prob over
  seqOctClamp(seqEditCh);     // content shrank: shift range may have grown
  return 0;
}

bool stepIsActive(uint8_t step) {
  if (step >= STEP_MAX) return false;
  // DRUM track: the step view shows the SELECTED part's pattern (does this
  // step contain the chosen voice?), not the union of all parts - otherwise
  // selecting another part would still display part 1's sequence
  if (seqTrack() == SEQ_TRACK_DRUM) {
    uint8_t *ch = stepChord[seqEditCh][step];
    for (uint8_t i = 0; i < stepChordLen[seqEditCh][step]; i++)
      if (ch[i] == drumPart) return true;
    return false;
  }
  return stepChordLen[seqEditCh][step] > 0;
}

// toggle an attribute on the EDIT channel's step.  MONO tracks use the
// per-step flags (a PROB toggle-on stores the clipboard value, inverted);
// DRUM tracks toggle it for the SELECTED PART only - each voice carries
// its own accent and probability.  Returns the new state so the caller
// can show "14 kick acc on" feedback
bool stepAttrToggle(uint8_t step, uint8_t att, uint8_t probVal) {
  if (step >= STEP_MAX || !att) return false;
  if (seqTrack() == SEQ_TRACK_DRUM) {
    uint8_t v = drumPart;              // 36..51 -> voice index 0..15
    if (v < 36 || v >= 52) return false;
    uint8_t bit = (uint8_t)(1 << (v - 36));
    if (att & SEQ_ATT_ACCENT) {
      drumAccent[seqEditCh][step] ^= bit;
      return (drumAccent[seqEditCh][step] & bit) != 0;
    }
    if (att & SEQ_ATT_PROB) {
      if (drumProbOn[seqEditCh][step] & bit) {
        drumProbOn[seqEditCh][step] &= (uint8_t)~bit;
        drumProbInv[seqEditCh][step][v - 36] = 0;
        return false;
      }
      if (probVal > 100) probVal = 100;
      drumProbOn[seqEditCh][step] |= bit;
      drumProbInv[seqEditCh][step][v - 36] = 100 - probVal;
      return true;
    }
    return false;                      // TIE/GLIDE never apply to drums
  }
  uint8_t *a = &stepAtt[seqEditCh][step];
  if (*a & att) {
    *a &= (uint8_t)~att;
    return false;
  }
  *a |= att;
  if (att & SEQ_ATT_PROB) {
    if (probVal > 100) probVal = 100;
    stepProbInv[seqEditCh][step] = 100 - probVal;
  }
  return true;
}

// attribute mask of the EDIT channel's step (step LED pulse / display).
// On DRUM tracks the view follows the SELECTED PART: its accent / its own
// probability - switching the part select re-maps the yellow pulse live
uint8_t stepAttrs(uint8_t step) {
  if (step >= STEP_MAX) return 0;
  if (seqTrack() == SEQ_TRACK_DRUM) {
    uint8_t v = drumPart;
    if (v < 36 || v >= 52) return 0;
    uint8_t bit = (uint8_t)(1 << (v - 36));
    uint8_t m = 0;
    if (drumAccent[seqEditCh][step] & bit) m |= SEQ_ATT_ACCENT;
    if (drumProbOn[seqEditCh][step] & bit) m |= SEQ_ATT_PROB;
    return m;
  }
  return stepAtt[seqEditCh][step];
}

// notes the sequencer (playhead) currently has sounding, per channel
static uint8_t seqNotes[SEQ_CH_COUNT][CHORD_MAX];
static uint8_t seqLen[SEQ_CH_COUNT] = {0};

// ARP phase per sequence (MONO/ARP playback); reset on transport stop
static uint8_t arpIdx[SEQ_CH_COUNT] = {0};
// ARP phase of the LIVE keyboard voice (MONO+ARP on the keyboard channel)
static uint8_t liveArpIdx = 0;

// chord-root detection: try each chord note as candidate root (lowest
// first), accept when every interval above it (mod 12) fits a known chord
// template; fallback = lowest note
static uint8_t rootOf(const uint8_t *ch, uint8_t n) {
  static const uint8_t t0[] = {0,4,7}, t1[] = {0,3,7}, t2[] = {0,3,6},
                       t3[] = {0,4,6}, t4[] = {0,5,7}, t5[] = {0,2,7},
                       t6[] = {0,7},   t7[] = {0,4,7,11}, t8[] = {0,3,7,10},
                       t9[] = {0,4,7,10}, t10[] = {0,4,7,9}, t11[] = {0,3,7,9};
  static const uint8_t *tpl[12] = {t0,t1,t2,t3,t4,t5,t6,t7,t8,t9,t10,t11};
  static const uint8_t tlen[12] = {3,3,3,3,3,3,2,4,4,4,4,4};

  uint8_t lo = ch[0];
  for (uint8_t i = 1; i < n; i++) if (ch[i] < lo) lo = ch[i];

  // candidates in ascending pitch order (root position matches first)
  uint8_t cand[CHORD_MAX];
  for (uint8_t i = 0; i < n; i++) cand[i] = ch[i];
  for (uint8_t i = 1; i < n; i++) {
    uint8_t v = cand[i], j = i;
    while (j > 0 && cand[j - 1] > v) { cand[j] = cand[j - 1]; j--; }
    cand[j] = v;
  }

  for (uint8_t ci = 0; ci < n; ci++) {
    uint8_t iv[CHORD_MAX], in = 0;
    for (uint8_t i = 0; i < n; i++) {
      int d = (int)ch[i] - (int)cand[ci];
      uint8_t v = (uint8_t)(((d % 12) + 12) % 12);
      bool dup = false;
      for (uint8_t j = 0; j < in; j++) if (iv[j] == v) dup = true;
      if (!dup) iv[in++] = v;
    }
    for (uint8_t t = 0; t < 12; t++) {
      bool fits = true;
      for (uint8_t i = 0; i < in && fits; i++) {
        bool hit = false;
        for (uint8_t j = 0; j < tlen[t]; j++) if (iv[i] == tpl[t][j]) hit = true;
        if (!hit) fits = false;
      }
      if (fits) return cand[ci];
    }
  }
  return lo;
}

// ---------------------------------------------------------------------------
// live keyboard voice: the keyboard channel's MONO track processes what you
// play in real time (ROOT = root note, BASS = lowest, ARP = next chord note
// per playhead step); raw notes stay suppressed while engaged.  Recording is
// untouched - the gesture still fills the pending chord of every listener
// ---------------------------------------------------------------------------

static bool monoEngaged() { return seqTrackOf(keyCh - 1) == SEQ_TRACK_MONO; }
static uint8_t monoType() { return seqMonoTypeOf(keyCh - 1); }

// held gesture as the voice source: KEYMODE2 uses the chord engine's
// sounding notes (latch included), single mode the held keys (ascending)
static uint8_t monoSrc(const uint8_t **out) {
  if (keyBehaviorVariant() == 2) {
    if (chordSounding()) { *out = chordNotes(); return chordCount(); }
    return 0;
  }
  static uint8_t buf[12];   // static: pointer handed to the caller
  uint8_t n = 0;
  for (uint8_t k = 0; k < 12; k++)
    if (keyActive[k]) buf[n++] = keyNote[k];
  if (!n) return 0;
  *out = buf;
  return n;
}

static void monoVoiceOff() {
  if (monoOn) {
    usbMIDI.sendNoteOff(monoNote, 0, keyCh);
    monoOn = false;
  }
}

// POLY -> MONO while keys are held: the raw notes stop...
static void cutRaw() {
  for (uint8_t k = 0; k < 12; k++)
    if (keySounding[k]) {
      usbMIDI.sendNoteOff(keyNote[k], 0, keyCh);
      keySounding[k] = false;
    }
}

// ...and MONO -> POLY while keys are held: they sound raw again (single
// mode only - in KEYMODE2 the chord engine re-sends via chordMute(false))
static void strikeRaw() {
  if (keyBehaviorVariant() == 2) return;
  for (uint8_t k = 0; k < 12; k++)
    if (keyActive[k]) {
      keyNote[k] = noteFor(k, octave);
      usbMIDI.sendNoteOn(keyNote[k], KEY_VEL, keyCh);
      keySounding[k] = true;
    }
}

static void monoLiveUpdate() {
  if (!monoEng) return;
  uint8_t t = monoType();
  if (t != monoLastT) {          // TYPE knob turned mid-hold: drop the voice
    monoVoiceOff();
    monoLastT = t;
  }
  if (t == SEQ_MONO_ARP) return; // the playhead advances the arp note only
  const uint8_t *src;
  uint8_t n = monoSrc(&src);
  if (!n) { monoVoiceOff(); return; }
  uint8_t want;
  if (t == SEQ_MONO_BASS) {
    want = src[0];
    for (uint8_t i = 1; i < n; i++) if (src[i] < want) want = src[i];
  } else {
    want = rootOf(src, n);       // SEQ_MONO_ROOT
  }
  if (monoOn && monoNote == want) return;   // gesture kept the same note
  monoVoiceOff();
  usbMIDI.sendNoteOn(want, KEY_VEL, keyCh);
  monoNote = want;
  monoOn = true;
}

// reconcile the voice with the keyboard channel's TRACK/TYPE and the held
// gesture; called from the loop poll, every gesture and every octave change
static void monoApply() {
  bool eng = monoEngaged();
  if (eng != monoEng) {
    monoEng = eng;
    if (eng) {
      cutRaw();
      chordMute(true);
    } else {
      monoVoiceOff();
      chordMute(false);
      strikeRaw();
    }
  }
  if (eng) monoLiveUpdate();
}

void keyMonoPoll() { monoApply(); }

// release one sequence's sounding notes and cancel its gate timer
static void releaseCh(uint8_t c) {
  for (uint8_t i = 0; i < seqLen[c]; i++)
    usbMIDI.sendNoteOff(seqNotes[c][i], 0, c + 1);
  seqLen[c] = 0;
  gateOffAt[c] = 0;
}

static void seqRelease() {
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) releaseCh(c);
}

// playback note = stored note + 12 * the sequence's octave shift, kept in
// 0..127 (seqOctClamp keeps the shift inside the -2..8 convention anyway)
static uint8_t shNote(uint8_t n, int8_t sh) {
  int v = (int)n + 12 * (int)sh;
  if (v < 0) v = 0;
  if (v > 127) v = 127;
  return (uint8_t)v;
}

// playhead tick: every track steps its OWN position (its pattern length or
// its looped bar), so lengths may differ (polymeter).  start = transport
// (re)start: every track snaps to its range head first.  MONO sequences
// transform their chord at playback (ROOT = root note, BASS = lowest note,
// ARP = next chord note), then every channel applies its own octave shift
// (ENC1) - and honors the step-edit attributes: TIE bridges into the next
// step, GLIDE overlaps the notes (legato), ACCENT plays 127, PROB gates by
// its stored chance, the track's gate time cuts the note mid-step
void seqStepAdvance(bool start) {
  uint32_t now = millis();
  // gate length is a % of ONE step: measure the gap between advances so
  // tempo changes and the external clock are followed live
  static uint32_t lastAdvMs = 0;
  uint32_t stepDur = 0;
  if (lastAdvMs) {
    stepDur = now - lastAdvMs;
    if (stepDur > 2000) stepDur = 2000;   // paused transport gap
  }
  lastAdvMs = now;
  // position of every track for this tick (own window: pattern length, or
  // the looped bar), plus whether it is inside it (= allowed to sound)
  uint8_t lo[SEQ_CH_COUNT], hi[SEQ_CH_COUNT];
  bool inRng[SEQ_CH_COUNT];
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    seqPlayRange(c, &lo[c], &hi[c]);
    if (start || chStep[c] < lo[c] || chStep[c] >= hi[c]) {
      chStep[c] = lo[c];                  // start, or length/loop changed
    } else {
      chStep[c]++;                        // one step forward inside range
      if (chStep[c] >= hi[c]) chStep[c] = lo[c];   // wrap at its own end
    }
    inRng[c] = (hi[c] > lo[c]) && chStep[c] >= lo[c] && chStep[c] < hi[c];
  }
  // per-channel release honoring the previous step's attributes: TIE keeps
  // the note sounding (no release, and the retrigger below is skipped),
  // GLIDE defers its note-off until after the new note-on; a track that
  // left its window (length shrunk, loop moved) releases right here
  bool tied[SEQ_CH_COUNT], defer[SEQ_CH_COUNT];
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    tied[c] = defer[c] = false;
    if (!inRng[c]) { releaseCh(c); advPrev[c] = 0xFF; continue; }
    uint8_t pf = (advPrev[c] < STEP_MAX && seqTrackOf(c) == SEQ_TRACK_MONO)
                   ? stepAtt[c][advPrev[c]] : 0;
    if (pf & SEQ_ATT_TIE) { tied[c] = true; continue; }
    if (pf & SEQ_ATT_GLIDE) { defer[c] = true; continue; }
    releaseCh(c);
  }
  // carry the current pending chords forward into the new position: the
  // chord played during steps 1-8 is the "actual chord" of step 8 even
  // though it was fed while the playhead sat on step 1
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) histAt(c, chStep[c]);
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    if (!inRng[c]) continue;              // parked outside its window
    if (tied[c]) continue;              // merged note keeps sounding
    uint8_t stp = chStep[c];
    uint8_t len = stepChordLen[c][stp];
    // FOLLOW: the own active step gates the rhythm, the NOTES are read
    // from the feeding track's current playhead (MONO/POLY listener with
    // its input pointing at another track; no feeder / DRUM = unchanged)
    bool follow = seqFollowOf(c) && seqTrackOf(c) != SEQ_TRACK_DRUM &&
                  seqInCh[c] != (uint8_t)(c + 1);
    uint8_t fstp = 0;                  // feeder's step while following
    if (follow) {
      uint8_t fx = (uint8_t)(seqInCh[c] - 1);
      bool own = len > 0;              // activation stays the listener's
      len = 0;
      if (own && inRng[fx]) {          // feeder must be inside its range
        fstp = chStep[fx];
        len = stepChordLen[fx][fstp];
        if (len == 0) {
          // empty feeder step: hold its LAST chord/note - scan backward,
          // cyclic inside the feeder's play range (a looped bar only
          // reads its own tail), until content shows up
          uint8_t N = (uint8_t)(hi[fx] - lo[fx]);
          uint8_t n = (uint8_t)(fstp - lo[fx]);
          for (uint8_t k = 1; k < N && len == 0; k++) {
            uint8_t j = (uint8_t)(lo[fx] + (n + N - k) % N);
            if (stepChordLen[fx][j] > 0) {
              fstp = j;
              len = stepChordLen[fx][j];
            }
          }
        }
      }
    }
    bool mono = (seqTrackOf(c) == SEQ_TRACK_MONO);
    bool drumTrack = (seqTrackOf(c) == SEQ_TRACK_DRUM);
    // MONO uses the per-step flags; DRUM rolls accent/probability per voice
    uint8_t att = mono ? stepAtt[c][stp] : 0;
    bool plays = (len > 0) && !seqIsMuted(c);
    // probability gate (MONO only): a miss silences the step this pass
    if (plays && (att & SEQ_ATT_PROB) &&
        (uint8_t)random(100) >= (uint8_t)(100 - stepProbInv[c][stp]))
      plays = false;
    if (!plays) {
      releaseCh(c);                     // also ends a deferred glide
      continue;
    }
    uint8_t *ch = follow ? stepChord[seqInCh[c] - 1][fstp]
                         : stepChord[c][stp];
    int8_t sh = seqOct[c];
    if (mono) {
      uint8_t note;
      if (seqMonoTypeOf(c) == SEQ_MONO_BASS) {
        note = ch[0];
        for (uint8_t i = 1; i < len; i++) if (ch[i] < note) note = ch[i];
      } else if (seqMonoTypeOf(c) == SEQ_MONO_ARP) {
        note = ch[arpIdx[c] % len];
        arpIdx[c]++;
      } else {
        note = rootOf(ch, len);         // SEQ_MONO_ROOT
      }
      note = shNote(note, sh);
      uint8_t vel = (att & SEQ_ATT_ACCENT) ? 127 : seqVel[c];
      if (defer[c]) {
        // GLIDE: new note-on BEFORE the old note-off (legato overlap)
        usbMIDI.sendNoteOn(note, vel, c + 1);
        for (uint8_t i = 0; i < seqLen[c]; i++)
          usbMIDI.sendNoteOff(seqNotes[c][i], 0, c + 1);
        seqLen[c] = 0;
        gateOffAt[c] = 0;
      } else {
        usbMIDI.sendNoteOn(note, vel, c + 1);
      }
      seqNotes[c][0] = note;
      seqLen[c] = 1;
      // gate timer: TIE/GLIDE hold to the boundary by definition
      if (stepDur && seqGate[c] < 100 && !(att & (SEQ_ATT_TIE | SEQ_ATT_GLIDE)))
        gateOffAt[c] = now + (stepDur * (uint32_t)seqGate[c]) / 100;
    } else {
      if (defer[c]) releaseCh(c);       // track flipped mid-play: cut old
      for (uint8_t i = 0; i < len && i < CHORD_MAX; i++) {
        uint8_t raw = ch[i];
        // DRUM voices muted in the ENCMODE overlay stay silent (match the
        // stored note, like the part select - the octave shift applies after)
        if (drumTrack && drumVoiceMuted(raw)) continue;
        uint8_t dvel = KEY_VEL;
        // per-voice accent + probability (DRUM only; POLY ignores the masks)
        uint8_t v = (drumTrack && raw >= 36 && raw < 52)
                      ? (uint8_t)(raw - 36) : 0xFF;
        if (v != 0xFF) {
          if ((drumProbOn[c][stp] >> v) & 1) {
            if ((uint8_t)random(100) >=
                (uint8_t)(100 - drumProbInv[c][stp][v]))
              continue;                 // this voice misses its roll
          }
          if ((drumAccent[c][stp] >> v) & 1) dvel = 127;
        }
        uint8_t note = shNote(raw, sh);
        seqNotes[c][seqLen[c]] = note;
        usbMIDI.sendNoteOn(note, dvel, c + 1);
        seqLen[c]++;
      }
    }
  }
  // F-row blink: attributes of the edit channel's step the playhead reads
  // (MONO = the step flags; DRUM = any part carrying accent/probability)
  fBlinkMask = 0;
  if (inRng[seqEditCh]) {
    uint8_t fTrk = seqTrackOf(seqEditCh);
    uint8_t fstp = chStep[seqEditCh];
    if (fTrk == SEQ_TRACK_MONO) {
      fBlinkMask = stepChordLen[seqEditCh][fstp] > 0
                     ? stepAtt[seqEditCh][fstp] : 0;
    } else if (fTrk == SEQ_TRACK_DRUM) {
      if (stepChordLen[seqEditCh][fstp] > 0) {
        if (drumAccent[seqEditCh][fstp]) fBlinkMask |= SEQ_ATT_ACCENT;
        if (drumProbOn[seqEditCh][fstp]) fBlinkMask |= SEQ_ATT_PROB;
      }
    }
  }
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++)
    advPrev[c] = inRng[c] ? chStep[c] : 0xFF;
  // live keyboard arp (MONO+ARP on the keyboard channel): one note of the
  // held gesture per step, transport-gated - recorded steps above play
  // independently, the previous arp note is released first
  if (monoEngaged() && monoType() == SEQ_MONO_ARP) {
    const uint8_t *src;
    uint8_t n = monoSrc(&src);
    monoVoiceOff();
    if (n > 0) {
      uint8_t note = src[liveArpIdx++ % n];
      usbMIDI.sendNoteOn(note, KEY_VEL, keyCh);
      monoNote = note;
      monoOn = true;
    }
  }
}

void seqStepStop() {
  seqRelease();
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++)
    advPrev[c] = 0xFF;               // no carry-over into the next run
  fBlinkMask = 0;
  // the live arp is step-driven: silence it with the transport (the
  // ROOT/BASS voice follows the keys and keeps sounding)
  if (monoEngaged() && monoType() == SEQ_MONO_ARP) monoVoiceOff();
  liveArpIdx = 0;
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) arpIdx[c] = 0;
}

// playhead pad of the edit WINDOW (0..15): the edit track's head mapped
// into the 16-step bar shown on the pads - 0xFF when the head is outside
// that bar (another bar playing) or outside its play range (silent)
uint8_t seqStepPlayPad(void) {
  uint8_t lo, hi;
  seqPlayRange(seqEditCh, &lo, &hi);
  uint8_t p = chStep[seqEditCh];
  if (hi <= lo || p < lo || p >= hi) return 0xFF;
  uint8_t base = seqStepViewBase();
  if (p < base || p >= (uint8_t)(base + STEP_COUNT)) return 0xFF;
  return (uint8_t)(p - base);
}

// ghost pad: the head's position WITHIN ITS OWN bar, mirrored onto the
// pads even while another bar is shown - the hooks paint it dark blue so
// time keeps marching while you edit a bar the playback is not reading
uint8_t seqStepPlayGhost(void) {
  uint8_t lo, hi;
  seqPlayRange(seqEditCh, &lo, &hi);
  uint8_t p = chStep[seqEditCh];
  if (hi <= lo || p < lo || p >= hi) return 0xFF;
  return (uint8_t)(p % STEP_COUNT);
}

// bar (0..3) the edit track's playhead currently reads, 0xFF when it is
// outside its window (silent track) - the SEQ3 bar LEDs pulse on it so the
// dot walks C D E F as playback crosses the bars
uint8_t seqPlayBar(void) {
  uint8_t lo, hi;
  seqPlayRange(seqEditCh, &lo, &hi);
  uint8_t p = chStep[seqEditCh];
  if (hi <= lo || p < lo || p >= hi) return 0xFF;
  return (uint8_t)(p / STEP_COUNT);
}

// gate timers fire from loop context (never the ISR): note-off once the
// track's gate time (a % of the measured step) has elapsed
void seqGatePoll() {
  uint32_t now = millis();
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    if (gateOffAt[c] && (int32_t)(now - gateOffAt[c]) >= 0) releaseCh(c);
  }
}

// attributes the playhead is reading on the edit channel right now
// (F-row blink mask; zero on non-MONO tracks or between attributed steps)
uint8_t seqFMask() { return fBlinkMask; }

uint8_t seqSoundingCount(uint8_t ch) {
  if (ch < 1 || ch > SEQ_CH_COUNT) return 0;
  return seqLen[ch - 1];
}

uint8_t seqSoundingNote(uint8_t ch, uint8_t i) {
  if (ch < 1 || ch > SEQ_CH_COUNT || i >= seqLen[ch - 1]) return 0;
  return seqNotes[ch - 1][i];
}

uint8_t seqEditChannel() { return seqEditCh + 1; }

void seqSetEditChannel(uint8_t ch) {
  // focus switch only: sounding notes and pending chords stay untouched
  // (each sequence owns its own input state)
  if (ch < 1 || ch > SEQ_CH_COUNT || ch == seqEditCh + 1) return;
  seqEditCh = ch - 1;
  inRes = 0;     // partial ENC5 turn belongs to the previous focus
  octSRes = 0;   // partial ENC1 turn belongs to the previous focus
  gateSRes = 0;
  velSRes = 0;
}

uint8_t seqInputChannel() { return seqInCh[seqEditCh]; }

// ENC1 in SEQMODE: octave shift of the edit sequence's playback, 4 counts
// = 1 octave, clamped to the -2..8 convention, no wrap.  The range is
// recalculated from the stored notes on every content change (notes at C3
// -> -5..+5), so the displayed range always fits the convention
void seqOctaveShiftAdjust(int16_t counts) {
  octSRes += counts;
  int16_t st = 0;
  while (octSRes >= 4) { octSRes -= 4; st++; }
  while (octSRes <= -4) { octSRes += 4; st--; }
  if (!st) return;
  seqOctClamp(seqEditCh);            // current content bounds first
  int v = (int)seqOct[seqEditCh] + st;
  if (v < -60) v = -60;              // keep the cast safe; clamp decides
  if (v > 60) v = 60;
  seqOct[seqEditCh] = (int8_t)v;
  seqOctClamp(seqEditCh);
}

int seqOctaveShift() { return seqOct[seqEditCh]; }

// ENC5 in SEQMODE (MONO + step-edit): gate time of the edit sequence as a
// % of one measured step, 4 counts = 5%, clamped 10..100 (100 = hold until
// the next step).  Track parameters survive an erase (like seqOct)
void seqGateAdjust(int16_t counts) {
  gateSRes += counts;
  int16_t st = 0;
  while (gateSRes >= 4) { gateSRes -= 4; st++; }
  while (gateSRes <= -4) { gateSRes += 4; st--; }
  if (!st) return;
  int v = (int)seqGate[seqEditCh] + st * 5;
  if (v < 10) v = 10;
  if (v > 100) v = 100;
  seqGate[seqEditCh] = (uint8_t)v;
}

int seqGatePct() { return seqGate[seqEditCh]; }

// ENC6 in SEQMODE (MONO + step-edit): default note velocity of the edit
// sequence, 4 counts = 1, clamped 1..127 (accent steps stay 127)
void seqVelocityAdjust(int16_t counts) {
  velSRes += counts;
  int16_t st = 0;
  while (velSRes >= 4) { velSRes -= 4; st++; }
  while (velSRes <= -4) { velSRes += 4; st--; }
  if (!st) return;
  int v = (int)seqVel[seqEditCh] + st;
  if (v < 1) v = 1;
  if (v > 127) v = 127;
  seqVel[seqEditCh] = (uint8_t)v;
}

int seqVelocity() { return seqVel[seqEditCh]; }

// ENC5 in SEQMODE3: input channel of the edit sequence, 4 counts = 1 step,
// clamped 1..16, no wrap
void seqInputChannelAdjust(int16_t counts) {
  inRes += counts;
  int16_t step = 0;
  while (inRes >= 4) { inRes -= 4; step++; }
  while (inRes <= -4) { inRes += 4; step--; }
  if (!step) return;
  int16_t v = (int16_t)seqInCh[seqEditCh] + step;
  if (v < 1) v = 1;
  if (v > 16) v = 16;
  seqInCh[seqEditCh] = (uint8_t)v;
  // backfill: now listening to channel v -> inherit the pending chord of
  // track v (e.g. set ch3 input = ch2 after recording track2: ch3 gets
  // track2's chord, so its steps can be activated right away)
  uint8_t src = (uint8_t)v - 1;
  pendingLen[seqEditCh] = pendingLen[src];
  for (uint8_t i = 0; i < pendingLen[src] && i < CHORD_MAX; i++)
    pending[seqEditCh][i] = pending[src][i];
  // the input source just changed: drop stale step history so MONO
  // activation falls back to the backfilled chord, not an old run's chords
  for (uint8_t st = 0; st < STEP_MAX; st++) histLen[seqEditCh][st] = 0;
}

// external MIDI input: a note feeds EVERY sequence whose input channel
// matches, independently of the edit focus - so setting ch3's input to ch2
// makes ch3 receive exactly what ch2 receives (identical pending chords ->
// activating the same steps stores the same sequence in both)
void extNoteOn(uint8_t ch, uint8_t note) {
  for (uint8_t s = 0; s < SEQ_CH_COUNT; s++) {
    if (seqInCh[s] != ch) continue;
    bool held = false;
    for (uint8_t i = 0; i < extHeldLen[s]; i++)
      if (extHeld[s][i] == note) held = true;
    if (held) continue;
    if (extHeldLen[s] == 0) pendingLen[s] = 0;   // fresh gesture = new chord
    if (extHeldLen[s] < CHORD_MAX) extHeld[s][extHeldLen[s]++] = note;
    if (pendingLen[s] < CHORD_MAX) pending[s][pendingLen[s]++] = note;
    histFeed(s);
    // holding a step = assign mode: input also appends to the edit step
    if (assignStep >= 0 && s == seqEditCh) {
      if (!assignStarted) {
        stepChordLen[assignCh][assignStep] = 0;
        assignStarted = true;
      }
      if (stepChordLen[assignCh][assignStep] < CHORD_MAX)
        stepChord[assignCh][assignStep][stepChordLen[assignCh][assignStep]++] = note;
      seqOctClamp(assignCh);
      stepSyncFrom(assignCh);
    }
  }
}

void extNoteOff(uint8_t ch, uint8_t note) {
  (void)ch;
  // release bookkeeping for every sequence (matched by note, robust against
  // input-channel changes while held); pending chords are kept after release
  for (uint8_t s = 0; s < SEQ_CH_COUNT; s++) {
    for (uint8_t i = 0; i < extHeldLen[s]; i++) {
      if (extHeld[s][i] != note) continue;
      extHeldLen[s]--;
      extHeld[s][i] = extHeld[s][extHeldLen[s]];
      break;
    }
  }
  if (assignStep >= 0) {
    bool any = false;
    for (uint8_t k = 0; k < 12; k++) any = any || keyActive[k];
    if (!any && extHeldLen[assignCh] == 0) assignStarted = false;
  }
}

// --- ENC mode output mute (hold M2) ----------------------------------------
static bool encMuteSel = false;                 // M2 held while in ENCMODE
static bool seqMute[SEQ_CH_COUNT] = {false};    // playback output gated

void encMuteHold(bool on) { encMuteSel = on; }

// composite: the raw flag can outlive a mid-hold mode switch - only the
// ENC mode shows the overlay (modeOnEvent clears the flag on a mode change)
bool encMuteActive() { return encMuteSel && modeBase == MODE_ENC; }

bool seqIsMuted(uint8_t seq) { return seq < SEQ_CH_COUNT && seqMute[seq]; }

// pad press: mute = cut that sequence's sounding notes at once; unmute =
// playback resumes with the next step.  Editing state is never touched
void seqMuteToggle(uint8_t seq) {
  if (seq >= SEQ_CH_COUNT) return;
  if (seqMute[seq]) {
    seqMute[seq] = false;
    return;
  }
  seqMute[seq] = true;
  for (uint8_t i = 0; i < seqLen[seq]; i++)
    usbMIDI.sendNoteOff(seqNotes[seq][i], 0, seq + 1);
  seqLen[seq] = 0;
  arpIdx[seq] = 0;
}

// --- SEQMODE3 erase --------------------------------------------------------
// cut first so a running transport never re-sends erased notes; the step
// history goes too - otherwise the recording chains would resurrect old
// chords.  Listener sequences keep their own copy (stepSyncFrom never
// propagates an empty source), so "erase all" loops every track
static void seqEraseNotes(uint8_t s) {
  for (uint8_t i = 0; i < seqLen[s]; i++)
    usbMIDI.sendNoteOff(seqNotes[s][i], 0, s + 1);
  seqLen[s] = 0;
  arpIdx[s] = 0;
}

void seqErase(uint8_t s) {
  if (s >= SEQ_CH_COUNT) return;
  seqEraseNotes(s);
  for (uint8_t st = 0; st < STEP_MAX; st++) {
    stepChordLen[s][st] = 0;
    histLen[s][st] = 0;
    stepAttClear(s, st);        // empty steps carry no attributes
  }
  gateOffAt[s] = 0;
  seqOctClamp(s);               // content gone: shift range re-derives
}

void seqEraseAll() {
  for (uint8_t s = 0; s < SEQ_CH_COUNT; s++) seqErase(s);
}

// --- EEPROM persistence (SEQMODE4 + REC saves, boot loads) ----------------
// Settings only - the note patterns themselves stay RAM.  Layout at address
// 160 (after the 131-byte CC block): magic | editCh | prob | oct[16] |
// inCh[16] | vel[16] | gate[16] | mute[16] | track[16] | mono[16] |
// checksum = 116 bytes; blank or corrupt keeps the defaults
static const uint8_t  SEQ_EE_MAGIC = 0x53;
static const uint16_t SEQ_EE_ADDR = 160;
static const uint16_t SEQ_EE_LEN = 116;

void seqCfgSave() {
  uint8_t b[SEQ_EE_LEN];
  b[0] = SEQ_EE_MAGIC;
  b[1] = seqEditCh + 1;
  b[2] = stepProbValue();
  for (uint8_t i = 0; i < SEQ_CH_COUNT; i++) {
    b[3 + i] = (uint8_t)seqOct[i];
    b[19 + i] = seqInCh[i];
    b[35 + i] = seqVel[i];
    b[51 + i] = seqGate[i];
    b[67 + i] = seqMute[i] ? 1 : 0;
    b[83 + i] = seqTrackOf(i);
    b[99 + i] = seqMonoTypeOf(i);
  }
  uint8_t sum = 0;
  for (uint16_t i = 0; i < SEQ_EE_LEN - 1; i++) sum += b[i];
  b[SEQ_EE_LEN - 1] = sum;
  EEPROM.put(SEQ_EE_ADDR, b);   // put() writes only the bytes that changed
}

void seqCfgLoad() {
  uint8_t b[SEQ_EE_LEN];
  EEPROM.get(SEQ_EE_ADDR, b);
  uint8_t sum = 0;
  for (uint16_t i = 0; i < SEQ_EE_LEN - 1; i++) sum += b[i];
  if (b[0] != SEQ_EE_MAGIC || sum != b[SEQ_EE_LEN - 1]) return;
  for (uint8_t i = 0; i < SEQ_CH_COUNT; i++) {
    int v = (int8_t)b[3 + i];            // octave: keep the cast signed
    if (v < -60) v = -60;
    if (v > 60) v = 60;
    seqOct[i] = (int8_t)v;
    v = b[19 + i];
    if (v < 1) v = 1;
    if (v > 16) v = 16;
    seqInCh[i] = (uint8_t)v;
    v = b[35 + i];
    if (v < 1) v = 1;
    if (v > 127) v = 127;
    seqVel[i] = (uint8_t)v;
    v = b[51 + i];
    if (v < 10) v = 10;
    if (v > 100) v = 100;
    seqGate[i] = (uint8_t)v;
    seqMute[i] = (b[67 + i] != 0);
    seqSetTrackType(i, b[83 + i]);
    seqSetMonoType(i, b[99 + i]);
    seqOctClamp(i);                      // content bounds (empty = convention)
  }
  stepProbSet(b[2]);                     // clamped inside
  seqSetEditChannel(b[1]);               // validates 1..16, keeps focus sane
}

// --- DRUM track: per-voice (part) sequencing -----------------------------
static bool drumMuteSel = false;              // M1 held while in ENCMODE
static bool drumVoiceMute[16] = {false};      // drum notes 36..51 muted

void drumMuteHold(bool on) { drumMuteSel = on; }

// composite: only the ENC mode shows the overlay (modeOnEvent clears the
// flag on a mode change)
bool drumVoiceMuteActive() { return drumMuteSel && modeBase == MODE_ENC; }

bool drumVoiceMuted(uint8_t note) {
  return note >= 36 && note <= 51 && drumVoiceMute[note - 36];
}

// overlay pad press: mute/unmute that drum voice for EVERY drum-track
// sequence; state persists until toggled back
void drumVoiceMuteToggle(uint8_t note) {
  if (note >= 36 && note <= 51) drumVoiceMute[note - 36] = !drumVoiceMute[note - 36];
}

const char* drumVoiceName(uint8_t note) {
  static const char* nn[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
  static char buf[4];
  uint8_t n = note - 24;
  snprintf(buf, sizeof(buf), "%s%u", nn[n % 12], n / 12);
  return buf;
}

void drumPartSelectHold(bool on) { drumPartSel = on; }
bool drumPartSelectActive() { return drumPartSel; }

void drumPartSet(uint8_t idx) {
  if (idx >= STEP_COUNT) return;
  drumPart = 36 + idx;                 // step1 = C1, step16 = D#2
}

uint8_t drumPartNote() { return drumPart; }

const char* drumPartName() { return drumVoiceName(drumPart); }

// toggle the selected part at a step of the edit sequence: add or remove -
// no sound here either, the playhead plays the pattern
int stepDrumToggle(uint8_t step) {
  if (step >= STEP_MAX) return -1;
  uint8_t *ch = stepChord[seqEditCh][step];
  uint8_t len = stepChordLen[seqEditCh][step];
  for (uint8_t i = 0; i < len; i++) {
    if (ch[i] != drumPart) continue;
    for (uint8_t j = i; j + 1 < len; j++) ch[j] = ch[j + 1];
    stepChordLen[seqEditCh][step] = len - 1;
    seqOctClamp(seqEditCh);
    stepSyncFrom(seqEditCh);
    return -2;                         // part removed
  }
  if (len >= CHORD_MAX) return -1;
  ch[len] = drumPart;
  stepChordLen[seqEditCh][step] = len + 1;
  seqOctClamp(seqEditCh);
  stepSyncFrom(seqEditCh);
  return drumPart;
}

uint8_t keyChannel() { return keyCh; }

void keySetChannel(uint8_t ch) {
  if (ch < 1 || ch > 16 || ch == keyCh) return;
  monoVoiceOff();                  // live mono/arp voice, still OLD channel
  // cut every keyboard note sounding on the OLD channel first: prevents
  // stuck notes (held pads release later -> harmless off on the new channel)
  for (uint8_t k = 0; k < 12; k++)
    if (keySounding[k]) {
      usbMIDI.sendNoteOff(keyNote[k], 0, keyCh);
      keySounding[k] = false;
    }
  chordCut();                     // chord engine notes, old channel
  keyCh = ch;
  if (monoEngaged()) {
    // new channel is a MONO track: held keys stay silent, the chord engine
    // stays muted (chordMute(true) is a no-op if it already was) - the
    // monoApply() below starts the voice
    chordMute(true);
  } else {
    // was POLY before the switch: held keys re-strike raw here (a MONO->
    // POLY switch restores them instead via monoApply's strikeRaw, so the
    // two never double-send)
    if (!monoEng && keyBehaviorVariant() != 2)
      for (uint8_t k = 0; k < 12; k++)
        if (keyActive[k]) {
          usbMIDI.sendNoteOn(keyNote[k], KEY_VEL, keyCh);
          keySounding[k] = true;
        }
    chordRestore();               // chord engine notes, new channel
  }
  monoApply();                    // re-sync engage state + voice for ch
}
