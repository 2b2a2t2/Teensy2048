#include "SeqClock.h"
#include "Modes.h"
#include "Buttons.h"
#include <uClock.h>

#define STEPS_COUNT 16

// written from uClock timer ISR (via usbMIDI.read in loop), read from loop()
static volatile uint8_t playStep = 0;    // 0..15
static volatile bool clockRun = false;

static void onStepCallback(uint32_t step) {
  playStep = (uint8_t)(step % STEPS_COUNT);
}

static void onClockStartCb() {
  playStep = 0;
  clockRun = true;
}

static void onClockStopCb() {
  clockRun = false;
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

// green playhead only while SEQ mode; never clobber touch feedback (white)
static void drawSteps() {
  uint8_t s = playStep;
  bool show = (modeBase == MODE_SEQ);
  bool changed = false;
  for (uint8_t i = 0; i < STEPS_COUNT; i++) {
    uint8_t led = groupSteps[i];
    if (leds[led] == CRGB(CRGB::White)) continue;  // pad currently touched
    CRGB c = (show && i == s) ? CRGB::Green : CRGB::Black;
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
  uClock.init();
  uClock.setClockMode(uClock.EXTERNAL_CLOCK);

  usbMIDI.setHandleClock(midiClock);
  usbMIDI.setHandleStart(midiStart);
  usbMIDI.setHandleStop(midiStop);
  usbMIDI.setHandleContinue(midiContinue);
}

void seqPumpMidi() {
  uint8_t n = 0;
  while (n < 64 && usbMIDI.read()) n++;   // bounded; 64 covers catch-up bursts
}

void pollSeqClock() {
  seqPumpMidi();

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
