#pragma once

#include <Arduino.h>

// drain input event queues each loop.
// capped at MAX_EVENTS_PER_LOOP per call so loop() can never be starved
// (queues are also fixed-size rings, so drains are bounded anyway).
#define MAX_EVENTS_PER_LOOP 8

void handleButtonEvents();
void handleEncoderEvents();

// ENCMODE: current CC value (0..127) of encoder 0..7 (sends CC 1..8) on
// the CURRENT encoder channel (values are stored per channel)
uint8_t encCcValue(uint8_t idx);

// incoming CC feedback from the DAW: syncs the ENCMODE knob positions
// (channel 1..16, CC 1..8); store only, never re-sent
void encCcIncoming(uint8_t ch, uint8_t cc, uint8_t val);

// ENCMODE4 + REC: persist the 128 knob values + selected channel to
// EEPROM; encCcLoad() restores them at boot (before the first draw).
// Save only - nothing is ever pushed to the DAW
void encCcSave();
void encCcLoad();

// SEQMODE step-edit radio (variants 1/2/4): is the edit on, which target
// is selected (0=STEPS, 1=TIE, 2=GLIDE, 3=ACCENT, 4=PROB), the current
// ENC8 probability clipboard (0..100, boot 100), and whether an attribute
// target is engaged for the current track (MONO = 1..4, DRUM = ACCENT/
// PROB only)
bool    stepEditIsOn();
uint8_t stepEditSel();
uint8_t stepProbValue();
void    stepProbSet(uint8_t v);      // clamped 0..100 (EEPROM load)
bool    stepEditActive();

// HOLD F3 in SEQMODE (variants 1/2/4): ENC6 edits the track's ACCENT
// velocity instead of its default velocity (release restores ENC6)
void    f3AccentHold(bool on);
bool    f3AccentActive();
