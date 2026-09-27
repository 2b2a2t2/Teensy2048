#include "SeqClock.h"
#include "Modes.h"
#include "Buttons.h"
#include "Keyboard.h"
#include <uClock.h>

#define STEPS_COUNT 16

// written from uClock timer ISR (via usbMIDI.read in loop), read from loop()
static volatile uint8_t playStep = 0;    // 0..15
static volatile bool clockRun = false;

// ISR -> loop handoff: sequenced notes are sent from loop, never the ISR
static volatile uint8_t pendingStep = 255;   // 255 = nothing pending
static volatile bool pendingStop = false;

// SEQMODE3 sync state: EXT (default, follow incoming clock) or INT (master)
static bool syncInternal = false;
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
  pendingStep = 0;              // play step 1 as soon as the loop wakes
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
static void midiStart() { uClock.start(); }
static void midiStop()  { uClock.stop(); }
static void midiContinue() { uClock.start(); }

// incoming notes feed the record workflow (loop context via seqPumpMidi)
static void midiNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
  if (vel) extNoteOn(ch, note);
  else extNoteOff(ch, note);      // velocity 0 = note off
}
static void midiNoteOff(uint8_t ch, uint8_t note, uint8_t vel) {
  (void)vel;
  extNoteOff(ch, note);
}

// green playhead in every mode (SEQMODE owns it, all modes inherit);
// active (recorded) steps = dimmed green, empty steps = dimmed dark blue
static void drawSteps() {
  // variant-3 channel overlays (KEY3/SEQ3/ENC3) own the step LEDs (hook
  // repaints them on mode change), the playhead must not paint over them
  if (modeVariant == 3 && (modeBase == MODE_KEY || modeBase == MODE_SEQ ||
                           modeBase == MODE_ENC))
    return;
  // DRUM part-select overlay (hold M1) also owns the step LEDs
  if (modeBase == MODE_SEQ && modeVariant != 3 &&
      drumPartSelectActive() && seqTrack() == SEQ_TRACK_DRUM)
    return;
  uint8_t s = playStep;
  bool changed = false;
  for (uint8_t i = 0; i < STEPS_COUNT; i++) {
    uint8_t led = groupSteps[i];
    if (leds[led] == CRGB(CRGB::White)) continue;  // pad currently touched
    CRGB c;
    if (i == s) {
      c = CRGB::Green;
    } else if (stepIsActive(i)) {
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
  uClock.setClockMode(uClock.EXTERNAL_CLOCK);

  usbMIDI.setHandleClock(midiClock);
  usbMIDI.setHandleStart(midiStart);
  usbMIDI.setHandleStop(midiStop);
  usbMIDI.setHandleContinue(midiContinue);
  usbMIDI.setHandleNoteOn(midiNoteOn);
  usbMIDI.setHandleNoteOff(midiNoteOff);
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
  interrupts();
  if (st) seqStepStop();       // transport stopped: let every note go
  else if (sAdv != 255) seqStepAdvance(sAdv);

  // redraw when playhead moved or mode changed — no dirty-flag race
  static ModeBase lastBase = modeBase;
  static uint8_t lastDrawn = 255;
  uint8_t s = playStep;
  if (s != lastDrawn || modeBase != lastBase) {
    lastDrawn = s;
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
