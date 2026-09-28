#include "Events.h"
#include "Buttons.h"
#include "Display.h"
#include "Modes.h"
#include "Encoders.h"
#include "Bindings.h"
#include "SeqClock.h"
#include "Keyboard.h"
#include "Chords.h"

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
      // (KEY = keyboard output, SEQ = sequencer edit, ENC = encoder CCs)
      bool chOverlay = (modeVariant == 3 &&
                        (modeBase == MODE_KEY || modeBase == MODE_SEQ ||
                         modeBase == MODE_ENC));
      // DRUM track: step press = toggle the selected part (voice)
      bool drum = (modeBase == MODE_SEQ && modeVariant != 3 &&
                   seqTrack() == SEQ_TRACK_DRUM);
      // hold M1 = part-select overlay: steps choose the drum voice
      bool partSel = drum && drumPartSelectActive();
      if (chOverlay) {
        if (ev == EV_PRESSED) {
          if (modeBase == MODE_KEY) keySetChannel(sidx + 1);
          else if (modeBase == MODE_SEQ) seqSetEditChannel(sidx + 1);
          else encSetChannel(sidx + 1);
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
      } else if (ev == EV_PRESSED) {
        if (drum) {
          int r = stepDrumToggle(sidx);
          if (r == -2 && logRoom(48)) {
            Serial.println("  part off");
          } else if (r >= 0 && logRoom(48)) {
            Serial.print("  part on ");
            Serial.println(drumPartName());
          }
        } else {
          int n = stepChordPress(sidx);
          if (n > 0 && logRoom(48)) {
            Serial.print("  step record notes=");
            Serial.println(n);
          }
        }
      } else if (ev == EV_CLICKED) {
        if (!drum && stepChordClick(sidx) == 0 && logRoom(48))
          Serial.println("  step disabled");
      } else if (ev == EV_HOLD) {
        if (!drum) {           // DRUM steps are toggles, no assign mode
          stepAssignSet((int8_t)sidx);
          if (logRoom(48)) {
            Serial.print("  step assign ");
            Serial.println(sidx + 1);
          }
        }
      } else if (ev == EV_RELEASED) {
        stepAssignSet(-1);
      }
      refreshAllLeds();   // recording may have toggled the step's dim-green LED
    }

    // PART SELECT (DRUM track): press M1 -> orange overlay on the steps to
    // pick which drum voice the steps sequence (release closes it; the
    // overlay comes up on press so it appears immediately, no 300ms wait)
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
      }
    }

    // KEYMODE2 chord buttons (momentary): M1/M2/M3/NEXT select the quality
    // Maj/Min/Dim/Sus, F1..F4 the extensions 6/m7/M7/9 - press sets,
    // release clears, a sounding chord re-triggers.  M1 in SEQ+DRUM keeps
    // its part-select priority above
    if ((ev == EV_PRESSED || ev == EV_RELEASED) && keyBehaviorVariant() == 2) {
      int8_t ci = -1;
      for (uint8_t i = 0; i < CONTROLS_NUM_G; i++)
        if (groupControls[i] == kl) ci = (int8_t)i;
      bool m1Drum = (ci == 2 && modeBase == MODE_SEQ && modeVariant != 3 &&
                     seqTrack() == SEQ_TRACK_DRUM);
      if (ci > 0 && !m1Drum) {          // ci 0 = PREV, handled below
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

    // PREV = momentary encoder overlay in KEYMODE2: while held the row
    // remaps to Oct/Scale/Root/Latch/KLock/Perf, release restores it
    if (kl == groupControls[0]) {
      if (ev == EV_PRESSED && keyBehaviorVariant() == 2) {
        setOverlay(G_ENC, ENC_KEY2_PREV);
        prevOvActive = true;
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  overlay KEY2 PREV");
      } else if (ev == EV_RELEASED && prevOvActive) {
        clearOverlay(G_ENC);
        prevOvActive = false;
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  overlay off");
      }
    }

    // PLAY in SEQMODE: Start (or Stop while the INT transport runs)
    if (ev == EV_PRESSED && modeBase == MODE_SEQ &&
        ledForPad[pad] == groupTransport[0]) {
      seqTogglePlay();
      if (logRoom(48)) Serial.println("  -> MIDI Play toggle");
    }

    // keyboard pads -> MIDI notes in every mode (behavior defined by KEYMODE,
    // inherited by SEQMODE/ENCMODE); release always clears the key state
    // (even if the mode changed while it was held: no stuck notes)
    if (kidx != 255) {
      if (ev == EV_PRESSED) {
        int n = keyPadPress(kidx);
        if (n >= 0 && logRoom(48)) {
          Serial.print("  note on ");
          Serial.println(n);
        }
        // KEYMODE2: the header shows the chord that just started
        if (n >= 0 && keyBehaviorVariant() == 2 && chordName()[0])
          headerShow(chordName());
      } else if (ev == EV_RELEASED) {
        keyPadRelease(kidx);
      }
      refreshAllLeds();   // first assign key can activate the held step's LED
    }

    if (ev == EV_PRESSED || ev == EV_CLICKED || ev == EV_HOLD || ev == EV_DOUBLE)
      logSets();
  }
}

// ENCMODE knob positions: encoder n sends CC n+1 on the keyboard channel
static int16_t ccVal[8] = {0};

uint8_t encCcValue(uint8_t idx) {
  return (idx < 8) ? (uint8_t)ccVal[idx] : 0;
}

void handleEncoderEvents() {
  uint8_t ei;
  int16_t ed;
  bool any = false;
  for (uint8_t n = 0; n < MAX_EVENTS_PER_LOOP && popEncEvent(ei, ed); n++) {
    // KEYMODE: ENC1 = octave -2..8 (clamped, no wrap; 4 counts = 1 octave)
    if (modeBase == MODE_KEY && ei == 0 && ed != 0) {
      keyOctaveAdjust(ed);
      if (logRoom(48)) {
        Serial.print("  octave ");
        Serial.println(keyOctave());
      }
      any = true;
    }

    // KEYMODE: ENC2 = scale (every variant, and the PREV overlay row);
    // 4 counts = 1 scale, clamped 0..12, no wrap.  Selecting a scale
    // flashes its mood ("Happy", "Sad", ...) in the temporary header
    if (modeBase == MODE_KEY && ei == 1 && ed != 0) {
      keyScaleAdjust(ed);
      headerShow(keyScaleMood());
      refreshAllLeds();             // in-scale / out-of-scale keys repaint
      if (logRoom(48)) {
        Serial.print("  scale ");
        Serial.println(keyScaleName());
      }
      any = true;
    }

    // KEYMODE2: ENC3 = root, ENC4..ENC7 = voicing/bass/spread/density,
    // ENC8 = latch.  With PREV held the row remaps: ENC4 = latch,
    // ENC5 = KeyLock, ENC6 = Perf (state only)
    if (modeBase == MODE_KEY && modeVariant == 2 && ed != 0) {
      ModeBinding b = currentBinding();
      if (ei == 2) {
        keyRootAdjust(ed);
        refreshAllLeds();           // the scale mask is rooted here too
        if (logRoom(48)) {
          Serial.print("  root ");
          Serial.println(keyRootName());
        }
        any = true;
      } else if (b.enc == ENC_KEY2_PREV) {
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
      } else if (ei >= 3 && ei <= 6) {
        chordParamAdjust(ei - 3, ed);   // VOIC / BASS / SPRD / DENS
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
      int16_t v = ccVal[ei] + ed;
      if (v < 0) v = 0;
      if (v > 127) v = 127;
      if (v != ccVal[ei]) {
        ccVal[ei] = v;
        usbMIDI.sendControlChange(ei + 1, (uint8_t)v, encChannel());
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
    // SEQMODE3: ENC4 = SYNC INT/EXT (two-state, no wrap: CW=INT, CCW=EXT)
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
        // nothing else bound in variants 1/2/4 yet
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
        bool want = (ed > 0);              // CW = INT, CCW = EXT (no wrap)
        if (want != seqIsSyncInternal()) {
          seqSetSyncInternal(want);
          if (logRoom(48)) {
            Serial.print("  sync ");
            Serial.println(seqIsSyncInternal() ? "INT" : "EXT");
          }
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
        if (logRoom(48)) {
          Serial.print("  track ");
          Serial.println(seqTrackName());
        }
        any = true;
      } else if (ei == 6 && ed != 0 && seqTrack() == SEQ_TRACK_MONO) {
        seqMonoTypeAdjust(ed);             // TYPE: ROOT/BASS/ARP (no wrap)
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
