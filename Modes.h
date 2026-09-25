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
