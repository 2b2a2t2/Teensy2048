#include "Keyboard.h"
#include "SeqClock.h"   // seqTrack() - step view follows the TRACK type

#define KEY_OCT_MIN   (-2)
#define KEY_OCT_MAX    8
#define KEY_OCT_DEF    3      // C3 = 60 (middle C)
#define KEY_VEL      100
#define KEY_COUNTS_OCT 4      // encoder counts per octave step

#define STEP_COUNT     16
#define CHORD_MAX      8      // notes stored per chord
#define SEQ_CH_COUNT   16     // one sequence per MIDI channel (1..16)

static int8_t  octave = KEY_OCT_DEF;
static int16_t octRes = 0;              // leftover encoder counts
static uint8_t keyCh = 1;               // keyboard MIDI channel (1..16)
static bool    keyActive[12] = {false}; // key currently sounding
static uint8_t keyNote[12] = {0};       // note sounding on that key

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
static uint8_t histChord[SEQ_CH_COUNT][STEP_COUNT][CHORD_MAX];
static uint8_t histLen[SEQ_CH_COUNT][STEP_COUNT];   // statics are zero-init

// record sequence s's pending chord at playhead position p
static void histAt(uint8_t s, uint8_t p) {
  if (p >= STEP_COUNT || pendingLen[s] == 0) return;
  histLen[s][p] = pendingLen[s];
  for (uint8_t i = 0; i < pendingLen[s] && i < CHORD_MAX; i++)
    histChord[s][p][i] = pending[s][i];
}

// feed hooks: keep the history in sync wherever a pending chord is written
static void histFeed(uint8_t s) { histAt(s, seqCurrentStep()); }

// sequencer edit focus: 0-based channel index (MIDI channel = seqEditCh + 1);
// the playhead plays EVERY channel's sequence at once, this only selects
// which channel the step pads display / edit
static uint8_t seqEditCh = 0;

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

// per-channel per-step stored chord; len 0 = unassigned (plays pending[])
static uint8_t stepChord[SEQ_CH_COUNT][STEP_COUNT][CHORD_MAX];
static uint8_t stepChordLen[SEQ_CH_COUNT][STEP_COUNT];

// step held for assignment; first key press clears the stored chord
// (assignCh = edit channel captured when the hold started)
static int8_t  assignStep = -1;
static uint8_t assignCh = 0;
static bool    assignStarted = false;

// an ACTIVE step was pressed during the current tap (candidate for disable;
// confirmed only on click, so a hold-to-edit does not clear the step)
static bool tapCandidate[STEP_COUNT] = {false};

// source step on track x for listener position n: the last active step at
// or before n in the sequence (cyclic - wrapping past step 1 uses the
// previous loop's chord), so "B at step 1, G at step 9" partitions steps
// 1-8 / 9-16 exactly.  Returns -1 when the track has no active step
static int8_t srcStepBack(uint8_t x, uint8_t n) {
  if (x >= SEQ_CH_COUNT || n >= STEP_COUNT) return -1;
  for (uint8_t k = 0; k < STEP_COUNT; k++) {
    uint8_t idx = (n + STEP_COUNT - k) % STEP_COUNT;
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
  for (uint8_t st = 0; st < STEP_COUNT; st++) {
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
// sequence listening to x's channel (input channel = x+1) re-derives its
// ACTIVE steps from x's timeline - each takes the last chord at or before
// its position, so ch3 follows "the last played chord in the sequence",
// never the global last-played buffer while x has chords.  Content only:
// the listener's active/inactive state is untouched ("keeping the same
// active steps") and no-source steps keep their content.  Single level:
// copies do not re-trigger, so input-channel cycles cannot recurse
static void stepSyncFrom(uint8_t x) {
  if (x >= SEQ_CH_COUNT) return;
  for (uint8_t y = 0; y < SEQ_CH_COUNT; y++) {
    if (y == x || seqInCh[y] != x + 1) continue;
    if (seqTrackOf(y) != SEQ_TRACK_MONO) continue;
    bool changed = false;
    for (uint8_t n = 0; n < STEP_COUNT; n++) {
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
// keyboard chord recorded on ch2 also feeds ch3 when ch3 input = ch2)
static void updatePending() {
  for (uint8_t s = 0; s < SEQ_CH_COUNT; s++) {
    if (s + 1 != keyCh && seqInCh[s] != keyCh) continue;
    pendingLen[s] = 0;
    for (uint8_t i = 0; i < 12 && pendingLen[s] < CHORD_MAX; i++) {
      if (keyActive[i]) pending[s][pendingLen[s]++] = keyNote[i];
    }
    histFeed(s);
  }
}

static void setOctave(int8_t o) {
  if (o < KEY_OCT_MIN) o = KEY_OCT_MIN;
  if (o > KEY_OCT_MAX) o = KEY_OCT_MAX;
  if (o == octave) return;
  octave = o;
  // re-trigger every sounding key so no note is stranded in the old octave
  for (uint8_t k = 0; k < 12; k++) {
    if (!keyActive[k]) continue;
    usbMIDI.sendNoteOff(keyNote[k], 0, keyCh);
    keyNote[k] = noteFor(k, octave);
    usbMIDI.sendNoteOn(keyNote[k], KEY_VEL, keyCh);
  }
}

int keyPadPress(uint8_t idx) {
  if (idx >= 12) return -1;
  keyNote[idx] = noteFor(idx, octave);
  usbMIDI.sendNoteOn(keyNote[idx], KEY_VEL, keyCh);
  keyActive[idx] = true;
  updatePending();
  // holding a step = assign mode: this key (and the rest of the chord
  // played while still held) replaces that step's stored chord
  if (assignStep >= 0) {
    if (!assignStarted) {
      stepChordLen[assignCh][assignStep] = 0;
      assignStarted = true;
    }
    if (stepChordLen[assignCh][assignStep] < CHORD_MAX)
      stepChord[assignCh][assignStep][stepChordLen[assignCh][assignStep]++] = keyNote[idx];
    seqOctClamp(assignCh);
    stepSyncFrom(assignCh);
  }
  return keyNote[idx];
}

int keyPadRelease(uint8_t idx) {
  if (idx >= 12 || !keyActive[idx]) return -1;
  usbMIDI.sendNoteOff(keyNote[idx], 0, keyCh);
  keyActive[idx] = false;
  // all keys up while assigning = that chord is finished; the next press
  // starts a fresh replacement instead of appending
  if (assignStep >= 0) {
    bool any = false;
    for (uint8_t k = 0; k < 12; k++) any = any || keyActive[k];
    if (!any && extHeldLen[assignCh] == 0) assignStarted = false;
  }
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

int stepChordPress(uint8_t step) {
  if (step >= STEP_COUNT) return -1;
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
  // latest pending chord
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
  }
  if (n == 0) return 0;
  for (uint8_t i = 0; i < n && i < CHORD_MAX; i++)
    stepChord[seqEditCh][step][i] = src[i];
  stepChordLen[seqEditCh][step] = n;
  seqOctClamp(seqEditCh);
  stepSyncFrom(seqEditCh);
  return n;
}

void stepAssignSet(int8_t step) {
  // a hold means "edit", never "disable": drop the tap candidate
  if (step >= 0 && step < STEP_COUNT) {
    tapCandidate[step] = false;
    assignCh = seqEditCh;    // the channel being edited when the hold began
  }
  assignStep = step;
  assignStarted = false;
}

// tap on an active step completed (EV_CLICKED): disable it now
int stepChordClick(uint8_t step) {
  if (step >= STEP_COUNT || !tapCandidate[step]) return -1;
  tapCandidate[step] = false;
  if (stepChordLen[seqEditCh][step] == 0) return -1;
  stepChordLen[seqEditCh][step] = 0;
  seqOctClamp(seqEditCh);     // content shrank: shift range may have grown
  return 0;
}

bool stepIsActive(uint8_t step) {
  if (step >= STEP_COUNT) return false;
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

// notes the sequencer (playhead) currently has sounding, per channel
static uint8_t seqNotes[SEQ_CH_COUNT][CHORD_MAX];
static uint8_t seqLen[SEQ_CH_COUNT] = {0};

// ARP phase per sequence (MONO/ARP playback); reset on transport stop
static uint8_t arpIdx[SEQ_CH_COUNT] = {0};

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

static void seqRelease() {
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    for (uint8_t i = 0; i < seqLen[c]; i++)
      usbMIDI.sendNoteOff(seqNotes[c][i], 0, c + 1);
    seqLen[c] = 0;
  }
}

// playback note = stored note + 12 * the sequence's octave shift, kept in
// 0..127 (seqOctClamp keeps the shift inside the -2..8 convention anyway)
static uint8_t shNote(uint8_t n, int8_t sh) {
  int v = (int)n + 12 * (int)sh;
  if (v < 0) v = 0;
  if (v > 127) v = 127;
  return (uint8_t)v;
}

// playhead reached a step: every channel with a chord here plays at once
// (on its own MIDI channel), so channel 1 keeps running while another
// channel is being edited.  MONO sequences transform their chord at
// playback (ROOT = root note, BASS = lowest note, ARP = next chord note),
// then every channel applies its own octave shift (ENC1)
void seqStepAdvance(uint8_t step) {
  seqRelease();
  if (step >= STEP_COUNT) return;
  // carry the current pending chords forward into the new position: the
  // chord played during steps 1-8 is the "actual chord" of step 8 even
  // though it was fed while the playhead sat on step 1
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) histAt(c, step);
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    uint8_t len = stepChordLen[c][step];
    if (!len) continue;              // ARP phase also stays put on empty steps
    uint8_t *ch = stepChord[c][step];
    int8_t sh = seqOct[c];
    if (seqTrackOf(c) == SEQ_TRACK_MONO) {
      uint8_t note;
      if (seqMonoTypeOf(c) == SEQ_MONO_BASS) {
        note = ch[0];
        for (uint8_t i = 1; i < len; i++) if (ch[i] < note) note = ch[i];
      } else if (seqMonoTypeOf(c) == SEQ_MONO_ARP) {
        note = ch[arpIdx[c] % len];
        arpIdx[c]++;
      } else {
        note = rootOf(ch, len);      // SEQ_MONO_ROOT
      }
      note = shNote(note, sh);
      seqNotes[c][0] = note;
      seqLen[c] = 1;
      usbMIDI.sendNoteOn(note, KEY_VEL, c + 1);
    } else {
      for (uint8_t i = 0; i < len && i < CHORD_MAX; i++) {
        uint8_t note = shNote(ch[i], sh);
        seqNotes[c][seqLen[c]] = note;
        usbMIDI.sendNoteOn(note, KEY_VEL, c + 1);
        seqLen[c]++;
      }
    }
  }
}

void seqStepStop() {
  seqRelease();
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) arpIdx[c] = 0;
}

uint8_t seqEditChannel() { return seqEditCh + 1; }

void seqSetEditChannel(uint8_t ch) {
  // focus switch only: sounding notes and pending chords stay untouched
  // (each sequence owns its own input state)
  if (ch < 1 || ch > SEQ_CH_COUNT || ch == seqEditCh + 1) return;
  seqEditCh = ch - 1;
  inRes = 0;     // partial ENC5 turn belongs to the previous focus
  octSRes = 0;   // partial ENC1 turn belongs to the previous focus
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
  for (uint8_t st = 0; st < STEP_COUNT; st++) histLen[seqEditCh][st] = 0;
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

// --- DRUM track: per-voice (part) sequencing -----------------------------
void drumPartSelectHold(bool on) { drumPartSel = on; }
bool drumPartSelectActive() { return drumPartSel; }

void drumPartSet(uint8_t idx) {
  if (idx >= STEP_COUNT) return;
  drumPart = 36 + idx;                 // step1 = C1, step16 = D#2
}

uint8_t drumPartNote() { return drumPart; }

const char* drumPartName() {
  static const char* nn[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
  static char buf[4];
  uint8_t n = drumPart - 24;
  snprintf(buf, sizeof(buf), "%s%u", nn[n % 12], n / 12);
  return buf;
}

// toggle the selected part at a step of the edit sequence: add or remove -
// no sound here either, the playhead plays the pattern
int stepDrumToggle(uint8_t step) {
  if (step >= STEP_COUNT) return -1;
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
  // cut every keyboard note sounding on the OLD channel first: prevents
  // stuck notes (held pads release later -> harmless off on the new channel)
  for (uint8_t k = 0; k < 12; k++)
    if (keyActive[k]) usbMIDI.sendNoteOff(keyNote[k], 0, keyCh);
  keyCh = ch;
  // keys still held re-strike on the new channel (same idea as octave change)
  for (uint8_t k = 0; k < 12; k++)
    if (keyActive[k]) usbMIDI.sendNoteOn(keyNote[k], KEY_VEL, keyCh);
}
