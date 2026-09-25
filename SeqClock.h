#pragma once

#include <Arduino.h>

// USB-MIDI slave clock (uClock EXTERNAL_CLOCK, 24 PPQN input).
// Advances a playhead 0..15 on every 16th note (setOnStep).

void initSeqClock();        // uClock config + usbMIDI handlers; call once in setup
void seqPumpMidi();         // drain USB MIDI (clock/start/stop); bounded, call often
void pollSeqClock();        // pump + redraw playhead; call every loop
uint8_t seqCurrentStep();   // playhead 0..15 (safe to read from loop)
bool seqIsRunning();
void seqDiagNote(uint8_t section, uint32_t us);  // track worst loop stalls; 1Hz report
