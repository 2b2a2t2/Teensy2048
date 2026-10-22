#include "Keyboard.h"
#include "SeqClock.h"   // seqTrack() - step view follows the TRACK type
#include "Modes.h"      // keyBehaviorVariant() - KEYMODE2 = chord mode
#include "Chords.h"     // chord engine + scale filter state
#include "Events.h"     // prob clipboard accessors (SEQ save/load)
#include "Display.h"    // headerShow (strum option / string feedback)
#include "Key4.h"       // KEYMODE4 proposal gesture (press/release/commit)
#include "Gen.h"        // GNR8 params (the SEQ EEPROM block stores them)
#include "Skank.h"      // SKANK derive (POLY TYPE = SKANK playback)
#include "bjorklund.h"  // COMP: Euclid stab pattern (same primitive as GNR8)
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
static int16_t compSRes = 0;             // leftover encoder counts (comp HITS)
static int16_t skankRes[8] = {0};        // leftover counts (SKANK page dials)

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
// per-step embellishment overrides for KEYMODE4 (PREV hold encoders):
// [0] = sub prob 0..100 (255 = use global), [1] = inv bias 0..3 (255 = global)
// [2] = pass amt 0..100 (255 = global), [3] = color amt 0..100 (255 = global)
static uint8_t stepEmb[SEQ_CH_COUNT][STEP_MAX][4];
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
// GNR8 per-step pitch modifiers (F2/F3/F4 pages on the BASS track):
// octave transposition -2..+2, borrowed chord tone (0 = off, n = n-th
// tone of the incoming chord, ascending) and the probability % that the
// two actually apply on a pass (default 100 = always).  RAM only like
// every step field; defaults ride in stepAttClear, rotate/snapshot copy
static int8_t  stepOctM[SEQ_CH_COUNT][STEP_MAX];
static uint8_t stepBrwM[SEQ_CH_COUNT][STEP_MAX];
static uint8_t stepModP[SEQ_CH_COUNT][STEP_MAX];
// per-step semitone transpose -12..+12 (the TRANS knob on the step-hold
// row): non-destructive playback offset like stepOctM - the stored chord
// never changes.  RAM only like every step field; rotates with SHIFT and
// rides the GNR8 snapshot; defaults ride in stepAttClear
static int8_t  stepTrM[SEQ_CH_COUNT][STEP_MAX];
// per-step SKANK locks (hold-row encoders 1..6 on a POLY+SKANK track):
// raw 0 = inherit the page knob, else value+1 (1..101 = 0..100).  Stored
// with the step content (stepAttClear wipes), RAM only like every field
static uint8_t stepSkankM[SEQ_CH_COUNT][STEP_MAX][7];
static uint8_t seqVel[SEQ_CH_COUNT] = {90,90,90,90,90,90,90,90,
                                       90,90,90,90,90,90,90,90};
// per-track PLAYBACK probability in % (100 = the step always plays).  ENC8
// edits the edit track and every PROB step of that track feels it live -
// no per-step copy to re-toggle afterwards
static uint8_t seqProb[SEQ_CH_COUNT] = {100,100,100,100,100,100,100,100,
                                        100,100,100,100,100,100,100,100};
// per-track COMP: chord stabs per POLY STEP step (Euclid pattern in 8
// slots).  0 or 1 = today's single block chord, 2..8 = the chord re-strikes
// on the Euclid on-positions.  Edited live on the COMP page (M2 on a POLY
// STEP track, ENC1 = HITS); persists in the SEQ EEPROM block
static uint8_t compHits[SEQ_CH_COUNT] = {0};
// per-track SKANK page knobs (M2 on a POLY SKANK track): TES/MOT/SHP/THN/
// EXT/SUB, all 0..100 - MOT boots 50 (centered on the tessitura anchor),
// the rest 0 (no walk/no color/no sub = a plain block chord, like STEP).
// Persisted in the SEQ EEPROM block
static uint8_t skankPrmM[SEQ_CH_COUNT][8] = {
  {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0},
  {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0},
  {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0},
  {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0}, {0,50,0,0,0,0,0,0}};
// per-track semitone transpose -12..+12 (the TRANS knob on the PREV-held
// shift row): non-destructive playback offset, composed with the octave
// shift at the send sites.  Persisted in the SEQ EEPROM block; 0 = off.
// The DRUM track ignores it (its "notes" are drum-map ids, not pitches)
static int8_t seqTransM[SEQ_CH_COUNT] = {0};
// per-track velocity of an ACCENTED step (127 is the old hard-wire; 101 is
// the boot default) - F3 held + ENC6 edits it
static uint8_t seqAccVel[SEQ_CH_COUNT] = {101,101,101,101,101,101,101,101,
                                          101,101,101,101,101,101,101,101};
static uint8_t seqGate[SEQ_CH_COUNT] = {100,100,100,100,100,100,100,100,
                                        100,100,100,100,100,100,100,100};
static uint32_t gateOffAt[SEQ_CH_COUNT] = {0};   // 0 = no pending note-off
static uint8_t  harmLiveN[SEQ_CH_COUNT] = {0};   // LIVE HARM follow: chord
static uint8_t  harmLive[SEQ_CH_COUNT][CHORD_MAX]; // carried from the last
                                 // pass (a silent feeder replays it - the
                                 // harmony holds through a prob miss)
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
// and the DRUM per-voice accent/probability masks; the POLY FX byte, the
// HAND strum onsets and the GNR8 oct/borrow/prob modifiers too)
static void stepAttClear(uint8_t ch, uint8_t st) {
  stepAtt[ch][st] = 0;
  stepFx[ch][st] = 0;
  for (uint8_t i = 0; i < 4; i++) stepEmb[ch][st][i] = 255;  // 255 = use global
  drumAccent[ch][st] = 0;
  drumProbOn[ch][st] = 0;
  stepOctM[ch][st] = 0;          // GNR8 mods: no octave shift, no borrow,
  stepBrwM[ch][st] = 0;          // modifier always passes (prob 100)
  stepModP[ch][st] = 100;
  stepTrM[ch][st] = 0;           // per-step TRANS: no semitone offset
  for (uint8_t i = 0; i < 7; i++)
    stepSkankM[ch][st][i] = 0;   // SKANK locks: inherit the page knobs
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

// --- deferred HARM auto re-bake --------------------------------------------
// A POLY track with TYPE = HARM derives its steps from its input channel
// (the FOLLOW feeder), so every write to that channel leaves the bake
// stale.  All content writes funnel through stepSyncFrom, which only MARKS
// the HARM tracks feeding from the changed channel; harmRebakePoll (main
// loop, before the OLED draw) runs the actual re-bakes, so a burst of
// writes - a full GNR8 produce, a live knob re-generate - costs one Viterbi
// pass instead of one per step.  FOLLOW gates it exactly like the manual M3
// bake, a track is never marked through its own channel (manual HARM step
// edits and an M3 bake on track X stand), and a mark whose inference did
// not move converges to zero writes (k4HarmRebake skips unchanged steps),
// so input-channel cycles stay bounded
static bool sHarmDirty[SEQ_CH_COUNT];
static bool sHarmDirtyAny = false;

void harmFeederChanged(uint8_t ch0) {
  if (ch0 >= SEQ_CH_COUNT) return;
  for (uint8_t w = 0; w < SEQ_CH_COUNT; w++) {
    if (w == ch0) continue;                    // never self (edits stand)
    if (seqInCh[w] != (uint8_t)(ch0 + 1)) continue;   // its SRC points at ch0
    if (seqTrackOf(w) != SEQ_TRACK_POLY || seqPolyTypeOf(w) != SEQ_POLY_HARM)
      continue;
    if (!seqFollowOf(w)) continue;             // FOLLOW gates the source
    sHarmDirty[w] = true;
    sHarmDirtyAny = true;
  }
}

// run every marked re-bake (Teensy2048.ino calls this once per loop pass,
// right before displayUpdate, so the drawn step grid and the next playhead
// pass already see the re-derived chords).  The edit focus is switched by
// direct assignment and restored around the pass - seqSetEditChannel would
// reset the partial-encoder residues that belong to the real focus - and a
// re-bake only ever writes its own track, which its own write never marks,
// so each pass runs a given track at most once
void harmRebakePoll() {
  if (!sHarmDirtyAny) return;
  sHarmDirtyAny = false;
  uint8_t saved = seqEditCh;
  for (uint8_t y = 0; y < SEQ_CH_COUNT; y++) {
    if (!sHarmDirty[y]) continue;
    sHarmDirty[y] = false;
    seqEditCh = y;
    k4HarmRebake();
  }
  seqEditCh = saved;
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
  harmFeederChanged(x);        // HARM tracks feeding from x re-derive (deferred)
  for (uint8_t y = 0; y < SEQ_CH_COUNT; y++) {
    if (y == x || seqInCh[y] != x + 1) continue;
    uint8_t ltr = seqTrackOf(y);
    if (ltr != SEQ_TRACK_MONO && ltr != SEQ_TRACK_POLY) continue;  // DRUM never follows
    // ...but a POLY track with TYPE = HARM is never re-derived from its
    // input: that channel is only the GENERATION source (k4Harm reads it to
    // bake chords), so its baked steps must not be clobbered by a later
    // write to the feeder (otherwise a bass re-record would stamp its raw
    // single notes over the inferred triads).  It re-bakes instead - see
    // harmFeederChanged above, which marked it when x changed
    if (ltr == SEQ_TRACK_POLY && seqPolyTypeOf(y) == SEQ_POLY_HARM) continue;
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
    if (changed) {
      seqOctClamp(y);           // content changed -> re-clamp
      harmFeederChanged(y);     // cascade: A -> B (mirror) -> HARM feeding B
    }
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
  return (idx < 12) ? w[idx] : -1;
}

// --- strum voicing (Le Strum port) ----------------------------------------
// The keyboard track's STRING BUFFER: what the playhead resolved at the
// last played POLY step (post-FX/FScale = what WOULD have sounded), kept
// per channel.  HAND is silent now - the keyboard STRUMS this buffer
// (source priority in handStrings(): buffer first = the progression,
// then the pending chord = manual build / input, then the engine snapshot)
static uint8_t handBuf[SEQ_CH_COUNT][CHORD_MAX];
static uint8_t handBufN[SEQ_CH_COUNT];

// strum options (black keys while handLive(), RAM only): Le Strum's set,
// booted as SUS+SUSCOMMON (ring the strum till the chord changes, keep
// its common tones through the change)
static uint8_t strumOpts = STRUM_OPT_SUS | STRUM_OPT_SUSCOM;
static uint8_t strumExt = 0;   // F-row ADDS colors: bit0 sus4, bit1 add6, bit2 add9

uint8_t strumOptsGet() { return strumOpts; }
uint8_t strumExtGet() { return strumExt; }
void strumExtSet(uint8_t v) { strumExt = (uint8_t)(v & 7); }
static bool strumSus() { return (strumOpts & STRUM_OPT_SUS) != 0; }

// guitar shapes: [shape][string] MIDI bases (0xFF = string not used),
// then the per-extension fret shifts - arithmetic ported 1:1 from the
// guitarXShape() functions in Voici-Le-Strum-master 2/src/StrumController.c
// (shapes: C C7 A Am A7 D Dm D7 E Em E7 G; no-bass-string option not ported)
static const uint8_t gtrShapeN[12][6] = {
  { 0xFF, 48, 52, 55, 60, 64 },   // C
  { 0xFF, 48, 52, 58, 60, 64 },   // C7
  { 40, 45, 52, 57, 61, 64 },     // A
  { 40, 45, 52, 57, 60, 64 },     // Am
  { 40, 45, 52, 55, 61, 64 },     // A7
  { 0xFF, 45, 50, 57, 62, 66 },   // D
  { 0xFF, 45, 50, 57, 62, 65 },   // Dm
  { 0xFF, 45, 50, 57, 60, 66 },   // D7
  { 40, 47, 52, 56, 59, 64 },     // E
  { 40, 47, 52, 55, 59, 64 },     // Em
  { 40, 47, 50, 56, 59, 64 },     // E7
  { 43, 47, 50, 55, 59, 67 }      // G
};
static const int8_t gtrSus4[12][6] = {
  { 0, 0, 1, 0, 0, 1 },   { 0, 0, 1, 0, 0, 1 },
  { 0, 0, 0, 0, 1, 0 },   { 0, 0, 0, 0, 2, 0 },
  { 0, 0, 0, 0, 1, 0 },   { 0, 0, 0, 0, 0, 1 },
  { 0, 0, 0, 0, 0, 1 },   { 0, 0, 0, 0, 0, 1 },
  { 0, 0, 0, 1, 0, 0 },   { 0, 0, 0, 1, 0, 0 },
  { 0, 0, 0, 1, 0, 0 },   { 0, 1, 0, 0, 1, 0 }
};
static const int8_t gtrAdd6[12][6] = {
  { 0, 0, 0, 2, 0, 0 },   { 0, 0, 0, -1, 0, 0 },
  { 0, 0, 2, 0, 0, 0 },   { 0, 0, 2, 0, 0, 0 },
  { 0, 0, 2, 0, 0, 0 },   { 0, 0, 0, 2, 0, 0 },
  { 0, 0, 0, 2, 0, 0 },   { 0, 0, 0, 2, 0, 0 },
  { 0, 0, 0, 0, 2, 0 },   { 0, 0, 0, 0, 2, 0 },
  { 0, 0, 0, 0, 2, 0 },   { 0, 0, 2, 0, 0, 0 }
};
static const int8_t gtrAdd9[12][6] = {
  { 0, 0, 0, 0, 2, 0 },   { 0, 0, 0, 0, 2, 0 },
  { 0, 0, 0, 2, 0, 0 },   { 0, 0, 0, 2, 0, 0 },
  { 0, 0, 0, 4, 0, 0 },   { 0, 0, 0, 0, 0, -2 },
  { 0, 0, 0, 0, 0, -1 },  { 0, 0, 0, 0, 0, -2 },
  { 0, 0, 2, 0, 0, 0 },   { 0, 0, 2, 0, 0, 0 },
  { 0, 0, 4, 0, 0, 0 },   { 0, 0, 0, 2, 0, 0 }
};
// root + quality -> shape index / fret offset (port of guitarChord());
// rows 0..2 = MAJ / MIN / DOM7, columns = root pc 0 = C .. 11 = B
static const uint8_t gtrMapShape[3][12] = {
  { 0, 2, 5, 2, 8, 8, 8, 11, 8, 2, 2, 2 },
  { 3, 3, 6, 3, 9, 9, 9, 9, 9, 3, 3, 3 },
  { 1, 4, 7, 4, 10, 10, 10, 10, 10, 4, 4, 10 }
};
static const uint8_t gtrMapOfs[3][12] = {
  { 0, 4, 0, 6, 0, 1, 2, 0, 4, 0, 1, 2 },
  { 3, 4, 0, 6, 0, 1, 2, 3, 4, 0, 1, 2 },
  { 0, 4, 0, 6, 0, 1, 2, 3, 4, 0, 1, 2 }
};

// string map from an explicit chord: 7 notes, one per white key (0xFF =
// string has no note - a gap stays dark).  The chord's intervals
// ride from its FIRST note (root convention); GUITAR picks a shape for
// MAJ/MIN/DOM7, everything else spreads the chord tones as an inversion
// (root position on E F G, lower inversion notes on C D, upper on A B)
static uint8_t handStringsFrom(const uint8_t *src, uint8_t n, uint8_t *out) {
  uint8_t root = (uint8_t)(src[0] % 12);
  int8_t ivs[12];
  uint8_t ivn = 0;
  for (uint8_t i = 0; i < n && ivn < 12; i++) {
    int8_t d = (int8_t)(((src[i] % 12) + 12 - root) % 12);
    bool dup = false;
    for (uint8_t j = 0; j < ivn; j++) if (ivs[j] == d) dup = true;
    if (!dup) ivs[ivn++] = d;
  }
  if (!ivn) ivs[ivn++] = 0;
  // ADDS colors (temporary F-row, gated by the black ADDS master):
  // sus4 replaces the 3rd with the 4th, add6 appends the 6th, add9 the 9th
  uint8_t ext = (strumOpts & STRUM_OPT_ADDS) ? strumExt : 0;
  if ((ext & 1) && ivn < 12) {
    bool had3 = false;
    for (uint8_t i = 0; i < ivn; i++)
      if (ivs[i] == 3 || ivs[i] == 4) { ivs[i] = 5; had3 = true; }
    if (!had3) ivs[ivn++] = 5;
  }
  if ((ext & 2) && ivn < 12) {
    bool has = false;
    for (uint8_t i = 0; i < ivn; i++) if (ivs[i] == 9) has = true;
    if (!has) ivs[ivn++] = 9;
  }
  if ((ext & 4) && ivn < 12) {
    bool has = false;
    for (uint8_t i = 0; i < ivn; i++) if (ivs[i] == 2) has = true;
    if (!has) ivs[ivn++] = 2;
  }
  // sort + dedup the interval set
  for (uint8_t i = 1; i < ivn; i++) {
    int8_t v = ivs[i];
    uint8_t j = i;
    while (j && ivs[j - 1] > v) { ivs[j] = ivs[j - 1]; j--; }
    ivs[j] = v;
  }
  uint8_t m = 0;
  for (uint8_t i = 0; i < ivn; i++)
    if (!m || ivs[m - 1] != ivs[i]) ivs[m++] = ivs[i];
  ivn = m;

  int base = 12 * (int)octave + 24 + (int)root;  // root at the keyboard octave
  if (base < 0) base = 0;
  if (base > 120) base = 120;
  uint8_t str[7] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  bool guitar = false;
  if (strumOpts & STRUM_OPT_GUITAR) {
    // quality: b7 without a b3 = DOM7, b3 = MIN, else MAJ; maj7 / dim /
    // aug / b3+b7 clash / b3&M3 clash have no shape in the reference -> stacked
    uint8_t q = 0;
    bool m3 = false, M3 = false, b7 = false, none = false;
    for (uint8_t i = 0; i < ivn; i++) {
      if (ivs[i] == 3) m3 = true;
      if (ivs[i] == 4) M3 = true;
      if (ivs[i] == 10) b7 = true;
      if (ivs[i] == 6 || ivs[i] == 8 || ivs[i] == 11) none = true;
    }
    if (none || (m3 && M3) || (b7 && m3)) q = 3;
    else if (b7) q = 2;
    else if (m3) q = 1;
    if (q < 3) {
      guitar = true;
      uint8_t sh = gtrMapShape[q][root];
      uint8_t ofs = gtrMapOfs[q][root];
      int8_t raw[6];
      for (uint8_t i = 0; i < 6; i++) {
        if (gtrShapeN[sh][i] == 0xFF) { raw[i] = -1; continue; }
        int v = (int)gtrShapeN[sh][i] + (int)ofs;
        if (ext & 1) v += gtrSus4[sh][i];
        if (ext & 2) v += gtrAdd6[sh][i];
        if (ext & 4) v += gtrAdd9[sh][i];
        raw[i] = (int8_t)v;
      }
      // shift the whole shape so its root lands at the keyboard octave
      int delta = 0;
      for (uint8_t i = 0; i < 6; i++)
        if (raw[i] >= 0 && (uint8_t)(raw[i] % 12) == root) { delta = base - raw[i]; break; }
      for (uint8_t i = 0; i < 6; i++) {
        if (raw[i] < 0) { str[i] = 0xFF; continue; }
        int v = (int)raw[i] + delta;
        str[i] = (v < 0) ? 0 : (v > 127) ? 127 : (uint8_t)v;
      }
      if (strumOpts & STRUM_OPT_GUITAR2) {
        // the real shape notes an octave up fill D..B; the lowest real
        // note of the shape keeps C (bass) - compact fill, so a bassless
        // shape (C/A/D) leaves no hole in the middle of the run
        uint8_t up[6];
        for (uint8_t i = 0; i < 6; i++)
          up[i] = (str[i] == 0xFF) ? 0xFF
                  : (uint8_t)((str[i] + 12 > 127) ? 127 : str[i] + 12);
        uint8_t low = 0xFF;
        for (uint8_t i = 0; i < 6; i++) if (str[i] != 0xFF) { low = str[i]; break; }
        str[0] = low;
        uint8_t k = 1;
        for (uint8_t i = 0; i < 6 && k < 7; i++)
          if (up[i] != 0xFF) str[k++] = up[i];
        while (k < 7) str[k++] = 0xFF;
      }
    }
  }
  if (!guitar) {
    // inversion spread: the sorted chord tones land root-position on
    // E F G, C D take the lower-inversion bass notes below, A B the
    // upper ones above; -999 = string has no note (stays dark)
    int P[12];
    uint8_t tn = 0;
    for (uint8_t i = 0; i < ivn && tn < 12; i++) {
      int v = base + (int)ivs[i];
      P[tn++] = (v < 0) ? 0 : (v > 127) ? 127 : v;
    }
    int s[7];
    for (uint8_t i = 0; i < 7; i++) s[i] = -999;
    if (tn >= 1) s[2] = P[0];                    // E = root position
    if (tn >= 2) { s[3] = P[1]; s[0] = P[1] - 12; s[5] = P[1] + 12; }
    if (tn >= 3) { s[4] = P[2]; s[6] = P[2] + 12; s[1] = P[2] - 12; }
    if (tn >= 4) s[5] = P[3];                    // A = 4th tone when present
    for (uint8_t i = 0; i < 7; i++)
      str[i] = (s[i] == -999) ? 0xFF
             : (s[i] < 0) ? 0
             : (s[i] > 127) ? 127 : (uint8_t)s[i];
    if (strumOpts & STRUM_OPT_GUITAR2)
      for (uint8_t i = 3; i < 7; i++)
        if (str[i] != 0xFF)
          str[i] = (uint8_t)((str[i] + 12 > 127) ? 127 : str[i] + 12);
  }
  for (uint8_t i = 0; i < 7; i++) out[i] = str[i];
  return 7;
}

// live string map: the HAND buffer first (the progression the playhead
// resolved), then the pending chord (manual build / input), then the
// chord engine snapshot.  Returns 0 = no chord at all (strings silent)
static uint8_t handStrings(uint8_t *out) {
  const uint8_t *src = 0;
  uint8_t n = 0;
  uint8_t c = (uint8_t)(keyCh - 1);
  if (c < SEQ_CH_COUNT && handBufN[c]) {
    src = handBuf[c];
    n = handBufN[c];
  } else if (c < SEQ_CH_COUNT && pendingLen[c]) {
    src = pending[c];
    n = pendingLen[c];
  } else if (keyChordMode() && chordCount()) {
    src = chordNotes();
    n = chordCount();
  }
  if (!n) return 0;
  uint8_t total = handStringsFrom(src, n, out);
  // Strum density controlled by color amount (EN8 when PREV held in KEYMODE4)
  // 0% = 1 note (root only), 100% = all strings
  if (modeBase == MODE_KEY && modeVariant == 4) {
    uint8_t colorP = k4EmbColorAmt();
    if (colorP < 100) {
      uint8_t keep = 1 + (total * colorP) / 100;
      if (keep < total) {
        for (uint8_t i = keep; i < total; i++) out[i] = 0xFF;
        total = keep;
      }
    }
  }
  return total;
}

uint8_t handStrNote(uint8_t s) {
  uint8_t f[7];
  if (s >= 7 || !handStrings(f)) return 0;
  return f[s];
}

static void handRingAdd(uint8_t note);   // defined with releaseCh below
static bool handRingHas(uint8_t note);   // is the note alive in the ring?

// sound one HAND string (the caller has already checked handStrings())
static void handPluck(uint8_t idx, uint8_t note) {
  keyNote[idx] = note;
  keyActive[idx] = true;
  keySounding[idx] = true;
  usbMIDI.sendNoteOn(note, KEY_VEL, keyCh);
  handRingAdd(note);            // the track's ring must see strummed notes
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
  // HAND: the 5 black keys are the silent strum-option toggles (Le Strum
  // GUITAR / GTR2 / ADDS / SUS / SUSCOMMON) - tap flips the bit, the
  // header says on/off, nothing is written to the pattern and no chord
  // build happens on the black row while the strum owns the keyboard
  if (hand && (idx == 1 || idx == 3 || idx == 6 || idx == 8 || idx == 10)) {
    static const char *const nm[5] = { "GUITAR", "GTR2", "ADDS", "SUS", "SUSCOMMON" };
    static const uint8_t bit[5] = { STRUM_OPT_GUITAR, STRUM_OPT_GUITAR2,
                                    STRUM_OPT_ADDS, STRUM_OPT_SUS, STRUM_OPT_SUSCOM };
    uint8_t k = (idx == 1) ? 0 : (idx == 3) ? 1 : (idx == 6) ? 2 : (idx == 8) ? 3 : 4;
    strumOpts = (uint8_t)(strumOpts ^ bit[k]);
    bool on = (strumOpts & bit[k]) != 0;
    char hb[24];
    snprintf(hb, sizeof(hb), "%s %s", nm[k], on ? "on" : "off");
    headerShow(hb);
    if (Serial.availableForWrite() >= 48) {
      Serial.print("  strum ");
      Serial.println(hb);
    }
    return -1;
  }
  int8_t sw = hand ? handStringOf(idx) : -1;
  bool plucked = false;
  uint8_t pnote = 0;
  if (sw >= 0) {
    uint8_t fill[7] = { 0, 0, 0, 0, 0, 0, 0 };
    uint8_t fn = handStrings(fill);
    if (fn && fill[sw] != 0xFF) {
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
  // a mode change while the key was held - never a stuck note.  HAND: a
  // strummed string the track's ring still owns must NOT die on release
  // (SUS rings it to the chord change / boundary / mute / stop)
  if (keySounding[idx]) {
    if (!(handLive() && handRingHas(keyNote[idx])))
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
    } else if (seqPolyType() == SEQ_POLY_HARM) {
      // HARM, no held gesture: with FOLLOW on, record the chord it infers for
      // this step from the input channel (the same Viterbi the M3 bake uses) -
      // a chord that fits the source, never the feeder's raw single note.
      // FOLLOW off = the input channel is ignored, so it keeps the pending
      // chord (default above).  A HARM track never mirrors its input raw
      if (seqFollow()) {
        static uint8_t hnotes[CHORD_MAX];
        int hn2 = k4HarmStepChord(step, hnotes);
        if (hn2 > 0) { src = hnotes; n = (uint8_t)hn2; }
      }
      // else: FOLLOW off / source empty up to here - keep pending (above)
    } else {
      // a POLY follower mirrors its input timeline (raw source content)
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
  stepSyncFrom(seqEditCh);    // the tap IS a content change: mirrors and the
                              // HARM re-bake follow the disabled step too
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

// true when the step already stores exactly what stepChordWrite would store
// for these notes - the automatic HARM re-bake uses it to leave untouched
// (attribute-carrying) steps alone when the inference did not move
bool stepChordSame(uint8_t step, const uint8_t *notes, uint8_t n) {
  if (step >= STEP_MAX || n == 0 || n > CHORD_MAX || notes == 0) return false;
  uint8_t ch = seqEditCh;
  if (stepChordLen[ch][step] != n) return false;
  int pre = -12 * (int)seqOct[ch];
  for (uint8_t i = 0; i < n; i++) {
    int v = (int)notes[i] + pre;
    if (v < 0) v = 0;
    else if (v > 127) v = 127;
    if (stepChord[ch][step][i] != (uint8_t)v) return false;
  }
  return true;
}

const uint8_t* stepChordAt(uint8_t step, uint8_t *len) {
  if (step >= STEP_MAX || len == 0) { if (len) *len = 0; return stepChord[0][0]; }
  *len = stepChordLen[seqEditCh][step];
  return stepChord[seqEditCh][step];
}

// HARM source: the input channel the edit channel follows (1-based, self by
// default) and a channel-targeted step reader, so the harmonizer in Key4.cpp
// can read another track's stored bassline/arp
uint8_t seqFeederChannel() {
  if (seqEditCh >= SEQ_CH_COUNT) return 0;
  return seqInCh[seqEditCh];                    // 1-based (self unless remapped)
}

const uint8_t* stepChordAtCh(uint8_t ch, uint8_t step, uint8_t *len) {
  if (ch < 1 || ch > SEQ_CH_COUNT || step >= STEP_MAX || len == 0) {
    if (len) *len = 0;
    return stepChord[0][0];
  }
  *len = stepChordLen[ch - 1][step];
  return stepChord[ch - 1][step];
}

// --- GNR8 generator write path (Gen.cpp drives it) -------------------------
// one note per step, stamped exactly like stepChordWrite (pre-compensated
// for the sequence's octave shift, so the step SOUNDS the note the
// generator picked; shift range + followers re-derived) plus the
// accent/glide mask it rolled for that step
int stepGenWrite(uint8_t step, uint8_t note, uint8_t att) {
  if (step >= STEP_MAX) return -1;
  uint8_t ch = seqEditCh;
  int pre = -12 * (int)seqOct[ch];
  int v = (int)note + pre;
  if (v < 0) v = 0;
  else if (v > 127) v = 127;
  stepChordLen[ch][step] = 0;
  stepAttClear(ch, step);
  stepAtt[ch][step] = (uint8_t)(att & 0x3F);   // bits 6-7 = POLY TYPE only
  stepChord[ch][step][0] = (uint8_t)v;
  stepChordLen[ch][step] = 1;
  seqOctClamp(ch);
  stepSyncFrom(ch);
  return 1;
}

int stepGenClear(uint8_t step) {
  if (step >= STEP_MAX) return -1;
  stepChordLen[seqEditCh][step] = 0;
  stepAttClear(seqEditCh, step);   // a rest carries no attribute over
  stepSyncFrom(seqEditCh);
  return 0;
}

// rotate one fixed-size row of the step table by a single slot (dir > 0 =
// content moves to the NEXT step and the last wraps to 0 = the pattern
// delays; the widest element is the per-note strum-onset row)
static void stepRotRow(void *base, uint16_t esz, uint8_t n, int8_t dir) {
  uint8_t *b = (uint8_t*)base;
  uint8_t tmp[CHORD_MAX * 2];
  if (dir > 0) {
    memcpy(tmp, b + (n - 1) * esz, esz);
    for (int i = (int)n - 1; i > 0; i--)
      memcpy(b + (uint16_t)i * esz, b + (uint16_t)(i - 1) * esz, esz);
    memcpy(b, tmp, esz);
  } else {
    memcpy(tmp, b, esz);
    for (uint8_t i = 0; i + 1 < n; i++)
      memcpy(b + (uint16_t)i * esz, b + (uint16_t)(i + 1) * esz, esz);
    memcpy(b + (uint16_t)(n - 1) * esz, tmp, esz);
  }
}

void stepShiftPattern(uint8_t n, int8_t dir) {
  if (n == 0 || n > STEP_MAX || !dir) return;
  uint8_t ch = seqEditCh;
  int reps = dir > 0 ? dir : -dir;
  if (reps > n) reps = n;                 // a full lap lands back anyway
  int8_t d = dir > 0 ? 1 : -1;
  while (reps--) {
    stepRotRow(stepChord[ch], CHORD_MAX, n, d);
    stepRotRow(stepChordLen[ch], 1, n, d);
    stepRotRow(stepAtt[ch], 1, n, d);
    stepRotRow(stepFx[ch], 1, n, d);
    stepRotRow(stepStrumDt[ch], CHORD_MAX * 2, n, d);
    stepRotRow(drumAccent[ch], 1, n, d);
    stepRotRow(drumProbOn[ch], 1, n, d);
    stepRotRow(stepOctM[ch], 1, n, d);
    stepRotRow(stepBrwM[ch], 1, n, d);
    stepRotRow(stepModP[ch], 1, n, d);
    stepRotRow(stepTrM[ch], 1, n, d);
    stepRotRow(stepSkankM[ch], 7, n, d);   // SKANK per-step locks travel too
  }
  seqOctClamp(ch);            // the note set moved: the shift range re-derives
  stepSyncFrom(ch);
}

// stamp one attribute across from..to-1 (end exclusive, wrapping inside
// the pattern length) - the GNR8 chain gesture: TIE on X..Y-1 rings X's
// note through step Y with the engine's normal tie semantics
uint8_t stepAttrChain(uint8_t from, uint8_t to, uint8_t att) {
  if (!att) return 0;
  uint8_t ch = seqEditCh;
  uint8_t n = seqPatLenOf(ch);
  if (n == 0 || n > STEP_MAX || from >= n || to >= n || from == to)
    return 0;
  uint8_t cnt = 0;
  uint8_t i = from;
  while (i != to) {
    stepAtt[ch][i] |= (uint8_t)(att & 0x3F);
    cnt++;
    if (++i >= n) i = 0;
    if (cnt >= n) break;
  }
  return cnt;
}

// --- GNR8 per-step pitch modifiers (F2/F3/F4 pages) -----------------------
// edit-sequence accessors for the UI; the playhead reads the arrays
// directly.  Setters clamp: octave -2..+2, borrowed chord tone 0..8
// (0 = off), modifier probability 0..100
int8_t stepOctAt(uint8_t step) {
  return step < STEP_MAX ? stepOctM[seqEditCh][step] : 0;
}
void stepOctSet(uint8_t step, int8_t v) {
  if (step >= STEP_MAX) return;
  if (v < -2) v = -2;
  if (v > 2) v = 2;
  stepOctM[seqEditCh][step] = v;
}
uint8_t stepBrwAt(uint8_t step) {
  return step < STEP_MAX ? stepBrwM[seqEditCh][step] : 0;
}
void stepBrwSet(uint8_t step, uint8_t v) {
  if (step >= STEP_MAX || v > 8) return;
  stepBrwM[seqEditCh][step] = v;
}
uint8_t stepModPAt(uint8_t step) {
  return step < STEP_MAX ? stepModP[seqEditCh][step] : 100;
}
void stepModPSet(uint8_t step, uint8_t v) {
  if (step >= STEP_MAX) return;
  if (v > 100) v = 100;
  stepModP[seqEditCh][step] = v;
}
int8_t stepTrAt(uint8_t step) {
  return step < STEP_MAX ? stepTrM[seqEditCh][step] : 0;
}
void stepTrSet(uint8_t step, int8_t v) {
  if (step >= STEP_MAX) return;
  if (v < -12) v = -12;
  if (v > 12) v = 12;
  stepTrM[seqEditCh][step] = v;
}

// per-step SKANK locks (the hold-step lock row): value 0..100, -1 = inherit.
// The playback reads the array of ITS channel directly; these accessors
// walk the edit sequence like every other step field
int16_t stepSkankLockAt(uint8_t step, uint8_t idx) {
  if (step >= STEP_MAX || idx >= 7) return -1;
  uint8_t raw = stepSkankM[seqEditCh][step][idx];
  return raw ? (int16_t)(raw - 1) : -1;
}
void stepSkankLockSet(uint8_t step, uint8_t idx, int16_t v) {
  if (step >= STEP_MAX || idx >= 7) return;
  if (v < -1) v = -1;
  if (v > 100) v = 100;
  stepSkankM[seqEditCh][step][idx] = (uint8_t)(v < 0 ? 0 : v + 1);
}

// --- KEYMODE4 step-hold row: in-place chord transforms ---------------------
// ENC2/3/4 of the held-step overlay rewrite the STORED notes (the edit
// sequence), index-stable so a timed HAND step keeps its onset per slot.
// Each returns true when a note actually moved, then re-clamps the shift
// range and re-syncs follower channels like every other write path.
// OCT uses the existing stepOctSet/stepOctM pair instead (playback applies
// it - the stored chord stays a clean root-shape for the generator).

// shift every tone by one (or |d|) scale degrees in the current key:
// the intervals keep their shape (a major triad stays major), force-to-
// scale reshapes it into the scale at playback like everywhere else
bool stepDegAdjust(uint8_t step, int8_t d) {
  if (step >= STEP_MAX || !d) return false;
  uint8_t ch = seqEditCh;
  uint8_t n = stepChordLen[ch][step];
  if (n == 0) return false;
  uint8_t root = 0, q = CHORD_Q_MAJ;
  decodeChord(stepChord[ch][step], n, &root, &q);
  // the key's pitch classes, ascending from the current root
  uint8_t sc[12];
  uint8_t L = 0;
  uint8_t kr = (uint8_t)(keyRoot() % 12);
  for (uint8_t i = 0; i < 12; i++) {
    uint8_t pc = (uint8_t)((kr + i) % 12);
    if (keyIsInScale(pc)) sc[L++] = pc;
  }
  if (!L) return false;
  // the chord root's degree (a borrowed root snaps to its nearest degree)
  int dR = 0;
  int best = 7;
  for (uint8_t i = 0; i < L; i++) {
    int diff = (int)sc[i] - (int)root;
    if (diff < 0) diff = -diff;
    if (diff > 6) diff = 12 - diff;
    if (diff < best) { best = diff; dR = (int)i; }
  }
  int steps = (d > 0) ? d : -d;
  if (steps > 12) steps = 12;
  long delta = 0;
  for (int k = 0; k < steps; k++) {
    int nd = dR + ((d > 0) ? 1 : -1);
    if (nd >= (int)L) nd = 0;
    if (nd < 0) nd = (int)L - 1;
    int dd = (int)sc[nd] - (int)sc[dR];
    if (d > 0 && dd <= 0) dd += 12;   // wrapping up goes over the top
    if (d < 0 && dd >= 0) dd -= 12;
    delta += dd;
    dR = nd;
  }
  bool moved = false;
  for (uint8_t i = 0; i < n; i++) {
    int v = (int)stepChord[ch][step][i] + (int)delta;
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    if (v != (int)stepChord[ch][step][i]) {
      stepChord[ch][step][i] = (uint8_t)v;
      moved = true;
    }
  }
  if (moved) {
    seqOctClamp(ch);
    stepSyncFrom(ch);
  }
  return moved;
}

// cycle the inversion: d > 0 raises the bass an octave, d < 0 drops the
// top down - refused (stays put) when that would duplicate a tone or
// leave 0..127, so the transform can always be walked back
bool stepInvAdjust(uint8_t step, int8_t d) {
  if (step >= STEP_MAX || !d) return false;
  uint8_t ch = seqEditCh;
  uint8_t n = stepChordLen[ch][step];
  if (n < 2) return false;
  uint8_t *nt = stepChord[ch][step];
  int sel = 0, want;
  if (d > 0) {                       // bass = the lowest tone
    for (uint8_t i = 1; i < n; i++) if (nt[i] < nt[sel]) sel = i;
    want = (int)nt[sel] + 12;
  } else {                           // top = the highest tone
    for (uint8_t i = 1; i < n; i++) if (nt[i] > nt[sel]) sel = i;
    want = (int)nt[sel] - 12;
  }
  if (want < 0 || want > 127) return false;
  for (uint8_t i = 0; i < n; i++)
    if ((int)nt[i] == want) return false;   // octave pair: no room to move
  nt[sel] = (uint8_t)want;
  seqOctClamp(ch);
  stepSyncFrom(ch);
  return true;
}

// spread or close the upper voices around the bass: d > 0 lifts every
// tone above the lowest one an octave, d < 0 brings them back down - and
// refuses to close past the bass (an already-closed chord just stays),
// which makes the pair walkable both ways without extra state
bool stepVoiAdjust(uint8_t step, int8_t d) {
  if (step >= STEP_MAX || !d) return false;
  uint8_t ch = seqEditCh;
  uint8_t n = stepChordLen[ch][step];
  if (n < 2) return false;
  uint8_t *nt = stepChord[ch][step];
  uint8_t low = nt[0];
  for (uint8_t i = 1; i < n; i++) if (nt[i] < low) low = nt[i];
  for (uint8_t i = 0; i < n; i++) {
    if (nt[i] == low) continue;              // the bass anchors the spread
    int v = (int)nt[i] + ((d > 0) ? 12 : -12);
    if (v < 0 || v > 127) return false;
    if (d < 0 && v <= (int)low) return false; // would fold under/onto bass
    for (uint8_t j = 0; j < n; j++)
      if (j != i && (int)nt[j] == v) return false;  // collapse: leave it
  }
  for (uint8_t i = 0; i < n; i++) {
    if (nt[i] == low) continue;
    nt[i] = (uint8_t)((int)nt[i] + ((d > 0) ? 12 : -12));
  }
  seqOctClamp(ch);
  stepSyncFrom(ch);
  return true;
}

// one-level undo of the last KEYMODE4 write batch (apply / key write /
// variation): a full snapshot of the edit sequence - chords, lengths, the
// step attributes (incl. the TYPE override), the per-note HAND strum
// onsets and the drum accent/probability masks (~1.8 KB static)
static uint8_t snapChord[STEP_MAX][CHORD_MAX];
static uint8_t snapLen[STEP_MAX];
static uint8_t snapAttB[STEP_MAX];
static uint8_t snapFxB[STEP_MAX];
static uint8_t snapEmbB[STEP_MAX][4];  // embellishment overrides
static uint8_t snapDrumA[STEP_MAX];
static uint8_t snapDrumP[STEP_MAX];
static uint16_t snapStrumB[STEP_MAX][CHORD_MAX];
static int8_t  snapOctB[STEP_MAX];      // GNR8 per-step mods
static uint8_t snapBrwB[STEP_MAX];
static uint8_t snapModP[STEP_MAX];
static int8_t  snapTrB[STEP_MAX];       // per-step TRANS
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
    for (uint8_t i = 0; i < 4; i++) snapEmbB[st][i] = stepEmb[ch][st][i];
    snapDrumA[st] = drumAccent[ch][st];
    snapDrumP[st] = drumProbOn[ch][st];
    snapOctB[st] = stepOctM[ch][st];
    snapBrwB[st] = stepBrwM[ch][st];
    snapModP[st] = stepModP[ch][st];
    snapTrB[st] = stepTrM[ch][st];
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
    for (uint8_t i = 0; i < 4; i++) stepEmb[ch][st][i] = snapEmbB[st][i];
    drumAccent[ch][st] = snapDrumA[st];
    drumProbOn[ch][st] = snapDrumP[st];
    stepOctM[ch][st] = snapOctB[st];
    stepBrwM[ch][st] = snapBrwB[st];
    stepModP[ch][st] = snapModP[st];
    stepTrM[ch][st] = snapTrB[st];
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

// per-step embellishment overrides for KEYMODE4 (PREV hold encoders EN5-8)
// param 0 = sub prob 0..100 (255 = global), 1 = inv bias 0..3 (255 = global)
// param 2 = pass amt 0..100 (255 = global), 3 = color amt 0..100 (255 = global)
uint8_t stepEmbGet(uint8_t step, uint8_t param) {
  if (step >= STEP_MAX || param >= 4) return 255;
  return stepEmb[seqEditCh][step][param];
}

void stepEmbSet(uint8_t step, uint8_t param, uint8_t val) {
  if (step >= STEP_MAX || param >= 4) return;
  stepEmb[seqEditCh][step][param] = val;
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

// cycle through embellishment params (SUB/INV/PAS/COL) for a step.
// Each tap advances the active param index (0..3) and increments its value.
// Returns true if any param is non-global (for feedback).
bool stepEmbToggle(uint8_t step) {
  if (step >= STEP_MAX) return false;
  uint8_t *e = &stepEmb[seqEditCh][step][0];
  // Find next param that's not 255, or cycle through all 4
  static uint8_t lastParam = 0;
  uint8_t p = lastParam;
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t idx = (p + i) % 4;
    if (e[idx] != 255 || i == 3) {  // found non-global or last param
      p = idx;
      break;
    }
  }
  // Increment the selected param
  if (e[p] == 255) e[p] = 50;  // default to middle
  else {
    e[p] = (e[p] + 25) % 125;  // step by 25: 0, 25, 50, 75, 100
    if (e[p] > 100) e[p] = 255;  // 125 wraps to 255 (global)
  }
  lastParam = (p + 1) % 4;
  // Return true if any param is overridden
  return (e[0] != 255 || e[1] != 255 || e[2] != 255 || e[3] != 255);
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

// HAND's currently RINGING notes per track (bitmask): what a pluck or a
// replayed gesture left sounding, bookkept against boundaries and chord
// changes.  STRUM = plucked strings sustaining under SUS (they skip the
// step boundary and ring until the buffered chord changes); GEST = a timed
// recorded gesture ringing until the next content step
#define HAND_RING_STRUM 0x01
#define HAND_RING_GEST  0x02
static uint8_t handRing[SEQ_CH_COUNT];

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

// mute = USB OUTPUT gate only: a muted track keeps PLAYING internally -
// follow chains advance, gates fire, seqNotes fills - so listeners (the
// HARM live follow reads the feeder's sounding notes) stay coherent; only
// the wire traffic is suppressed
static void seqOutOn(uint8_t c, uint8_t note, uint8_t vel) {
  if (seqIsMuted(c)) return;
  usbMIDI.sendNoteOn(note, vel, c + 1);
}

static void seqOutOff(uint8_t c, uint8_t note) {
  if (seqIsMuted(c)) return;
  usbMIDI.sendNoteOff(note, 0, c + 1);
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
          seqOutOn(c, note, vel);
          if (strumQ[c].offOld) {
            for (uint8_t j = 0; j < seqLen[c]; j++)
              seqOutOff(c, seqNotes[c][j]);
            seqLen[c] = 0;
            gateOffAt[c] = 0;
            strumQ[c].offOld = false;
          }
        } else {
          if (seqLen[c])
            seqOutOff(c, seqNotes[c][0]);
          seqOutOn(c, note, vel);
        }
        seqNotes[c][0] = note;
        seqLen[c] = 1;
      } else {
        if (seqLen[c] < CHORD_MAX) {
          seqNotes[c][seqLen[c]] = note;
          seqLen[c]++;
        }
        seqOutOn(c, note, vel);
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

// COMP: a POLY STEP chord re-struck on the Euclid on-positions of 8 slots
// inside the step (per-track HITS 2..8).  compQ[] carries the pending
// stabs, drained from loop context beside seqStrumPoll; each stab cuts the
// previous one (the last rings to the step boundary, where releaseCh
// cancels the tail - a rest, a length change or stop drops it, like the
// strum queue).  MONO and DRUM never enqueue; STRUM/HAND/HARM steps keep
// their own gestures
static struct {
  uint8_t note[CHORD_MAX];
  uint8_t vel[CHORD_MAX];
  uint8_t nn;               // notes per stab
  uint8_t idx;              // next stab to send
  uint8_t hi;               // stabs in the pattern
  uint32_t at[8];           // absolute onset (millis) of each stab
  bool on;
} compQ[SEQ_CH_COUNT];

static void compCancel(uint8_t c) { compQ[c].on = false; }

// stage one step's comp stabs: notes arrive playback-shifted and
// velocity-shaped; sd = this channel's step duration (ms, speed-scaled).
// HITS 0/1 or a missing sd fall back to the block chord (today's behavior)
static void compEnqueue(uint8_t c, const uint8_t *note, const uint8_t *vel,
                        uint8_t n, uint32_t sd) {
  uint8_t hits = compHits[c];
  if (n > CHORD_MAX) n = CHORD_MAX;
  if (hits < 2 || sd == 0) {            // one stab = the block chord
    for (uint8_t i = 0; i < n; i++) {
      seqNotes[c][i] = note[i];
      seqOutOn(c, note[i], vel[i]);
    }
    seqLen[c] = n;
    return;
  }
  if (hits > 8) hits = 8;
  Bjorklund<uint16_t, 10> eu;
  uint16_t bits = eu.compute(8, (int8_t)hits);
  uint32_t t0 = millis();
  uint8_t hi = 0;
  for (uint8_t s = 0; s < 8 && hi < 8; s++) {
    if ((bits >> s) & 1)
      compQ[c].at[hi++] = t0 + (uint32_t)(sd * (uint32_t)s) / 8;
  }
  for (uint8_t i = 0; i < n; i++) {
    compQ[c].note[i] = note[i];
    compQ[c].vel[i] = vel[i];
  }
  compQ[c].nn = n;
  compQ[c].idx = 0;
  compQ[c].hi = hi;
  compQ[c].on = true;
}

// drain due stabs (called from the loop, next to seqStrumPoll): cut the
// sounding stab, re-strike the chord.  The last stab stays in seqNotes so
// the step-boundary release cuts it, and the HARM live follow reads it
void compPoll() {
  uint32_t now = millis();
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    if (!compQ[c].on || compQ[c].idx >= compQ[c].hi) continue;
    if ((int32_t)(now - compQ[c].at[compQ[c].idx]) < 0) continue;
    for (uint8_t i = 0; i < seqLen[c]; i++)
      seqOutOff(c, seqNotes[c][i]);
    seqLen[c] = 0;
    for (uint8_t i = 0; i < compQ[c].nn; i++) {
      seqNotes[c][seqLen[c]] = compQ[c].note[i];
      seqOutOn(c, compQ[c].note[i], compQ[c].vel[i]);
      seqLen[c]++;
    }
    compQ[c].idx++;
    if (compQ[c].idx >= compQ[c].hi) compQ[c].on = false;
  }
}

// release one sequence's sounding notes and cancel its gate timer
static void releaseCh(uint8_t c) {
  for (uint8_t i = 0; i < seqLen[c]; i++)
    seqOutOff(c, seqNotes[c][i]);
  seqLen[c] = 0;
  gateOffAt[c] = 0;
  strumCancel(c);
  compCancel(c);            // a rest / length change drops the stab tail
  handRing[c] = 0;
}

static void seqRelease() {
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) releaseCh(c);
}

// a pluck joins the keyboard track's ring: the boundary / chord-change
// bookkeeping has to see the strummed notes (dedup - a re-plucked string
// only retriggers; a full ring restarts clean)
static void handRingAdd(uint8_t note) {
  uint8_t c = (uint8_t)(keyCh - 1);
  if (c >= SEQ_CH_COUNT) return;
  bool dup = false;
  for (uint8_t i = 0; i < seqLen[c] && !dup; i++)
    dup = (seqNotes[c][i] == note);
  if (!dup) {
    if (seqLen[c] >= CHORD_MAX) releaseCh(c);
    if (seqLen[c] < CHORD_MAX) {
      seqNotes[c][seqLen[c]] = note;
      seqLen[c]++;
    }
  }
  if (strumSus()) handRing[c] |= HAND_RING_STRUM;
}

// buffer the step's chord: this is what the keyboard strums from here on
// (post-FX/FScale = what WOULD have sounded).  A chord CHANGE decides what
// happens to the notes already ringing: SUS off cuts everything (per-step
// length), SUS on without SUSCOMMON swaps clean, SUS+SUSCOMMON keeps the
// ringing notes whose pitch class survives in the new voicing
static void handBufStore(uint8_t c, const uint8_t *src, uint8_t n) {
  if (c >= SEQ_CH_COUNT || !n) return;
  if (n > CHORD_MAX) n = CHORD_MAX;
  if (handRing[c] && seqLen[c]) {
    uint8_t np[CHORD_MAX], npn = 0;
    if (strumSus() && (strumOpts & STRUM_OPT_SUSCOM))
      for (uint8_t i = 0; i < n; i++) {
        uint8_t p = (uint8_t)(src[i] % 12);
        bool dup = false;
        for (uint8_t j = 0; j < npn && !dup; j++) dup = (np[j] == p);
        if (!dup && npn < CHORD_MAX) np[npn++] = p;
      }
    uint8_t m = 0;
    for (uint8_t i = 0; i < seqLen[c]; i++) {
      bool keep = false;
      for (uint8_t j = 0; j < npn && !keep; j++)
        keep = ((uint8_t)(seqNotes[c][i] % 12) == np[j]);
      if (keep) seqNotes[c][m++] = seqNotes[c][i];
      else seqOutOff(c, seqNotes[c][i]);
    }
    seqLen[c] = m;
    handRing[c] = (m && strumSus()) ? HAND_RING_STRUM : 0;
  }
  for (uint8_t i = 0; i < n; i++) handBuf[c][i] = src[i];
  handBufN[c] = n;
}

// is this note currently kept alive by the keyboard track's ring?  A pad
// release must not cut a strummed string the ring still owns
static bool handRingHas(uint8_t note) {
  uint8_t c = (uint8_t)(keyCh - 1);
  if (c >= SEQ_CH_COUNT || !handRing[c]) return false;
  for (uint8_t i = 0; i < seqLen[c]; i++)
    if (seqNotes[c][i] == note) return true;
  return false;
}

// cut a ringing HAND track AND drop its string buffer: POLY TYPE change
// away from HAND (the strum mode is gone - full reset)
void handRingCut(uint8_t c) {
  if (c >= SEQ_CH_COUNT) return;
  releaseCh(c);
  handBufN[c] = 0;
}

// playback note = stored note + 12 * the sequence's octave shift + its
// semitone TRANS offset, kept in 0..127 (seqOctClamp keeps the octave
// inside the -2..8 convention, TRANS clamps at the knob)
static uint8_t shNote(uint8_t n, int8_t sh, int8_t tr = 0) {
  int v = (int)n + 12 * (int)sh + (int)tr;
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

// --- GNR8 BASS + FOLLOW: map a generated step note onto the incoming chord
// The window is the generator's live range (LOW..LOW+RANGE-1) and the
// content FOLDS into it (foldWin): RANGE 2 -> 1 lowers the upper-octave
// notes into the new octave, 1 -> 2 raises the original pitches back -
// stored steps are never rewritten, so the round-trip is lossless and
// the per-step OCT shifts ride on top of the folded base.  The whole
// mapping runs in STORED space; the listener's octave (SEQMODE ENC1)
// is applied ONCE at the return - folding into a static window after
// shNote would cancel the transposition.  Every candidate is folded
// into [lo,hi] which spans at least one octave, so each pitch class
// always has a candidate inside.  Returns a sound-space note (the
// caller skips shNote for it).
// fold a sounding pitch into [lo,hi] by whole octaves: the LINE follows
// LOW..RANGE live (RANGE 2->1 lowers the upper octave into the window,
// 1->2 raises the original pitch back - pure playback transform, the
// stored note and any per-step OCT data are untouched)
static uint8_t foldWin(int v, int lo, int hi) {
  if (hi < lo) hi = lo;
  while (v > hi) v -= 12;
  while (v < lo) v += 12;
  if (v < 0) v = 0;
  if (v > 127) v = 127;
  return (uint8_t)v;
}
// nearest window representation of class t to target n
static uint8_t winNear(int t, uint8_t lo, uint8_t hi, uint8_t n) {
  int first = (int)lo + (((t - (int)lo) % 12 + 12) % 12);
  int best = first, bd = 0x7FFF;
  for (int v = first; v <= (int)hi; v += 12) {
    int d = v - (int)n;
    if (d < 0) d = -d;
    if (d < bd) { bd = d; best = v; }
  }
  return (uint8_t)best;
}
// octave-nearest-to-n representation of class t inside the window
static uint8_t winOct(int t, int n, uint8_t lo, uint8_t hi) {
  int d = n - t;
  int k = (d >= 0) ? (d + 6) / 12 : -(((-d) + 6) / 12);
  int v = t + 12 * k;
  while (v < (int)lo) v += 12;
  while (v > (int)hi) v -= 12;
  return (uint8_t)v;
}
static uint8_t mapBassNote(uint8_t c, uint8_t stp, const uint8_t *ch,
                           uint8_t len, int8_t sh, bool modOn) {
  // live window (LOW..LOW+RANGE-1); the own pitch folds into it, so the
  // mapped line sits exactly where the content plays right now
  int wlo = 24 + 12 * seqGenLowOf(c);
  int whi = wlo + 12 * seqGenRangeOf(c) - 1;
  if (whi > 127) whi = 127;
  if (whi < wlo) whi = wlo;             // degenerate guard (window >= 1 oct)
  uint8_t lo = (uint8_t)wlo;
  uint8_t hi = (uint8_t)whi;
  // own generated pitch (its min, like the plain BASS pick), folded
  // into the live window like the plain playback pick - stored space
  uint8_t ownN = stepChord[c][stp][0];
  for (uint8_t i = 1; i < stepChordLen[c][stp]; i++)
    if (stepChord[c][stp][i] < ownN) ownN = stepChord[c][stp][i];
  uint8_t n = foldWin((int)ownN, wlo, whi);
  // feeder tones as the feeder stored them, sorted ascending (the
  // degree/interval/root picks index the chord root-first)
  uint8_t t[CHORD_MAX];
  uint8_t tn = 0;
  for (uint8_t i = 0; i < len && i < CHORD_MAX; i++)
    t[tn++] = ch[i];
  if (!tn) return shNote(n, sh, seqTransOf(c));   // degenerate: own pitch
  for (uint8_t i = 1; i < tn; i++) {
    uint8_t v = t[i];
    uint8_t j = i;
    while (j > 0 && t[j - 1] > v) { t[j] = t[j - 1]; j--; }
    t[j] = v;
  }
  uint8_t root = (uint8_t)(keyRoot() % 12);
  int relI = ((int)n - (int)root) % 12;  // semitones above KEY root
  if (relI < 0) relI += 12;
  uint8_t rel = (uint8_t)relI;
  uint8_t base;
  switch (seqMap()) {
    case SEQ_MAP_ROOT:                   // always the chord's lowest tone
      base = winOct(t[0], n, lo, hi);    // octave-shaped to the own pitch
      break;
    case SEQ_MAP_DEG: {                  // own scale degree -> Nth chord
      uint8_t dIdx = 0xFF;               // tone (wraps over the chord)
      uint8_t cnt = 0;
      int bestJ = -1, bestD = 12;
      for (uint8_t j = 0; j < 12; j++) {
        if (!keyIsInScale((uint8_t)((root + j) % 12))) continue;
        int d = (j > rel) ? (int)(j - rel) : (int)(rel - j);
        if (d < bestD) { bestD = d; bestJ = (int)cnt; }
        if ((int)j == (int)rel) dIdx = cnt;
        cnt++;
      }
      if (dIdx == 0xFF) dIdx = (bestJ >= 0) ? (uint8_t)bestJ : 0;  // off-scale snap
      base = winOct(t[dIdx % tn], n, lo, hi);
      break;
    }
    case SEQ_MAP_IVL:                    // lowest tone + semitone offset
      base = winOct((int)t[0] + (int)rel, n, lo, hi);  // may leave the chord
      break;
    case SEQ_MAP_CT: {                   // chord TONE, octave follows the
      uint8_t best = winOct(t[0], n, lo, hi);         // generated contour
      int bd = (int)(best > n ? best - n : n - best); // note (melody shape)
      for (uint8_t i = 1; i < tn; i++) {              // kept across the chord
        uint8_t cand = winOct(t[i], n, lo, hi);
        int d = (int)(cand > n ? cand - n : n - cand);
        if (d < bd) { bd = d; best = cand; }
      }
      base = best;
      break;
    }
    case SEQ_MAP_GD: {                   // GUIDE tones: the chord's 3rd and
      uint8_t gt[2];                     // 7th (2nd and 4th tone) only
      uint8_t gn = 0;
      if (tn > 1) gt[gn++] = t[1];       // 3rd
      if (tn > 3) gt[gn++] = t[3];       // 7th
      if (gn == 0) {                     // single tone: behave like NEAR
        base = winNear(t[0], lo, hi, n);
        break;
      }
      uint8_t best = winNear(gt[0], lo, hi, n);
      int bd = (int)(best > n ? best - n : n - best);
      for (uint8_t i = 1; i < gn; i++) {
        uint8_t cand = winNear(gt[i], lo, hi, n);
        int d = (int)(cand > n ? cand - n : n - cand);
        if (d < bd) { bd = d; best = cand; }
      }
      base = best;
      break;
    }
    default: {                           // NEAR: nearest chord tone in the
      uint8_t best = winNear(t[0], lo, hi, n);         // window
      int bd = (int)(best > n ? best - n : n - best);
      for (uint8_t i = 1; i < tn; i++) {
        uint8_t cand = winNear(t[i], lo, hi, n);
        int d = (int)(cand > n ? cand - n : n - cand);
        if (d < bd) { bd = d; best = cand; }
      }
      base = best;
      break;
    }
  }
  // borrow: replace the mapped tone with the step's chosen chord tone
  // (octave-shaped to the base), gated by the step's modifier probability
  uint8_t brw = stepBrwM[c][stp];
  if (modOn && brw) base = winOct(t[(brw - 1) % tn], (int)base, lo, hi);
  return shNote(base, sh, seqTransOf(c)); // listener's shifts apply LAST
}

// SKANK playback: resolve this step's locks (raw 0 = inherit the page
// knob; a locked TES owns the whole hit - MOT parks centered so the walk
// stays where the lock put it) and derive the voicing from the source
// chord the caller resolved (feeder while FOLLOW runs, own stored step
// otherwise).  Locks read the PLAYBACK channel, page knobs its defaults
static void skankDerive(uint8_t c, uint8_t stp, const Chord &src,
                        Chord &out) {
  uint8_t p[8];
  bool tesLocked = false;
  for (uint8_t i = 0; i < 7; i++) {
    uint8_t raw = stepSkankM[c][stp][i];
    if (raw) {
      p[i] = (uint8_t)(raw - 1);
      if (i == 0) tesLocked = true;
    } else {
      p[i] = skankPrmM[c][i];
    }
  }
  p[7] = skankPrmM[c][7];         // NOT% stays page-level (no lock slot)
  if (tesLocked) p[1] = 50;              // the lock sets the inversion alone
  deriveSkankChord(src, out, stp, p[0], p[1], p[2], p[3], p[4], p[5], p[6],
                   p[7]);
}

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
    // strummed strings ring across the boundary while SUS is on; a timed
    // recorded gesture rings until the next content step (release below
    // cuts both when SUS goes off / mute / stop / erase)
    if ((handRing[c] & HAND_RING_GEST) ||
        ((handRing[c] & HAND_RING_STRUM) && strumSus()))
      continue;
    releaseCh(c);
  }
  // carry the current pending chords forward into the new position: the
  // chord played during steps 1-8 is the "actual chord" of step 8 even
  // though it was fed while the playhead sat on step 1
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++)
    if (go[c]) histAt(c, chStep[c]);
  // two passes over the note-on section: every track first, then the POLY
  // +HARM followers (FOLLOW on, input != self), every SKANK track (a
  // following skank reads the feeder's chord for THIS tick; a plain one
  // just derives later in its own pass) AND the MONO/POLY followers whose
  // feeder is a SKANK track (the walk lives only in the feeder's sounding
  // notes, so they read those instead of its stored steps) - so a
  // follower always reads the feeder's note for THIS tick regardless of
  // channel order
  for (uint8_t pass = 0; pass < 2; pass++)
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    bool harmFol = seqTrackOf(c) == SEQ_TRACK_POLY &&
                   seqPolyTypeOf(c) == SEQ_POLY_HARM && seqFollowOf(c) &&
                   seqInCh[c] != (uint8_t)(c + 1);
    bool skankTrack = seqTrackOf(c) == SEQ_TRACK_POLY &&
                      seqPolyTypeOf(c) == SEQ_POLY_SKANK;
    // follower of a SKANK feeder (MONO arp/bass/lead/strum or POLY
    // STEP/STRUM/HAND): the walked voicing exists only at the feeder's
    // send time, so stored-step follow would be indistinguishable from
    // the mirrored raw chords - read the feeder's SOUNDING notes instead
    // (scoped to POLY+SKANK feeders; other feeders keep the stored-step
    // convention; HARM listeners keep their own bass-line inference)
    uint8_t fxf = (uint8_t)(seqInCh[c] - 1);
    bool skankFol = !harmFol && !skankTrack &&
                    seqTrackOf(c) != SEQ_TRACK_DRUM && seqFollowOf(c) &&
                    seqInCh[c] != (uint8_t)(c + 1) &&
                    seqTrackOf(fxf) == SEQ_TRACK_POLY &&
                    seqPolyTypeOf(fxf) == SEQ_POLY_SKANK;
    if ((harmFol || skankTrack || skankFol) != (pass == 1)) continue;
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
    // ...but a POLY track with TYPE = HARM never follows at playback: its
    // input channel is only the GENERATION source (k4Harm reads it to bake
    // chords), so it always plays back its own baked steps - otherwise a
    // follow-on HARM track would replay the feeder's raw bass notes instead
    // of the triads it inferred
    bool harmTrack = seqTrackOf(c) == SEQ_TRACK_POLY &&
                     seqPolyTypeOf(c) == SEQ_POLY_HARM;
    bool follow = seqFollowOf(c) && seqTrackOf(c) != SEQ_TRACK_DRUM &&
                  seqInCh[c] != (uint8_t)(c + 1) && !harmTrack;
    uint8_t fstp = 0;                  // feeder's step while following
    if (follow) {
      uint8_t fx = (uint8_t)(seqInCh[c] - 1);
      bool own = len > 0;              // activation stays the listener's
      // HAND zone: an empty OWN step still resolves the feeder - the strum
      // follows the progression over a blank pattern too; written own steps
      // keep gating the rhythm
      bool handZone = !own && seqTrackOf(c) == SEQ_TRACK_POLY &&
                      effStepType(c, stp) == SEQ_POLY_HAND;
      len = 0;
      if ((own || handZone) && inRng[fx]) {   // feeder must be inside its range
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
    // a muted track still PLAYS here (internal communication: follow
    // chains, arp index, the HARM live follow) - seqOutOn/seqOutOff
    // suppress the wire traffic at the send
    bool plays = (len > 0);
    // probability gate (MONO/POLY): the track's own %, read live so ENC8
    // moves every armed step at once; a miss silences the step this pass
    if (plays && (att & SEQ_ATT_PROB) &&
        (uint8_t)random(100) >= (uint8_t)seqProb[c])
      plays = false;
    if (!plays) {
      // ARP SKIP: a rest (or a probability miss) still walks the arp
      // position, so the sequence keeps running over dead steps instead
      // of pausing on them
      if (seqTrackOf(c) == SEQ_TRACK_MONO &&
          seqMonoTypeOf(c) == SEQ_MONO_ARP && seqArpSkipOf(c))
        arpIdx[c]++;
      // a rest / probability miss keeps HAND's ring sounding (the next
      // chord, a mute or leaving the window cuts it)
      if (!handRing[c]) releaseCh(c);       // also ends a glide
      continue;
    }
    uint8_t *ch = follow ? stepChord[seqInCh[c] - 1][fstp]
                         : stepChord[c][stp];
    // LIVE HARM follow: with FOLLOW on the chord is re-derived from the
    // feeder's SOUNDING note this tick (post-probability) instead of the
    // stored step chord - prob rolls and arp walks re-harmonize every
    // pass.  A feeder silent for ONE pass (prob miss / rest) replays the
    // chord carried from the previous pass; a muted feeder keeps playing
    // internally (mute gates the USB output only), so its notes still
    // feed this path.  Cold start = stored baked chord.  FOLLOW off =
    // stored playback
    uint8_t live[CHORD_MAX];
    if (harmFol) {
      uint8_t fx2 = (uint8_t)(seqInCh[c] - 1);
      uint8_t ln = 0;
      if (seqLen[fx2] > 0) {             // sounding: this tick's note
        uint8_t src = seqNotes[fx2][0];
        for (uint8_t i = 1; i < seqLen[fx2]; i++)
          if (seqNotes[fx2][i] < src) src = seqNotes[fx2][i];
        ln = k4HarmLiveChord(src, live);
        if (ln) {                       // remember for the carry
          for (uint8_t i = 0; i < ln; i++) harmLive[c][i] = live[i];
          harmLiveN[c] = ln;
        }
      }
      if (ln == 0 && harmLiveN[c]) {    // quiet pass: harmony holds
        ln = harmLiveN[c];
        for (uint8_t i = 0; i < ln; i++) live[i] = harmLive[c][i];
      }
      if (ln) { ch = live; len = ln; }
      // ln == 0 (cold start): the stored baked chord plays as-is
    }
    // live SKANK source: with FOLLOW on the chord is re-read from the
    // feeder's SOUNDING notes this tick (post-probability, its ringing
    // pad bar - the same freshness the HARM live path gets) instead of
    // the stored feeder step the follow block above resolved; a quiet
    // feeder keeps that stored chord (cold start / arp rests), FOLLOW off
    // = the track's own stored step.  A follower of a SKANK feeder
    // (skankFol, MONO or POLY) reads the same way - it gets the feeder's
    // WALKED voicing - and falls back to the stored-step chord the follow
    // block resolved while the feeder is quiet.  `live` is free here:
    // harmFol, skankTrack and skankFol never overlap
    if ((skankTrack || skankFol) && follow) {
      uint8_t fx2 = (uint8_t)(seqInCh[c] - 1);
      if (seqLen[fx2] > 0) {
        uint8_t ln = seqLen[fx2] > CHORD_MAX ? CHORD_MAX : seqLen[fx2];
        for (uint8_t i = 0; i < ln; i++) live[i] = seqNotes[fx2][i];
        ch = live;
        len = ln;
      }
    }
    // SKANK derive: shape the voicing about to play from the source chord
    // (the feeder above, or the track's own stored step) - this step's
    // locks override the page knobs inside skankDerive, and everything
    // downstream (step FX, FScale, OCT/TRANS, gate, COMP) treats the
    // result as the step's chord.  The stored steps are never rewritten
    uint8_t skankOut[CHORD_MAX];
    if (skankTrack) {
      Chord f, o;
      f.count = (len > CHORD_MAX) ? CHORD_MAX : len;
      for (uint8_t i = 0; i < f.count; i++) f.notes[i] = ch[i];
      skankDerive(c, stp, f, o);
      if (o.count) {
        for (uint8_t i = 0; i < o.count; i++) skankOut[i] = o.notes[i];
        ch = skankOut;
        len = o.count;
      }
    }
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
    // GNR8 per-step OCT on POLY (the step-hold row's ENC1 / GNR8 F2 page)
    // and per-step TRANS (step-hold row, -12..+12 st): transpose the
    // PLAYBACK copy - the stored step chord never changes, the MONO track
    // keeps its probability-gated OCT roll below (TRANS is a plain offset,
    // never rolled)
    if (!mono && !drumTrack &&
        (stepOctM[c][stp] || stepTrM[c][stp])) {
      if (ch != mapped)
        for (uint8_t i = 0; i < len; i++) mapped[i] = ch[i];
      for (uint8_t i = 0; i < len; i++) {
        int v = (int)mapped[i] + 12 * (int)stepOctM[c][stp] +
                (int)stepTrM[c][stp];
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        mapped[i] = (uint8_t)v;
      }
      ch = mapped;
    }
    // HAND POLY buffers instead of sounding: the step's chord (post-FX,
    // post-FScale = what WOULD have sounded) becomes the keyboard's string
    // buffer, the rhythm still gates WHEN it refreshes.  Only an explicit
    // recorded gesture (stored u16 dts) still auto-replays note by note
    if (handStep) handBufStore(c, ch, len);
    if (handStep && !handTimed) continue;
    int8_t sh = seqOct[c];
    int8_t tr = seqTransOf(c);       // per-track semitone TRANS (0 on DRUM)
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
          order[i] = shNote(order[i], sh, tr);
          ovel[i] = vel;
        }
        uint32_t e = n ? (sd / ((uint32_t)n * 2)) : sd;
        if (e < 8) e = 8;
        if (e > 40) e = 40;
        strumEnqueue(c, order, ovel, n, (uint16_t)e, true, defer[c], arm);
      } else {
        uint8_t note;
        // GNR8 per-step modifiers: the roll gates OCT (all mono types)
        // and BRW (inside mapBassNote) - no mod set = no roll consumed
        bool hasMod = stepBrwM[c][stp] != 0 || stepOctM[c][stp] != 0;
        bool modOn = !hasMod ||
                     (uint8_t)random(100) < (uint8_t)stepModP[c][stp];
        bool mapped = false;
        uint8_t mtype = seqMonoTypeOf(c);
        if (mtype == SEQ_MONO_BASS || mtype == SEQ_MONO_LEAD) {
          if (follow) {
            // map the own generated pitch onto the feeder chord (window +
            // MAP mode; LEAD uses ct/gd for chord/guide-tone resolution) -
            // sound space already, shNote below is skipped
            note = mapBassNote(c, stp, ch, len, sh, modOn);
            mapped = true;
          } else {
            note = ch[0];
            for (uint8_t i = 1; i < len; i++) if (ch[i] < note) note = ch[i];
          }
        } else if (mtype == SEQ_MONO_ARP) {
          uint8_t elen = 0;
          note = arpNoteAt(ch, len, c, arpIdx[c], &elen);
          if (!elen) note = ch[0];       // degenerate walk: hold the chord
          arpIdx[c]++;
        } else {
          note = rootOf(ch, len);         // SEQ_MONO_ROOT
        }
        if (!mapped) {
          // BASS/LEAD: fold the STORED line into the live LOW..RANGE window
          // first (RANGE 2->1 lowers the upper octave into it, 1->2
          // restores the originals - stored notes never change), then
          // apply SEQMODE ENC1's octave shift: shifting AFTER the fold
          // keeps the transposition (folding after shNote would cancel it)
          if (mtype == SEQ_MONO_BASS || mtype == SEQ_MONO_LEAD) {
            int wlo = 24 + 12 * seqGenLowOf(c);
            int whi = wlo + 12 * seqGenRangeOf(c) - 1;
            if (whi > 127) whi = 127;
            note = foldWin((int)note, wlo, whi);
          }
          note = shNote(note, sh, tr);
        }
        if (modOn && stepOctM[c][stp]) {  // OCT: octave shift 0..127 clamp
          int v = (int)note + 12 * (int)stepOctM[c][stp];
          if (v < 0) v = 0;
          if (v > 127) v = 127;
          note = (uint8_t)v;
        }
        if (stepTrM[c][stp]) {            // TRANS: plain offset, never rolled
          int v = (int)note + (int)stepTrM[c][stp];
          if (v < 0) v = 0;
          if (v > 127) v = 127;
          note = (uint8_t)v;
        }
        if (defer[c]) {
          // GLIDE: new note-on BEFORE the old note-off (legato overlap)
          seqOutOn(c, note, vel);
          for (uint8_t i = 0; i < seqLen[c]; i++)
            seqOutOff(c, seqNotes[c][i]);
          seqLen[c] = 0;
          gateOffAt[c] = 0;
        } else {
          seqOutOn(c, note, vel);
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
      // COMP: a plain POLY STEP (or SKANK - the stab pattern rides the
      // derived voicing, HITS set while the track was STEP) with per-track
      // HITS >= 2 stages the chord into the stab queue instead of one
      // block send (DRUM keeps its per-voice path even though its et is
      // always STEP)
      bool compOn = !drumTrack && !doStrum &&
                    (et == SEQ_POLY_STEP || et == SEQ_POLY_SKANK) &&
                    compHits[c] >= 2;
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
        uint8_t note = shNote(raw, sh, tr);
        if (doStrum) {                  // stage the rake (STRUM / HAND)
          if (sn < CHORD_MAX) {
            snote[sn] = note;
            svel[sn] = dvel;
            // timed HAND: the stored onset for this STORED index (untimed
            // steps carry all zeros -> handled by the spread below)
            sdts[sn] = handTimed ? stepStrumDt[c][stp][i] : 0;
            sn++;
          }
        } else if (compOn) {         // stage the stab pattern
          if (sn < CHORD_MAX) {
            snote[sn] = note;
            svel[sn] = dvel;
            sn++;
          }
        } else {
          seqNotes[c][seqLen[c]] = note;
          seqOutOn(c, note, dvel);
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
        if (handStep) handRing[c] |= HAND_RING_GEST;   // rings until next content
      } else if (compOn && sn) {
        compEnqueue(c, snote, svel, sn, sd);   // stabs at the Euclid slots
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
  for (uint8_t c = 0; c < SEQ_CH_COUNT; c++) {
    advPrev[c] = 0xFF;               // no carry-over into the next run
    harmLiveN[c] = 0;                // LIVE HARM: no chord carried into a run
  }
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
  compSRes = 0;
  for (uint8_t i = 0; i < 8; i++) skankRes[i] = 0;  // SKANK page dials too
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

// COMP page ENC1: stabs per POLY STEP step, 4 counts = 1, clamped 0..8
// (0/1 = one block chord, like before; live - the playhead re-reads
// compHits on every advance, so a turn is audible on the next step)
void seqCompHitsAdjust(int16_t counts) {
  compSRes += counts;
  int16_t st = 0;
  while (compSRes >= 4) { compSRes -= 4; st++; }
  while (compSRes <= -4) { compSRes += 4; st--; }
  if (!st) return;
  int v = (int)compHits[seqEditCh] + st;
  if (v < 0) v = 0;
  if (v > 8) v = 8;
  compHits[seqEditCh] = (uint8_t)v;
}

int seqCompHits() { return compHits[seqEditCh]; }

uint8_t seqCompHitsOf(uint8_t i) { return i < SEQ_CH_COUNT ? compHits[i] : 0; }

void seqCompHitsSet(uint8_t i, uint8_t v) {
  if (i < SEQ_CH_COUNT) compHits[i] = (v > 8) ? 0 : v;
}

// --- SKANK page params (M2 on a POLY+SKANK track) --------------------------
// 0 TES, 1 MOT, 2 SHP, 3 THN, 4 EXT, 5 SUB, 6 TES%, 7 NOT% - all 0..100,
// 4 counts = 1 unit, clamped, no wrap (one residual per dial, like the
// GNR8 option knobs; a focus switch drops the partial turn like every
// other dial)
static uint8_t skankPrmDef(uint8_t idx) { return idx == 1 ? 50 : 0; }

void skankPrmAdjust(uint8_t idx, int16_t counts) {
  if (idx >= 8) return;
  skankRes[idx] += counts;
  int16_t st = 0;
  while (skankRes[idx] >= 4)  { skankRes[idx] -= 4; st++; }
  while (skankRes[idx] <= -4) { skankRes[idx] += 4; st--; }
  if (!st) return;
  int v = (int)skankPrmM[seqEditCh][idx] + (int)st;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  skankPrmM[seqEditCh][idx] = (uint8_t)v;
}

uint8_t skankPrm(uint8_t idx) {
  return (idx < 8) ? skankPrmM[seqEditCh][idx] : 0;
}

uint8_t skankPrmOf(uint8_t ch, uint8_t idx) {
  return (ch < SEQ_CH_COUNT && idx < 8) ? skankPrmM[ch][idx] : 0;
}

void skankPrmSet(uint8_t ch, uint8_t idx, uint8_t v) {
  if (ch >= SEQ_CH_COUNT || idx >= 8) return;
  if (v > 100) v = skankPrmDef(idx);     // corrupt byte: back to the default
  skankPrmM[ch][idx] = v;
}

// PREV-held shift row ENC2: semitone transpose of the edit sequence's
// playback, 1 count = 1 st, clamped -12..+12, no wrap (non-destructive -
// the stored steps never change; DRUM tracks ignore it at playback)
void seqTransAdjust(int16_t counts) {
  if (seqTrackOf(seqEditCh) == SEQ_TRACK_DRUM) return;
  int v = (int)seqTransM[seqEditCh] + counts;
  if (v < -12) v = -12;
  if (v > 12) v = 12;
  seqTransM[seqEditCh] = (int8_t)v;
}
int seqTrans() { return seqTransM[seqEditCh]; }
int8_t seqTransOf(uint8_t i) {
  if (i >= SEQ_CH_COUNT || seqTrackOf(i) == SEQ_TRACK_DRUM) return 0;
  return seqTransM[i];
}
void seqTransSet(uint8_t i, int8_t v) {
  if (i >= SEQ_CH_COUNT) return;
  if (v < -12) v = -12;
  if (v > 12) v = 12;
  seqTransM[i] = v;
}

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
  handRing[seq] = 0;            // a mute cuts the HAND ring too
}

// --- SEQMODE3 erase --------------------------------------------------------
// cut first so a running transport never re-sends erased notes; the step
// history goes too - otherwise the recording chains would resurrect old
// chords.  Listener sequences keep their own copy (stepSyncFrom never
// propagates an empty source), so "erase all" loops every track
static void seqEraseNotes(uint8_t s) {
  for (uint8_t i = 0; i < seqLen[s]; i++)
    seqOutOff(s, seqNotes[s][i]);
  seqLen[s] = 0;
  arpIdx[s] = 0;
  strumCancel(s);              // an erase also drops the un-sent rake tail
  compCancel(s);               // ...and the pending comp stabs
  handRing[s] = 0;
  handBufN[s] = 0;             // and the buffered string map (pattern gone)
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
// prob[16] | patLen[16] | follow[16] | genFill[16] | genAcc[16] |
// genGlide[16] | genTie[16] | genTones[16] | genLow[16] | genRange[16] |
// genAlgo[16] | leadContour[16] | compHits[16] | trans[16] | skankPrmM[128] |
// checksum = 564 bytes; blank or corrupt keeps the defaults.  A block
// written by an older build (532/436/420/404/388/260 bytes) still loads: the
// core fields are at the same offsets and the newer params keep their
// defaults - saved config survives the upgrade
static const uint8_t  SEQ_EE_MAGIC = 0x53;
static const uint16_t SEQ_EE_ADDR = 160;
static const uint16_t SEQ_EE_LEN = 564;       // + SKANK knobs[128] (b[435..562])
static const uint16_t SEQ_EE_LEN_SKANK = 532; // previous: SKANK knobs[96]
                                              // TES..SUB stride-6 (b[435..530])
static const uint16_t SEQ_EE_LEN_TRANS = 436; // previous: TRANS[16]
                                              // (b[419..434])
static const uint16_t SEQ_EE_LEN_COMP = 420;  // previous: COMP hits[16]
                                              // (b[403..418])
static const uint16_t SEQ_EE_LEN_LEAD = 404;  // previous: LEAD contour[16]
                                              // (b[387..402])
static const uint16_t SEQ_EE_LEN_GNR8 = 388;  // previous: GNR8 params (b[259..386])
static const uint16_t SEQ_EE_LEN_OLD = 260;   // legacy: before GNR8

void seqCfgSave() {
  uint8_t b[SEQ_EE_LEN];
  b[0] = SEQ_EE_MAGIC;
  b[1] = seqEditCh + 1;
  b[2] = seqMap();                    // GNR8/LEAD mapping mode (0..5; was the
                                      // shared prob clipboard)
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
    b[259 + i] = seqGenFillOf(i);             // GNR8 fill %
    b[275 + i] = seqGenAccOf(i);              // GNR8 accent %
    b[291 + i] = seqGenGlideOf(i);            // GNR8 glide %
    b[307 + i] = seqGenTieOf(i);              // GNR8 tie %
    b[323 + i] = seqGenTonesOf(i);            // GNR8 tones 0..3
    b[339 + i] = seqGenLowOf(i);              // GNR8 low note 0..2
    b[355 + i] = seqGenRangeOf(i);            // GNR8 range 1..5 octaves
    b[371 + i] = seqGenAlgoOf(i);             // GNR8 classic/euclid
    b[387 + i] = seqLeadContourOf(i);         // LEAD contour 0..2 (ARCH/DESC/WALK)
    b[403 + i] = seqCompHitsOf(i);            // COMP stabs per POLY STEP (0..8)
    b[419 + i] = (uint8_t)seqTransM[i];      // TRANS offset -12..+12 (int8)
    for (uint8_t p = 0; p < 8; p++)
      b[435 + i * 8 + p] = skankPrmOf(i, p); // SKANK knobs TES..NOT% (0..100)
  }
  uint8_t sum = 0;
  for (uint16_t i = 0; i < SEQ_EE_LEN - 1; i++) sum += b[i];
  b[SEQ_EE_LEN - 1] = sum;
  EEPROM.put(SEQ_EE_ADDR, b);   // put() writes only the bytes that changed
}

void seqCfgLoad() {
  uint8_t b[SEQ_EE_LEN];
  EEPROM.get(SEQ_EE_ADDR, b);
  // pick the NEWEST block length whose magic + checksum validate, so an
  // older save (532 = first SKANK block, 436 = TRANS, 404 = LEAD,
  // 388 = GNR8, 260 = legacy) still restores its fields and the newer ones
  // keep their defaults
  uint16_t len = 0;
  for (uint16_t L = SEQ_EE_LEN;;) {
    uint8_t sum = 0;
    for (uint16_t i = 0; i < L - 1; i++) sum += b[i];
    if (b[0] == SEQ_EE_MAGIC && sum == b[L - 1]) { len = L; break; }
    if (L == SEQ_EE_LEN) L = SEQ_EE_LEN_SKANK;
    else if (L == SEQ_EE_LEN_SKANK) L = SEQ_EE_LEN_TRANS;
    else if (L == SEQ_EE_LEN_TRANS) L = SEQ_EE_LEN_COMP;
    else if (L == SEQ_EE_LEN_COMP) L = SEQ_EE_LEN_LEAD;
    else if (L == SEQ_EE_LEN_LEAD) L = SEQ_EE_LEN_GNR8;
    else if (L == SEQ_EE_LEN_GNR8) L = SEQ_EE_LEN_OLD;
    else break;
  }
  if (!len) return;
  bool extG = (len >= SEQ_EE_LEN_GNR8);   // GNR8 params present
  bool extL = (len >= SEQ_EE_LEN_LEAD);   // LEAD contour present
  bool extC = (len >= SEQ_EE_LEN_COMP);   // COMP hits present
  bool extT = (len >= SEQ_EE_LEN_TRANS);  // TRANS offset present
  bool extS = (len >= SEQ_EE_LEN_SKANK);  // first 6 SKANK knobs (stride-6)
  bool extS2 = (len >= SEQ_EE_LEN);       // + TES%/NOT% (stride-8 layout)
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
  seqMapSet(b[2]);                       // GNR8/LEAD mapping (bad -> NEAR)
  if (extG) {                            // GNR8 params (new-format block;
    for (uint8_t i = 0; i < SEQ_CH_COUNT; i++) {   // legacy blocks keep the
      seqGenFillSet(i, b[259 + i]);                 // defaults above)
      seqGenAccSet(i, b[275 + i]);
      seqGenGlideSet(i, b[291 + i]);
      seqGenTieSet(i, b[307 + i]);
      seqGenTonesSet(i, b[323 + i]);
      seqGenLowSet(i, b[339 + i]);
      seqGenRangeSet(i, b[355 + i]);
      seqGenAlgoSet(i, b[371 + i]);
    }
  }
  if (extL) {                            // LEAD contour (newest block only)
    for (uint8_t i = 0; i < SEQ_CH_COUNT; i++)
      seqLeadContourSet(i, b[387 + i]);  // clamped inside (bad -> ARCH)
  }
  if (extC) {                            // COMP stabs (newer block only)
    for (uint8_t i = 0; i < SEQ_CH_COUNT; i++)
      seqCompHitsSet(i, b[403 + i]);     // clamped inside (bad -> 0)
  }
  if (extT) {                            // TRANS offset (newest block only)
    for (uint8_t i = 0; i < SEQ_CH_COUNT; i++) {
      int v = (int8_t)b[419 + i];        // signed semitones
      if (v < -12 || v > 12) v = 0;
      seqTransSet(i, (int8_t)v);
    }
  }
  if (extS2) {                           // SKANK page knobs, newest layout
    for (uint8_t i = 0; i < SEQ_CH_COUNT; i++)
      for (uint8_t p = 0; p < 8; p++)
        skankPrmSet(i, p, b[435 + i * 8 + p]);  // clamped inside (bad -> def)
  } else if (extS) {                     // first 6 knobs, old stride-6 layout
    for (uint8_t i = 0; i < SEQ_CH_COUNT; i++)
      for (uint8_t p = 0; p < 6; p++)
        skankPrmSet(i, p, b[435 + i * 6 + p]);  // TES%/NOT% keep their defaults
  }
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
  // playback notes on the OLD channel stay: the playhead owns them (gate
  // timer / step boundary / SUS chord-change release them there) - cutting
  // them here would truncate a recorded step the moment the keyboard moves
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
