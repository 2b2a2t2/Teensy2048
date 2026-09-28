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
uint8_t seqTrack();                 // track type of the edit sequence
const char* seqTrackName();         // "DRUM" / "MONO" / "POLY"
uint8_t seqTrackOf(uint8_t idx);    // track type of sequence idx 0..15

// SEQMODE3 TYPE (ENC7): mono transform of a MONO sequence, applied at
// playback (steps keep storing the full incoming chord).
// ROOT = chord-root detection (template based, fallback lowest note),
// BASS = lowest note, ARP = chord notes ascending, 1 note per playhead
// step, wraps; counter resets on transport stop.  Default ROOT everywhere.
enum SeqMonoType : uint8_t { SEQ_MONO_ROOT = 0, SEQ_MONO_BASS = 1, SEQ_MONO_ARP = 2 };
void seqMonoTypeAdjust(int16_t counts);  // edit channel; 4 counts = 1 step, no wrap
uint8_t seqMonoType();                   // edit channel
const char* seqMonoTypeName();           // "ROOT" / "BASS" / "ARP"
uint8_t seqMonoTypeOf(uint8_t idx);      // sequence idx 0..15
void seqDiagNote(uint8_t section, uint32_t us);  // track worst loop stalls; 1Hz report
