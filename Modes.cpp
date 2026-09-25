#include "Modes.h"
#include "Buttons.h"
#include "Display.h"
#include "SeqClock.h"

ModeBase modeBase = MODE_NONE;
uint8_t modeVariant = 0;

// hold (variant 3) is momentary: remember what to restore on release
static ModeBase heldPrevBase = MODE_NONE;
static uint8_t heldPrevVariant = 0;
static bool heldActive = false;

static const uint8_t modeLeds[3] = { 32, 33, 34 };  // ENC, SEQ, KEY
static const ModeBase modeOfLed[3] = { MODE_ENC, MODE_SEQ, MODE_KEY };
static const char* baseNames[4] = { "---", "ENC", "SEQ", "KEY" };

#define MODE_DIM 64  // 25% brightness for unselected mode buttons

#define PULSE_PERIOD_MS 1000  // variant 2: breathing pulse
#define BLINK_MS        250   // variant 4: on/off half-period

// last non-momentary variant per family (index = ModeBase), default 1
static uint8_t familyMem[4] = { 0, 1, 1, 1 };

static int8_t activeModeLed() {
  for (uint8_t i = 0; i < 3; i++) {
    if (modeBase == modeOfLed[i]) return modeLeds[i];
  }
  return -1;
}

// color for the selected mode LED: solid / pulsing / blinking
static CRGB selectedModeColor() {
  if (modeVariant == 2) {
    uint8_t s = sin8((uint32_t)millis() * 256UL / PULSE_PERIOD_MS);
    uint8_t b = 24 + scale8(s, 220);   // 24..244
    return CRGB(b, b, b);
  }
  if (modeVariant == 4) {
    return ((millis() / BLINK_MS) & 1) ? CRGB::White : CRGB::Black;
  }
  return CRGB::White;
}

static CRGB dimColorFor(ModeBase b) {
  CRGB c;
  switch (b) {
    case MODE_ENC: c = CRGB::Purple;       break;
    case MODE_SEQ: c = CRGB::Blue;         break;
    case MODE_KEY: c = CRGB::MediumSeaGreen; break;
    default:       c = CRGB::Black;        break;
  }
  c.nscale8(MODE_DIM);
  return c;
}

static CRGB modeLedHook(uint8_t pad, bool isTouched, CRGB defaultColor) {
  int8_t led = ledForPad[pad];
  for (uint8_t i = 0; i < 3; i++) {
    if (led == modeLeds[i]) {
      if (isTouched) return CRGB::White;
      if (modeBase == modeOfLed[i]) return selectedModeColor();
      return dimColorFor(modeOfLed[i]);
    }
  }
  // step pads = LEDs 0..15: green playhead while SEQ mode
  if (led >= 0 && led < 16) {
    if (isTouched) return CRGB::White;
    if (modeBase == MODE_SEQ && led == groupSteps[seqCurrentStep()])
      return CRGB::Green;
    return CRGB::Black;
  }
  return defaultColor;
}

static void applyModeLeds() {
  int8_t active = activeModeLed();
  for (uint8_t i = 0; i < 3; i++) {
    leds[modeLeds[i]] = (modeLeds[i] == active)
      ? selectedModeColor()
      : dimColorFor(modeOfLed[i]);
  }
  FastLED.show();
}

void pollModeLeds() {
  static unsigned long lastPulseShow = 0;
  static int8_t lastBlink = -1;

  int8_t active = activeModeLed();
  if (active < 0) { lastBlink = -1; return; }

  if (modeVariant == 2) {
    unsigned long now = millis();
    if (now - lastPulseShow < 30) return;   // ~30 fps
    lastPulseShow = now;
    leds[active] = selectedModeColor();
    FastLED.show();
  } else if (modeVariant == 4) {
    int8_t phase = (millis() / BLINK_MS) & 1;
    if (phase == lastBlink) return;
    lastBlink = phase;
    leds[active] = selectedModeColor();
    FastLED.show();
  } else {
    lastBlink = -1;
  }
}

void initModes() {
  modeBase = MODE_NONE;
  modeVariant = 0;
  setLedColorHook(modeLedHook);
  applyModeLeds();
  headerSetLeft(modeName());
}

void modeOnEvent(uint8_t pad, uint8_t ev) {
  if (ev != EV_CLICKED && ev != EV_HOLD && ev != EV_DOUBLE && ev != EV_RELEASED) return;

  int8_t led = ledForPad[pad];
  ModeBase b = MODE_NONE;
  for (uint8_t i = 0; i < 3; i++) {
    if (led == modeLeds[i]) b = modeOfLed[i];
  }
  if (b == MODE_NONE) return;

  if (ev == EV_RELEASED) {
    // releasing the held mode button returns to the previous mode
    if (heldActive && modeBase == b && modeVariant == 3) {
      modeBase = heldPrevBase;
      modeVariant = heldPrevVariant;
      heldActive = false;
      applyModeLeds();
      headerSetLeft(modeName());
    }
    return;
  }

  if (ev == EV_HOLD) {
    if (!heldActive) {
      heldPrevBase = modeBase;
      heldPrevVariant = modeVariant;
      heldActive = true;
    }
    modeVariant = 3;
  } else if (ev == EV_DOUBLE) {
    modeVariant = 4;
    familyMem[b] = 4;
  } else {
    // click: same family — 1->2, 2->1, 4->1; otherwise recall last variant
    if (modeBase == b) {
      if (modeVariant == 1) modeVariant = 2;
      else modeVariant = 1;          // 2 or 4 -> 1
    } else {
      modeVariant = familyMem[b] ? familyMem[b] : 1;
    }
    familyMem[b] = modeVariant;
  }
  modeBase = b;

  applyModeLeds();
  headerSetLeft(modeName());
  refreshMainScreen();   // encoder labels belong to the new binding
}

const char* modeName() {
  static char name[8];
  if (modeBase == MODE_NONE || modeVariant == 0) return "---";
  if (modeVariant == 1) return baseNames[modeBase];
  snprintf(name, sizeof(name), "%s%u", baseNames[modeBase], modeVariant);
  return name;
}
