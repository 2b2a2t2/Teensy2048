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

// KEYMODE3 ENC3/ENC4 channel link: ENCOD / KBOARD, states free/locked.
// The SEQ edit channel is the hub - a LOCKED family moves with it (and a
// change in a locked family drives the hub, so the other locked family
// follows too); free stays independent (each mode's own channel, as
// always).  Flags persist in the SEQ EEPROM block; at boot the locked
// families take the hub channel
bool chanEncLocked();
bool chanKeyLocked();
void chanEncLinkSet(uint8_t on);
void chanKeyLinkSet(uint8_t on);
void keyChannelForce(uint8_t ch);  // boot reconcile only (raw, no cleanup)

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
// DRUM generator write path (Drum.cpp drives it): replace a step's parts
// with these drum notes and stamp the per-voice accent mask (bit n = note
// 36+n, the mask the playhead reads for accent velocity).  Clears the
// step's attributes first - stepAttClear wipes the drum masks too
int stepDrumWrite(uint8_t step, const uint8_t *notes, uint8_t n,
                  uint16_t accentMask);   // notes written, -1 = bad step

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
// clears with the step (record / tap-disable / erase).
// Bits 6-7 are the POLY step TYPE override (F2 target): 00 = inherit the
// track's TYPE, 01/10/11 = STEP/STRUM/HAND - never stored as the classic
// flags, so the LED pulse masks (bits 0..5) stay clean
#define SEQ_ATT_TIE    0x01
#define SEQ_ATT_GLIDE  0x02
#define SEQ_ATT_ACCENT 0x04
#define SEQ_ATT_PROB   0x08
bool stepAttrToggle(uint8_t step, uint8_t att);
                                      // true = attribute now on.  MONO: the
                                      // step's own flag; DRUM: the SELECTED
                                      // PART's accent / probability (PROB
                                      // only marks the step - the % comes
                                      // from the track's own probability)
uint8_t stepAttrs(uint8_t step);      // edit channel's attribute mask for the
                                      // LED pulse - DRUM follows the part select
bool stepTypeToggle(uint8_t step, uint8_t type);
                                      // stamp / clear the bits6-7 override
                                      // (type 0..2 = STEP/STRUM/HAND): true =
                                      // the step now carries an override

// POLY step FX (embellishments, F1 = FX target in SEQMODE): one byte per
// step beside stepAtt - SUS2/SUS4, COLOR (quiet half-step-below-root ghost)
// in the low nibble, a CHORD_EXT-style extension mask (6/m7/M7/9) in the
// top nibble.  Playback transforms the chord as it leaves (buildNotes
// keeps writing plain triads/extensions).  Strumming moved to the TYPE
// system (track default on ENC7 / per-step override on F2).
// Same lifecycle as the attributes: RAM only, dies with the step
enum StepFxType : uint8_t {
  SEQ_FX_SUS2 = 0, SEQ_FX_SUS4, SEQ_FX_EXT6, SEQ_FX_EXTm7,
  SEQ_FX_EXTM7, SEQ_FX_EXT9, SEQ_FX_COLOR
};
uint8_t stepFxBit(uint8_t type);      // type index -> bit in the FX byte
bool stepFxToggle(uint8_t step, uint8_t type);
                                      // true = FX now on; the SUS pair is
                                      // mutually exclusive (sus2 vs sus4)

// HAND performs RIGHT NOW: the keyboard channel reaches a POLY sequence
// whose TRACK type is HAND (live gate is track-level - per-step overrides
// only shape the sequence's playback).  The track's chords resolve into a
// silent string BUFFER (the keyboard strums instead of hearing them), the
// 7 white keys are the strings, the black keys toggle the strum options,
// the chord engine stays muted (chordMute) so keys build silently
bool handLive();

// strum options - the black keys while handLive() (tap toggles, RAM only,
// booted as SUS+SUSCOMMON): Le Strum's set ported from StrumController.c
#define STRUM_OPT_GUITAR  0x01  // guitar chord voicing (6-string shapes)
#define STRUM_OPT_GUITAR2 0x02  // upper-octave layer on the high strings
#define STRUM_OPT_ADDS    0x04  // master: the F-row ADDS colors apply
#define STRUM_OPT_SUS     0x08  // strum rings till the buffered chord changes
#define STRUM_OPT_SUSCOM  0x10  // common tones survive that chord change
uint8_t strumOptsGet();             // current option bitfield
// selected ADDS colors (temporary F-row, gated by STRUM_OPT_ADDS):
// bit0 = sus4 (replaces the 3rd), bit1 = add6, bit2 = add9 - combinable
uint8_t strumExtGet();
void    strumExtSet(uint8_t v);
// note of white-key string s (0..6): 0 = no chord yet, 0xFF = this string
// has no note (guitar shape gap) - the LED dims those strings
uint8_t handStrNote(uint8_t s);
// cut a ringing HAND track (POLY TYPE change / keyboard channel change)
void    handRingCut(uint8_t c);

// SEQMODE4 + REC: persist all SEQ settings to EEPROM - track type, mono
// type, octave, input channel, velocity, accent velocity, gate, mutes,
// probability, pattern length, FOLLOW, edit channel.  NOTE PATTERNS ARE
// NOT STORED: the steps, their contents and the tie/glide/accent/prob
// attributes stay RAM only.
// seqCfgLoad() restores them at boot (before the first draw); a blank or
// corrupt block keeps the defaults
void seqCfgSave();
void seqCfgLoad();
void seqGateAdjust(int16_t counts);   // ENC5: 4 counts = 5%, clamped 10..100
int  seqGatePct();                    // edit track's gate (% of one step)
void seqVelocityAdjust(int16_t counts); // ENC6: 4 counts = 1, clamped 1..127
int  seqVelocity();                   // edit track's default velocity
void seqProbAdjust(int16_t counts);   // ENC8: 4 counts = 1%, clamped 0..100
int  seqProbPct();                    // edit track's probability (%)
void seqAccentVelocityAdjust(int16_t counts); // ENC6 + F3 held: accent vel
int  seqAccentVelocity();             // edit track's accented-step velocity
void seqSetAccentVelocity(uint8_t seq, uint8_t v); // EEPROM load, clamped
void seqGatePoll();                   // fire gate note-offs (loop context)
void seqStrumPoll();                  // drain staggered strum note-ons
                                      // (loop context, beside seqGatePoll)
void compPoll();                      // drain COMP stab queue (loop context,
                                      // beside seqStrumPoll)
void seqCompHitsAdjust(int16_t counts); // COMP page ENC1: 4 counts = 1 stab
int  seqCompHits();                   // edit track's comp stabs (0..8)
uint8_t seqCompHitsOf(uint8_t i);     // raw per-track (EEPROM save)
void seqCompHitsSet(uint8_t i, uint8_t v); // EEPROM load (bad -> 0)
// SKANK page params of the edit track (M2, POLY TYPE=SKANK):
// 0 = TES (tessitura anchor), 1 = MOT (motion, boot 50 = centered),
// 2 = SHP (shape LIN/PP/RND), 3 = THN (thinning), 4 = EXT (extension %),
// 5 = SUB (substitution %) - all 0..100, 4 counts = 1 unit, no wrap
void    skankPrmAdjust(uint8_t idx, int16_t counts);
uint8_t skankPrm(uint8_t idx);            // edit track (page display)
uint8_t skankPrmOf(uint8_t ch, uint8_t idx);   // raw per-track (EEPROM save)
void    skankPrmSet(uint8_t ch, uint8_t idx, uint8_t v); // load (bad -> default)
void seqTransAdjust(int16_t counts);  // PREV-shift row ENC2: 1 count = 1 st,
                                      // -12..+12, DRUM tracks refuse
int  seqTrans();                      // edit track's semitone TRANS
int8_t seqTransOf(uint8_t i);         // raw per-track (playback/EEPROM), 0 on DRUM
void seqTransSet(uint8_t i, int8_t v); // EEPROM load (clamped -12..+12)
bool seqKlockSetDir(int16_t dir);        // PREV-shift row ENC3: per-track
                                        // scale-lock for TRANS - two-state,
                                        // NO wrap (CW = ON, CCW = OFF, like
                                        // ENCOD/KBOARD; DRUM refuses); true
                                        // when the flag changed
bool seqKlock();                        // edit track's Klock flag
bool seqKlockOf(uint8_t i);             // raw per-track (playback/EEPROM),
                                        // false on DRUM
void seqKlockSet(uint8_t i, bool v);    // EEPROM load
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

// KEYMODE4 (Key4.cpp drives it): direct write of a generated chord into the
// edit sequence (pre-compensated for the sequence octave shift, like the
// live gesture) and a one-level undo of the last write batch
int            stepChordWrite(uint8_t step, const uint8_t *notes, uint8_t n);
                                                           // stored n, -1 = bad
const uint8_t* stepChordAt(uint8_t step, uint8_t *len);    // stored chord (edit seq)
// HARM (Key4.cpp reads the feeder bassline): the input channel the edit
// channel follows (1-based, 0 = none/self) and a channel-targeted step
// reader so the harmonizer can read another track's stored notes
uint8_t        seqFeederChannel();
const uint8_t* stepChordAtCh(uint8_t ch, uint8_t step, uint8_t *len);
// FOLLOW FREEZE: latch the chord/note the feeder is sounding at this instant
// (SeqClock calls these from seqFollowModeSet - entering FREEZE captures, any
// other mode drops the latch so the next entry recaptures fresh).  The
// capture returns false when the feeder has nothing right now, and the
// playback path then captures on the follower's first frozen step
bool           seqFollowFreezeCapture(uint8_t ch);
void           seqFollowFreezeClear(uint8_t ch);
void           stepSnapshotTake();     // snapshot the edit sequence for undo
int            stepSnapshotRestore();  // 0 = restored (switches back the ch), -1 = none
// true when the step already holds exactly these notes as stepChordWrite
// would store them (same count, same octave pre-compensation) - lets the
// automatic HARM re-bake skip a step whose chord did not move
bool           stepChordSame(uint8_t step, const uint8_t *notes, uint8_t n);
// HARM auto re-bake: harmFeederChanged marks every POLY+HARM track that
// feeds from ch0 (FOLLOW on); harmRebakePoll runs the marked bakes - called
// once per loop pass from Teensy2048.ino right before the display draw, so
// a burst of feeder writes costs one Viterbi pass instead of one per step
void           harmFeederChanged(uint8_t ch0);
void           harmRebakePoll();

// GNR8 generator write path (Gen.cpp drives it): one note per step,
// stamped like stepChordWrite (octave pre-compensated, followers and the
// shift range re-derived) plus the accent/glide mask the generator rolled;
// stepGenClear empties a step the roll turned into a rest.  The rotate
// moves every stored element of the edit sequence's steps within its own
// pattern length (dir > 0 = content delays one step, wraps at the end;
// elements past the length stay put), stepAttrChain stamps one attribute
// onto steps from..to-1 (wrapping inside the pattern length, end
// exclusive - TIE on X..Y-1 rings X's note through step Y) and returns
// how many steps were touched
int      stepGenWrite(uint8_t step, uint8_t note, uint8_t att);
int      stepGenClear(uint8_t step);
int      stepSkeletonWrite(uint8_t step, uint8_t stored);  // LEAD flip restore
void     stepShiftPattern(uint8_t n, int8_t dir);
uint8_t  stepAttrChain(uint8_t from, uint8_t to, uint8_t att);

// GNR8 per-step pitch modifiers (F2/F3/F4): edit-sequence accessors,
// clamped on set (octave -2..+2, borrowed chord tone 0..8 = off..n-th,
// modifier probability 0..100)
int8_t   stepOctAt(uint8_t step);
void     stepOctSet(uint8_t step, int8_t v);
uint8_t  stepBrwAt(uint8_t step);
void     stepBrwSet(uint8_t step, uint8_t v);
uint8_t  stepModPAt(uint8_t step);
void     stepModPSet(uint8_t step, uint8_t v);
int8_t   stepTrAt(uint8_t step);     // per-step TRANS: playback-only semitone
void     stepTrSet(uint8_t step, int8_t v);  // offset, clamped -12..+12
// per-step SKANK locks (hold a step on a POLY+SKANK track = the lock row,
// encoders 1..6): value 0..100 or -1 = inherit the track's page knob.
// Stored with the step content (stepAttClear wipes them), RAM only like
// every step field
int16_t  stepSkankLockAt(uint8_t step, uint8_t idx);
void     stepSkankLockSet(uint8_t step, uint8_t idx, int16_t v);

// KEYMODE4 step-hold row (ENC2/3/4): rewrite the held step's STORED chord
// in place, each returning true when a note actually moved.  DEG shifts
// every tone by one scale degree in the key (intervals keep their shape;
// force-to-scale shapes them into the scale at playback), INV cycles the
// bass (d > 0 = lowest up an octave, d < 0 = highest down), VOICE spreads
// or closes the upper voices around the bass (+1 = all-but-lowest +12,
// -1 = all-but-lowest -12 while they stay above it).  OCT uses the
// existing stepOctSet/stepOctM pair
bool stepDegAdjust(uint8_t step, int8_t d);
bool stepInvAdjust(uint8_t step, int8_t d);
bool stepVoiAdjust(uint8_t step, int8_t d);

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

// KEYMODE4 per-step embellishment overrides (PREV hold encoders EN5-8):
// [0] = sub prob 0..100 (255 = use global), [1] = inv bias 0..3 (255 = global)
// [2] = pass amt 0..100 (255 = global), [3] = color amt 0..100 (255 = global)
uint8_t stepEmbGet(uint8_t step, uint8_t param);  // param 0..3
void    stepEmbSet(uint8_t step, uint8_t param, uint8_t val);
// SEQMODE EMB target (POLY: F1 cycles FX -> EMB): a step tap cycles its
// SUB/INV/PAS/COL override - true = the step carries a non-global param
bool    stepEmbToggle(uint8_t step);
