#include "Events.h"
#include <EEPROM.h>
#include "Buttons.h"
#include "Display.h"
#include "Modes.h"
#include "Encoders.h"
#include "Bindings.h"
#include "SeqClock.h"
#include "Keyboard.h"
#include "Chords.h"
#include "Key4.h"       // KEYMODE4 controls (focus/apply/undo/locks)

// skip a log line if the USB TX buffer is congested (never block the loop —
// a blocked Serial.write stalls pollSeqClock and freezes the playhead)
static bool logRoom(int bytes) {
  return Serial.availableForWrite() >= bytes;
}

static void logSets() {
  if (!logRoom(128)) return;
  ModeBinding b = currentBinding();
  Serial.print("  [enc=");
  Serial.print(encSetName(b.enc));
  Serial.print(" ctrl=");
  Serial.print(ctrlSetName(b.ctrl));
  Serial.print(" step=");
  Serial.print(stepSetName(b.step));
  Serial.print(" key=");
  Serial.print(keySetName(b.key));
  Serial.println("]");
}

// PREV overlay (KEYMODE2) is held down - clear its slot on release
static bool prevOvActive = false;

// --- SEQMODE step-edit radio (all track types) -----------------------------
// M1 toggles the edit on/off, F1-F4 pick the target (0=STEPS, 1=TIE,
// 2=GLIDE, 3=ACCENT, 4=PROB).  The state persists across mode switches;
// while ON, the step pads edit attributes instead of recording (STEPS
// target = normal recording) and ENC5/6/8 shape gate/velocity/probability.
// DRUM/POLY take ACCENT/PROB from F3/F4 only (M1 keeps its part-select
// hold on DRUM).  ENCMODE keeps its CCs, KEYMODE its keys - never there
static bool    stepEditOn = true;     // boot: pads record (STEPS)
static uint8_t stepEditTarget = 0;    // 0 = STEPS (normal step recording)

// active in SEQMODE variants 1/2/4 while the edit track is MONO
static bool stepEditCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_MONO;
}

// DRUM/POLY variant: accent (F3) and probability (F4) only - TIE/GLIDE
// make no sense for one-shot drums or per-chord playback
static bool stepEditAccCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         (seqTrack() == SEQ_TRACK_DRUM || seqTrack() == SEQ_TRACK_POLY);
}

// true while an ATTRIBUTE target is engaged for the current track: MONO =
// TIE/GLIDE/ACCENT/PROB (1..4), DRUM/POLY = ACCENT/PROB (3,4).  Drives
// the header label, the step pulse and the attribute pad mode
bool stepEditActive() {
  return stepEditOn &&
         ((seqTrack() == SEQ_TRACK_MONO && stepEditTarget != 0) ||
          (stepEditAccCtx() && stepEditTarget >= 3)) &&
         modeBase == MODE_SEQ && modeVariant != 3;
}

static void stepEditSet(bool on, uint8_t target) {
  stepEditOn = on;
  stepEditTarget = target;
}

bool    stepEditIsOn() { return stepEditOn; }
uint8_t stepEditSel()  { return stepEditTarget; }

// --- SEQMODE M2 "ARP" page (variants 1/2/4) -------------------------------
// Clicking M2 rebinds the encoder row while the edit track is a MONO
// arp.  Like the other overlays the active check is a composite, so a
// mid-hold channel/TYPE/mode switch can never leave a stale page behind.
static bool arpPageOn = false;

static bool arpTrackCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_MONO && seqMonoType() == SEQ_MONO_ARP;
}

void arpPageToggle() { arpPageOn = !arpPageOn; }
void arpPageDismiss() { arpPageOn = false; }
bool arpPageActive()  { return arpPageOn && arpTrackCtx(); }

// HOLD F3 in SEQMODE: ENC6 rebinds from the track's default velocity to
// its ACCENT velocity (the quick tap still picks the ACCENT target)
static bool f3AccentSel = false;

void f3AccentHold(bool on) { f3AccentSel = on; }

// composite: the flag can outlive a mid-hold mode switch - the rebind
// only applies while the panel is still in SEQMODE variants 1/2/4
bool f3AccentActive() {
  return f3AccentSel && modeBase == MODE_SEQ && modeVariant != 3;
}

void handleButtonEvents() {
  uint8_t pad, ev;
  for (uint8_t n = 0; n < MAX_EVENTS_PER_LOOP && popEvent(pad, ev); n++) {
    if (logRoom(48)) {
      Serial.print(buttonName(pad));
      Serial.print(" ");
      Serial.println(eventName(ev));
    }

    if (ev == EV_PRESSED) headerShow(buttonName(pad));
    if (ev == EV_CLICKED || ev == EV_HOLD || ev == EV_DOUBLE || ev == EV_RELEASED)
      modeOnEvent(pad, ev);

    // pad classification (LED numbers): keyboard chromatic group, step group
    int8_t kl = ledForPad[pad];
    uint8_t kidx = 255, sidx = 255;
    for (uint8_t i = 0; i < KEYBOARD_NUM_G; i++)
      if (groupKeyboard[i] == kl) kidx = i;
    for (uint8_t i = 0; i < STEPS_NUM_G; i++)
      if (groupSteps[i] == kl) sidx = i;

    // step pads (all modes, behavior owned by SEQMODE):
    //  press   -> trigger the step with its chord (or the last keyboard chord)
    //  hold    -> assign mode: play keys to replace this step's chord
    //  release -> note-off
    if (sidx != 255) {
      // variant-3 channel overlays: steps select a MIDI channel
      // (KEY = keyboard output, SEQ = sequencer edit, ENC = encoder CCs).
      // SEQ3's selector is gone once an encoder moved - the pads are back
      // on the sequencer until SEQ is held into SEQMODE3 again
      bool chOverlay = (modeVariant == 3 &&
                        (modeBase == MODE_KEY || modeBase == MODE_ENC ||
                         (modeBase == MODE_SEQ && seq3ChSelActive())));
      // DRUM track: step press = toggle the selected part (voice)
      bool drum = (modeBase == MODE_SEQ && modeVariant != 3 &&
                   seqTrack() == SEQ_TRACK_DRUM);
      // hold M1 = part-select overlay: steps choose the drum voice
      bool partSel = drum && drumPartSelectActive();
      // hold M2 in ENC mode = channel-mute overlay: steps toggle channel mutes
      bool encMute = encMuteActive();
      // hold M1 in ENC mode = drum-voice mute overlay: steps toggle the
      // muted drum voices (note 36+N) across all drum-track sequences
      bool drumVoice = drumVoiceMuteActive();
      // attribute mode: an attribute target is engaged - presses toggle it
      // on ACTIVE steps (empty steps stay inert)
      bool attrMode = stepEditActive();
      // edit off: the step pads are inert on a MONO track (no accidental
      // recording; release still runs for defensive cleanup)
      bool editLocked = stepEditCtx() && !stepEditOn;
      // pads address the edit WINDOW (one bar of 16): map pad -> absolute
      // step of the sequence (overlays above keep using the raw pad index)
      uint8_t stp = (uint8_t)(sidx + seqStepViewBase());
      if (encMute) {
        if (ev == EV_PRESSED) {
          seqMuteToggle(sidx);
          char mb[12];
          snprintf(mb, sizeof(mb), "ch%u %s", sidx + 1,
                   seqIsMuted(sidx) ? "mute" : "on");
          headerShow(mb);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(mb);
          }
        }
      } else if (drumVoice) {
        if (ev == EV_PRESSED) {
          drumVoiceMuteToggle(36 + sidx);
          char db[12];
          snprintf(db, sizeof(db), "%s %s", drumVoiceName(36 + sidx),
                   drumVoiceMuted(36 + sidx) ? "mute" : "on");
          headerShow(db);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(db);
          }
        }
      } else if (chOverlay) {
        if (ev == EV_PRESSED) {
          if (modeBase == MODE_KEY) keySetChannel(sidx + 1);
          else if (modeBase == MODE_SEQ) {
            seqSetEditChannel(sidx + 1);
            // a channel switch always lands back in step edit on the new
            // channel: M1 (STEPS target) selected, radio on
            stepEditSet(true, 0);
            arpPageDismiss();       // the new track may not be an arp
            refreshAllLeds();        // M1 + F-row follow the radio again
            // the whole grid belongs to the new channel (length, speed,
            // follow, gate/vel/prob, ...) and the header may have been
            // showing the ARP page - repaint both right away
            refreshMainScreen();
            refreshHeader();
          } else encSetChannel(sidx + 1);
          if (modeBase == MODE_ENC)
            refreshMainScreen();   // CC values belong to the new channel
          if (logRoom(48)) {
            Serial.print(modeBase == MODE_KEY ? "  kbd channel " :
                         modeBase == MODE_SEQ ? "  seq channel " :
                                                "  enc channel ");
            Serial.println(modeBase == MODE_KEY ? keyChannel() :
                            modeBase == MODE_SEQ ? seqEditChannel() :
                                                   encChannel());
          }
        } else if (ev == EV_RELEASED) {
          stepAssignSet(-1);   // defensive: never leave assign mode armed
        }
      } else if (partSel) {
        if (ev == EV_PRESSED) {
          drumPartSet(sidx);
          if (logRoom(48)) {
            Serial.print("  part ");
            Serial.println(drumPartName());
          }
        }
      } else if (!seqStepUsed(stp) && (ev == EV_PRESSED || ev == EV_CLICKED ||
                                       ev == EV_HOLD)) {
        // past the pattern length: the pad is dark AND inert - no record,
        // no toggle, no assign, no attribute edit (release still falls
        // through to the defensive cleanup below)
        if (ev == EV_PRESSED && logRoom(48)) Serial.println("  step unused");
      } else if (attrMode && (ev == EV_PRESSED || ev == EV_CLICKED ||
                              ev == EV_HOLD)) {
        // attribute edit: a tap on an ACTIVE step toggles the selected
        // attribute; empty steps, clicks and holds do nothing here
        if (ev == EV_PRESSED && stepIsActive(stp)) {
          static const uint8_t attOf[4] = { SEQ_ATT_TIE, SEQ_ATT_GLIDE,
                                            SEQ_ATT_ACCENT, SEQ_ATT_PROB };
          static const char *const nmOf[4] = { "tie", "glide", "accent",
                                               "prob" };
          uint8_t att = attOf[stepEditTarget - 1];
          bool on = stepAttrToggle(stp, att);
          char ab[24];
          if (seqTrack() == SEQ_TRACK_DRUM) {
            // per voice: name the SELECTED PART and its short attribute
            snprintf(ab, sizeof(ab), "%u %s %s %s", stp + 1,
                     drumPartName(), att == SEQ_ATT_ACCENT ? "acc" : "prb",
                     on ? "on" : "off");
          } else {
            snprintf(ab, sizeof(ab), "%u %s %s", stp + 1,
                     nmOf[stepEditTarget - 1], on ? "on" : "off");
          }
          headerShow(ab);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(ab);
          }
        }
      } else if (editLocked && (ev == EV_PRESSED || ev == EV_CLICKED ||
                                ev == EV_HOLD)) {
        // edit off: pads inert (no record, no assign mode, no disable)
        if (ev == EV_PRESSED && logRoom(48))
          Serial.println("  step edit off");
      } else if (ev == EV_PRESSED) {
        // KEYMODE4: a step pad press moves the write target there first,
        // then the usual trigger/assign behavior runs on it
        if (modeBase == MODE_KEY && modeVariant == 4) k4FocusSet(stp);
        if (drum) {
          int r = stepDrumToggle(stp);
          if (r == -2 && logRoom(48)) {
            Serial.println("  part off");
          } else if (r >= 0 && logRoom(48)) {
            Serial.print("  part on ");
            Serial.println(drumPartName());
          }
        } else {
          int n = stepChordPress(stp);
          if (n > 0 && logRoom(48)) {
            Serial.print("  step record notes=");
            Serial.println(n);
          }
        }
      } else if (ev == EV_CLICKED) {
        if (!drum && stepChordClick(stp) == 0 && logRoom(48))
          Serial.println("  step disabled");
      } else if (ev == EV_HOLD) {
        if (!drum) {           // DRUM steps are toggles, no assign mode
          stepAssignSet((int8_t)stp);
          if (logRoom(48)) {
            Serial.print("  step assign ");
            Serial.println(stp + 1);
          }
        }
      } else if (ev == EV_RELEASED) {
        stepAssignSet(-1);
      }
      refreshAllLeds();   // recording may have toggled the step's dim-green LED
    }

    // PART SELECT (DRUM track): press M1 -> orange overlay on the steps to
    // pick which drum voice the steps sequence (release closes it; the
    // overlay comes up on press so it appears immediately, no 300ms wait).
    // ENCMODE: press M1 instead opens the drum-VOICE mute overlay
    if (ledForPad[pad] == groupControls[2]) {
      if (ev == EV_PRESSED && modeBase == MODE_SEQ && modeVariant != 3 &&
          seqTrack() == SEQ_TRACK_DRUM) {
        drumPartSelectHold(true);
        headerShow("PART SELECT");
        if (logRoom(48)) {
          Serial.print("  part select (");
          Serial.print(drumPartName());
          Serial.println(")");
        }
        refreshAllLeds();
      } else if (ev == EV_RELEASED && drumPartSelectActive()) {
        drumPartSelectHold(false);
        refreshAllLeds();
      } else if (ev == EV_PRESSED && modeBase == MODE_ENC) {
        drumMuteHold(true);
        headerShow("DRUM MUTE");
        if (logRoom(48)) Serial.println("  drum voice mute");
        refreshAllLeds();
      } else if (ev == EV_RELEASED && drumVoiceMuteActive()) {
        drumMuteHold(false);
        refreshAllLeds();
      } else if (ev == EV_CLICKED && stepEditCtx()) {
        // SEQMODE + MONO: CLICK on M1 cycles the edit radio - OFF -> ON
        // (STEPS target), ON+STEPS -> OFF, ON+attribute -> back to STEPS
        if (!stepEditOn) {
          stepEditSet(true, 0);
          headerShow("EDIT ON");
          if (logRoom(48)) Serial.println("  step edit on (steps)");
        } else if (stepEditTarget != 0) {
          stepEditSet(true, 0);
          headerShow("STEPS");
          if (logRoom(48)) Serial.println("  edit target steps");
        } else {
          stepEditSet(false, 0);
          headerShow("EDIT OFF");
          if (logRoom(48)) Serial.println("  step edit off");
        }
        refreshAllLeds();
      } else if (ev == EV_CLICKED && stepEditAccCtx()) {
        // SEQMODE + DRUM/POLY: a QUICK tap of M1 toggles the edit master only
        // (a HOLD still opens the part select - hold suppresses the click)
        if (stepEditOn) {
          stepEditSet(false, 0);
          headerShow("EDIT OFF");
          if (logRoom(48)) Serial.println("  step edit off");
        } else {
          // master on: reuse the last attribute, default to ACCENT
          uint8_t t = (stepEditTarget >= 3) ? stepEditTarget : 3;
          stepEditSet(true, t);
          headerShow(t == 3 ? "ACCENT" : "PROB");
          if (logRoom(48)) Serial.println("  step edit on (accent)");
        }
        refreshAllLeds();
      }
    }

    // HOLD F3 in SEQMODE: ENC6 becomes the track's ACCENT velocity knob
    // (the quick tap below still picks the ACCENT target - a hold ends in
    // EV_RELEASED and never reaches EV_CLICKED)
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        kl >= groupControls[5] && kl <= groupControls[8] &&
        (uint8_t)(kl - groupControls[5]) == 2) {
      if (ev == EV_PRESSED) {
        f3AccentHold(true);
        headerShow("ACCENT VEL");
        refreshMainScreen();
        if (logRoom(48)) {
          Serial.print("  accent vel ");
          Serial.println(seqAccentVelocity());
        }
      } else if (ev == EV_RELEASED && f3AccentActive()) {
        f3AccentHold(false);
        refreshMainScreen();
        if (logRoom(48)) {
          Serial.print("  accent vel done ");
          Serial.println(seqAccentVelocity());
        }
      }
    }

    // F1-F4 in SEQMODE (MONO): pick the edit target and switch the radio
    // on (TIE/GLIDE/ACCENT/PROB).  DRUM/POLY tracks only take ACCENT/PROB
    // from F3/F4 - clicking the selected one again closes the radio there.
    // SEQMODE only: ENCMODE/KEYMODE keep their F-pad jobs
    if (ev == EV_CLICKED && (stepEditCtx() || stepEditAccCtx()) &&
        kl >= groupControls[5] && kl <= groupControls[8]) {
      uint8_t idx = (uint8_t)(kl - groupControls[5]);      // 0..3
      static const char *const tn[4] = { "TIE", "GLIDE", "ACCENT", "PROB" };
      if (stepEditCtx()) {
        stepEditSet(true, (uint8_t)(idx + 1));
        headerShow(tn[idx]);
        if (logRoom(48)) {
          Serial.print("  edit target ");
          Serial.println(tn[idx]);
        }
      } else if (idx >= 2) {               // DRUM: ACCENT (F3) / PROB (F4)
        if (stepEditOn && stepEditTarget == idx + 1) {
          stepEditSet(false, 0);
          headerShow("EDIT OFF");
          if (logRoom(48)) Serial.println("  step edit off");
        } else {
          stepEditSet(true, (uint8_t)(idx + 1));
          headerShow(tn[idx]);
          if (logRoom(48)) {
            Serial.print("  edit target ");
            Serial.println(tn[idx]);
          }
        }
      }
      refreshAllLeds();
    }

    // MUTE overlay (ENC mode): hold M2 -> the steps show the channel mute
    // state (dim orange-red = playing, bright orange-red = muted), tapping
    // one toggles that channel's playback; release closes the overlay.
    // SEQMODE3: a CLICK on M2 instead erases the selected track
    if (ledForPad[pad] == groupControls[3]) {
      if (ev == EV_PRESSED && modeBase == MODE_ENC) {
        encMuteHold(true);
        headerShow("MUTE");
        if (logRoom(48)) Serial.println("  mute overlay");
        refreshAllLeds();
      } else if (ev == EV_RELEASED && encMuteActive()) {
        encMuteHold(false);
        refreshAllLeds();
      } else if (ev == EV_CLICKED && modeBase == MODE_SEQ && modeVariant == 3) {
        seqErase((uint8_t)(seqEditChannel() - 1));
        char eb[14];
        snprintf(eb, sizeof(eb), "ch%u erased", seqEditChannel());
        headerShow(eb);
        if (logRoom(48)) {
          Serial.print("  ");
          Serial.println(eb);
        }
        refreshAllLeds();   // step LEDs dim: that pattern is gone
      } else if (ev == EV_CLICKED && arpTrackCtx()) {
        // SEQMODE 1/2/4 + MONO/ARP: M2 clicks the ARP encoder page on/off
        arpPageToggle();
        headerShow(arpPageActive() ? "ARP" : "ARP OFF");
        if (logRoom(48)) {
          Serial.print("  arp page ");
          Serial.println(arpPageActive() ? "on" : "off");
        }
        refreshAllLeds();
        refreshMainScreen();
      }
    }

    // SEQMODE3 + REC: erase the COMPLETE pattern (all 16 tracks) - allowed
    // while the transport runs; every sounding note is cut at once
    if (ev == EV_PRESSED && ledForPad[pad] == groupTransport[1] &&
        modeBase == MODE_SEQ && modeVariant == 3) {
      seqEraseAll();
      headerShow("ALL ERASED");
      if (logRoom(48)) Serial.println("  erase all tracks");
      refreshAllLeds();
    }

    // ENCMODE4 + REC: save the knob values + channel to EEPROM (restored
    // on the next boot; DAW feedback still wins live) - save only, no push
    if (ev == EV_PRESSED && ledForPad[pad] == groupTransport[1] &&
        modeBase == MODE_ENC && modeVariant == 4) {
      encCcSave();
      headerShow("CC SAVED");
      if (logRoom(48)) Serial.println("  cc values saved to eeprom");
    }

    // SEQMODE4 + REC: save every SEQ setting (track type, mono type, octave,
    // input channel, velocity, accent, gate, mutes, probability, pattern
    // length, FOLLOW, arp options, edit channel).  Notes, step contents and
    // the tie/glide/accent/prob attributes are NOT stored, they stay RAM
    if (ev == EV_PRESSED && ledForPad[pad] == groupTransport[1] &&
        modeBase == MODE_SEQ && modeVariant == 4) {
      seqCfgSave();
      headerShow("SEQ SAVED");
      if (logRoom(48)) Serial.println("  seq config saved to eeprom");
    }

    // SEQMODE1/2 and KEYMODE1/2/4 + REC: live record - arm one pass over
    // the edit track's pattern from its next 1st bar (auto-off after one
    // pass; the keyboard channel must match the edit track or arming is
    // refused).  KEYMODE2 records the whole chord the engine builds
    if (ev == EV_PRESSED && ledForPad[pad] == groupTransport[1] &&
        ((modeBase == MODE_SEQ && (modeVariant == 1 || modeVariant == 2)) ||
         (modeBase == MODE_KEY &&
          (modeVariant == 1 || modeVariant == 2 || modeVariant == 4)))) {
      uint8_t r = seqRecToggle();
      if (r == 0) headerShow("kbd ch mismatch");
      else if (r == 1) headerShow("rec armed");
      else headerShow("rec off");
      refreshAllLeds();
      if (logRoom(48)) {
        Serial.print("  rec ");
        Serial.println(r == 0 ? "refused (kbd ch)" : (r == 1 ? "armed" : "off"));
      }
    }

    // KEYMODE2 chord buttons (momentary): M1/M2/M3/NEXT select the quality
    // Maj/Min/Dim/Sus, F1..F4 the extensions 6/m7/M7/9 - press sets,
    // release clears, a sounding chord re-triggers.  M1 keeps its DRUM
    // part-select / ENCMODE voice-mute priority, M2 its ENCMODE mute /
    // SEQMODE3 erase above the quality keys
    if ((ev == EV_PRESSED || ev == EV_RELEASED) && keyBehaviorVariant() == 2) {
      int8_t ci = -1;
      for (uint8_t i = 0; i < CONTROLS_NUM_G; i++)
        if (groupControls[i] == kl) ci = (int8_t)i;
      bool m1SeqRadio = (ci == 2 && modeBase == MODE_SEQ &&
                         modeVariant != 3);               // edit radio M1
      bool m1Enc = (ci == 2 && modeBase == MODE_ENC);
      bool m2Enc = (ci == 3 && modeBase == MODE_ENC);
      bool m2Seq3 = (ci == 3 && modeBase == MODE_SEQ && modeVariant == 3);
      bool fSeq = (ci >= 5 && modeBase == MODE_SEQ && modeVariant != 3);
      // F1-F4 belong to the SEQMODE edit radio (target / accent / prob)
      // plus the F3 accent-velocity hold - they never touch the chords
      // there, on any track type
      if (ci > 0 && !m1SeqRadio && !m1Enc && !m2Enc && !m2Seq3 &&
          !fSeq) {                                        // ci 0 = PREV
        bool on = (ev == EV_PRESSED);
        static const uint8_t extOf[4] = { CHORD_EXT_6, CHORD_EXT_m7,
                                          CHORD_EXT_M7, CHORD_EXT_9 };
        if (ci >= 5) {
          chordSetExt(extOf[ci - 5], on);
        } else {
          static const uint8_t qOf[5] = { CHORD_Q_NONE, CHORD_Q_SUS,
                                          CHORD_Q_MAJ, CHORD_Q_MIN, CHORD_Q_DIM };
          // order on the panel: NEXT=1 Sus, M1=2 Maj, M2=3 Min, M3=4 Dim
          chordSetQuality(on ? qOf[ci] : CHORD_Q_NONE);
        }
        refreshAllLeds();               // sounding pitch classes may change
        if (chordName()[0]) headerShow(chordName());
        if (logRoom(48)) {
          Serial.print(on ? "  chord set " : "  chord clear ");
          Serial.println(chordName());
        }
      }
    }

    // KEYMODE4 control row: NEXT = focus+1, M1 = write latch (WRITE lands
    // the proposal on the focused step / AUDITION plays it only), M2 =
    // auto-advance after every write, M3 tap = apply the generator to every
    // unlocked step / M3 hold = undo, F1 = variation (reseed + generate),
    // F2 = lock the focused step (apply skips it), F3 = VOICE-LEAD
    // inversions, F4 held = audition only (no write, no advance)
    int8_t k4ci = -1;
    for (uint8_t i = 0; i < CONTROLS_NUM_G; i++)
      if (groupControls[i] == kl) k4ci = (int8_t)i;
    if (modeBase == MODE_KEY && modeVariant == 4) {
      if (k4ci == 1 && ev == EV_CLICKED) {
        k4FocusStep(1);            // announces focus + progression itself
        refreshAllLeds();
        if (logRoom(48)) Serial.println("  focus next");
      } else if (k4ci == 2 && ev == EV_CLICKED) {
        bool on = k4WriteToggle();
        headerShow(on ? "KEY WRITE" : "KEY AUDITION");
        if (logRoom(48))
          Serial.println(on ? "  key write" : "  key audition");
      } else if (k4ci == 3 && ev == EV_CLICKED) {
        bool on = k4AutoToggle();
        headerShow(on ? "AUTO ADV" : "AUTO OFF");
        if (logRoom(48)) Serial.println(on ? "  auto on" : "  auto off");
      } else if (k4ci == 4 && ev == EV_CLICKED) {
        int w = k4ApplyAll();
        if (w > 0) k4AnnouncePattern();   // the progression that was written
        else headerShow("APPLIED 0");
        refreshAllLeds();
        refreshMainScreen();   // focus/locks may have been re-read
        if (logRoom(48)) Serial.println(w > 0 ? "  applied" : "  applied 0");
      } else if (k4ci == 4 && ev == EV_HOLD) {
        int u = k4Undo();
        if (u == 0) k4AnnouncePattern();   // the restored progression
        else headerShow("nothing to undo");
        refreshAllLeds();
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  undo");
      } else if (k4ci == 5 && ev == EV_CLICKED) {
        int w = k4Variation();
        if (w > 0) k4AnnouncePattern();   // the reseeded progression
        else headerShow("VAR 0");
        refreshAllLeds();
        if (logRoom(48)) Serial.println(w > 0 ? "  variation" : "  var 0");
      } else if (k4ci == 6 && ev == EV_CLICKED) {
        bool on = k4LockToggle(k4Focus());
        char lb[14];
        snprintf(lb, sizeof(lb), "%s %u", on ? "lock" : "unlock",
                 (unsigned)(k4Focus() + 1));
        headerShow(lb);
        refreshAllLeds();       // amber focus/lock pads follow
        if (logRoom(48)) { Serial.print("  "); Serial.println(lb); }
      } else if (k4ci == 7 && ev == EV_CLICKED) {
        bool on = k4LeadToggle();
        headerShow(on ? "LEAD ON" : "LEAD OFF");
        if (logRoom(48)) Serial.println(on ? "  lead on" : "  lead off");
      } else if (k4ci == 8 && ev == EV_PRESSED) {
        k4HoldSet(true);
        if (logRoom(48)) Serial.println("  hold audition");
      }
    }
    // F4 release, ungated by mode: the momentary hold flag clears even if
    // the mode changed while the key was down (never a stuck audition)
    if (k4ci == 8 && ev == EV_RELEASED && k4Hold()) {
      k4HoldSet(false);
      if (logRoom(48)) Serial.println("  hold off");
    }

    // PREV = momentary encoder overlay: KEYMODE2 remaps the row to
    // Oct/Scale/Root/Latch/KLock/Perf, KEYMODE4 to Oct/Scale/Key; release
    // restores it.  A quick KEYMODE4 tap (no hold) steps the focus back
    if (kl == groupControls[0]) {
      if (ev == EV_PRESSED && keyBehaviorVariant() == 2) {
        setOverlay(G_ENC, ENC_KEY2_PREV);
        prevOvActive = true;
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  overlay KEY2 PREV");
      } else if (ev == EV_PRESSED && modeBase == MODE_KEY &&
                 modeVariant == 4) {
        setOverlay(G_ENC, ENC_KEY4_PREV);
        prevOvActive = true;
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  overlay KEY4 PREV");
      } else if (ev == EV_RELEASED && prevOvActive) {
        clearOverlay(G_ENC);
        prevOvActive = false;
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  overlay off");
      } else if (ev == EV_CLICKED && modeBase == MODE_KEY &&
                 modeVariant == 4) {
        k4FocusStep(-1);           // announces focus + progression itself
        refreshAllLeds();
        if (logRoom(48)) Serial.println("  focus prev");
      }
    }

    // PLAY in SEQMODE: Start (or Stop while the INT transport runs).
    // ENCMODE3: PLAY = toggle the clock source instead (frees ENC4 for
    // the pattern speed) - it never starts the transport there
    if (ev == EV_PRESSED && ledForPad[pad] == groupTransport[0]) {
      if (modeBase == MODE_ENC && modeVariant == 3) {
        seqSetSyncInternal(!seqIsSyncInternal());
        headerShow(seqIsSyncInternal() ? "clock INT" : "clock EXT");
        if (logRoom(48)) {
          Serial.print("  clock ");
          Serial.println(seqIsSyncInternal() ? "INT" : "EXT");
        }
      } else if (modeBase == MODE_SEQ ||
                 (modeBase == MODE_KEY && modeVariant == 4)) {
        seqTogglePlay();
        if (logRoom(48)) Serial.println("  -> MIDI Play toggle");
      }
    }

    // keyboard pads -> MIDI notes in every mode (behavior defined by KEYMODE,
    // inherited by SEQMODE/ENCMODE); release always clears the key state
    // (even if the mode changed while it was held: no stuck notes).
    // SEQMODE3 intercepts C D E F first: they pick the edit window (bar)
    // and press the selected bar again to loop it - swallowed presses are
    // remembered so the matching release never sends a stray note-off
    if (kidx != 255) {
      static uint8_t barHeld = 0;          // kidx bits eaten as pattern bars
      bool isBar = (ev == EV_PRESSED && modeBase == MODE_SEQ &&
                    modeVariant == 3 && kl >= 25 && kl <= 28);
      if (isBar) {
        uint8_t bar = (uint8_t)(kl - 25);  // 25,26,27,28 -> bar 0..3
        barHeld |= (uint8_t)(1 << kidx);
        uint8_t r = seqBarPress(bar);
        char bb[20];
        if (r == 0)
          snprintf(bb, sizeof(bb), "steps %u-%u",
                   bar * 16 + 1, bar * 16 + 16);
        else if (r == 1)
          snprintf(bb, sizeof(bb), "loop %u-%u", bar * 16 + 1, bar * 16 + 16);
        else
          snprintf(bb, sizeof(bb), "loop off");
        headerShow(bb);
        if (logRoom(48)) {
          Serial.print("  bar ");
          Serial.println(bb);
        }
      } else if (ev == EV_PRESSED) {
        int n = keyPadPress(kidx);
        if (n >= 0 && logRoom(48)) {
          Serial.print("  note on ");
          Serial.println(n);
        }
        // chord engine variants: the header shows the chord that started
        if (n >= 0 && keyChordMode() && chordName()[0])
          headerShow(chordName());
      } else if (ev == EV_RELEASED) {
        if (barHeld & (1 << kidx)) barHeld &= (uint8_t)~(1 << kidx);
        else keyPadRelease(kidx);
      }
      refreshAllLeds();   // first assign key can activate the held step's LED
    }

    if (ev == EV_PRESSED || ev == EV_CLICKED || ev == EV_HOLD || ev == EV_DOUBLE)
      logSets();
  }
}

// ENCMODE knob positions: encoder n sends CC n+1 on the encoder channel.
// Stored PER CHANNEL (8 encoders x 16 channels): switching channels loads
// that channel's own value instead of inheriting the previous channel's
// (no jump); DAW feedback updates the slots via encCcIncoming()
static int16_t ccVal[8][16] = {{0}};

uint8_t encCcValue(uint8_t idx) {
  if (idx >= 8) return 0;
  return (uint8_t)ccVal[idx][encChannel() - 1];
}

// incoming CC from the DAW: sync the knob positions of CC1..CC8 on any
// channel (store only - never re-sent, so no feedback loop; deduped so an
// echo of our own move doesn't repaint the screen)
void encCcIncoming(uint8_t ch, uint8_t cc, uint8_t val) {
  if (ch < 1 || ch > 16 || cc < 1 || cc > 8) return;
  if (ccVal[cc - 1][ch - 1] == (int16_t)(val & 0x7F)) return;
  ccVal[cc - 1][ch - 1] = val & 0x7F;
  if (modeBase == MODE_ENC) refreshMainScreen();   // knobs are on screen
}

// EEPROM persistence (ENCMODE4 + REC saves, boot loads).  Layout at
// address 0: magic | encCh | 128 values (enc*16+ch) | checksum = 131 of
// 4096 bytes; a blank or corrupt block just keeps the defaults (zeros)
static const uint8_t CC_EE_MAGIC = 0xCC;
static const uint16_t CC_EE_LEN = 131;

void encCcSave() {
  uint8_t b[CC_EE_LEN];
  b[0] = CC_EE_MAGIC;
  b[1] = encChannel();
  for (uint8_t e = 0; e < 8; e++)
    for (uint8_t c = 0; c < 16; c++)
      b[2 + e * 16 + c] = (uint8_t)ccVal[e][c] & 0x7F;
  uint8_t sum = 0;
  for (uint16_t i = 0; i < CC_EE_LEN - 1; i++) sum += b[i];
  b[CC_EE_LEN - 1] = sum;
  EEPROM.put(0, b);   // put() writes only the bytes that changed
}

void encCcLoad() {
  uint8_t b[CC_EE_LEN];
  EEPROM.get(0, b);
  uint8_t sum = 0;
  for (uint16_t i = 0; i < CC_EE_LEN - 1; i++) sum += b[i];
  if (b[0] != CC_EE_MAGIC || sum != b[CC_EE_LEN - 1]) return;
  for (uint8_t e = 0; e < 8; e++)
    for (uint8_t c = 0; c < 16; c++)
      ccVal[e][c] = b[2 + e * 16 + c] & 0x7F;
  encSetChannel(b[1]);
}

void handleEncoderEvents() {
  uint8_t ei;
  int16_t ed;
  bool any = false;
  for (uint8_t n = 0; n < MAX_EVENTS_PER_LOOP && popEncEvent(ei, ed); n++) {
    // SEQMODE3: ANY encoder movement drops the step-pad channel selector
    // for the rest of the hold - the pads go back to the sequencer (so the
    // playhead, recorded steps and the pattern length read again) and only
    // holding SEQ into SEQMODE3 brings the selector up
    if (ed != 0 && modeBase == MODE_SEQ && modeVariant == 3 &&
        seq3ChSelDismiss())
      refreshAllLeds();

    // KEYMODE: ENC1 = octave -2..8 (clamped, no wrap; 4 counts = 1 octave).
    // KEYMODE4: the main row owns ENC1 for GENRE - the octave lives on the
    // PREV overlay row (same composite guard the scale row uses)
    if (modeBase == MODE_KEY && ei == 0 && ed != 0 &&
        (modeVariant != 4 || currentBinding().enc == ENC_KEY4_PREV)) {
      keyOctaveAdjust(ed);
      if (logRoom(48)) {
        Serial.print("  octave ");
        Serial.println(keyOctave());
      }
      any = true;
    }

    // KEYMODE: ENC2 = scale (every variant, and the PREV overlay row);
    // 4 counts = 1 scale, clamped 0..12, no wrap.  Selecting a scale
    // flashes its mood ("Happy", "Sad", ...) in the temporary header -
    // KEYMODE4 announces its new proposal ranking there instead
    if (modeBase == MODE_KEY && ei == 1 && ed != 0 &&
        (modeVariant != 4 || currentBinding().enc == ENC_KEY4_PREV)) {
      keyScaleAdjust(ed);
      refreshAllLeds();             // in-scale / out-of-scale keys repaint
      if (modeVariant == 4) {
        k4Refresh();                // the framework follows the scale
        k4Announce();               // the header names the new ranking
      } else {
        headerShow(keyScaleMood());
      }
      if (logRoom(48)) {
        Serial.print("  scale ");
        Serial.println(keyScaleName());
      }
      any = true;
    }

    // KEYMODE1/2: ENC3 = KEY (the key/root) - shared by both variants: it
    // roots the scale mask in KEYMODE1 and the chord engine in KEYMODE2.
    // KEYMODE4: same, but only while its PREV overlay row is up (the main
    // row uses ENC3 for TENS)
    if (modeBase == MODE_KEY &&
        (modeVariant == 1 || modeVariant == 2 ||
         currentBinding().enc == ENC_KEY4_PREV) &&
        ei == 2 && ed != 0) {
      keyRootAdjust(ed);
      refreshAllLeds();           // the scale mask is rooted here too
      if (modeVariant == 4) {     // the framework follows the root
        k4Refresh();
        k4Announce();
      }
      if (logRoom(48)) {
        Serial.print("  root ");
        Serial.println(keyRootName());
      }
      any = true;
    }

    // KEYMODE4: the main context row = ENC1 GENRE (1 count = 1 genre,
    // wraps), ENC2..ENC8 = MOOD/TENS/MOTION/CAD/CPX/ERA/SURP (1 count =
    // 1 unit, clamped 0..100).  Every change re-ranks the proposals
    if (modeBase == MODE_KEY && modeVariant == 4 && ed != 0 &&
        currentBinding().enc == ENC_KEY4) {
      if (ei == 0) {
        k4GenreAdjust(ed);
        if (logRoom(48)) {
          Serial.print("  genre ");
          Serial.println(k4GenreName());
        }
      } else {
        k4KnobAdjust((uint8_t)(ei - 1), ed);
        if (logRoom(48)) {
          static const char* kn[7] = { "mood ", "tens ", "motion ", "cad ",
                                       "cpx ", "era ", "surp " };
          Serial.print(kn[ei - 1]);
          Serial.println(k4Knob((uint8_t)(ei - 1)));
        }
      }
      refreshAllLeds();   // the keyboard repaints the re-ranked proposals
      k4Announce();       // ...and the header names them (I V vi IV)
      any = true;
    }

    // KEYMODE4: the PREV overlay row = ENC1 Octave / ENC2 Scale / ENC3 Key
    // / ENC4 = Force-to-scale (same global ON/OFF as KEYMODE2's ENC4: with
    // it on, sequencer playback snaps stored notes to the current scale,
    // so turning the scale recolors recorded progressions live)
    if (modeBase == MODE_KEY && modeVariant == 4 && ei == 3 && ed != 0 &&
        currentBinding().enc == ENC_KEY4_PREV) {
      chordFScaleAdjust(ed);
      if (logRoom(48)) {
        Serial.print("  fscale ");
        Serial.println(chordFScale() ? "ON" : "OFF");
      }
      any = true;
    }

    // KEYMODE2: ENC4 = Force-to-scale (FScale), ENC5..ENC7 = bass/spread/
    // density, ENC8 = latch.  With PREV held the row remaps: ENC4 = latch,
    // ENC5 = KeyLock, ENC6 = Perf (state only)
    if (modeBase == MODE_KEY && modeVariant == 2 && ei != 2 && ed != 0) {
      ModeBinding b = currentBinding();
      if (b.enc == ENC_KEY2_PREV) {
        if (ei == 3) chordLatchAdjust(ed);
        else if (ei == 4) chordKeyLockAdjust(ed);
        else if (ei == 5) chordPerfAdjust(ed);
        if (ei >= 3 && ei <= 5) {
          refreshAllLeds();         // latch off silences the chord's tint
          if (logRoom(48)) {
            Serial.print(ei == 3 ? "  latch " : ei == 4 ? "  klock " : "  perf ");
            Serial.println(ei == 3 ? (chordLatch() ? "ON" : "OFF") :
                           ei == 4 ? (chordKeyLock() ? "ON" : "OFF") :
                                     (chordPerf() ? "ON" : "OFF"));
          }
          any = true;
        }
      } else if (ei == 7) {
        chordLatchAdjust(ed);       // main row: ENC8 = latch
        refreshAllLeds();
        if (logRoom(48)) {
          Serial.print("  latch ");
          Serial.println(chordLatch() ? "ON" : "OFF");
        }
        any = true;
      } else if (ei == 3) {
        chordFScaleAdjust(ed);      // main row: ENC4 = Force-to-scale
        refreshAllLeds();
        if (logRoom(48)) {
          Serial.print("  fscale ");
          Serial.println(chordFScale() ? "ON" : "OFF");
        }
        any = true;
      } else if (ei >= 4 && ei <= 6) {
        chordParamAdjust(ei - 3, ed);   // BASS / SPRD / DENS
        if (logRoom(48)) {
          static const char* pn[4] = { "voic ", "bass ", "sprd ", "dens " };
          Serial.print(pn[ei - 3]);
          Serial.println(chordParam(ei - 3));
        }
        any = true;
      }
    }

    // ENCMODE: ENC1..ENC8 send CC1..CC8 on the encoder channel (ENCMODE3);
    // 1 count = 1 value step, clamped 0..127, no wrap; positions persist
    // across mode changes like a real knob
    if (modeBase == MODE_ENC && ei < 8 && ed != 0) {
      uint8_t cch = encChannel();
      int16_t v = ccVal[ei][cch - 1] + ed;
      if (v < 0) v = 0;
      if (v > 127) v = 127;
      if (v != ccVal[ei][cch - 1]) {
        ccVal[ei][cch - 1] = v;
        usbMIDI.sendControlChange(ei + 1, (uint8_t)v, cch);
        if (logRoom(48)) {
          Serial.print("  CC ");
          Serial.print(ei + 1);
          Serial.print("=");
          Serial.println(v);
        }
      }
      any = true;
    }

    // SEQMODE: ENC1 = octave shift of the selected sequence (all variants)
    // SEQMODE3: ENC4 = pattern SPEED of the edit sequence (7 rungs, 4 =
    //           normal; sync moved to ENCMODE3+PLAY)
    //           ENC5 = input channel of the edit sequence, 1..16
    //           ENC6 = TRACK DRUM/MONO/POLY (three-state, no wrap)
    //           ENC7 = TYPE ROOT/BASS/ARP (MONO sequences only, no wrap)
    //           ENC8 = tempo 40..240 (moved from ENC1)
    if (modeBase == MODE_SEQ) {
      if (ei == 0 && ed != 0) {
        seqOctaveShiftAdjust(ed);           // octave shift of the edit seq
        if (logRoom(48)) {
          Serial.print("  octshift ");
          Serial.println(seqOctaveShift());
        }
        any = true;
      } else if (modeVariant != 3) {
        // M2 ARP page: ENC2 = span (1..4 octaves), ENC3 = skip rests,
        // ENC4 = mode, ENC7 = insert mode.  These four are the page's own
        // knobs; ENC1 (octave), ENC5/6 (gate/vel) and ENC8 (prob) keep the
        // bindings they had, and a TYPE/channel/mode change drops the page
        if (ed != 0 && arpPageActive() &&
            (ei == 1 || ei == 2 || ei == 3 || ei == 6)) {
          uint8_t seq = (uint8_t)(seqEditChannel() - 1);
          if (ei == 1) {
            seqArpSpanAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  span ");
              Serial.print(seqArpSpanOf(seq));
              Serial.println(" oct");
            }
          } else if (ei == 2) {
            seqArpSkipToggle(ed);
            if (logRoom(48)) {
              Serial.print("  skip ");
              Serial.println(seqArpSkipOf(seq) ? "ON" : "OFF");
            }
          } else if (ei == 3) {
            seqArpModeAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  arpmode ");
              Serial.println(seqArpModeName(seqArpModeOf(seq)));
            }
          } else {
            seqArpInstAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  inst ");
              Serial.println(seqArpInstName(seqArpInstOf(seq)));
            }
          }
        // F3 held: ENC6 = ACCENT velocity of the edit track (every track
        // type).  Otherwise the step-edit value encoders: ENC5 = gate %,
        // ENC6 = default velocity (MONO only), ENC8 = the probability of
        // the edit track itself - live, so every PROB step of that track
        // re-rolls at the new rate.  ENC7 stays free
        } else if (ei == 5 && ed != 0 && f3AccentActive()) {
          seqAccentVelocityAdjust(ed);
          if (logRoom(48)) {
            Serial.print("  accvel ");
            Serial.println(seqAccentVelocity());
          }
        } else if (ed != 0 && stepEditOn) {
          if (ei == 4 && stepEditCtx()) {
            seqGateAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  gate ");
              Serial.print(seqGatePct());
              Serial.println("%");
            }
          } else if (ei == 5 && stepEditCtx()) {
            seqVelocityAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  vel ");
              Serial.println(seqVelocity());
            }
          } else if (ei == 7 &&
                     (stepEditCtx() || stepEditAccCtx())) {
            seqProbAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  prob ");
              Serial.print(seqProbPct());
              Serial.println("%");
            }
          }
        }
      } else if (ei == 1 && ed != 0) {
        // SEQMODE3: ENC2 = pattern length of the edit sequence (0..64
        // steps, boot default 16; 0 = silent track; per-track RAM only)
        seqPatLenAdjust(ed);
        refreshAllLeds();       // playhead / step view follows the length
        if (logRoom(48)) {
          Serial.print("  len ");
          Serial.println(seqPatLen());
        }
        any = true;
      } else if (ei == 2 && ed != 0) {
        // SEQMODE3: ENC3 = FOLLOW, two-state (CW = ON, CCW = OFF): the
        // track's active steps gate playback, the notes come live from
        // the feeding track's current bar (own pattern length unchanged)
        bool want = (ed > 0);
        if (want != seqFollow()) {
          seqFollowSet(want);
          if (logRoom(48)) {
            Serial.print("  follow ");
            Serial.println(want ? "ON" : "OFF");
          }
        }
        any = true;
      } else if (ei == 7 && ed != 0) {
        long bpm = (long)seqTempo() + ed;
        if (bpm < 40) bpm = 40;
        if (bpm > 240) bpm = 240;
        seqSetTempo((uint16_t)bpm);
        if (logRoom(48)) {
          Serial.print("  tempo ");
          Serial.println(seqTempo());
        }
        any = true;
      } else if (ei == 3 && ed != 0) {
        seqSpeedAdjust(ed);               // SPEED: 1/16 .. 1 .. 4 (no wrap)
        if (logRoom(48)) {
          Serial.print("  speed ");
          Serial.println(seqSpeedName(seqSpeedIdxOf(
              (uint8_t)(seqEditChannel() - 1))));
        }
        any = true;
      } else if (ei == 4 && ed != 0) {
        seqInputChannelAdjust(ed);         // input channel of the edit seq
        if (logRoom(48)) {
          Serial.print("  in ch ");
          Serial.println(seqInputChannel());
        }
        any = true;
      } else if (ei == 5 && ed != 0) {
        seqTrackAdjust(ed);                // TRACK: DRUM/MONO/POLY (no wrap)
        if (seqTrack() != SEQ_TRACK_MONO) arpPageDismiss();  // no arp left
        refreshAllLeds();                  // the step-edit radio follows the track type
        if (logRoom(48)) {
          Serial.print("  track ");
          Serial.println(seqTrackName());
        }
        any = true;
      } else if (ei == 6 && ed != 0 && seqTrack() == SEQ_TRACK_MONO) {
        seqMonoTypeAdjust(ed);             // TYPE: ROOT/BASS/ARP (no wrap)
        if (seqMonoType() != SEQ_MONO_ARP) arpPageDismiss();
        if (logRoom(48)) {
          Serial.print("  mono ");
          Serial.println(seqMonoTypeName());
        }
        any = true;
      }
    }

    if (logRoom(80)) {
      Serial.print("ENC ");
      Serial.print(ei + 1);
      Serial.print(" ");
      Serial.print(ed);
      ModeBinding b = currentBinding();
      Serial.print("  [");
      Serial.print(encSetName(b.enc));
      Serial.print(" ");
      Serial.print(encLabel(b.enc, ei));
      Serial.println("]");
    }
    any = true;
  }
  if (any) refreshMainScreen();
}
