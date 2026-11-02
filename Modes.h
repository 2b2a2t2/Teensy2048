#pragma once

#include <Arduino.h>
#include <FastLED.h>

// mode buttons = groupMode LEDs: 32=ENC, 33=SEQ, 34=KEY
enum ModeBase : uint8_t { MODE_NONE = 0, MODE_ENC, MODE_SEQ, MODE_KEY };

extern ModeBase modeBase;    // which family
extern uint8_t modeVariant;  // 0=none, 1..4

void initModes();                    // installs LED color hook
void pollModeLeds();                 // call every loop: pulse/blink animation
void modeOnEvent(uint8_t pad, uint8_t ev);  // EV_CLICKED / EV_HOLD / EV_DOUBLE
const char* modeName();              // "---", "ENC", "ENC2", "ENC3", "ENC4"...
// the OLED left-slot spelling of the same state: "ENCODER 1" / "SEQUENCER 2"
// / "CHANNEL SELECT" (variant 3) - whole words, never the internal code
const char* modeHeaderLabel();

// which KEY variant defines the keyboard behavior: the current one while
// in KEYMODE, the last latched KEY variant elsewhere (KEYMODE2 = chord
// mode is inherited by SEQMODE/ENCMODE)
uint8_t keyBehaviorVariant();

// per-channel KEYMODE memory: each keyboard channel recalls the KEY
// variant last latched on it (RAM only).  keySetChannel() calls
// keyModeRecall(); modeOnEvent() stores on every latched variant change
void keyModeRecall();
uint8_t keyModeMemOf(uint8_t ch);        // EEPROM accessors (1..16)
void    keyModeMemSet(uint8_t ch, uint8_t v);

// true while the keyboard sounds through the chord engine: KEYMODE2
// (diatonic chords) and KEYMODE4 (genre proposals) share it for the sound,
// the pending snapshot and the MONO voice - the latched variant elsewhere
// inherits the same way keyBehaviorVariant() does
bool keyChordMode();

// SEQMODE3 step-pad channel overlay: the 16 steps become the edit-channel
// selector while it is up.  Holding SEQ into SEQMODE3 brings it back; any
// encoder movement dismisses it for the rest of the hold so the pads show
// the sequencer again (playhead, recorded/empty, pattern length)
bool seq3ChSelActive();
void seq3ChSelEnter();               // SEQ held into SEQMODE3
bool seq3ChSelDismiss();         // encoder moved: true if it was up
// Is the step-pad CHANNEL PICKER up right now?  Variant 3 in all three
// families (momentary hold) AND ENCMODE4, where the picker is the locked
// screen: double-clicking ENC puts the pads on channel select and they stay
// there until the mode changes, so the CC channel can be picked without
// holding three buttons at once.  One predicate because four places must
// agree (the press handler, the LED row, the playhead and the header label)
bool chSelOverlay();
// the keyboard's pads stay on the SEQ3 bar / verb row for the track a
// bar or verb was pressed on, in every mode, until KEYMODE3 picks the
// keyboard's own track again (seq3KbRelease)
bool seq3KbRole();
void seq3KbLatch(uint8_t ch);
void seq3KbRelease();
