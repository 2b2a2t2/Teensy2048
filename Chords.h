#pragma once

#include <Arduino.h>

// KEYMODE scale state (shared by all KEY variants) and the KEYMODE2 chord
// mode - a port of TeensikDiM's chordmode: quality/extension buttons,
// diatonic auto-chords, voicing/bass/spread/density, latch, KeyLock.
// The keyboard (Keyboard.cpp) owns the pads; this module owns the sound.

#define KEY_SCALE_COUNT 39

// momentary quality buttons (M1/M2/M3/NEXT) - CHORD_Q_NONE on release
#define CHORD_Q_NONE 0
#define CHORD_Q_MAJ  1
#define CHORD_Q_MIN  2
#define CHORD_Q_DIM  3
#define CHORD_Q_SUS  4

// extension buttons F1..F4 (bitmask, held together)
#define CHORD_EXT_6   0x01
#define CHORD_EXT_m7  0x02
#define CHORD_EXT_M7  0x04
#define CHORD_EXT_9   0x08

// --- scale (ENC2 in every KEY variant; root in KEYMODE2 / the PREV row) --
void        keyScaleAdjust(int16_t counts);  // 4 counts = 1 scale, clamp, no wrap
int         keyScale();                      // 0 = Chrom (no filter, no LED change)
const char* keyScaleName();
const char* keyScaleMood();                  // 1-word mood for the header
void        keyRootAdjust(int16_t counts);   // 4 counts = 1 root, clamp, no wrap
int         keyRoot();                       // 0 = C
const char* keyRootName();
bool        keyIsInScale(uint8_t kidx);      // key 0..11 in scale rooted at keyRoot

// --- chord engine (KEYMODE2) ---------------------------------------------
// press/release come from Keyboard.cpp (the root note = the pad's note)
int     chordPadPress(uint8_t kidx, uint8_t note);   // returns the root played
int     chordPadRelease(uint8_t kidx);               // -1 = key was not held

void    chordSetQuality(uint8_t q);          // press sets, release -> NONE
void    chordSetExt(uint8_t ext, bool on);   // press sets, release clears
void    chordParamAdjust(uint8_t idx, int16_t counts);  // 0..3 = voic/bass/sprd/dens
uint8_t chordParam(uint8_t idx);             // 0..3
void    chordLatchAdjust(int16_t counts);    // CW = latch on, CCW = off
void    chordKeyLockAdjust(int16_t counts);  // CW = KeyLock on, CCW = off
void    chordPerfAdjust(int16_t counts);     // CW = Perf on, CCW = off (state only)
bool    chordLatch();
bool    chordKeyLock();
bool    chordPerf();

const uint8_t* chordNotes();   // notes sounding through the engine
uint8_t        chordCount();
bool           chordSounding();
bool           chordPlaysPitch(uint8_t pc);  // pitch class currently sounding
const char*    chordName();    // "GMaj7" for the header ("" before first chord)

// lifecycle (keyboard output channel / octave / mode changes)
void chordShift(int semitones);  // octave shift of the held/sounding chord
void chordCut();                 // note-offs on the current channel
void chordRestore();             // re-send what chordCut() silenced
void chordStopAll();             // silence + reset gesture (leave KEYMODE2)
