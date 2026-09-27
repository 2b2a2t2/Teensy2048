#pragma once

#include <Arduino.h>

// drain input event queues each loop.
// capped at MAX_EVENTS_PER_LOOP per call so loop() can never be starved
// (queues are also fixed-size rings, so drains are bounded anyway).
#define MAX_EVENTS_PER_LOOP 8

void handleButtonEvents();
void handleEncoderEvents();

// ENCMODE: current CC value (0..127) of encoder 0..7 (sends CC 1..8)
uint8_t encCcValue(uint8_t idx);
