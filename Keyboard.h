#pragma once

#include <Arduino.h>

// KEYMODE keyboard pads -> USB-MIDI notes with octave control (ENC1).
// Octave range -2..8, clamped (never wraps).
// note = 12*octave + 24 + semitone  (octave 3 -> C3 = 60 = middle C)
// Keys are indexed 0..11 in groupKeyboard order (C, C#, D ... A#, B).

int  keyPadPress(uint8_t idx);       // returns note sent, or -1
int  keyPadRelease(uint8_t idx);     // returns note released, or -1
void keyOctaveAdjust(int16_t counts); // 4 encoder counts = 1 octave step
int  keyOctave();                    // current octave (-2..8)

// keyboard MIDI output channel (1..16); changing it first cuts every note
// sounding on the old channel (no stuck notes), held keys re-strike on the
// new channel.  The keyboard channel also SELECTS which sequence the
// keyboard feeds: plays on channel N update sequence N's pending chord.
void keySetChannel(uint8_t ch);
uint8_t keyChannel();

// sequencer edit channel (1..16): the playhead plays EVERY channel's
// sequence at once (each on its own MIDI channel); this selects which
// channel the step pads display and edit.  Focus switch only - running
// sequences are never cut.
void seqSetEditChannel(uint8_t ch);
uint8_t seqEditChannel();

// input (listen) channel of the edit sequence (1..16): notes arriving
// there feed THAT sequence's pending chord - every sequence listening to
// the channel is fed at once (no edit-focus gating).  Default sequence N
// listens to channel N.  ENC5 in SEQMODE3 adjusts it (4 counts = 1 step,
// clamped, no wrap)
void seqInputChannelAdjust(int16_t counts);
uint8_t seqInputChannel();

// incoming USB-MIDI notes -> feed the pending chord of every sequence
// whose input channel matches, and the step held in assign mode
void extNoteOn(uint8_t ch, uint8_t note);
void extNoteOff(uint8_t ch, uint8_t note);

// DRUM track: per-voice sequencing.  Parts = 16 drum notes C1(36)..D#2(51);
// part-select overlay pad N = note 36+N (pad0 = C1, the GM kick).  Press M1
// in SEQMODE (TRACK=DRUM) for the overlay (dim orange, selected part bright,
// shows on press = instant); pressing a step there selects the part.  In the
// normal step view a press toggles the selected part on/off at that step
// (the playhead plays all of them).
void drumPartSelectHold(bool on);     // M1 held / released
bool drumPartSelectActive();
void drumPartSet(uint8_t idx);        // overlay pad 0..15
uint8_t drumPartNote();
const char* drumPartName();           // "C0" ... "D#1"
int stepDrumToggle(uint8_t step);     // -2 = part removed, -1 = no room/invalid,
                                      // else = part note added

// Step pads (0..15):
//  press   = unassigned: SILENTLY record the sequence's pending chord
//            (fed by its input channel and by the keyboard when the
//            keyboard channel selects it); sounds only when the playhead
//            reads it during playback; active: silent candidate ->
//            disabled on click
//  hold    = assign mode: keyboard keys played while held REPLACE that
//            step's chord (first key starts the new chord)
int  stepChordPress(uint8_t step);   // notes recorded; -1 = silent (active/bad)
int  stepChordClick(uint8_t step);   // EV_CLICKED: 0 = step disabled
void stepAssignSet(int8_t step);     // step = 0..15 to arm, -1 to disarm

// sequencing (called from loop context, never from the clock ISR):
// press on an unassigned step records the last keyboard chord into it;
// seqStepAdvance runs each time the playhead reaches a step
bool stepIsActive(uint8_t step);     // step has a recorded chord
void seqStepAdvance(uint8_t step);   // release previous step, play this one
void seqStepStop();                  // transport stopped: release notes
