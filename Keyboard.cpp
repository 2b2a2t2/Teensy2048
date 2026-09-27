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

// sequencer edit focus: 0-based channel index (MIDI channel = seqEditCh + 1);
// the playhead plays EVERY channel's sequence at once, this only selects
// which channel the step pads display / edit
static uint8_t seqEditCh = 0;

// input (listen) channel per sequence: notes arriving there feed the record
// workflow; default sequence N listens to channel N
static uint8_t seqInCh[SEQ_CH_COUNT] = {1, 2, 3, 4, 5, 6, 7, 8,
                                        9, 10, 11, 12, 13, 14, 15, 16};
static int16_t inRes = 0;                // leftover encoder counts (ENC5)

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
static bool    tapCandidate[STEP_COUNT] = {false};

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
// chord of the sequence the keyboard channel (KEY3) currently selects
static void updatePending() {
  uint8_t k = keyCh - 1;
  pendingLen[k] = 0;
  for (uint8_t i = 0; i < 12 && pendingLen[k] < CHORD_MAX; i++) {
    if (keyActive[i]) pending[k][pendingLen[k]++] = keyNote[i];
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
  // pressing an unassigned step RECORDS this sequence's pending chord into
  // it - silently: the chord is only sent when the playhead (green dot)
  // reads the step during playback
  if (pendingLen[seqEditCh] == 0) return 0;
  for (uint8_t i = 0; i < pendingLen[seqEditCh] && i < CHORD_MAX; i++)
    stepChord[seqEditCh][step][i] = pending[seqEditCh][i];
  stepChordLen[seqEditCh][step] = pendingLen[seqEditCh];
  return pendingLen[seqEditCh];
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

static void seqRelease() {
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    for (uint8_t i = 0; i < seqLen[c]; i++)
      usbMIDI.sendNoteOff(seqNotes[c][i], 0, c + 1);
    seqLen[c] = 0;
  }
}

// playhead reached a step: every channel with a chord here plays at once
// (on its own MIDI channel), so channel 1 keeps running while another
// channel is being edited
void seqStepAdvance(uint8_t step) {
  seqRelease();
  if (step >= STEP_COUNT) return;
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    for (uint8_t i = 0; i < stepChordLen[c][step] && i < CHORD_MAX; i++) {
      seqNotes[c][seqLen[c]] = stepChord[c][step][i];
      usbMIDI.sendNoteOn(seqNotes[c][seqLen[c]], KEY_VEL, c + 1);
      seqLen[c]++;
    }
  }
}

void seqStepStop() { seqRelease(); }

uint8_t seqEditChannel() { return seqEditCh + 1; }

void seqSetEditChannel(uint8_t ch) {
  // focus switch only: sounding notes and pending chords stay untouched
  // (each sequence owns its own input state)
  if (ch < 1 || ch > SEQ_CH_COUNT || ch == seqEditCh + 1) return;
  seqEditCh = ch - 1;
  inRes = 0;     // partial ENC5 turn belongs to the previous focus
}

uint8_t seqInputChannel() { return seqInCh[seqEditCh]; }

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
    // holding a step = assign mode: input also appends to the edit step
    if (assignStep >= 0 && s == seqEditCh) {
      if (!assignStarted) {
        stepChordLen[assignCh][assignStep] = 0;
        assignStarted = true;
      }
      if (stepChordLen[assignCh][assignStep] < CHORD_MAX)
        stepChord[assignCh][assignStep][stepChordLen[assignCh][assignStep]++] = note;
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
    return -2;                         // part removed
  }
  if (len >= CHORD_MAX) return -1;
  ch[len] = drumPart;
  stepChordLen[seqEditCh][step] = len + 1;
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
