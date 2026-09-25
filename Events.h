#pragma once

#include <Arduino.h>

// drain input event queues each loop.
// capped at MAX_EVENTS_PER_LOOP per call so loop() can never be starved
// (queues are also fixed-size rings, so drains are bounded anyway).
#define MAX_EVENTS_PER_LOOP 8

void handleButtonEvents();
void handleEncoderEvents();
