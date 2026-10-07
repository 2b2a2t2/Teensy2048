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
// is selected (0=STEPS, 1=TIE, 2=GLIDE, 3=ACCENT, 4=PROB; POLY adds
// 5=FX on F1 and 6=TYPE on F2), and whether an attribute target is engaged
// for the current track (MONO = 1..4, POLY = 3/4/5/6, DRUM = ACCENT/PROB
// only).  The probability % itself is per track (ENC8 -> seqProbAdjust /
// seqProbPct in Keyboard.h)
bool    stepEditIsOn();
uint8_t stepEditSel();
bool    stepEditActive();
uint8_t stepFxSel();    // selected FX type for the FX target (StepFxType,
                        // 0..6 - which FX the pads/ENC7 edit)
const char* stepFxSelName();  // its display name (sus2 ... color)
// TYPE arm of the F2 target: which value a pad tap stamps onto the step
// (0..2 = STEP/STRUM/HAND; tap again clears back to inherit-the-track).
// ENC7 cycles it while target 6 is engaged, like ENC7 picks the FX type
void    stepTypeArmAdjust(int16_t counts);
uint8_t stepTypeArm();
const char* stepTypeArmName();   // "STEP" / "STRUM" / "HAND"

// HOLD F3 in SEQMODE (variants 1/2/4): ENC6 edits the track's ACCENT
// velocity instead of its default velocity (release restores ENC6)
void    f3AccentHold(bool on);
bool    f3AccentActive();

// SEQMODE M2 (variants 1/2/4) on a MONO+ARP edit track: a click toggles
// the ARP encoder page - ENC2 = span, ENC3 = skip rests, ENC4 = mode,
// ENC7 = insert mode; ENC1/5/6/8 keep their knobs.  The page only reads
// active while the panel is on SEQMODE and the track still wants an arp;
// a channel switch, a TYPE change or leaving SEQMODE dismisses it
void arpPageToggle();
void arpPageDismiss();
bool arpPageActive();
