#include "Modes.h"
#include "Buttons.h"
#include "Display.h"
#include "SeqClock.h"
#include "Keyboard.h"
#include "Encoders.h"

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
#define KEY_DIM 128  // keyboard idle brightness in KEY mode (dark blue)
#define STEP_DIM 32   // step pad background (dark blue, barely visible)

// semitone index (0=C..11=B) for LEDs 25..41; -1 = LEDs 32..36 (not keys)
static const int8_t kbPitchIdx[17] = {
  0, 2, 4, 5, 7, 9, 11,      // 25..31: C D E F G A B
  -1, -1, -1, -1, -1,        // 32..36: mode/transport (not keys)
  1, 3, 6, 8, 10             // 37..41: C# D# F# G# A#
};

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
  // step pads = LEDs 0..15 (groupSteps is identity 0..15): green playhead
  // owned by SEQMODE but inherited by every mode; recorded steps dimmed
  // green, empty steps dimmed dark blue
  if (led >= 0 && led < 16) {
    if (isTouched) return CRGB::White;
    // variant-3 channel overlays: the 16 steps become the MIDI channel
    // selector - dimmed yellow background, selected channel in bright yellow
    // (KEY = keyboard output, SEQ = sequencer edit, ENC = encoder CCs)
    if (modeVariant == 3 && (modeBase == MODE_KEY || modeBase == MODE_SEQ ||
                             modeBase == MODE_ENC)) {
      uint8_t sel = (modeBase == MODE_KEY) ? keyChannel() :
                     (modeBase == MODE_SEQ) ? seqEditChannel() : encChannel();
      if (led == (int8_t)(sel - 1)) return CRGB::Yellow;
      CRGB y = CRGB::Yellow;
      y.nscale8(16);
      return y;
    }
    // DRUM part-select overlay (hold M1): dim orange, selected part bright
    // orange (pad N = drum note 36+N, pad0 = C1 ... pad15 = D#2)
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        drumPartSelectActive() && seqTrack() == SEQ_TRACK_DRUM) {
      if (led == (int8_t)(drumPartNote() - 36)) return CRGB(255, 72, 0);
      CRGB o = CRGB(255, 72, 0);
      o.nscale8(16);
      return o;
    }
    if (led == groupSteps[seqCurrentStep()]) return CRGB::Green;
    CRGB c;
    if (stepIsActive((uint8_t)led)) {
      c = CRGB::Green;
      c.nscale8(64);
    } else {
      c = CRGB::DarkBlue;
      c.nscale8(STEP_DIM);
    }
    return c;
  }
  // keyboard pads (white keys 25..31, black keys 37..41): visible in every
  // mode (behavior inherited); color follows the KEY family variant —
  // current variant while in KEYMODE, last latched KEY variant elsewhere
  if ((led >= 25 && led <= 31) || (led >= 37 && led <= 41)) {
    if (isTouched) return CRGB::White;
    uint8_t kv = (modeBase == MODE_KEY) ? modeVariant : familyMem[MODE_KEY];
    CRGB c;
    if (kv == 4) {
      // FastLED hue: 170=blue ... 213=magenta ... 255=red
      int8_t k = kbPitchIdx[led - 25];          // -1 for non-key LEDs in range
      uint8_t hue = (k >= 0) ? (uint8_t)(170 + (k * 85 + 5) / 11) : 170;
      c = CHSV(hue, 255, 255);
    } else if (kv == 2) {
      c = CRGB(60, 0, 255);                     // blue-violet
    } else {
      c = CRGB::DarkBlue;                       // KEYMODE / KEYMODE3
    }
    c.nscale8(KEY_DIM);
    return c;
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
  refreshAllLeds();   // keyboard/step pads follow the new mode via the hook
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
  modeBase = MODE_KEY;      // boot into KEYMODE
  modeVariant = 1;
  setLedColorHook(modeLedHook);
  applyModeLeds();
  headerSetLeft(modeName());
  refreshMainScreen();      // main screen shows the KEY binding from boot
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
      refreshMainScreen();   // labels/values belong to the restored binding
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
