#pragma once

#include <Arduino.h>

// KEYMODE4: genre-driven chord proposals (double-click the KEY button).
//
// The 8 context encoders shape WHAT the machine suggests: GENRE picks the
// transition/quality/inversion tables of genre_data.h, the seven knobs
// (MOOD/TENS/MOTION/CAD/CPX/ERA/SURP, 0..100, boot 50) bias the scoring.
// Holding PREV overlays the encoder row onto Oct/Scale/Root.
//
// The keyboard lays out the whole scored degree set: hue-mapped by key as
// originally designed (C=blue ... B=red), rank 0 (the next chord from the
// highest probability) hottest and brightness stepping down the list, so
// the colors tell how probable each degree is after the last chord;
// borrowed degrees light orange when the TENS knob opens them.  Pressing
// a proposed key auditions the chord through the chord engine and CHAINS
// the progression: the next proposals follow that chord, so the keyboard
// walks a sequence by ear even in AUDITION (nothing written).  Entering
// from KEYMODE2 seeds the walk with the chord played there; the header
// shows it all: "f3 V vi > I IV" (focus, walk, top-4 proposals) and
// "I V vi IV +8" (the written pattern) after M3/F1/undo.  With WRITE on
// the chord also lands on the FOCUSED step (lit red) and the focus
// auto-advances (M2); M1 itself is dim red in AUDITION, bright red in
// WRITE.  M3 applies the generator to every unlocked step, M3 held undoes
// the last write, F1 reseeds.  Nothing persists across a reboot (RAM
// only, by design).

// --- context encoders (main row) ------------------------------------------
void        k4GenreAdjust(int16_t counts);   // wraps 0..GENRE_COUNT-1
uint8_t     k4Genre();
const char* k4GenreName();                   // full name (header / log)
const char* k4GenreShort();                  // <= 7 chars (OLED value column)

void    k4KnobAdjust(uint8_t idx, int16_t counts);  // 0..6 = MOOD..SURP
uint8_t k4Knob(uint8_t idx);

// --- control buttons --------------------------------------------------------
bool k4WriteToggle();      // M1: key presses land on the focused step
bool k4Write();
bool k4AutoToggle();       // M2: focus advances after every write
bool k4Auto();
bool k4LeadToggle();       // F3: inversions choose minimal bass movement
bool k4Lead();
void k4HoldSet(bool on);   // F4 held: audition only (no write, no advance)
bool k4Hold();

// --- focus and locks --------------------------------------------------------
void    k4FocusSet(uint8_t step);   // step pad press: focus + window follow
void    k4FocusStep(int8_t delta);  // PREV/NEXT: wraps inside the pattern
uint8_t k4Focus();
bool    k4Locked(uint8_t step);
bool    k4LockToggle(uint8_t step); // F2: lock/unlock a step (apply skips it)

// --- gesture (Keyboard.cpp drives the pads) ---------------------------------
int  k4KeyOn(uint8_t kidx);     // press: audition the proposal (or -1)
void k4BucketFor(uint8_t kidx); // release: re-voice from that held key
void k4GestureEnd();            // last key up / mode exit
int  k4KeyCommitted();          // after press: write focus + advance
                                 // 0 = written, 1 = audition only, -1 = blocked
int  k4ApplyAll();              // M3 tap: generate every unlocked step
int  k4Variation();             // F1: reseed, then generate
int  k4Undo();                  // M3 hold: restore the last snapshot
void k4Stop();                  // leaving KEYMODE4
void k4Enter();                 // entering: seed from the engine's chord

// --- proposals (LED hook) ---------------------------------------------------
int8_t k4KeyRank(uint8_t pc);       // 0 = top pick, -1 = not proposed
bool   k4KeyBorrowed(uint8_t pc);   // proposed, but outside the scale

void k4Refresh();   // re-rank: knobs, genre, focus, scale or root changed
void k4Announce();  // header: focus + top-4 proposals as "f3 I V vi IV"
void k4AnnouncePattern();  // header: written chords "I V vi IV +8"
