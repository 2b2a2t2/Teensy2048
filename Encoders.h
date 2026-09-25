#pragma once

#include <Arduino.h>

#define NUM_ENCODERS 8

void initEncoders();
void pollEncoders();                              // call every loop
bool popEncEvent(uint8_t &idx, int16_t &delta);   // drain deltas
long encPosition(uint8_t idx);                    // current accumulated position
