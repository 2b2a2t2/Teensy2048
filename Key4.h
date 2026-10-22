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
void        k4GenreAdjust(int16_t counts);   // clamps at 0..GENRE_COUNT-1
uint8_t     k4Genre();
const char* k4GenreName();                   // full name (header / log)
const char* k4GenreShort();                  // <= 7 chars (OLED value column)

void    k4KnobAdjust(uint8_t idx, int16_t counts);  // 0..6 = MOOD..SURP
uint8_t k4Knob(uint8_t idx);

// --- embellish menu (NEXT tap: sticky row = EMB / PLACE) -------------------
// EMB shapes WHAT M3 writes: OFF = the plain generator, PASS = the odd
// steps become passing chords (one scale degree below the following main
// chord), PASS+SUB = also swaps main chords through the substitution table
// (I->iii, ii->IV, iii->I, IV->ii, V->bVII, vi->IV, bVII->V) with the SURP
// knob as the chance.  PLACE picks the passing rule: ODD = every odd step
// that qualifies, DIST = only when the bracketing main chords stand at
// least two scale degrees apart (the odd step keeps its main chord else).
void    k4EmbMenuSet(bool on);   // NEXT tap; also opens/closes the overlay
bool    k4EmbMenu();
void    k4EmbModeAdjust(int16_t counts);   // off/pass/p+s, wraps
uint8_t k4EmbMode();                      // 0 off, 1 pass, 2 pass+sub
const char* k4EmbModeName();              // "OFF" / "PASS" / "P+S"
void    k4EmbPlaceAdjust(int16_t counts); // CW = dist, CCW = odd
uint8_t k4EmbPlace();                     // 0 odd, 1 distance
const char* k4EmbPlaceName();             // "ODD" / "DIST"

// --- HARM settings (POLY TYPE=HARM page: ENC1 = GENRE, ENC2 = SRC, ENC3 = HOLD)
void    k4HarmInputAdjust(int16_t counts); // CW = arp, CCW = bass
bool    k4HarmInput();                     // false = bass (A), true = arp (B)
const char* k4HarmInputName();             // "BASS" / "ARP"
void    k4HarmHoldAdjust(int16_t counts);  // CW = on, CCW = off
bool    k4HarmHold();                      // force hold on repeated bass
const char* k4HarmHoldName();              // "OFF" / "ON"

// --- SKANK substitution source (POLY TYPE=SKANK page: ENC6 = SUB) ----------
// Sample the current genre's transition row from the degree nearest the
// sounding bass (a passing chord out of the same 18-genre grammar the
// HARM page and KEYMODE4 use) and return the sampled degree's root pitch
// class + diatonic triad quality.  false = nothing coherent to offer,
// the caller keeps the voicing it had.
bool    k4BorrowSample(uint8_t bassPc, uint8_t *outPc, uint8_t *outQ);

// --- embellishment encoders (PREV hold: EN5-8) -------------------------------
void    k4EmbSubProbAdjust(int16_t counts);  // EN5: substitution probability 0..100
uint8_t k4EmbSubProb();
void    k4EmbInvBiasAdjust(int16_t counts);  // EN6: inversion bias 0..3
uint8_t k4EmbInvBias();
void    k4EmbPassAmtAdjust(int16_t counts);  // EN7: passing chord amount 0..100
uint8_t k4EmbPassAmt();
void    k4EmbColorAmtAdjust(int16_t counts); // EN8: extensions/sus/strum 0..100
uint8_t k4EmbColorAmt();

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

// --- step hold (per-step encoder row) ---------------------------------------
// holding a step pad overlays ENC1..ENC4 onto OCT/DEG/INV/VOICE for THAT
// step; release restores the mode row (or the EMB menu if it was open).
// WRITE keeps the old hold behavior on top (the assign gesture)
void   k4StepSelSet(int8_t step);   // EV_HOLD sets it, EV_RELEASED clears (-1)
int8_t k4StepSelGet();
bool   k4StepLabels(uint8_t step, char *deg, size_t dcap,
                    char *inv, size_t icap);   // false = the step is empty

// chord root + quality from its tones (nearest-fit triad search): the
// header announcements use it, and the step-hold row's DEG transform and
// label read it too
uint8_t decodeChord(const uint8_t *nt, uint8_t n, uint8_t *root, uint8_t *q);

// --- gesture (Keyboard.cpp drives the pads) ---------------------------------
int  k4KeyOn(uint8_t kidx);     // press: audition the proposal (or -1)
void k4BucketFor(uint8_t kidx); // release: re-voice from that held key
void k4GestureEnd();            // last key up / mode exit
int  k4KeyCommitted();          // after press: write focus + advance
                                 // 0 = written, 1 = audition only, -1 = blocked
int  k4ApplyAll();              // M3 tap: generate every unlocked step
int  k4Regenerate();            // GENRE/knob turn: re-generate live (burst undo)
int  k4Variation();             // F1: reseed, then generate
int  k4Harm();                  // POLY TYPE=HARM page M3: inverse FOLLOW -
                                // harmonize the FOLLOW feeder's bassline/arp
                                // into chords baked into the HARM track
                                // (Viterbi over source-fit + genre; returns
                                // steps written, -1 refused)
int  k4HarmRebake();            // automatic re-bake after a feeder change:
                                // same inference, no undo snapshot, unchanged
                                // steps left untouched (K4 context untouched)
int  k4HarmStepChord(uint8_t step, uint8_t *notes);  // the chord HARM infers
                                // for ONE step (HARM track step activation with
                                // no held gesture): same Viterbi the bake uses;
                                // returns note count (0 = nothing to record)
uint8_t k4HarmLiveChord(uint8_t note, uint8_t *out);  // LIVE follow: one
                                // feeder note that actually PLAYED (post-
                                // probability) -> the plain root-position triad
                                // of its nearest scale degree, bake register;
                                // playback re-derives each active HARM step
                                // from it while FOLLOW is on (returns count)
int  k4Undo();                  // M3 hold: restore the last snapshot
void k4Stop();                  // leaving KEYMODE4
void k4Enter();                 // entering: seed from the engine's chord

// --- proposals (LED hook) ---------------------------------------------------
int8_t k4KeyRank(uint8_t pc);       // 0 = top pick, -1 = not proposed
bool   k4KeyBorrowed(uint8_t pc);   // proposed, but outside the scale

void k4Refresh();   // re-rank: knobs, genre, focus, scale or root changed
void k4Announce();  // header: persistent focus + top-4 proposals as "f3 I V vi IV"
void k4AnnouncePattern();  // header: written chords "I V vi IV +8"
