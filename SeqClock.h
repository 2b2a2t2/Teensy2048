#pragma once

#include <Arduino.h>

// USB-MIDI slave clock (uClock EXTERNAL_CLOCK, 24 PPQN input).
// Advances a playhead 0..15 on every 16th note (setOnStep).

void initSeqClock();        // uClock config + usbMIDI handlers; call once in setup
void seqPumpMidi();         // drain USB MIDI (clock/start/stop); bounded, call often
void pollSeqClock();        // pump + redraw playhead; call every loop
uint8_t seqCurrentStep();   // playhead 0..15 (safe to read from loop)
bool seqIsRunning();
void seqSendStart();        // emit USB-MIDI System Real-Time Start (0xFA)
                            // (+ starts internal transport when SYNC=INT)
void seqTogglePlay();       // PLAY: start, or stop when INT transport runs

// SEQMODE3 sync settings: INT = Teensy is clock master (streams Clock+Start
// out at tempoBpm); EXT = follow incoming USB-MIDI clock (original behavior)
void seqSetSyncInternal(bool on);
bool seqIsSyncInternal();
void seqSetTempo(uint16_t bpm);     // clamped 40..240
uint16_t seqTempo();

// SEQMODE3 TRACK (ENC6): track type of the edit sequence — DRUM / MONO /
// POLY, clamped, never wraps.  Per channel: channel 1 defaults to DRUM,
// channels 2..16 to POLY.  4 encoder counts = 1 step (residual kept).
enum SeqTrackType : uint8_t { SEQ_TRACK_DRUM = 0, SEQ_TRACK_MONO = 1, SEQ_TRACK_POLY = 2 };
void seqTrackAdjust(int16_t counts);
uint8_t seqTrack();                 // 0..2
const char* seqTrackName();         // "DRUM" / "MONO" / "POLY"
void seqDiagNote(uint8_t section, uint32_t us);  // track worst loop stalls; 1Hz report
