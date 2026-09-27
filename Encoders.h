#pragma once

#include <Arduino.h>

#define NUM_ENCODERS 8

void initEncoders();
void pollEncoders();                              // call every loop
bool popEncEvent(uint8_t &idx, int16_t &delta);   // drain deltas
long encPosition(uint8_t idx);                    // current accumulated position

// ENCMODE3: MIDI channel of the encoder CCs (1..16, default 1); selected
// on the step-pad channel overlay (yellow theme like KEY3/SEQ3)
void encSetChannel(uint8_t ch);
uint8_t encChannel();
