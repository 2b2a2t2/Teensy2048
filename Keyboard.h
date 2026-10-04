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

// called by the chord engine (Chords.cpp) whenever the sounding chord's
// notes change: re-snap the pending chord of every sequence the keyboard
// channel reaches (chords feed the sequencer like single notes)
void keyPendingRefresh();

// keyboard MIDI output channel (1..16); changing it first cuts every note
// sounding on the old channel (no stuck notes), held keys re-strike on the
// new channel.  The keyboard channel also SELECTS which sequence the
// keyboard feeds: plays on channel N update sequence N's pending chord.
// When that channel's TRACK is MONO the live keyboard is processed by it:
// ROOT = root note, BASS = lowest, ARP = next chord note per playhead step
// (transport-gated), raw notes suppressed - recording still sees the chord
void keySetChannel(uint8_t ch);
uint8_t keyChannel();
void keyMonoPoll();               // reconcile the live voice (loop context)

// sequencer edit channel (1..16): the playhead plays EVERY channel's
// sequence at once (each on its own MIDI channel); this selects which
// channel the step pads display and edit.  Focus switch only - running
// sequences are never cut.
void seqSetEditChannel(uint8_t ch);
uint8_t seqEditChannel();

// input (listen) channel of the edit sequence (1..16): notes arriving
// there feed THAT sequence's pending chord - every sequence listening to
// the channel is fed at once (no edit-focus gating).  Default sequence N
// listens to channel N.  Changing the input channel BACKFILLS the pending
// chord from that channel's track (record on ch2, point ch3 at ch2, ch3
// can activate immediately).  ENC5 in SEQMODE3 adjusts it (4 counts = 1
// step, clamped, no wrap)
void seqInputChannelAdjust(int16_t counts);
uint8_t seqInputChannel();

// octave shift of the SELECTED sequence's playback (ENC1 in SEQMODE, all
// variants): +1 = one octave up, applied by the playhead only - stored
// notes stay raw so the shift is live-changeable.  Clamped so shifted
// notes fit the -2..8 octave convention; the range is recalculated from
// the stored notes (notes at C3 -> -5..+5, empty -> keyboard-octave
// reference), 4 counts = 1 octave, no wrap
void seqOctaveShiftAdjust(int16_t counts);
int  seqOctaveShift();                // edit sequence's shift (for the OLED)

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

// ENCMODE + hold M1 = drum-voice mute: step pad N toggles the mute of
// drum note 36+N (C1..D#2) across EVERY drum-track sequence (state
// persists; playback skips the muted voice's stored note, MONO/POLY
// sequences are never affected)
void drumMuteHold(bool on);           // M1 held / released
bool drumVoiceMuteActive();           // overlay up (flag + ENCMODE)
bool drumVoiceMuted(uint8_t note);    // drum note 36..51
void drumVoiceMuteToggle(uint8_t note);
const char* drumVoiceName(uint8_t note);  // "C1" .. "D#2"

// ENC mode output mute: hold M2 = overlay where step pad N toggles the mute
// of output channel N+1 (pad N plays sequence N: dim orange-red = playing,
// bright orange-red = muted).  Mute gates that sequence's PLAYBACK only -
// recording, pending chords and the live keyboard stay untouched; muting
// cuts its sounding notes at once, unmuting resumes at the next step.
// The flags persist until toggled back (and clear if the mode changes)
void encMuteHold(bool on);            // M2 held / released
bool encMuteActive();                 // overlay up (flag + ENCMODE)
bool seqIsMuted(uint8_t seq);         // sequence index 0..15
void seqMuteToggle(uint8_t seq);      // step pad press

// SEQMODE3 erase: clear one sequence (all steps + its history + sounding
// notes) or the COMPLETE pattern (all 16, also while playing - sounding
// notes are cut at once).  Listener sequences keep their own copy
void seqErase(uint8_t seq);           // sequence index 0..15
void seqEraseAll();

// --- SEQMODE step-edit for MONO tracks (radio M1/F1-F4) ---------------------
// per-step attributes: TIE sustains the note through the next step (no
// retrigger, chains, works over empty steps), GLIDE overlaps the note-on
// with the old note-off (legato - the synth's own portamento sounds),
// ACCENT plays the step at velocity 127 (other steps use the track's
// default), PROB gates the step by its stored 0..100% chance.  Track note
// shaping via ENC5/ENC6 in SEQMODE: gate = % of one step (100 = hold to
// the next step), velocity = default note velocity (boot 100).  Storage
// clears with the step (record / tap-disable / erase)
#define SEQ_ATT_TIE    0x01
#define SEQ_ATT_GLIDE  0x02
#define SEQ_ATT_ACCENT 0x04
#define SEQ_ATT_PROB   0x08
bool stepAttrToggle(uint8_t step, uint8_t att, uint8_t probVal);
                                      // true = attribute now on.  MONO: the
                                      // step's own flag; DRUM: the SELECTED
                                      // PART's accent / probability (PROB
                                      // stores probVal per voice)
uint8_t stepAttrs(uint8_t step);      // edit channel's attribute mask for the
                                      // LED pulse - DRUM follows the part select

// SEQMODE4 + REC: persist all SEQ settings to EEPROM - track type, mono
// type, octave, input channel, velocity, accent velocity, gate, mutes,
// probability clipboard, edit channel (NOTE PATTERNS ARE NOT STORED).
// seqCfgLoad() restores them at boot (before the first draw); a blank or
// corrupt block keeps the defaults
void seqCfgSave();
void seqCfgLoad();
void seqGateAdjust(int16_t counts);   // ENC5: 4 counts = 5%, clamped 10..100
int  seqGatePct();                    // edit track's gate (% of one step)
void seqVelocityAdjust(int16_t counts); // ENC6: 4 counts = 1, clamped 1..127
int  seqVelocity();                   // edit track's default velocity
void seqAccentVelocityAdjust(int16_t counts); // ENC6 + F3 held: accent vel
int  seqAccentVelocity();             // edit track's accented-step velocity
void seqSetAccentVelocity(uint8_t seq, uint8_t v); // EEPROM load, clamped
void seqGatePoll();                   // fire gate note-offs (loop context)
uint8_t seqFMask();                   // attributes the playhead reads this step
                                      // (F-row blink mask, edit channel)

// Step pads (0..15):
//  press   = unassigned: SILENTLY record a chord into it; sounds only when
//            the playhead reads it during playback; active: silent
//            candidate -> disabled on click.  Source: POLY recording from
//            ANOTHER channel (input != own) takes the held gesture first
//            (keys / latched chord / held external notes), then the INPUT
//            track's timeline (last active step at or before this position,
//            like MONO), history, pending; every other case records the
//            pending chord (fed by its input channel and by the keyboard
//            when the keyboard channel selects it)
//  hold    = assign mode: keyboard keys played while held REPLACE that
//            step's chord (first key starts the new chord)
int  stepChordPress(uint8_t step);   // notes recorded; -1 = silent (active/bad)
int  stepChordClick(uint8_t step);   // EV_CLICKED: 0 = step disabled
void stepAssignSet(int8_t step);     // step = 0..63 to arm, -1 to disarm

// sequencing (called from loop context, never from the clock ISR):
// press on an unassigned step records a chord into it (see above);
// seqStepAdvance runs each time the playhead reaches a step - every
// track carries its OWN position (independent pattern length / bar loop)
bool stepIsActive(uint8_t step);     // step has a recorded chord
bool seqStepUsed(uint8_t step);      // edit track reads it: inside its
                                     // pattern or inside its armed bar loop
                                     // (past it the pad is dark + inert)
void seqStepAdvance(bool start);   // transport step; start snaps to range heads
void seqStepStop();                  // transport stopped: release notes
uint8_t seqStepPlayPad();            // playhead pad 0..15 of the edit window,
                                     // 0xFF when another bar plays / silent
uint8_t seqStepPlayGhost();          // head's bar-relative pad 0..15 (dark-blue
                                     // ghost while another bar is shown)
// live record (SEQMODE REC button): 0 = off, 1 = armed, 2 = recording
uint8_t seqRecState();
uint8_t seqRecToggle();     // 0 = refused (kbd ch mismatch), 1 = armed, 2 = off
bool   seqRecTakeDone();    // true once when a pass finished by itself
uint8_t seqPlayBar();                // bar 0..3 the playhead reads (0xFF =
                                     // silent) - SEQ3 bar LED pulse

// notes the playhead currently has sounding on MIDI channel (1..16):
// the keyboard LEDs blink these pitch classes while they play on the
// keyboard channel (count 0 = silent)
uint8_t seqSoundingCount(uint8_t ch);
uint8_t seqSoundingNote(uint8_t ch, uint8_t i);
