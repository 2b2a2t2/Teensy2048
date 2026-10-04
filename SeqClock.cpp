#include "SeqClock.h"
#include "Modes.h"
#include "Buttons.h"
#include "Keyboard.h"
#include "Events.h"     // encCcIncoming - DAW CC feedback syncs ENCMODE knobs
#include <uClock.h>

#define STEPS_COUNT 16

// written from uClock timer ISR (via usbMIDI.read in loop), read from loop()
static volatile uint8_t playStep = 0;    // 0..15
static volatile bool clockRun = false;

// ISR -> loop handoff: sequenced notes are sent from loop, never the ISR
static volatile uint8_t pendingStep = 255;   // 255 = nothing pending
static volatile bool pendingStop = false;
static volatile bool pendingStart = false;   // transport (re)start: reset heads

// SEQMODE3 sync state: INT (default, master) or EXT (follow incoming clock)
static bool syncInternal = true;
static uint16_t tempoBpm = 120;

// fired from uClock's timer ISR on every output PPQN_24 tick (master mode only)
static void onSync24Callback(uint32_t tick) {
  if (syncInternal) usbMIDI.sendClock();   // 0xF8 to the DAW (Ableton slave)
  (void)tick;
}

static void onStepCallback(uint32_t step) {
  playStep = (uint8_t)(step % STEPS_COUNT);
  pendingStep = playStep;
}

static void onClockStartCb() {
  playStep = 0;
  clockRun = true;
  pendingStart = true;            // loop resets every track's position
  pendingStep = 0;                // play step 1 as soon as the loop wakes
}

static void onClockStopCb() {
  clockRun = false;
  pendingStop = true;           // release sequenced notes from loop context
}

static void midiClock() {
  static uint32_t lastUs = 0;
  uint32_t now = micros();
  if (lastUs != 0) {
    uint32_t gap = now - lastUs;
    // >100ms silence between ticks while running = pump was starved
    if (clockRun && gap > 100000UL && Serial.availableForWrite() > 48) {
      Serial.print("CLKGAP ");
      Serial.println(gap);
    }
    // <1ms apart = draining a backlog (tempo-poisoning burst)
    if (gap < 1000UL && Serial.availableForWrite() > 48) {
      Serial.println("CLKBURST");
    }
  }
  lastUs = now;
  uClock.clockMe();
}
// incoming clock transport is honored only as slave (EXT); in INT master
// mode the Teensy's own start/stop stays authoritative
static void midiStart() { if (!syncInternal) uClock.start(); }
static void midiStop()  { if (!syncInternal) uClock.stop(); }
static void midiContinue() { if (!syncInternal) uClock.start(); }

// incoming notes feed the record workflow (loop context via seqPumpMidi)
static void midiNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
  if (vel) extNoteOn(ch, note);
  else extNoteOff(ch, note);      // velocity 0 = note off
}
static void midiNoteOff(uint8_t ch, uint8_t note, uint8_t vel) {
  (void)vel;
  extNoteOff(ch, note);
}

// incoming CC feedback (loop context via seqPumpMidi): keeps the ENCMODE
// knob positions in step with the DAW so a knob turn never starts from a
// stale value
static void midiControlChange(uint8_t ch, uint8_t cc, uint8_t val) {
  encCcIncoming(ch, cc, val);
}

// green playhead in every mode (SEQMODE owns it, all modes inherit);
// active (recorded) steps = dimmed green, empty steps = dimmed dark blue
static void drawSteps() {
  // variant-3 channel overlays (KEY3/SEQ3/ENC3) own the step LEDs (hook
  // repaints them on mode change), the playhead must not paint over them.
  // SEQ3 gives them back to the sequencer as soon as an encoder moves
  if (modeVariant == 3 &&
      (modeBase == MODE_KEY || modeBase == MODE_ENC ||
       (modeBase == MODE_SEQ && seq3ChSelActive())))
    return;
  // DRUM part-select overlay (hold M1) also owns the step LEDs
  if (modeBase == MODE_SEQ && modeVariant != 3 &&
      drumPartSelectActive() && seqTrack() == SEQ_TRACK_DRUM)
    return;
  // ENC mute overlays (hold M2 / M1) also own the step LEDs
  if (encMuteActive() || drumVoiceMuteActive()) return;
  uint8_t base = seqStepViewBase();        // 16-step window of the edit track
  uint8_t ph = seqStepPlayPad();           // its playhead inside that window
  uint8_t gh = seqStepPlayGhost();         // ghost when it runs in another bar
  bool changed = false;
  for (uint8_t i = 0; i < STEPS_COUNT; i++) {
    uint8_t led = groupSteps[i];
    if (leds[led] == CRGB(CRGB::White)) continue;  // pad currently touched
    CRGB c;
    if (!seqStepUsed((uint8_t)(base + i))) {
      c = CRGB::Black;    // past the pattern length: pad off
    } else if (i == ph) {
      c = CRGB::Green;
    } else if (i == gh) {
      c = CRGB::DarkBlue;   // bar on the pads is not the one being read
    } else if (stepIsActive(base + i)) {
      c = CRGB::Green;
      c.nscale8(64);    // dimmed green = active step
    } else {
      c = CRGB::DarkBlue;
      c.nscale8(32);   // barely visible background (matches STEP_DIM in Modes.cpp)
    }
    if (leds[led] != c) { leds[led] = c; changed = true; }
  }
  if (changed) FastLED.show();
}

void initSeqClock() {
  uClock.setOutputPPQN(uClock.PPQN_96);
  uClock.setInputPPQN(uClock.PPQN_24);
  uClock.setOnStep(onStepCallback);
  uClock.setOnClockStart(onClockStartCb);
  uClock.setOnClockStop(onClockStopCb);
  uClock.setOnSync(uClock.PPQN_24, onSync24Callback);
  uClock.init();
  // boot default = INT master; ENC4 SYNC (SEQMODE3) can switch to EXT slave
  uClock.setClockMode(syncInternal ? uClock.INTERNAL_CLOCK : uClock.EXTERNAL_CLOCK);
  uClock.setTempo((float)tempoBpm);

  usbMIDI.setHandleClock(midiClock);
  usbMIDI.setHandleStart(midiStart);
  usbMIDI.setHandleStop(midiStop);
  usbMIDI.setHandleContinue(midiContinue);
  usbMIDI.setHandleNoteOn(midiNoteOn);
  usbMIDI.setHandleNoteOff(midiNoteOff);
  usbMIDI.setHandleControlChange(midiControlChange);
}

void seqPumpMidi() {
  uint8_t n = 0;
  while (n < 64 && usbMIDI.read()) n++;   // bounded; 64 covers catch-up bursts
}

void pollSeqClock() {
  seqPumpMidi();

  // sequencer note housekeeping: the ISR only queues flags, notes go out
  // here in loop context (no USB/MIDI calls from interrupt code)
  noInterrupts();
  uint8_t sAdv = pendingStep;  pendingStep = 255;
  bool st = pendingStop;       pendingStop = false;
  bool sn = pendingStart;      pendingStart = false;
  interrupts();
  if (st) seqStepStop();             // transport stopped: let every note go
  else if (sn) seqStepAdvance(true);       // start: every track back to its head
  else if (sAdv != 255) seqStepAdvance(false);
  keyMonoPoll();               // live MONO-track keyboard voice (TRACK/TYPE)
  seqGatePoll();               // track gate note-offs (MONO step-edit)

  // redraw when the displayed position moved or the mode changed.  The
  // trigger must include the DOT's own source (edit track head): playStep
  // is written by the uClock timer ISR and can bump between the dispatch
  // and this check, burning a trigger and leaving the dot one step behind
  // until it jumped over the missed position
  static ModeBase lastBase = modeBase;
  static uint8_t lastDrawn = 255;
  static uint8_t lastPh = 255;
  static uint8_t lastGh = 255;
  uint8_t s = playStep;
  uint8_t ph = seqStepPlayPad();
  uint8_t gh = seqStepPlayGhost();
  if (s != lastDrawn || ph != lastPh || gh != lastGh || modeBase != lastBase) {
    lastDrawn = s;
    lastPh = ph;
    lastGh = gh;
    lastBase = modeBase;
    drawSteps();
  }
}

uint8_t seqCurrentStep() { return playStep; }
bool seqIsRunning() { return clockRun; }

// INT = master: uClock generates the ticks and onSync24 streams 0xF8 out.
// EXT = slave: follow incoming USB-MIDI clock (original behavior).
// Switching always stops first so state never leaks across modes.
void seqSetSyncInternal(bool on) {
  if (on == syncInternal) return;
  uClock.stop();
  syncInternal = on;
  uClock.setClockMode(on ? uClock.INTERNAL_CLOCK : uClock.EXTERNAL_CLOCK);
  if (on) uClock.setTempo((float)tempoBpm);   // setTempo is ignored in EXTERNAL
}

bool seqIsSyncInternal() { return syncInternal; }

// tempo is stored always; applied live when the internal clock is running
void seqSetTempo(uint16_t bpm) {
  if (bpm < 40) bpm = 40;
  if (bpm > 240) bpm = 240;
  tempoBpm = bpm;
  if (syncInternal) uClock.setTempo((float)bpm);
}

uint16_t seqTempo() { return tempoBpm; }

// SEQMODE3 TRACK knob: per-sequence track type (clamped, no wrap).
// channel 1 = DRUM by default, channels 2..16 = POLY
static uint8_t trackType[16] = {SEQ_TRACK_DRUM, SEQ_TRACK_POLY, SEQ_TRACK_POLY,
                                SEQ_TRACK_POLY, SEQ_TRACK_POLY, SEQ_TRACK_POLY,
                                SEQ_TRACK_POLY, SEQ_TRACK_POLY, SEQ_TRACK_POLY,
                                SEQ_TRACK_POLY, SEQ_TRACK_POLY, SEQ_TRACK_POLY,
                                SEQ_TRACK_POLY, SEQ_TRACK_POLY, SEQ_TRACK_POLY,
                                SEQ_TRACK_POLY};
static int16_t trackRes = 0;           // leftover encoder counts

void seqTrackAdjust(int16_t counts) {
  trackRes += counts;
  int8_t step = 0;
  while (trackRes >= 4)  { trackRes -= 4;  step++; }
  while (trackRes <= -4) { trackRes += 4;  step--; }
  if (!step) return;
  uint8_t cur = trackType[seqEditChannel() - 1];
  int16_t t = (int16_t)cur + step;
  if (t < SEQ_TRACK_DRUM) t = SEQ_TRACK_DRUM;
  if (t > SEQ_TRACK_POLY) t = SEQ_TRACK_POLY;
  trackType[seqEditChannel() - 1] = (uint8_t)t;
}

uint8_t seqTrack() { return trackType[seqEditChannel() - 1]; }

const char* seqTrackName() {
  static const char* names[3] = { "DRUM", "MONO", "POLY" };
  return names[seqTrack()];
}

uint8_t seqTrackOf(uint8_t idx) {
  return (idx < 16) ? trackType[idx] : SEQ_TRACK_POLY;
}

void seqSetTrackType(uint8_t idx, uint8_t t) {
  if (idx >= 16) return;
  if (t > SEQ_TRACK_POLY) t = SEQ_TRACK_POLY;
  trackType[idx] = t;
}

// SEQMODE3 TYPE (ENC7): per-channel mono transform (default ROOT = 0)
static uint8_t monoType[16] = {0};
static int16_t monoRes = 0;             // leftover encoder counts

void seqMonoTypeAdjust(int16_t counts) {
  monoRes += counts;
  int8_t step = 0;
  while (monoRes >= 4)  { monoRes -= 4;  step++; }
  while (monoRes <= -4) { monoRes += 4;  step--; }
  if (!step) return;
  int16_t t = (int16_t)monoType[seqEditChannel() - 1] + step;
  if (t < SEQ_MONO_ROOT) t = SEQ_MONO_ROOT;
  if (t > SEQ_MONO_ARP)  t = SEQ_MONO_ARP;
  monoType[seqEditChannel() - 1] = (uint8_t)t;
}

uint8_t seqMonoType() { return monoType[seqEditChannel() - 1]; }

const char* seqMonoTypeName() {
  static const char* names[3] = { "ROOT", "BASS", "ARP" };
  return names[seqMonoType()];
}

uint8_t seqMonoTypeOf(uint8_t idx) {
  return (idx < 16) ? monoType[idx] : SEQ_MONO_ROOT;
}

void seqSetMonoType(uint8_t idx, uint8_t m) {
  if (idx >= 16) return;
  if (m > SEQ_MONO_ARP) m = SEQ_MONO_ARP;
  monoType[idx] = m;
}

// SEQMODE3 pattern setup, per track: length (ENC2, 0..64 steps, boot 16),
// the 16-step edit window picked by the C D E F bars, and which bar loops
// playback on itself.  Independent per track - every playhead wraps at its
// own length (polymeter); a looped bar plays regardless of the length
static uint8_t patLen[16] = {16, 16, 16, 16, 16, 16, 16, 16,
                             16, 16, 16, 16, 16, 16, 16, 16};
static int16_t patRes = 0;               // leftover encoder counts (ENC2)
static uint8_t seqPage[16] = {0};        // edit window 0..3 (zero = steps 1-16)
static uint8_t loopPage[16] = {0};       // bar looped by its own press
static bool    loopOn[16] = {false};

void seqPatLenAdjust(int16_t counts) {
  patRes += counts;
  int8_t step = 0;
  while (patRes >= 4)  { patRes -= 4;  step++; }
  while (patRes <= -4) { patRes += 4;  step--; }
  if (!step) return;
  uint8_t ch = seqEditChannel() - 1;
  int v = (int)patLen[ch] + step;
  if (v < 0) v = 0;
  if (v > 64) v = 64;
  patLen[ch] = (uint8_t)v;
}

int seqPatLen() { return patLen[seqEditChannel() - 1]; }

uint8_t seqPatLenOf(uint8_t ch) { return (ch < 16) ? patLen[ch] : 16; }

// SEQMODE3 ENC4: per-track pattern speed, clamped 0..6 (no wrap).  The
// ladder doubles each way around the default 4 = normal (one step per
// 16th note): 1/16, 1/8, 1/4, 1/2, 1, 2, 4 - num/den describe how many
// sub-steps a track takes inside one master tick
static const uint8_t spdNum[7] = { 1, 1, 1, 1, 1, 2, 4 };
static const uint8_t spdDen[7] = { 16, 8, 4, 2, 1, 1, 1 };
static uint8_t patSpeed[16] = { 4, 4, 4, 4, 4, 4, 4, 4,
                                4, 4, 4, 4, 4, 4, 4, 4 };
static int16_t spdRes = 0;               // leftover encoder counts (ENC4)

void seqSpeedAdjust(int16_t counts) {
  spdRes += counts;
  int8_t step = 0;
  while (spdRes >= 4)  { spdRes -= 4;  step++; }
  while (spdRes <= -4) { spdRes += 4;  step--; }
  if (!step) return;
  int v = (int)patSpeed[seqEditChannel() - 1] + step;
  if (v < 0) v = 0;
  if (v > 6) v = 6;
  patSpeed[seqEditChannel() - 1] = (uint8_t)v;
}

uint8_t seqSpeedIdxOf(uint8_t ch) { return (ch < 16) ? patSpeed[ch] : 4; }

void seqSpeedSetOf(uint8_t ch, uint8_t idx) {
  if (ch >= 16) return;
  patSpeed[ch] = (idx > 6) ? 4 : idx;
}

const char *seqSpeedName(uint8_t idx) {
  static const char *const nm[7] = { "1/16", "1/8", "1/4", "1/2",
                                     "1", "2", "4" };
  return nm[(idx > 6) ? 4 : idx];
}

uint8_t seqSpeedNumOf(uint8_t ch) { return spdNum[seqSpeedIdxOf(ch)]; }
uint8_t seqSpeedDenOf(uint8_t ch) { return spdDen[seqSpeedIdxOf(ch)]; }

void seqPlayRange(uint8_t ch, uint8_t *lo, uint8_t *hi) {
  if (ch >= 16) { *lo = 0; *hi = STEPS_COUNT; return; }
  if (loopOn[ch]) {                        // a looped bar plays by itself
    *lo = (uint8_t)(loopPage[ch] * STEPS_COUNT);
    *hi = (uint8_t)(*lo + STEPS_COUNT);
  } else {
    *lo = 0;
    *hi = patLen[ch];                      // 0 = silent track
  }
}

bool seqLoopOn(uint8_t ch) { return ch < 16 && loopOn[ch]; }

// bar press on the edit channel: another bar selects its window, the
// SELECTED bar toggles its loop (single loop slot per track)
uint8_t seqBarPress(uint8_t bar) {
  if (bar > 3) return 0;
  uint8_t ch = seqEditChannel() - 1;
  if (seqPage[ch] != bar) { seqPage[ch] = bar; return 0; }
  if (loopOn[ch] && loopPage[ch] == bar) { loopOn[ch] = false; return 2; }
  loopOn[ch] = true;
  loopPage[ch] = bar;
  return 1;
}

uint8_t seqStepViewBase() {
  return (uint8_t)(seqPage[seqEditChannel() - 1] * STEPS_COUNT);
}

uint8_t seqStepPage() { return seqPage[seqEditChannel() - 1]; }

bool seqBarLooped(uint8_t page) {
  uint8_t ch = seqEditChannel() - 1;
  return loopOn[ch] && loopPage[ch] == page;
}

// SEQMODE3 FOLLOW (ENC3): ON = the track keeps its own pattern length and
// its active steps as the rhythm mask, but its NOTES are read live from
// the feeding track (input channel's track) at the feeder's playhead - a
// 16-step track then mirrors a 64-step feeder bar by bar; an empty feeder
// step holds the feeder's last chord/note.  RAM only, boot OFF; meaningful
// for MONO/POLY listeners pointing at another track
static bool followOn[16] = {false};

bool seqFollow() { return followOn[seqEditChannel() - 1]; }
bool seqFollowOf(uint8_t ch) { return ch < 16 && followOn[ch]; }
void seqFollowSet(bool on) { followOn[seqEditChannel() - 1] = on; }

// System Real-Time Start (0xFA): with SYNC=INT also launch the internal
// transport so Clock ticks flow to the DAW; with SYNC=EXT just emit Start.
void seqSendStart() {
  if (syncInternal) uClock.start();
  usbMIDI.sendStart();
}

// PLAY toggle: while the internal (SYNC=INT) transport is running, PLAY
// stops it and emits Real-Time Stop (0xFC); otherwise behave like Start.
// (usbMIDI.sendStop() is unusable: the Teensy core sends 0xFB = Continue.)
void seqTogglePlay() {
  if (syncInternal && clockRun) {
    uClock.stop();                      // onClockStopCb releases sequenced notes
    usbMIDI.sendRealTime(0xFC);
  } else {
    seqSendStart();
  }
}

// section = 0 pump, 1 touch/inputs, 2 events, 3 display; report worst >3ms once/s
void seqDiagNote(uint8_t section, uint32_t us) {
  static uint32_t worst[4] = {0, 0, 0, 0};
  static uint32_t lastReport = 0;
  if (section > 3) return;
  if (us > worst[section]) worst[section] = us;
  uint32_t now = millis();
  if (now - lastReport < 1000) return;
  lastReport = now;
  if ((worst[0] | worst[1] | worst[2] | worst[3]) > 3000 &&
      Serial.availableForWrite() > 64) {
    Serial.print("DIAG pump=");
    Serial.print(worst[0]);
    Serial.print(" in=");
    Serial.print(worst[1]);
    Serial.print(" ev=");
    Serial.print(worst[2]);
    Serial.print(" dsp=");
    Serial.println(worst[3]);
  }
  worst[0] = worst[1] = worst[2] = worst[3] = 0;
}
