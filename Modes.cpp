#include "Modes.h"
#include "Buttons.h"
#include "Display.h"
#include "SeqClock.h"
#include "Keyboard.h"
#include "Encoders.h"
#include "Bindings.h"
#include "Chords.h"
#include "Events.h"

ModeBase modeBase = MODE_NONE;
uint8_t modeVariant = 0;

// hold (variant 3) is momentary: remember what to restore on release
static ModeBase heldPrevBase = MODE_NONE;
static uint8_t heldPrevVariant = 0;
static bool heldActive = false;
static uint8_t heldPad = 255;        // which mode button is being held

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

// playback blink phase: set by pollModeLeds(), read by modeLedHook() -
// while the playhead sounds notes on the keyboard channel, their pitch
// classes flash dim green on the keys (blink wins over the idle colors)
static bool kbBlinkOn = false;

// same driver for the F-row: while the playhead reads an attributed step
// of the MONO edit channel, the F1-F4 LEDs flash dim green
static bool fBlinkOn = false;

// is the playhead currently playing this pitch class on the keyboard channel?
static bool kbSeqPlays(uint8_t pc) {
  uint8_t n = seqSoundingCount(keyChannel());
  pc %= 12;
  for (uint8_t i = 0; i < n; i++)
    if (seqSoundingNote(keyChannel(), i) % 12 == pc) return true;
  return false;
}

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
    // ENC mute overlay (hold M2): pad N = output channel N+1, dim
    // orange-red = playing, bright orange-red = muted (wins over the
    // variant-3 channel selector while the overlay is up)
    if (encMuteActive()) {
      CRGB c = CRGB::OrangeRed;
      if (!seqIsMuted((uint8_t)led)) c.nscale8(32);
      return c;
    }
    // ENCMODE drum-voice mute (hold M1): pad N = drum note 36+N, dim
    // orange-red = voice playing, bright = muted (all drum channels)
    if (drumVoiceMuteActive()) {
      CRGB c = CRGB::OrangeRed;
      if (!drumVoiceMuted((uint8_t)(36 + led))) c.nscale8(32);
      return c;
    }
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
    {
      // playhead of the edit window (the pads show one 16-step bar at a
      // time; the sequence itself may run up to 64 steps): green when the
      // bar on the pads is the one being read, dark-blue ghost at the same
      // beat position while playback runs in another bar
      uint8_t ph = seqStepPlayPad();
      if (ph < 16 && led == groupSteps[ph]) return CRGB::Green;
      uint8_t gh = seqStepPlayGhost();
      if (gh < 16 && led == groupSteps[gh]) return CRGB::DarkBlue;
    }
    CRGB c;
    uint8_t vst = (uint8_t)(led + seqStepViewBase());
    if (stepIsActive(vst)) {
      // selected-attribute pulse: while an attribute target is engaged,
      // the steps carrying it breathe YELLOW instead of dim static green
      if (stepEditActive() &&
          (stepAttrs(vst) &
           (uint8_t)(1 << (stepEditSel() - 1)))) {
        uint8_t s = sin8((uint32_t)millis() * 256UL / PULSE_PERIOD_MS);
        uint8_t b = 48 + scale8(s, 120);           // 48..168 breathing
        c = CRGB::Yellow;
        c.nscale8(b);
      } else {
        c = CRGB::Green;
        c.nscale8(64);
      }
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
    // SEQMODE3: the keyboard is not an instrument here - C D E F become
    // the pattern bars (C = steps 1-16 ... F = 49-64): dim blue background
    // with the EDITED bar bright; the bar the playhead reads breathes
    // white (follows playback across bars, patterns > 16 steps only) and a
    // looping bar breathes green - the keys still play, just invisible
    if (modeBase == MODE_SEQ && modeVariant == 3) {
      if (isTouched) return CRGB::White;
      int8_t bar = (led == 25) ? 0 : (led == 26) ? 1 :
                   (led == 27) ? 2 : (led == 28) ? 3 : -1;
      if (bar < 0) return CRGB::Black;
      uint8_t s = sin8((uint32_t)millis() * 256UL / PULSE_PERIOD_MS);
      if (seqBarLooped((uint8_t)bar)) {
        CRGB g = CRGB::Green;         // looping bar: breathing green
        g.nscale8((uint8_t)(48 + scale8(s, 192)));
        return g;
      }
      // one-bar patterns never cross a bar edge: keep the LED static
      uint8_t pb = (seqPatLen() > 16) ? seqPlayBar() : 0xFF;
      if (bar == (int8_t)pb && seqIsRunning()) {
        CRGB w = CRGB::White;         // played bar: breathing white
        w.nscale8((uint8_t)(48 + scale8(s, 192)));
        return w;
      }
      if (bar == (int8_t)seqStepPage())
        return CRGB::DarkBlue;        // edited bar: full bright
      CRGB c = CRGB::DarkBlue;        // the other bars recede
      c.nscale8(32);
      return c;
    }
    int8_t k = kbPitchIdx[led - 25];          // -1 for non-key LEDs in range
    uint8_t kv = keyBehaviorVariant();
    bool inScale = (keyScale() == 0) || keyIsInScale((uint8_t)k);
    if (isTouched) {
      // a key the scale filter silences flashes red (chord mode / KeyLock
      // play it, so they flash white)
      if (kv != 2 && !chordKeyLock() && !inScale) return CRGB(40, 0, 0);
      return CRGB::White;
    }
    // sequence playing on the keyboard channel: those pitch classes blink
    // dim green - wins over the chord tint and the scale (playback feedback)
    if (kbBlinkOn && kbSeqPlays((uint8_t)k)) {
      CRGB g = CRGB::Green;
      g.nscale8(64);
      return g;
    }
    // sounding chord keys tint cyan (they win over the scale's dark keys)
    if (kv == 2 && chordPlaysPitch((uint8_t)k)) return CRGB(0, 90, 160);
    if (!inScale) return CRGB::Black;         // scale display: dead keys dark
    CRGB c;
    if (kv == 4) {
      // FastLED hue: 170=blue ... 213=magenta ... 255=red
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
  // SEQMODE step-edit radio: M1 + F1-F4 (MONO) or M1 + F3/F4 (DRUM) keep
  // their mode visible at all times - bright white = the target the pads
  // currently edit (M1 while no attribute is selected), dim dark blue
  // background otherwise.  Touch wins over everything
  bool seqMonoRadio = (modeBase == MODE_SEQ && modeVariant != 3 &&
                       seqTrack() == SEQ_TRACK_MONO);
  bool seqDrumRadio = (modeBase == MODE_SEQ && modeVariant != 3 &&
                       seqTrack() == SEQ_TRACK_DRUM);
  if ((seqMonoRadio &&
       (led == groupControls[2] ||
        (led >= groupControls[5] && led <= groupControls[8]))) ||
      (seqDrumRadio && (led == groupControls[2] ||
                        led == groupControls[5] + 2 ||
                        led == groupControls[5] + 3))) {
    if (isTouched) return CRGB::White;
    if ((seqMonoRadio || stepEditActive()) &&
        led >= groupControls[5] && fBlinkOn &&
        (seqFMask() & (uint8_t)(1 << (led - groupControls[5])))) {
      CRGB g = CRGB::Green;
      g.nscale8(64);
      return g;
    }
    if ((stepEditSel() == 0 && led == groupControls[2]) ||
        (stepEditSel() != 0 &&
         stepEditSel() == (uint8_t)(led - groupControls[5] + 1))) {
      CRGB s = CRGB::White;
      s.nscale8(80);
      return s;
    }
    CRGB d = CRGB::DarkBlue;
    d.nscale8(32);
    return d;
  }
  // ENCMODE function pads: always show their identity colors even when
  // idle - M1 dim orange, M2 dim orange-red (touch still wins)
  if (modeBase == MODE_ENC &&
      (led == groupControls[2] || led == groupControls[3])) {
    if (isTouched) return CRGB::White;
    CRGB c = (led == groupControls[2]) ? CRGB::Orange : CRGB::OrangeRed;
    c.nscale8(48);
    return c;
  }
  // PLAY transport pad: dim dark green = stopped, bright green = running
  // (touch always wins)
  if (led == groupTransport[0]) {
    if (isTouched) return CRGB::White;
    if (seqIsRunning()) return CRGB::Green;
    CRGB g = CRGB::Green;
    g.nscale8(48);
    return g;
  }
  // REC transport pad: dim dark red = idle, bright red = live record armed
  // / running (a touch always wins)
  if (led == groupTransport[1]) {
    if (isTouched) return CRGB::White;
    if (seqRecState()) return CRGB::Red;
    CRGB r = CRGB::Red;
    r.nscale8(48);
    return r;
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

  // playback blink driver: flash the keys the playhead sounds on the
  // keyboard channel (edge on/off repaints immediately - a finished chord
  // never keeps its green; the phase flip drives the flashing)
  static bool kbActive = false;
  static unsigned long kbFlip = 0;
  bool kbNow = (seqSoundingCount(keyChannel()) > 0);
  if (kbNow != kbActive) {
    kbActive = kbNow;
    if (!kbActive) kbBlinkOn = false;
    refreshAllLeds();
  }
  if (kbActive && (long)(millis() - kbFlip) >= 0) {
    kbFlip = millis() + BLINK_MS;
    kbBlinkOn = !kbBlinkOn;
    refreshAllLeds();
  }

  // F-row attribute blink driver (same cadence as the keyboard blink):
  // mask comes from the playhead, edge on/off repaints immediately so a
  // finished attributed step never keeps its green
  static bool fActive = false;
  static unsigned long fFlip = 0;
  bool fNow = (seqFMask() != 0);
  if (fNow != fActive) {
    fActive = fNow;
    if (!fActive) fBlinkOn = false;
    refreshAllLeds();
  }
  if (fActive && (long)(millis() - fFlip) >= 0) {
    fFlip = millis() + BLINK_MS;
    fBlinkOn = !fBlinkOn;
    refreshAllLeds();
  }

  // live-record edge: repaint the REC LED on every state change and show
  // the one-shot message when a record pass finished by itself
  static uint8_t recLast = 0;
  uint8_t recNow = seqRecState();
  if (recNow != recLast) {
    recLast = recNow;
    refreshAllLeds();
  }
  if (seqRecTakeDone()) headerShow("rec done");

  // transport edge: repaint the PLAY LED on every start/stop (button,
  // external clock, INT transport alike)
  static bool playLast = false;
  bool playNow = seqIsRunning();
  if (playNow != playLast) {
    playLast = playNow;
    refreshAllLeds();
  }

  // attribute-step pulse driver: while an attribute target is engaged,
  // repaint at ~30 fps so the marked steps breathe (edge restores static
  // colors when the radio closes or the target goes back to STEPS)
  static bool attrActive = false;
  static unsigned long attrFlip = 0;
  bool attrNow = stepEditActive();
  if (attrNow != attrActive) {
    attrActive = attrNow;
    refreshAllLeds();
  }
  if (attrActive && (long)(millis() - attrFlip) >= 0) {
    attrFlip = millis() + 30;
    refreshAllLeds();
  }

  // SEQMODE3 pattern-bar pulse: while SEQ3 is up, repaint at ~30 fps so
  // the played / looping bars breathe (edge restores static colors)
  static bool barActive = false;
  static unsigned long barFlip = 0;
  bool barNow = (modeBase == MODE_SEQ && modeVariant == 3);
  if (barNow != barActive) {
    barActive = barNow;
    refreshAllLeds();
  }
  if (barActive && (long)(millis() - barFlip) >= 0) {
    barFlip = millis() + 30;
    refreshAllLeds();
  }

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

uint8_t keyBehaviorVariant() {
  if (modeBase == MODE_KEY) return modeVariant;
  return familyMem[MODE_KEY];
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
    // releasing the held mode button ends the hold.  ENCMODE3 always exits
    // INTO ENCMODE: a hold started inside the ENCMODE family restores that
    // variant, a hold started anywhere else lands on ENCMODE variant 1
    if (heldActive && pad == heldPad) {
      heldActive = false;
      if (modeBase == b && modeVariant == 3) {
        if (b == MODE_ENC) {
          modeBase = MODE_ENC;
          modeVariant = (heldPrevBase == MODE_ENC) ? heldPrevVariant : 1;
        } else {
          modeBase = heldPrevBase;         // SEQ3 / KEY3: back where we were
          modeVariant = heldPrevVariant;
        }
        applyModeLeds();
        headerSetLeft(modeName());
        refreshMainScreen();   // labels/values belong to the restored binding
      }
    }
    return;
  }

  if (ev == EV_HOLD) {
    if (!heldActive) {
      heldPrevBase = modeBase;
      heldPrevVariant = modeVariant;
      heldActive = true;
      heldPad = pad;
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

  // leaving KEYMODE2 = leaving chord mode: silence it; the PREV overlay is
  // momentary - it never outlives the binding it remaps
  if (keyBehaviorVariant() != 2) chordStopAll();
  clearOverlay(G_ENC);
  encMuteHold(false);       // the M2 mute overlay never outlives the ENC mode
  drumMuteHold(false);      // nor the M1 drum-voice mute overlay

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
