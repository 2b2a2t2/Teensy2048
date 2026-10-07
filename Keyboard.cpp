#include "Keyboard.h"
#include "SeqClock.h"   // seqTrack() - step view follows the TRACK type
#include "Modes.h"      // keyBehaviorVariant() - KEYMODE2 = chord mode
#include "Chords.h"     // chord engine + scale filter state
#include "Events.h"     // prob clipboard accessors (SEQ save/load)
#include "Key4.h"       // KEYMODE4 proposal gesture (press/release/commit)
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

// shadow position: advances continuously as if no loop were active,
// used to resume at the correct global position when a loop is disabled
static uint8_t chStepShadow[SEQ_CH_COUNT] = {0};

// previous loop state per channel, to detect when a loop is disabled
static bool loopWasOn[SEQ_CH_COUNT] = {false};

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
static int16_t accSRes = 0;              // leftover encoder counts (ENC6 acc)
static int16_t probSRes = 0;             // leftover encoder counts (ENC8 prob)

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
// per-step flags (zero-init = none).  Track note shaping:
// seqVel = default velocity (boot 100, accent step = 127), seqGate = gate
// time in % of one step (100 = hold until the next step = today's behavior).
// Bits 6-7 double as the POLY step TYPE override (F2 target): 00 = inherit
// the track's TYPE, 01/10/11 = STEP/STRUM/HAND (stepTypeToggle below)
static uint8_t stepAtt[SEQ_CH_COUNT][STEP_MAX];
// POLY-only step FX (embellishments): SUS2/SUS4, COLOR (ghost approach)
// and a CHORD_EXT-style extension mask in the top nibble.  Same lifecycle
// as stepAtt: RAM only, dies with the step
static uint8_t stepFx[SEQ_CH_COUNT][STEP_MAX];
// HAND strum timing: onset of each stored note in ms since the gesture's
// first touch (u16 - a strum may span several steps/bars; playback never
// squeezes it into one step).  Zero = no recorded timing (block press or
// hand-written step) -> HAND falls back to the fixed even spread.
// Cleared with the step (stepAttClear), copied by the undo snapshot
static uint16_t stepStrumDt[SEQ_CH_COUNT][STEP_MAX][CHORD_MAX];
// DRUM tracks split accent/probability PER VOICE: mask bits say which of
// the 16 parts (notes 36..51) is accented / probability-gated in a step.
// Zero-init: no accents, no probability gates (mask bit = attribute present)
static uint8_t drumAccent[SEQ_CH_COUNT][STEP_MAX];
static uint8_t drumProbOn[SEQ_CH_COUNT][STEP_MAX];
static uint8_t seqVel[SEQ_CH_COUNT] = {90,90,90,90,90,90,90,90,
                                       90,90,90,90,90,90,90,90};
// per-track PLAYBACK probability in % (100 = the step always plays).  ENC8
// edits the edit track and every PROB step of that track feels it live -
// no per-step copy to re-toggle afterwards
static uint8_t seqProb[SEQ_CH_COUNT] = {100,100,100,100,100,100,100,100,
                                        100,100,100,100,100,100,100,100};
// per-track velocity of an ACCENTED step (127 is the old hard-wire; 101 is
// the boot default) - F3 held + ENC6 edits it
static uint8_t seqAccVel[SEQ_CH_COUNT] = {101,101,101,101,101,101,101,101,
                                          101,101,101,101,101,101,101,101};
static uint8_t seqGate[SEQ_CH_COUNT] = {100,100,100,100,100,100,100,100,
                                        100,100,100,100,100,100,100,100};
static uint32_t gateOffAt[SEQ_CH_COUNT] = {0};   // 0 = no pending note-off
static uint8_t advPrev[SEQ_CH_COUNT] = {          // per-track step of the
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // previous advance
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // (TIE/GLIDE carry)
static uint8_t fBlinkMask = 0;   // edit-channel attrs read by the playhead

// live record (SEQMODE REC button): 0 = off, 1 = armed (waiting for the
// edit track's range head), 2 = recording (the one-pass run)
static uint8_t recState = 0;
static uint8_t recCh = 0;        // track captured when armed
static uint8_t recLeft = 0;      // steps left in the record pass
static bool    recStarted = false;  // current gesture already wrote the step
static bool    recDone = false;  // one-shot: a pass finished by itself
static uint8_t recGestStep = 0;  // POLY: the step a gesture started on (one
                                 // gesture = one origin step, even while the
                                 // playhead walks on - a strum keeps timing)
static uint32_t recGestureT0 = 0;  // ms of the gesture's first touch

// a step's attributes live and die with its content (both the MONO flags
// and the DRUM per-voice accent/probability masks; the POLY FX byte and
// the HAND strum onsets too)
static void stepAttClear(uint8_t ch, uint8_t st) {
  stepAtt[ch][st] = 0;
  stepFx[ch][st] = 0;
  drumAccent[ch][st] = 0;
  drumProbOn[ch][st] = 0;
  for (uint8_t i = 0; i < CHORD_MAX; i++) stepStrumDt[ch][st][i] = 0;
}

// does this step carry a recorded HAND strum (any nonzero onset)?
static bool stepStrumTimed(uint8_t ch, uint8_t st) {
  if (ch >= SEQ_CH_COUNT || st >= STEP_MAX) return false;
  for (uint8_t i = 0; i < CHORD_MAX; i++)
    if (stepStrumDt[ch][st][i]) return true;
  return false;
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
  bool chordMode = keyChordMode();   // KEYMODE2 or KEYMODE4: the engine's notes
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
  // chord engine variants: the engine owns its notes - shift and re-send them
  if (keyChordMode()) {
    chordShift(delta);
    return;
  }
  // re-trigger every sounding key so no note is stranded in the old octave;
  // keys held under the mono voice stay silent (their keyNote follows so
  // the voice and the pending chord track the octave change).  HAND strings
  // keep their sounding pitch - the fill re-derives for the next pluck
  if (!handLive())
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

// live record state query for the LED hook / event edge drivers
uint8_t seqRecState() { return recState; }

// one-shot: true exactly once after a pass finished by itself (header msg)
bool seqRecTakeDone() {
  bool d = recDone;
  recDone = false;
  return d;
}

// SEQMODE REC button: arm one pass or cancel the current one.  The
// keyboard channel must already match the edit track - arming refuses
// with 0 until the channel is set (header shows the warning)
uint8_t seqRecToggle() {
  if (recState) {
    recState = 0;
    recStarted = false;
    recGestStep = 0;
    return 2;
  }
  if (keyCh != (uint8_t)(seqEditCh + 1)) return 0;
  recCh = seqEditCh;
  recLeft = 0;
  recStarted = false;
  recGestStep = 0;
  recState = 1;
  return 1;
}

// live record: the keyboard gesture lands on the step the playhead sits
// on.  POLY/MONO = replace (the first note of a fresh gesture clears the
// step and its attributes), DRUM = overdub (notes are added, never
// removed); the octave pre-compensation mirrors the hold-to-assign record.
// POLY gestures are ANCHORED: every note of one gesture (first touch ->
// all keys up) joins the step where it STARTED even after the playhead
// moved on, so a long strum lives on one origin step with its real u16
// onsets (dt, ms since the gesture's first touch; block presses pass 0 =
// no timing -> the fixed even spread).  MONO keeps the classic behavior
// (each press lands on the current step, so legato runs walk the pattern).
// Returns true when a note was written
static bool liveRecAppend(const uint8_t *notes, uint8_t n, uint16_t dt = 0) {
  if (recState != 2 || n == 0) return false;
  if (assignStep >= 0) return false;               // a held step pad owns the write
  if (keyCh != (uint8_t)(recCh + 1)) return false; // defensive (disarm on switch)
  bool drum = (seqTrackOf(recCh) == SEQ_TRACK_DRUM);
  bool poly = (seqTrackOf(recCh) == SEQ_TRACK_POLY);
  uint8_t st = chStep[recCh];
  if (poly) {                       // one gesture = one origin step
    if (recStarted) st = recGestStep;
    else recGestStep = st;
  }
  if (st >= STEP_MAX) return false;
  if (!drum && !recStarted) {
    stepChordLen[recCh][st] = 0;   // fresh gesture replaces the stored chord
    stepAttClear(recCh, st);       // ...and starts attribute/timing-free
    recStarted = true;
  }
  int pre = -12 * (int)seqOct[recCh];
  bool wrote = false;
  for (uint8_t i = 0; i < n && stepChordLen[recCh][st] < CHORD_MAX; i++) {
    int v = (int)notes[i] + pre;
    if (v < 0) v = 0;
    else if (v > 127) v = 127;
    uint8_t note = (uint8_t)v;
    bool dup = false;             // never store the same note twice
    for (uint8_t j = 0; j < stepChordLen[recCh][st]; j++)
      if (stepChord[recCh][st][j] == note) dup = true;
    if (!dup) {
      stepChord[recCh][st][stepChordLen[recCh][st]] = note;
      stepStrumDt[recCh][st][stepChordLen[recCh][st]] = dt;
      stepChordLen[recCh][st]++;
      wrote = true;
    }
  }
  seqOctClamp(recCh);
  stepSyncFrom(recCh);
  return wrote;
}

// HAND pluck during REC: the note joins the gesture with its real onset -
// 0 for the first touch, ms elapsed for every note after it (u16, up to
// ~65 s, so a slow multi-bar strum keeps its true timing; playback clamps
// by scheduling from the origin step, never by compressing the onsets)
static void handRecPluck(uint8_t note) {
  if (recState != 2) return;
  bool fresh = !recStarted;
  uint16_t dt = 0;
  if (!fresh) {
    uint32_t el = millis() - recGestureT0;
    dt = (el > 65535UL) ? 65535 : (uint16_t)el;
  }
  if (liveRecAppend(&note, 1, dt) && fresh)
    recGestureT0 = millis();
}

// HAND performs right now: the keyboard channel reaches a POLY sequence
// whose TRACK type is HAND (live gate is track-level; per-step overrides
// only shape the sequence's playback)
bool handLive() {
  uint8_t s = (uint8_t)(keyCh - 1);
  if (s >= SEQ_CH_COUNT) return false;
  return seqTrackOf(s) == SEQ_TRACK_POLY &&
         seqPolyTypeOf(s) == SEQ_POLY_HAND;
}

// the 7 white keys are the strummed strings: C..B -> 0..6, black keys -1
static int8_t handStringOf(uint8_t idx) {
  static const int8_t w[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };
  return w[idx & 11];
}

// HAND fill: the7 white keys' notes - the chord's pitch classes starting
// from its bass PC at the keyboard octave, walking up with octave wraps so
// every string always has a note (returns the count, 0 = no chord yet).
// Source = the keyboard sequence's pending chord (engine snapshot in
// KEYMODE2/4, held keys in KEYMODE1/3, incoming input chords everywhere),
// falling back to the engine itself
static uint8_t handFill(uint8_t *out) {
  const uint8_t *src = 0;
  uint8_t n = 0;
  if (keyCh >= 1 && keyCh <= SEQ_CH_COUNT && pendingLen[keyCh - 1]) {
    src = pending[keyCh - 1];
    n = pendingLen[keyCh - 1];
  } else if (keyChordMode() && chordCount()) {
    src = chordNotes();
    n = chordCount();
  }
  if (!n) return 0;
  uint8_t pc[CHORD_MAX], pn = 0;
  for (uint8_t i = 0; i < n && pn < CHORD_MAX; i++) {
    uint8_t p = (uint8_t)(src[i] % 12);
    bool dup = false;
    for (uint8_t j = 0; j < pn; j++) if (pc[j] == p) dup = true;
    if (!dup) pc[pn++] = p;
  }
  if (!pn) return 0;
  uint8_t bass = src[0];
  for (uint8_t i = 1; i < n; i++) if (src[i] < bass) bass = src[i];
  int note = 12 * (int)octave + 24 + (int)(bass % 12);
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  for (uint8_t i = 0; i < 7; i++) {
    if (i) {
      int cand = note + 1;
      for (uint8_t k = 0; k < 13 && cand <= 127; k++, cand++) {
        bool hit = false;
        for (uint8_t j = 0; j < pn; j++)
          if ((uint8_t)(cand % 12) == pc[j]) { hit = true; break; }
        if (hit) break;
      }
      if (cand > 127) cand = note;   // degenerate top: hold the last string
      note = cand;
    }
    out[i] = (uint8_t)note;
  }
  return 7;
}

// sound one HAND string (the caller has already checked handFill())
static void handPluck(uint8_t idx, uint8_t note) {
  keyNote[idx] = note;
  keyActive[idx] = true;
  keySounding[idx] = true;
  usbMIDI.sendNoteOn(note, KEY_VEL, keyCh);
  handRecPluck(note);           // REC: append with the gesture's real onset
}

int keyPadPress(uint8_t idx) {
  if (idx >= 12) return -1;
  monoApply();   // reconcile with the keyboard channel's track first
  // HAND track: the white keys strum the fill; every key below still BUILDS
  // the chord (silent - chordMute), only the pluck sounds.  Block recording
  // is suppressed in HAND: a strum gesture is captured note by note with
  // its timing (handRecPluck), so the stored step replays as performed
  bool hand = handLive();
  int8_t sw = hand ? handStringOf(idx) : -1;
  bool plucked = false;
  uint8_t pnote = 0;
  if (sw >= 0) {
    uint8_t fill[7];
    if (handFill(fill) > (uint8_t)sw) {
      pnote = fill[sw];
      plucked = true;
    }
  }
  // KEYMODE2: the chord engine owns the gesture (quality/diatonic chords,
  // latch, KeyLock) - it sends its own notes and feeds the pending chord
  // (muted while the mono voice or HAND is engaged; the state still builds)
  if (keyBehaviorVariant() == 2) {
    uint8_t dyn = noteFor(idx, octave);
    if (plucked) handPluck(idx, pnote);
    else {
      keyNote[idx] = dyn;
      keyActive[idx] = true;
      keySounding[idx] = false;   // the chord engine, not us, sends the sound
    }
    int r = chordPadPress(idx, dyn);
    assignAppend(chordNotes(), chordCount());
    if (!hand) liveRecAppend(chordNotes(), chordCount());
    return r;
  }
  // KEYMODE4: the genre engine proposes; an unproposed key is silent and
  // untracked (rank -1), a proposed one sounds its whole chord.  In HAND
  // the pluck still sounds on any white key - the proposal only steers the
  // chord behind it
  if (keyBehaviorVariant() == 4) {
    if (plucked) handPluck(idx, pnote);
    int r = k4KeyOn((uint8_t)idx);
    if (r < 0) return plucked ? pnote : -1;   // not proposed: no write/advance
    if (!plucked) {
      keyNote[idx] = noteFor(idx, octave);
      keyActive[idx] = true;
      keySounding[idx] = false;   // the chord engine, not us, sends the sound
    }
    assignAppend(chordNotes(), chordCount());
    if (!hand) liveRecAppend(chordNotes(), chordCount());
    if (assignStep < 0) k4KeyCommitted();   // a held step owns the target
    return r;
  }
  // chordless variants (KEYMODE1/3): in HAND only the strings speak - the
  // pluck bypasses the scale filter (chord tones may sit outside it) and
  // feeds neither pending nor the block recorder (the gesture does), black
  // keys are dark and silent
  if (plucked) {
    handPluck(idx, pnote);
    assignAppend(&pnote, 1);
    return pnote;
  }
  if (hand) return -1;
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
  liveRecAppend(&keyNote[idx], 1);
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
  if (keyBehaviorVariant() == 2) {
    chordPadRelease(idx);
  } else if (keyBehaviorVariant() == 4) {
    // re-voice from the first key still held: the engine rebuilds from it,
    // so its proposal bucket must be in place first
    for (uint8_t k = 0; k < 12; k++) {
      if (k != idx && keyActive[k]) {
        if (k4KeyRank((uint8_t)k) >= 0) k4BucketFor((uint8_t)k);
        break;
      }
    }
    chordPadRelease(idx);
  }
  // all keys up while assigning = that chord is finished; the next press
  // starts a fresh replacement instead of appending (live record: next
  // gesture replaces too, DRUM overdubs regardless)
  bool anyUp = false;
  for (uint8_t k = 0; k < 12; k++) anyUp = anyUp || keyActive[k];
  if (assignStep >= 0 && !anyUp && extHeldLen[assignCh] == 0) assignStarted = false;
  if (!anyUp) recStarted = false;
  if (!anyUp && keyBehaviorVariant() == 4) k4GestureEnd();  // engine home
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
  if (keyChordMode()) {
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

// --- KEYMODE4 write path (Key4.cpp drives it) ------------------------------
// the generated chord lands like the live gesture would: pre-compensated
// for the sequence's octave shift, so the step sounds the note the engine
// just built (assignAppend does the same)
int stepChordWrite(uint8_t step, const uint8_t *notes, uint8_t n) {
  if (step >= STEP_MAX || n == 0 || n > CHORD_MAX || notes == 0) return -1;
  uint8_t ch = seqEditCh;
  int pre = -12 * (int)seqOct[ch];
  stepChordLen[ch][step] = 0;
  stepAttClear(ch, step);
  for (uint8_t i = 0; i < n; i++) {
    int v = (int)notes[i] + pre;
    if (v < 0) v = 0;
    else if (v > 127) v = 127;
    stepChord[ch][step][stepChordLen[ch][step]++] = (uint8_t)v;
  }
  seqOctClamp(ch);
  stepSyncFrom(ch);
  return (int)stepChordLen[ch][step];
}

const uint8_t* stepChordAt(uint8_t step, uint8_t *len) {
  if (step >= STEP_MAX || len == 0) { if (len) *len = 0; return stepChord[0][0]; }
  *len = stepChordLen[seqEditCh][step];
  return stepChord[seqEditCh][step];
}

// one-level undo of the last KEYMODE4 write batch (apply / key write /
// variation): a full snapshot of the edit sequence - chords, lengths, the
// step attributes (incl. the TYPE override), the per-note HAND strum
// onsets and the drum accent/probability masks (~1.8 KB static)
static uint8_t snapChord[STEP_MAX][CHORD_MAX];
static uint8_t snapLen[STEP_MAX];
static uint8_t snapAttB[STEP_MAX];
static uint8_t snapFxB[STEP_MAX];
static uint8_t snapDrumA[STEP_MAX];
static uint8_t snapDrumP[STEP_MAX];
static uint16_t snapStrumB[STEP_MAX][CHORD_MAX];
static uint8_t snapCh = 0;
static bool    snapValid = false;

void stepSnapshotTake() {
  uint8_t ch = seqEditCh;
  if (ch >= SEQ_CH_COUNT) return;
  snapCh = ch;
  for (uint8_t st = 0; st < STEP_MAX; st++) {
    snapLen[st] = stepChordLen[ch][st];
    snapAttB[st] = stepAtt[ch][st];
    snapFxB[st] = stepFx[ch][st];
    snapDrumA[st] = drumAccent[ch][st];
    snapDrumP[st] = drumProbOn[ch][st];
    for (uint8_t i = 0; i < CHORD_MAX; i++) {
      snapChord[st][i] = stepChord[ch][st][i];
      snapStrumB[st][i] = stepStrumDt[ch][st][i];
    }
  }
  snapValid = true;
}

int stepSnapshotRestore() {
  if (!snapValid) return -1;
  snapValid = false;
  if (snapCh >= SEQ_CH_COUNT) return -1;
  if (seqEditCh != snapCh) seqSetEditChannel((uint8_t)(snapCh + 1));
  uint8_t ch = snapCh;
  for (uint8_t st = 0; st < STEP_MAX; st++) {
    stepChordLen[ch][st] = snapLen[st];
    stepAtt[ch][st] = snapAttB[st];
    stepFx[ch][st] = snapFxB[st];
    drumAccent[ch][st] = snapDrumA[st];
    drumProbOn[ch][st] = snapDrumP[st];
    for (uint8_t i = 0; i < CHORD_MAX; i++) {
      stepChord[ch][st][i] = snapChord[st][i];
      stepStrumDt[ch][st][i] = snapStrumB[st][i];
    }
  }
  stepSyncFrom(ch);
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

// toggle an attribute on the EDIT channel's step.  MONO/POLY use the
// per-step flags; DRUM tracks toggle it for the SELECTED PART only - each
// voice carries its own accent and can be probability-gated on its own.
// PROB only arms the gate: the % is the track's own probability (ENC8),
// so one knob moves every armed step of the track at once.  Returns the
// new state so the caller can show "14 kick acc on" feedback
bool stepAttrToggle(uint8_t step, uint8_t att) {
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
      drumProbOn[seqEditCh][step] ^= bit;
      return (drumProbOn[seqEditCh][step] & bit) != 0;
    }
    return false;                      // TIE/GLIDE never apply to drums
  }
  uint8_t *a = &stepAtt[seqEditCh][step];
  *a ^= att;
  return (*a & att) != 0;
}

// stamp / clear the POLY step TYPE override (bits6-7 of stepAtt): a tap on
// the F2-armed step writes the ENC7 arm (type 0..2 = STEP/STRUM/HAND), a
// second tap clears back to inherit-the-track.  Returns true when the step
// now carries an override (for "7 strum on" feedback)
bool stepTypeToggle(uint8_t step, uint8_t type) {
  if (step >= STEP_MAX || type > SEQ_POLY_HAND) return false;
  uint8_t *a = &stepAtt[seqEditCh][step];
  if (*a & 0xC0) {
    *a &= 0x3F;
    return false;
  }
  *a |= (uint8_t)((type + 1) << 6);
  return true;
}

// effective POLY playback type of a step: the bits6-7 override, else the
// track default (SEQ_POLY_STEP / SEQ_POLY_STRUM / SEQ_POLY_HAND)
static uint8_t effStepType(uint8_t ch, uint8_t st) {
  if (ch >= SEQ_CH_COUNT || st >= STEP_MAX) return SEQ_POLY_STEP;
  uint8_t a = (uint8_t)(stepAtt[ch][st] >> 6) & 3;
  if (a) return a - 1;
  return seqPolyTypeOf(ch);
}

// type index (SUS2 SUS4 6 m7 M7 9 COLOR) -> bit in the FX byte:
// SUS/COLOR sit in the low nibble, the extension mask rides the top
// nibble so (stepFx >> 4) is exactly a CHORD_EXT mask for chordFxApply
uint8_t stepFxBit(uint8_t type) {
  static const uint8_t bits[8] = { 0x01, 0x02, 0x10, 0x20,
                                   0x40, 0x80, 0x04, 0x08 };
  return bits[type & 7];
}

// toggle an FX type on the edit channel's POLY step; sus2 and sus4 are two
// voicings of one idea, so arming one drops the other.  Returns the new
// state of the requested type for "5 sus4 on" feedback
bool stepFxToggle(uint8_t step, uint8_t type) {
  if (step >= STEP_MAX) return false;
  uint8_t bit = stepFxBit(type);
  uint8_t *f = &stepFx[seqEditCh][step];
  *f ^= bit;
  bool on = (*f & bit) != 0;
  if (on && (type == SEQ_FX_SUS2 || type == SEQ_FX_SUS4))
    *f &= (uint8_t)~stepFxBit(type == SEQ_FX_SUS2 ? SEQ_FX_SUS4
                                                  : SEQ_FX_SUS2);
  return on;
}

// attribute mask of the EDIT channel's step (step LED pulse / display).
// MONO/POLY read the step flags; on DRUM tracks the view follows the
// SELECTED PART - switching the part select re-maps the yellow pulse live.
// The FX target (POLY, target 5) pulses from bit 0x10 = 1 << (5 - 1), and
// only when the step carries the SELECTED FX type (same "selected attribute
// present" rule as the four classic flags); the TYPE target (6) pulses from
// bit 0x20 whenever the step carries any bits6-7 override
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
  uint8_t m = stepAtt[seqEditCh][step];
  if (seqTrack() == SEQ_TRACK_POLY) {
    if (stepFx[seqEditCh][step] & stepFxBit(stepFxSel())) m |= 0x10;
    if (m & 0xC0) m |= 0x20;          // TYPE override present -> target 6
  }
  return m;
}

// notes the sequencer (playhead) currently has sounding, per channel
static uint8_t seqNotes[SEQ_CH_COUNT][CHORD_MAX];
static uint8_t seqLen[SEQ_CH_COUNT] = {0};

// ARP phase per sequence (MONO/ARP playback); reset on transport stop
static uint8_t arpIdx[SEQ_CH_COUNT] = {0};
// ARP phase of the LIVE keyboard voice (MONO+ARP on the keyboard channel)
static uint8_t liveArpIdx = 0;

// --- arp sequence builder (MONO + TYPE=ARP) -------------------------------
// Both the playhead track and the live keyboard voice read their note from
// here: the chord is sorted, stretched over the track's SPAN octaves,
// walked in its MODE order and finally expanded by the INST insert pattern
// (Arpie's rule set).  idx is the running arp position, the length of the
// expanded sequence comes back in *outLen (0 = nothing to play).
#define ARP_SEQ_MAX 64

static uint8_t  arpBuf[ARP_SEQ_MAX];    // expanded sequence (one walker)
static uint8_t  arpSrc[ARP_SEQ_MAX];    // canonical low-to-high walk
static uint8_t  arpOrd[ARP_SEQ_MAX];    // that walk in MODE order
static uint32_t arpPrng = 0x9E3779B9u;

static uint32_t arpRand() {
  uint32_t x = arpPrng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  arpPrng = x ? x : 0x9E3779B9u;
  return arpPrng;
}

// Arpie rerolls the chord once per octave of the walk
static void arpShuffle(uint8_t *b, uint8_t m) {
  if (m < 2) return;
  for (uint16_t i = (uint16_t)m - 1; i > 0; i--) {
    uint8_t j = (uint8_t)(arpRand() % (i + 1));
    uint8_t t = b[i]; b[i] = b[j]; b[j] = t;
  }
}

// Note insert: OFF passes through, HI/LOW put the extreme note in front of
// every other note, 3F1B/4F2B walk forward and step back.  Output is
// capped at ARP_SEQ_MAX; the returned length is what the index wraps on.
static uint8_t arpExpand(const uint8_t *src, uint8_t len, uint8_t inst,
                         uint8_t *out) {
  if (inst == SEQ_INST_HI || inst == SEQ_INST_LOW) {
    uint8_t ext = src[0];
    for (uint8_t i = 1; i < len; i++) {
      if (inst == SEQ_INST_HI) { if (src[i] > ext) ext = src[i]; }
      else                     { if (src[i] < ext) ext = src[i]; }
    }
    uint8_t o = 0;
    for (uint8_t i = 0; i < len && o + 2 <= ARP_SEQ_MAX; i++) {
      if (src[i] == ext) continue;
      out[o++] = ext;
      out[o++] = src[i];
    }
    return o;
  }
  if (inst == SEQ_INST_3F1B || inst == SEQ_INST_4F2B) {
    uint8_t fwd  = (inst == SEQ_INST_3F1B) ? 3 : 4;
    uint8_t back = (inst == SEQ_INST_3F1B) ? 1 : 2;
    int16_t i = 0;
    uint8_t j = 0, o = 0;
    while (i >= 0 && i < len && o < ARP_SEQ_MAX) {
      out[o++] = src[i];
      j++;
      i = (j % fwd == 0) ? (int16_t)(i - back) : (int16_t)(i + 1);
    }
    return o;
  }
  uint8_t k = len < ARP_SEQ_MAX ? len : ARP_SEQ_MAX;
  for (uint8_t i = 0; i < k; i++) out[i] = src[i];
  return k;
}

static uint8_t arpNoteAt(const uint8_t *ch, uint8_t n, uint8_t seq,
                         uint8_t idx, uint8_t *outLen) {
  *outLen = 0;
  if (n == 0) return 0;
  if (n > CHORD_MAX) n = CHORD_MAX;
  uint8_t span = seqArpSpanOf(seq);
  if (span < 1) span = 1; else if (span > 4) span = 4;
  uint8_t mode = seqArpModeOf(seq);
  uint8_t inst = seqArpInstOf(seq);

  // canonical walk: chord sorted low to high, one copy per octave, with
  // the extension folded back into the MIDI range (Arpie clamps as well)
  static uint8_t sorted[CHORD_MAX];
  for (uint8_t i = 0; i < n; i++) sorted[i] = ch[i];
  for (uint8_t i = 1; i < n; i++) {            // insertion sort (n <= 8)
    uint8_t v = sorted[i];
    uint8_t j = i;
    while (j > 0 && sorted[j - 1] > v) { sorted[j] = sorted[j - 1]; j--; }
    sorted[j] = v;
  }
  uint8_t L = 0;
  for (uint8_t o = 0; o < span && L < ARP_SEQ_MAX; o++)
    for (uint8_t i = 0; i < n && L < ARP_SEQ_MAX; i++) {
      uint8_t v = (uint8_t)(sorted[i] + 12 * o);
      while (v > 127) v = (uint8_t)(v - 12);
      arpSrc[L++] = v;
    }
  if (L == 0) return 0;

  for (uint8_t i = 0; i < L; i++) arpOrd[i] = arpSrc[i];
  uint8_t M = L;                             // length before INST expansion

  if (mode == SEQ_ARP_DOWN) {
    for (uint8_t i = 0; i < L; i++) arpOrd[i] = arpSrc[L - 1 - i];
  } else if (mode == SEQ_ARP_UPDOWN) {
    // up through every octave, back down without repeating either turning
    // point: 0..L-1, L-2..1, ... (period 2L-2; Arpie skips the top note
    // on the way down and the bottom one on the way up)
    if (L > 1) {
      uint8_t k = 0;
      for (uint8_t i = 0; i < L; i++) arpOrd[k++] = arpSrc[i];
      for (uint8_t i = (uint8_t)(L - 2); i > 0; i--) arpOrd[k++] = arpSrc[i];
      M = k;
    }
  } else if (mode == SEQ_ARP_RANDOM || mode == SEQ_ARP_RANDOM2) {
    // stable shuffle per chord: RANDOM keeps it until the chord changes,
    // RANDOM2 rerolls every full pass of the sequence (Arpie rebuilds it
    // on the wrap).  The pass length is order-independent, so it can be
    // measured on the unshuffled walk first.
    uint32_t h = 0x811C9DC5u;
    for (uint8_t i = 0; i < n; i++) { h ^= sorted[i]; h *= 16777619u; }
    h = h * 2654435761u + span;
    uint8_t cycle = 0;
    if (mode == SEQ_ARP_RANDOM2) {
      uint8_t e0 = arpExpand(arpSrc, L, inst, arpBuf);
      if (e0) cycle = (uint8_t)((idx / e0) & 0xFF);
    }
    for (uint8_t o = 0; o < span; o++) {
      arpPrng = h ^ (0x9E3779B9u * (uint32_t)(o + 1)) ^
                        (0x85EBCA6Bu * (uint32_t)(cycle + 1));
      if (!arpPrng) arpPrng = 0x9E3779B9u;
      arpShuffle(&arpOrd[o * n], n);
    }
  }

  uint8_t E = arpExpand(arpOrd, M, inst, arpBuf);
  if (E == 0) return 0;
  *outLen = E;
  return arpBuf[idx % E];
}

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
  if (keyChordMode()) {
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
// mode only - in chord modes the engine re-sends via chordMute(false); in
// HAND the strings are already sounding and the engine stays muted)
static void strikeRaw() {
  if (keyChordMode() || handLive()) return;
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
// gesture; called from the loop poll, every gesture and every octave change.
// The chord engine is muted while the MONO voice runs OR while HAND
// performs (chordMute is idempotent, so reconciling every call is free):
// state keeps building - incoming chords, the KEYMODE2/4 gesture and the
// KEY4 proposals all stay live - only the sound is cut
static void monoApply() {
  bool eng = monoEngaged();
  if (eng != monoEng) {
    monoEng = eng;
    if (eng) {
      cutRaw();
    } else {
      monoVoiceOff();
      strikeRaw();
    }
  }
  chordMute(eng || handLive());
  if (eng) monoLiveUpdate();
}

void keyMonoPoll() { monoApply(); }

// --- strum queue (POLY FX / MONO TYPE=STRUM / POLY TYPE=STRUM) ------------
// staggered note-ons drained by seqStrumPoll() from loop context, beside
// the gate timers.  Two flavors: the fixed-tempo rake (POLY FX, MONO/POLY
// STRUM - `timed` = false, notes go out every `every` ms) and the HAND
// gesture replay (`timed` = true: the recorded u16 onsets sit in `off[]`
// and are relative to the queue's start - the real timing the player
// performed, clamped only by step scheduling).  A POLY strum accumulates -
// the chord rakes note by note and the usual step-boundary release cuts
// the whole lot; a MONO strum is a single voice walking the chord, every
// rake note cutting the one before it (offOld = GLIDE: the old step's
// notes die AFTER the first rake note, like the legato overlap does).
// releaseCh() cancels it, so a rest, a length change or transport stop
// drops the un-sent tail
static struct {
  uint8_t note[CHORD_MAX];
  uint8_t vel[CHORD_MAX];
  uint8_t n, i;              // queued notes / next to send
  uint16_t every;            // ms between rake notes (fixed-tempo mode)
  uint16_t off[CHORD_MAX];   // timed mode: onset of each note (ms from start)
  uint32_t at;               // next note due (millis)
  uint32_t arm;              // MONO gate stamp armed on completion (0 = none)
  bool monoQ;
  bool offOld;
  bool timed;                // true = use off[] onsets, keep `at` as start
} strumQ[SEQ_CH_COUNT];

// HAND's current sounding chord (set when its notes went out via a
// HAND-typed step or live pluck); the boundary release lets it ring
static bool handRing[SEQ_CH_COUNT];

static void strumCancel(uint8_t c) {
  strumQ[c].n = 0;
  strumQ[c].i = 0;
}

// stage one step's rake; notes arrive playback-shifted and velocity-shaped.
// dts = per-note onsets (ms, u16) for HAND replay; null = fixed-tempo rake
static void strumEnqueue(uint8_t c, const uint8_t *note, const uint8_t *vel,
                         uint8_t n, uint16_t every, bool monoQ, bool offOld,
                         uint32_t arm, const uint16_t *dts = 0) {
  strumQ[c].n = 0;
  strumQ[c].i = 0;
  if (n > CHORD_MAX) n = CHORD_MAX;
  for (uint8_t k = 0; k < n; k++) {
    strumQ[c].note[k] = note[k];
    strumQ[c].vel[k] = vel[k];
    strumQ[c].off[k] = dts ? dts[k] : 0;
  }
  strumQ[c].n = n;
  strumQ[c].every = every ? every : 1;
  strumQ[c].at = millis();
  strumQ[c].arm = arm;
  strumQ[c].monoQ = monoQ;
  strumQ[c].offOld = offOld;
  strumQ[c].timed = dts != 0;
}

// drain due rake notes (called from the loop, next to seqGatePoll)
void seqStrumPoll() {
  uint32_t now = millis();
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    if (!strumQ[c].n) continue;
    while (strumQ[c].i < strumQ[c].n) {
      uint32_t due = strumQ[c].timed
                       ? strumQ[c].at + strumQ[c].off[strumQ[c].i]
                       : strumQ[c].at;
      if ((int32_t)(now - due) < 0) break;
      uint8_t k = strumQ[c].i;
      uint8_t note = strumQ[c].note[k];
      uint8_t vel = strumQ[c].vel[k];
      if (strumQ[c].monoQ) {
        if (k == 0) {
          // first rake note: on BEFORE the old notes go (legato overlap
          // when GLIDE deferred them, plain cut otherwise - the latter
          // cannot happen: the step-boundary release already ran)
          usbMIDI.sendNoteOn(note, vel, c + 1);
          if (strumQ[c].offOld) {
            for (uint8_t j = 0; j < seqLen[c]; j++)
              usbMIDI.sendNoteOff(seqNotes[c][j], 0, c + 1);
            seqLen[c] = 0;
            gateOffAt[c] = 0;
            strumQ[c].offOld = false;
          }
        } else {
          if (seqLen[c])
            usbMIDI.sendNoteOff(seqNotes[c][0], 0, c + 1);
          usbMIDI.sendNoteOn(note, vel, c + 1);
        }
        seqNotes[c][0] = note;
        seqLen[c] = 1;
      } else {
        if (seqLen[c] < CHORD_MAX) {
          seqNotes[c][seqLen[c]] = note;
          seqLen[c]++;
        }
        usbMIDI.sendNoteOn(note, vel, c + 1);
      }
      strumQ[c].i++;
      if (!strumQ[c].timed) strumQ[c].at = now + strumQ[c].every;
    }
    if (strumQ[c].i >= strumQ[c].n) {
      if (strumQ[c].arm) {              // MONO: the gate counts from the
        gateOffAt[c] = strumQ[c].arm;   // STEP start, not the rake end
        strumQ[c].arm = 0;
      }
      strumQ[c].n = 0;
      strumQ[c].i = 0;
    }
  }
}

// release one sequence's sounding notes and cancel its gate timer
static void releaseCh(uint8_t c) {
  for (uint8_t i = 0; i < seqLen[c]; i++)
    usbMIDI.sendNoteOff(seqNotes[c][i], 0, c + 1);
  seqLen[c] = 0;
  gateOffAt[c] = 0;
  strumCancel(c);
  handRing[c] = false;
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

// does the head at position p sound?  inside its pattern, plus (while a bar
// loop is armed) inside the looped bar - the head may walk its pattern
// toward a loop that starts later, and it may sit beyond the pattern
// inside a looped bar
static bool headSounds(uint8_t ch, uint8_t p) {
  if (p < seqPatLenOf(ch)) return true;
  if (!seqLoopOn(ch)) return false;
  uint8_t lo, hi;
  seqPlayRange(ch, &lo, &hi);
  return p >= lo && p < hi;
}

// is this absolute step of the EDIT track ever read?  steps past the
// pattern length are unused and their pads go dark + inert (an armed bar
// loop still covers its own 16 steps, so those stay live)
bool seqStepUsed(uint8_t step) {
  return step < STEP_MAX && headSounds(seqEditCh, step);
}

// playhead tick: every track steps its OWN position (its pattern length or
// its looped bar), so lengths may differ (polymeter).  start = transport
// (re)start: every track snaps to its range head first.  MONO sequences
// transform their chord at playback (ROOT = root note, BASS = lowest note,
// ARP = next chord note), then every channel applies its own octave shift
// (ENC1) - and honors the step-edit attributes: TIE bridges into the next
// step, GLIDE overlaps the notes (legato), ACCENT plays 127, PROB gates by
// its stored chance, the track's gate time cuts the note mid-step
// pattern-speed state: each track's accumulator turns master ticks into
// its own sub-steps (SEQMODE3 ENC4), spdGo = sub-steps still owed inside
// the current tick, chInRng = window state carried over skipped ticks
static uint16_t spdAcc[SEQ_CH_COUNT];
static uint8_t  spdGo[SEQ_CH_COUNT];
static bool     chInRng[SEQ_CH_COUNT];

// one sub-step of the transport: every track whose speed gate fired this
// sub-tick (`go[c]`) walks its position and re-voices, the others keep
// sounding untouched.  lo/hi are recomputed (stateless) so FOLLOW can
// still read a feeder that did not advance
static void seqAdvanceOnce(bool start, const bool *go,
                           uint32_t now, uint32_t stepDur) {
  // position of every track for this tick: it walks its own pattern toward
  // an armed bar loop.  Activating a loop TELEPORTS the playhead directly
  // to the looped bar (immediate loop).  A looped bar wraps at its own
  // end, a loop beyond the pattern falls in right away.  Disabling a loop
  // resumes at the global position (shadow).  inRng = allowed to
  // sound (pattern OR looped bar)
  uint8_t lo[SEQ_CH_COUNT], hi[SEQ_CH_COUNT];
  bool inRng[SEQ_CH_COUNT];
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    seqPlayRange(c, &lo[c], &hi[c]);
    if (!go[c]) { inRng[c] = chInRng[c]; continue; }   // skipped this tick
    uint8_t phi = seqPatLenOf(c);
    bool lp = seqLoopOn(c);
    if (start) {
      chStep[c] = lo[c];                  // start honors an armed loop
      chStepShadow[c] = lo[c];            // shadow starts at same position
    } else {
      // advance shadow position: always follows pattern length (no loop wrapping)
      // this tracks where the playhead WOULD be without any loop
      chStepShadow[c]++;
      if (phi == 0 || chStepShadow[c] >= phi) chStepShadow[c] = 0;

      // detect loop activation: was off, now on -> teleport to loop start
      if (!loopWasOn[c] && lp) {
        chStep[c] = lo[c];                // jump directly to looped bar
      }
      // detect loop disable: was looping, now not looping
      else if (loopWasOn[c] && !lp) {
        chStep[c] = chStepShadow[c];      // resume at global position
      }

      if (lp && chStep[c] >= lo[c] && chStep[c] < hi[c]) {
        chStep[c]++;                        // inside the looped bar
        if (chStep[c] >= hi[c]) chStep[c] = lo[c];   // wrap at the bar's end
      } else {
        chStep[c]++;                        // walking the pattern toward it
        if (lp && lo[c] >= phi) chStep[c] = lo[c];   // bar beyond pattern: fall in
        else if (phi == 0 || chStep[c] >= phi) chStep[c] = 0;  // pattern wrap
      }
    }
    loopWasOn[c] = lp;                    // remember for next tick
    chInRng[c] = inRng[c] = headSounds(c, chStep[c]);
  }
  // live record: one pass over the edit track's play range - armed waits
  // for its range head (the pattern's 1st bar, or the looped bar's head),
  // the run then covers the whole range and switches the mode off
  if (recState && go[recCh]) {
    if (recState == 1) {
      if (chStep[recCh] == lo[recCh] && hi[recCh] > lo[recCh]) {
        recState = 2;
        recLeft = (uint8_t)(hi[recCh] - lo[recCh]);
      }
    } else if (recLeft > 0 && --recLeft == 0) {
      recState = 0;
      recDone = true;
    }
  }
  // per-channel release honoring the previous step's attributes: TIE keeps
  // the note sounding (no release, and the retrigger below is skipped),
  // GLIDE defers its note-off until after the new note-on; a track that
  // left its window (length shrunk, loop moved) releases right here
  bool tied[SEQ_CH_COUNT], defer[SEQ_CH_COUNT];
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    tied[c] = defer[c] = false;
    if (!go[c]) continue;               // parked this sub-tick: keep sounding
    if (!inRng[c]) { releaseCh(c); advPrev[c] = 0xFF; continue; }
    uint8_t pf = (advPrev[c] < STEP_MAX && seqTrackOf(c) == SEQ_TRACK_MONO)
                   ? stepAtt[c][advPrev[c]] : 0;
    if (pf & SEQ_ATT_TIE) { tied[c] = true; continue; }
    if (pf & SEQ_ATT_GLIDE) { defer[c] = true; continue; }
    if (handRing[c]) continue;        // HAND chord rings across the boundary
    releaseCh(c);
  }
  // carry the current pending chords forward into the new position: the
  // chord played during steps 1-8 is the "actual chord" of step 8 even
  // though it was fed while the playhead sat on step 1
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++)
    if (go[c]) histAt(c, chStep[c]);
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    if (!go[c]) continue;                 // did not advance this sub-tick
    if (!inRng[c]) continue;              // parked outside its window
    if (tied[c]) continue;              // merged note keeps sounding
    // this track's own step length: its pattern speed scales the tick
    uint32_t sd = stepDur;
    if (sd) {
      sd = (sd * seqSpeedDenOf(c)) / seqSpeedNumOf(c);
      if (!sd) sd = 1;
    }
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
          // cyclic inside the domain the feeder is reading right now (its
          // looped bar while engaged, its pattern while walking toward an
          // armed loop), until content shows up
          uint8_t slo, shi;
          if (seqLoopOn(fx) && chStep[fx] >= lo[fx] && chStep[fx] < hi[fx]) {
            slo = lo[fx]; shi = hi[fx];    // inside its looped bar
          } else {
            slo = 0; shi = seqPatLenOf(fx);   // walking its pattern
          }
          uint8_t N = (uint8_t)(shi - slo);
          uint8_t n = (uint8_t)(fstp - slo);
          for (uint8_t k = 1; k < N && len == 0; k++) {
            uint8_t j = (uint8_t)(slo + (n + N - k) % N);
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
    uint8_t att = drumTrack ? 0 : stepAtt[c][stp];   // DRUM rolls per voice
    bool plays = (len > 0) && !seqIsMuted(c);
    // probability gate (MONO/POLY): the track's own %, read live so ENC8
    // moves every armed step at once; a miss silences the step this pass
    if (plays && (att & SEQ_ATT_PROB) &&
        (uint8_t)random(100) >= (uint8_t)seqProb[c])
      plays = false;
    if (!plays) {
      // ARP SKIP: a rest (or a probability miss) still walks the arp
      // position, so the sequence keeps running over dead steps instead
      // of pausing on them
      if (!seqIsMuted(c) && seqTrackOf(c) == SEQ_TRACK_MONO &&
          seqMonoTypeOf(c) == SEQ_MONO_ARP && seqArpSkipOf(c))
        arpIdx[c]++;
      // a rest / probability miss keeps HAND's ring sounding (the next
      // chord, a mute or leaving the window cuts it)
      if (!handRing[c] || seqIsMuted(c)) releaseCh(c);   // also ends a glide
      continue;
    }
    uint8_t *ch = follow ? stepChord[seqInCh[c] - 1][fstp]
                         : stepChord[c][stp];
    // step FX (POLY embellishments): SUS/ext reshape the chord and COLOR
    // appends its raw approach note BEFORE the FScale snap below, so the
    // ghost stays chromatic while the core follows the scale.  MONO picks
    // one note (FX would only muddy it) and DRUM carries drum-map ids -
    // both skip the transform
    uint8_t mapped[CHORD_MAX];
    uint8_t nCore = len;               // transformable core inside mapped[]
    // effective POLY playback type of this step: HAND/STRUM ride the
    // step's bits6-7 override, else the track default (DRUM/MONO = STEP)
    uint8_t et = (mono || drumTrack) ? SEQ_POLY_STEP : effStepType(c, stp);
    bool handStep = (et == SEQ_POLY_HAND);
    bool strumStep = (et == SEQ_POLY_STRUM);
    // a HAND gesture recorded with real timing replays literally: the stored
    // u16 onsets index the STORED chord order, so a timed step bypasses the
    // FX reshape and the FScale snap (order and dt survive).  All-zero dts
    // (block-written chord) fall back to the even 8..40 ms spread below
    bool handTimed = (handStep && !follow && stepStrumTimed(c, stp));
    uint8_t fxm = (drumTrack || mono || handTimed) ? 0 : stepFx[c][stp];
    if (fxm) {
      len = chordFxApply(ch, len, fxm, mapped, &nCore);
      ch = mapped;
    }
    // FScale: force-to-scale also governs PLAYBACK - with it ON the stored
    // notes are snapped to the current scale as they leave (duplicates
    // collapse, like buildNotes), so turning the scale recolors recorded
    // progressions live.  Stored notes stay raw; OFF/Chromatic = unchanged.
    // DRUM steps keep their raw note numbers (they are drum-map ids), timed
    // HAND steps keep their literal pitch (that is what was performed).  The
    // FX color tail (i >= nCore) bypasses the snap and dedups like the rest
    if (fxm || (chordFScale() && !drumTrack && !handTimed)) {
      bool snap = chordFScale();
      uint8_t m = 0;
      uint8_t coreOut = 0;
      for (uint8_t i = 0; i < len && i < CHORD_MAX; i++) {
        uint8_t v = (snap && i < nCore) ? chordForceNote(ch[i]) : ch[i];
        bool dup = false;
        for (uint8_t j = 0; j < m; j++) if (mapped[j] == v) dup = true;
        if (!dup) {
          if (i < nCore) coreOut++;
          mapped[m++] = v;
        }
      }
      if (m) { len = m; ch = mapped; }
      if (fxm) nCore = coreOut;        // compaction may have shrunk the core
    }
    int8_t sh = seqOct[c];
    if (mono) {
      uint8_t vel = (att & SEQ_ATT_ACCENT) ? seqAccVel[c] : seqVel[c];
      // gate timer: TIE/GLIDE hold to the boundary by definition
      uint32_t arm = 0;
      if (sd && seqGate[c] < 100 && !(att & (SEQ_ATT_TIE | SEQ_ATT_GLIDE)))
        arm = now + (sd * (uint32_t)seqGate[c]) / 100;
      if (seqMonoTypeOf(c) == SEQ_MONO_STRUM) {
        // TYPE=STRUM: rake through the whole chord INSIDE the step -
        // ascending, each note cutting the last (the funk scratch), the
        // last one held to the gate, which arms when the rake completes.
        // The queue owns the voice from here
        uint8_t order[CHORD_MAX];
        uint8_t ovel[CHORD_MAX];
        uint8_t n = 0;
        for (uint8_t i = 0; i < len && i < CHORD_MAX; i++) order[n++] = ch[i];
        for (uint8_t i = 1; i < n; i++) {          // ascending rake order
          uint8_t v = order[i];
          uint8_t j = i;
          while (j > 0 && order[j - 1] > v) { order[j] = order[j - 1]; j--; }
          order[j] = v;
        }
        for (uint8_t i = 0; i < n; i++) {
          order[i] = shNote(order[i], sh);
          ovel[i] = vel;
        }
        uint32_t e = n ? (sd / ((uint32_t)n * 2)) : sd;
        if (e < 8) e = 8;
        if (e > 40) e = 40;
        strumEnqueue(c, order, ovel, n, (uint16_t)e, true, defer[c], arm);
      } else {
        uint8_t note;
        if (seqMonoTypeOf(c) == SEQ_MONO_BASS) {
          note = ch[0];
          for (uint8_t i = 1; i < len; i++) if (ch[i] < note) note = ch[i];
        } else if (seqMonoTypeOf(c) == SEQ_MONO_ARP) {
          uint8_t elen = 0;
          note = arpNoteAt(ch, len, c, arpIdx[c], &elen);
          if (!elen) note = ch[0];       // degenerate walk: hold the chord
          arpIdx[c]++;
        } else {
          note = rootOf(ch, len);         // SEQ_MONO_ROOT
        }
        note = shNote(note, sh);
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
        if (arm) gateOffAt[c] = arm;
      }
    } else {
      // new content cuts HAND's ring (or a deferred glide) first
      if (defer[c] || handRing[c]) releaseCh(c);
      // STRUM rakes at the fixed 8..40 ms tempo (ascending), HAND replays
      // the gesture with its recorded onsets - or the even spread when the
      // chord was block-written (all dts zero)
      bool doStrum = strumStep || handStep;
      uint8_t snote[CHORD_MAX], svel[CHORD_MAX];
      uint16_t sdts[CHORD_MAX];
      uint8_t sn = 0;
      for (uint8_t i = 0; i < len && i < CHORD_MAX; i++) {
        uint8_t raw = ch[i];
        // DRUM voices muted in the ENCMODE overlay stay silent (match the
        // stored note, like the part select - the octave shift applies after)
        if (drumTrack && drumVoiceMuted(raw)) continue;
        uint8_t dvel = (att & SEQ_ATT_ACCENT) ? seqAccVel[c] : KEY_VEL;
        // FX color ghost: the approach note rides in at half velocity so
        // it whispers under the chord it leads into (FX is never DRUM/MONO)
        if (fxm && i >= nCore) {
          dvel = (uint8_t)(dvel / 2);
          if (!dvel) dvel = 1;
        }
        // per-voice accent + probability (DRUM only; POLY reads stepAtt).
        // The mask picks WHICH parts are gated, the track's probability
        // says HOW OFTEN - both stay live under ENC8
        uint8_t v = (drumTrack && raw >= 36 && raw < 52)
                      ? (uint8_t)(raw - 36) : 0xFF;
        if (v != 0xFF) {
          if ((drumProbOn[c][stp] >> v) & 1) {
            if ((uint8_t)random(100) >= (uint8_t)seqProb[c])
              continue;                 // this voice misses its roll
          }
          if ((drumAccent[c][stp] >> v) & 1) dvel = seqAccVel[c];
        }
        uint8_t note = shNote(raw, sh);
        if (doStrum) {                  // stage the rake (STRUM / HAND)
          if (sn < CHORD_MAX) {
            snote[sn] = note;
            svel[sn] = dvel;
            // timed HAND: the stored onset for this STORED index (untimed
            // steps carry all zeros -> handled by the spread below)
            sdts[sn] = handTimed ? stepStrumDt[c][stp][i] : 0;
            sn++;
          }
        } else {
          seqNotes[c][seqLen[c]] = note;
          usbMIDI.sendNoteOn(note, dvel, c + 1);
          seqLen[c]++;
        }
      }
      if (doStrum && sn) {
        if (handStep && handTimed) {
          // literal replay: the performed order and the u16 onsets, so a
          // long strum spills across the following steps/bars as recorded
          strumEnqueue(c, snote, svel, sn, 0, false, false, 0, sdts);
        } else {
          // stagger = half a note apart inside the step, capped so slow
          // steps still finish the rake well before the boundary.  STRUM
          // and the block-built HAND spread both rake ascending
          for (uint8_t i = 1; i < sn; i++) {
            uint8_t vn = snote[i], vv = svel[i];
            uint8_t j = i;
            while (j > 0 && snote[j - 1] > vn) {
              snote[j] = snote[j - 1];
              svel[j] = svel[j - 1];
              j--;
            }
            snote[j] = vn;
            svel[j] = vv;
          }
          uint32_t e = sd / ((uint32_t)sn * 2);
          if (e < 8) e = 8;
          if (e > 40) e = 40;
          strumEnqueue(c, snote, svel, sn, (uint16_t)e, false, false, 0);
        }
        if (handStep) handRing[c] = true;   // rings until next content
      }
    }
  }
  // F-row blink: attributes of the edit channel's step the playhead reads
  // (MONO/POLY = the step flags; DRUM = any part carrying accent/prob)
  fBlinkMask = 0;
  if (inRng[seqEditCh]) {
    uint8_t fTrk = seqTrackOf(seqEditCh);
    uint8_t fstp = chStep[seqEditCh];
    if (fTrk == SEQ_TRACK_MONO || fTrk == SEQ_TRACK_POLY) {
      // only the four classic attribute bits (0..3 = F1..F4); the TYPE
      // override lives in bits6-7 and never maps to a key on its own
      fBlinkMask = stepChordLen[seqEditCh][fstp] > 0
                     ? (uint8_t)(stepAtt[seqEditCh][fstp] & 0x0F) : 0;
      // POLY steps carrying FX blink F1 (the FX key) from bit 0, steps
      // with a TYPE override blink F2 - TIE/GLIDE's slots are unused on a
      // chord track, so the classic masks stay correct
      if (fTrk == SEQ_TRACK_POLY && stepChordLen[seqEditCh][fstp] > 0) {
        if (stepFx[seqEditCh][fstp]) fBlinkMask |= 0x01;
        if (stepAtt[seqEditCh][fstp] & 0xC0) fBlinkMask |= 0x02;
      }
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
  uint8_t kc = (uint8_t)(keyCh - 1);
  if (kc < SEQ_CH_COUNT && go[kc] && monoEngaged() &&
      monoType() == SEQ_MONO_ARP) {
    const uint8_t *src;
    uint8_t n = monoSrc(&src);
    monoVoiceOff();
    if (n > 0) {
      uint8_t elen = 0;
      uint8_t note = arpNoteAt(src, n, kc, liveArpIdx, &elen);
      liveArpIdx++;
      if (!elen) note = src[0];
      usbMIDI.sendNoteOn(note, KEY_VEL, keyCh);
      monoNote = note;
      monoOn = true;
    }
  }
}

// transport tick: the clock hands us ONE master step (a 16th note at the
// current tempo).  Every track converts that into its own sub-steps from
// its pattern speed (SEQMODE3 ENC4): a slow track skips ticks until its
// accumulator fills, a fast one runs 2 or 4 sub-steps inside the same
// tick - so tracks may pull apart rhythmically (polyrhythm on top of the
// existing polymeter).  start = transport (re)start: reset every
// accumulator and snap all tracks to their range head in one sub-step
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

  bool go[SEQ_CH_COUNT];
  if (start) {
    for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
      spdAcc[c] = 0;
      spdGo[c] = 0;
      go[c] = true;                       // every track snaps to its head
    }
    seqAdvanceOnce(true, go, now, stepDur);
    return;
  }
  uint8_t reps = 1;
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    uint8_t num = seqSpeedNumOf(c), den = seqSpeedDenOf(c);
    spdAcc[c] = (uint16_t)(spdAcc[c] + num);
    uint8_t n = 0;
    while (spdAcc[c] >= den) { spdAcc[c] = (uint16_t)(spdAcc[c] - den); n++; }
    spdGo[c] = n;
    if (n > reps) reps = n;
  }
  for (uint8_t r = 0; r < reps; r++) {
    bool any = false;
    for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
      go[c] = (spdGo[c] > 0);
      if (go[c]) { spdGo[c]--; any = true; }
    }
    if (any) seqAdvanceOnce(false, go, now, stepDur);
  }
}

void seqStepStop() {
  seqRelease();
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++)
    advPrev[c] = 0xFF;               // no carry-over into the next run
  fBlinkMask = 0;
  if (recState == 2) recState = 1;   // interrupted pass re-arms for the next run
  recStarted = false;
  // the live arp is step-driven: silence it with the transport (the
  // ROOT/BASS voice follows the keys and keeps sounding)
  if (monoEngaged() && monoType() == SEQ_MONO_ARP) monoVoiceOff();
  liveArpIdx = 0;
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) arpIdx[c] = 0;
}

// playhead pad of the edit WINDOW (0..15): the edit track's head mapped
// into the 16-step bar shown on the pads - 0xFF when the head is outside
// that bar (another bar playing) or silent (no pattern, no armed loop)
uint8_t seqStepPlayPad(void) {
  uint8_t p = chStep[seqEditCh];
  if (!headSounds(seqEditCh, p)) return 0xFF;
  uint8_t base = seqStepViewBase();
  if (p < base || p >= (uint8_t)(base + STEP_COUNT)) return 0xFF;
  return (uint8_t)(p - base);
}

// ghost pad: the head's position WITHIN ITS OWN bar, mirrored onto the
// pads even while another bar is shown - the hooks paint it dark blue so
// time keeps marching while you edit a bar the playback is not reading
uint8_t seqStepPlayGhost(void) {
  uint8_t p = chStep[seqEditCh];
  if (!headSounds(seqEditCh, p)) return 0xFF;
  return (uint8_t)(p % STEP_COUNT);
}

// bar (0..3) the edit track's playhead currently reads, 0xFF when it is
// silent - the SEQ3 bar LEDs pulse on it so the dot walks C D E F as
// playback crosses the bars, also while an armed loop still waits ahead
uint8_t seqPlayBar(void) {
  uint8_t p = chStep[seqEditCh];
  if (!headSounds(seqEditCh, p)) return 0xFF;
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
  probSRes = 0;
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
// sequence, 4 counts = 1, clamped 1..127 (accented steps use seqAccVel)
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

// ENC8 in SEQMODE: playback probability of the edit sequence, 4 counts =
// 1%, clamped 0..100 (100 = every armed step plays).  Nothing is copied
// into the steps - the playhead re-reads seqProb on every advance, so a
// turn is audible on the very next hit
void seqProbAdjust(int16_t counts) {
  probSRes += counts;
  int16_t st = 0;
  while (probSRes >= 4) { probSRes -= 4; st++; }
  while (probSRes <= -4) { probSRes += 4; st--; }
  if (!st) return;
  int v = (int)seqProb[seqEditCh] + st;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  seqProb[seqEditCh] = (uint8_t)v;
}

int seqProbPct() { return seqProb[seqEditCh]; }

// F3 held in SEQMODE: ENC6 edits the edit sequence's ACCENT velocity
// instead of its default one, 4 counts = 1, clamped 1..127 (boot 101)
void seqAccentVelocityAdjust(int16_t counts) {
  accSRes += counts;
  int16_t st = 0;
  while (accSRes >= 4) { accSRes -= 4; st++; }
  while (accSRes <= -4) { accSRes += 4; st--; }
  if (!st) return;
  int v = (int)seqAccVel[seqEditCh] + st;
  if (v < 1) v = 1;
  if (v > 127) v = 127;
  seqAccVel[seqEditCh] = (uint8_t)v;
}

int seqAccentVelocity() { return seqAccVel[seqEditCh]; }

void seqSetAccentVelocity(uint8_t seq, uint8_t v) {
  if (seq >= SEQ_CH_COUNT) return;
  if (v < 1) v = 1;
  if (v > 127) v = 127;
  seqAccVel[seq] = v;
}

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
  handRing[seq] = false;        // a mute cuts the HAND ring too
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
  strumCancel(s);              // an erase also drops the un-sent rake tail
  handRing[s] = false;
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
// Settings only - the note patterns, their contents (steps, tie/glide/
// accent/probability attributes) and the transport state stay RAM.  Layout
// at address 160 (after the 131-byte CC block): magic | editCh | reserved |
// oct[16] | inCh[16] | vel[16] | gate[16] | mute[16] | track[16] | mono[16]
// | speed[16] | acc[16] | span[16] | arpMode[16] | inst[16] | skip[16] |
// prob[16] | patLen[16] | follow[16] | checksum = 260 bytes; blank or
// corrupt keeps the defaults
static const uint8_t  SEQ_EE_MAGIC = 0x53;
static const uint16_t SEQ_EE_ADDR = 160;
static const uint16_t SEQ_EE_LEN = 260;

void seqCfgSave() {
  uint8_t b[SEQ_EE_LEN];
  b[0] = SEQ_EE_MAGIC;
  b[1] = seqEditCh + 1;
  b[2] = 0;                          // was the shared prob clipboard
  for (uint8_t i = 0; i < SEQ_CH_COUNT; i++) {
    b[3 + i] = (uint8_t)seqOct[i];
    b[19 + i] = seqInCh[i];
    b[35 + i] = seqVel[i];
    b[51 + i] = seqGate[i];
    b[67 + i] = seqMute[i] ? 1 : 0;
    b[83 + i] = seqTrackOf(i);
    b[99 + i] = seqMonoTypeOf(i);
    b[115 + i] = seqSpeedIdxOf((uint8_t)i);   // pattern speed 0..6
    b[131 + i] = seqAccVel[i];                // accent velocity 1..127
    b[147 + i] = seqArpSpanOf(i);             // arp span 1..4 octaves
    b[163 + i] = seqArpModeOf(i);             // arp mode 0..4
    b[179 + i] = seqArpInstOf(i);             // arp insert 0..4
    b[195 + i] = seqArpSkipOf(i) ? 1 : 0;     // arp skip rests
    b[211 + i] = seqProb[i];                  // playback probability %
    b[227 + i] = seqPatLenOf(i);              // pattern length 0..64
    b[243 + i] = seqFollowOf(i) ? 1 : 0;      // FOLLOW (notes from feeder)
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
    seqSpeedSetOf(i, b[115 + i]);         // clamped inside (bad -> 4)
    seqSetAccentVelocity(i, b[131 + i]);  // clamped inside (0 -> 1)
    seqArpSpanSet(i, b[147 + i]);         // clamped inside (bad -> 1)
    seqArpModeSet(i, b[163 + i]);         // clamped inside (bad -> UP)
    seqArpInstSet(i, b[179 + i]);         // clamped inside (bad -> OFF)
    seqArpSkipSet(i, b[195 + i] != 0);
    v = b[211 + i];
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    seqProb[i] = (uint8_t)v;
    seqPatLenSetOf(i, b[227 + i]);            // clamped inside (bad -> 64)
    seqFollowSetOf(i, b[243 + i] != 0);
    seqOctClamp(i);                      // content bounds (empty = convention)
  }
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
  if (recState && ch != (uint8_t)(recCh + 1)) {   // keyboard left the record track
    recState = 0;
    recStarted = false;
  }
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
    // two never double-send; HAND strings are plucks - never re-struck)
    if (!monoEng && !keyChordMode() && !handLive())
      for (uint8_t k = 0; k < 12; k++)
        if (keyActive[k]) {
          usbMIDI.sendNoteOn(keyNote[k], KEY_VEL, keyCh);
          keySounding[k] = true;
        }
    if (!handLive()) chordRestore();  // HAND: the engine stays muted
  }
  monoApply();                    // re-sync engage state + voice for ch
}
