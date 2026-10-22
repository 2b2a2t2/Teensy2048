#include "Display.h"
#include "Encoders.h"
#include "Bindings.h"
#include "SeqClock.h"
#include "Modes.h"
#include "Keyboard.h"
#include "Events.h"
#include "Chords.h"
#include "Gen.h"         // GNR8 page values (fill/acc/glide/tie)
#include "Key4.h"       // KEYMODE4 genre/knob values + control legend
#include "Skank.h"      // SKANK page: shape name (LIN/PP/RND)
#include <Wire.h>

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

static char headerLeft[16] = "";
static char headerText[24] = "";
static unsigned long headerDeadline = 0;
static bool headerActive = false;
static char headerStick[24] = "";
static bool headerStickOn = false;
static bool displayDirty = true;

// erase only the header band (y 0..15) in the full framebuffer
void clearHeader() {
  u8g2.setDrawColor(0);
  u8g2.drawBox(0, 0, SCREEN_W, HEADER_H);
  u8g2.setDrawColor(1);
}

// erase only the main band (y 16..63) in the full framebuffer
void clearMainScreen() {
  u8g2.setDrawColor(0);
  u8g2.drawBox(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H);
  u8g2.setDrawColor(1);
  displayDirty = true;
}

// main screen layout:
//  encoders grid 4x4:  label/value rows at y16,24  (enc1-4)
//                               and y32,40         (enc5-8)
//  control buttons grid: row1 y54  PREV/NEXT/M1/M2/M3
//                        row2 y62  the F row - labels follow the context

// context-sensitive legend: what each control does RIGHT NOW
// (i 0..4 = PREV NEXT M1 M2 M3 top row, i 5..8 = F row below)
static const char* ctrlLegend(uint8_t i) {
  if (i > 8) return "";
  bool key2 = keyBehaviorVariant() == 2;   // quality / extension momentaries
  bool key4 = (modeBase == MODE_KEY && modeVariant == 4);  // genre row
  if (i >= 5) {
    // SEQMODE + strum ADDS row (open with M2 on a POLY STRUM/HAND track):
    // F1..F4 = sus4 / add6 / add9 / clear - wins over the step-edit legend
    if (strumFRowActive()) {
      static const char* ad[4] = { "sus4", "add6", "add9", "off" };
      return ad[i - 5];
    }
    // SEQMODE + GNR8 page (open with M2 on a MONO BASS track): F1 = the
    // full OPTS page, F2 = per-step OCT + MAP, F3 = per-step BRW,
    // F4 = per-step MOD PROB - wins over the step-edit legend
    if (genPageActive()) {
      static const char* gn[4] = { "OPTS", "OCT", "BRW", "PROB" };
      return gn[i - 5];
    }
    // SEQMODE + MONO step-edit: F1-F4 select TIE/GLIDE/ACCENT/PROB
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        seqTrack() == SEQ_TRACK_MONO) {
      static const char* ed[4] = { "TIE", "GLIDE", "ACC", "PROB" };
      return ed[i - 5];
    }
    // SEQMODE + DRUM: only F3/F4 edit (accent / probability)
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        seqTrack() == SEQ_TRACK_DRUM && i >= 7) {
      static const char* ed[2] = { "ACC", "PROB" };
      return ed[i - 7];
    }
    // SEQMODE + POLY: F1 = FX (sus/ext/color per step), F2 = TYPE
    // (STEP/STRUM/HAND override per step), F3/F4 edit accent / probability
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        seqTrack() == SEQ_TRACK_POLY) {
      static const char* ed[4] = { "FX", "TYPE", "ACC", "PROB" };
      return ed[i - 5];
    }
    // ENCMODE: F1-F4 = the ENC5..8 CC-bank radio
    if (modeBase == MODE_ENC) {
      static const char* fr[4] = { "F1", "F2", "F3", "F4" };
      return fr[i - 5];
    }
    if (key4) {          // KEYMODE4: VAR / LOCK / LEAD / HOLD
      static const char* k4r2[4] = { "VAR", "LOCK", "LEAD", "HOLD" };
      return k4r2[i - 5];
    }
    if (!key2) {
      static const char* row2[4] = { "M1", "M2", "M3", "M4" };
      return row2[i - 5];
    }
    static const char* ext[4] = { "6", "m7", "M7", "9" };
    return ext[i - 5];
  }
  // KEYMODE4 advertises the EMB menu on the NEXT key itself
  if (i == 1) return key2 ? "Sus" : (key4 ? "EMB" : "NEXT");
  if (i == 2) {
    // hold M1 = DRUM part-select (beats the KEYMODE2 quality, like Events);
    // SEQMODE+MONO = step-edit radio; ENCMODE hold M1 = drum-VOICE mute
    if (modeBase == MODE_SEQ && modeVariant != 3 && seqTrack() == SEQ_TRACK_DRUM)
      return "PART";
    if (modeBase == MODE_SEQ && modeVariant != 3 && seqTrack() == SEQ_TRACK_MONO)
      return "EDIT";
    if (modeBase == MODE_ENC) return "DRUM";
    if (key4) return "MODE";
    return key2 ? "Maj" : "M1";
  }
  if (i == 3) {
    // hold M2 = ENCMODE channel mute; SEQMODE3 click = erase the track;
    // SEQMODE 1/2/4 + MONO/ARP = the ARP encoder page, + MONO/BASS = the
    // GNR8 page (all keep priority over the quality key, same as the handler)
    if (modeBase == MODE_ENC) return "MUTE";
    if (modeBase == MODE_SEQ && modeVariant == 3) return "ERASE";
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        seqTrack() == SEQ_TRACK_MONO && seqMonoType() == SEQ_MONO_ARP)
      return "ARP";
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        seqTrack() == SEQ_TRACK_MONO && seqMonoType() == SEQ_MONO_BASS)
      return "GNR8";
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        seqTrack() == SEQ_TRACK_MONO && seqMonoType() == SEQ_MONO_LEAD)
      return "LEAD";
    if (key4) return "AUTO";
    return key2 ? "Min" : "M2";
  }
  if (i == 4) {
    // GNR8/LEAD track: M3 = generate (tap) / undo (hold), like KEYMODE4 APPLY
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        seqTrack() == SEQ_TRACK_MONO &&
        (seqMonoType() == SEQ_MONO_BASS || seqMonoType() == SEQ_MONO_LEAD))
      return "GEN";
    // ENCMODE M3 = the ENC5..8 page-group toggle (CC1..16 / CC21..36)
    if (modeBase == MODE_ENC) return "PAGE";
    if (key4) return "APPLY";
    return key2 ? "Dim" : "M3";
  }
  return "PREV";
}

void refreshMainScreen() {
  clearMainScreen();
  u8g2.setFont(u8g2_font_4x6_tr);

  ModeBinding b = currentBinding();
  char val[12];
  char ccL2[8];              // ENCMODE row-2 CC labels (follow the F bank)
  // SEQMODE3 encoders show state, not the raw shared counter
  bool seq3 = (modeBase == MODE_SEQ && modeVariant == 3);
  // SEQMODE + MONO + radio on: ENC5/6/8 shape the track (gate/vel/prob)
  bool seqGe = (modeBase == MODE_SEQ && modeVariant != 3 &&
                seqTrack() == SEQ_TRACK_MONO && stepEditIsOn());
  // SEQMODE + M2 arp page: ENC2/3/4/7 take span/skip/mode/insert, the
  // other four keep their knobs (ENC1 octave, 5/6/8 gate/vel/prob)
  bool arpP = arpPageActive();
  uint8_t arpSeq = (uint8_t)(seqEditChannel() - 1);
  // SEQMODE + M2 GNR8 page: ENC2/3/4/7 take fill/accent/glide/tie, the
  // other four keep their knobs (ENC1 octave, 5/6/8 gate/vel/prob)
  bool genP = genPageActive();
  uint8_t genSeq = (uint8_t)(seqEditChannel() - 1);
  // SEQMODE + M2 LEAD page: ENC2/3/4/7 take contour/low/range/density,
  // the other four keep their knobs (ENC1 octave, 5/6/8 gate/vel/prob)
  bool leadP = leadPageActive();
  uint8_t leadSeq = (uint8_t)(seqEditChannel() - 1);
  // SEQMODE + M2 HARM page (POLY TYPE=HARM): ENC1..4 take genre/fill/src/
  // hold, the other four keep their knobs
  bool harmP = harmPageActive();
  // SEQMODE + M2 COMP page (POLY TYPE=STEP): ENC1 = HITS, the rest idle
  bool compP = compPageActive();
  // SEQMODE + M2 SKANK page (POLY TYPE=SKANK): ENC1..4 take TES/MOT/SHP/
  // THN, ENC5..8 take EXT/SUB/LEN/SPEED (the whole row is the page)
  bool skankP = skankPageActive();
  // ...or a held step on a POLY+SKANK track arms the per-step lock row
  // (encoders 1..6 override that step's knob, 8 = TRANS)
  bool skankStepOv = (b.enc == ENC_SEQ_SKANK_STEP);
  // ...and with PREV held the SHIFT/TRANS row takes the encoders (ENC1 =
  // rotate, ENC2 = semitone transpose).  The GNR8 page reuses this same
  // overlay, so the rotate value below still applies there
  bool genOv = (b.enc == ENC_SEQ_ROT_OV);
  // ...or a held step in SEQMODE (non-DRUM) arms the per-step TRANS overlay
  bool seqTrOv = (b.enc == ENC_SEQ_STEPOV);
  // either SEQ-page overlay wins: suppress the M2-page label overrides so
  // SHIFT/TRANS (or the held-step TRANS / SKANK locks) keeps the row
  bool seqOv = genOv || seqTrOv || skankStepOv;
  // ...or with an F radio armed the whole row is the option sub-page
  // (F1 = the full page: TONES/RNG/LOW/ALGO on ENC1-4; F2 = OCT on
  // ENC2 + MAP on ENC3, F3 = BRW on ENC3, F4 = PROB on ENC4 - the rest
  // idle; the rotate row still wins whenever PREV is held)
  uint8_t gSub = (genP && !seqOv) ? genOptionSel() : 0xFF;
  // SEQMODE + DRUM/POLY + radio on: only ENC8 (track probability) is bound
  bool seqDrumGe = (modeBase == MODE_SEQ && modeVariant != 3 &&
                    seqTrack() != SEQ_TRACK_MONO && stepEditIsOn());
  // KEYMODE2 rows: the main chord row, or the PREV overlay row
  bool key2 = (modeBase == MODE_KEY && modeVariant == 2 && b.enc != ENC_KEY2_PREV);
  bool keyOv = (b.enc == ENC_KEY2_PREV);
  // KEYMODE4: the genre row, or its OCT/SCALE/KEY overlay
  bool key4 = (modeBase == MODE_KEY && modeVariant == 4 && b.enc == ENC_KEY4);
  bool key4Ov = (b.enc == ENC_KEY4_PREV);
  // ...or its sticky EMB menu (EMB / PLACE)
  bool key4Emb = (b.enc == ENC_KEY4_EMB);
  // ...or the held-step row (OCT/DEG/INV/VOICE editing ONE step)
  bool key4Step = (b.enc == ENC_KEY4_STEP);

  for (uint8_t c = 0; c < 4; c++) {
    uint8_t x = c * 32 + 1;
    const char *l1 = encLabel(b.enc, c);
    if (arpP && !seqOv && c >= 1 && c <= 3) {
      static const char *al[4] = { "OCT", "SPAN", "SKIP", "MODE" };
      l1 = al[c];
    }
    if (genP && !seqOv && c >= 1 && c <= 3) {
      static const char *gl[4] = { "OCT", "FILL", "ACC", "GLIDE" };
      l1 = gl[c];
    }
    if (leadP && !seqOv && c >= 1 && c <= 3) {
      static const char *ll[4] = { "OCT", "CONTOUR", "LOW", "RANGE" };
      l1 = ll[c];
    }
    if (harmP && !seqOv) {
      static const char *hl[4] = { "GENRE", "FILL", "SRC", "HOLD" };
      l1 = hl[c];
    }
    if (compP && !seqOv) {
      static const char *cl[4] = { "HITS", "---", "---", "---" };
      l1 = cl[c];
    }
    if (skankP && !seqOv) {
      static const char *kl[4] = { "TES", "MOT", "SHP", "THN" };
      l1 = kl[c];
    }
    if (key4Emb) {
      static const char *el[4] = { "EMB", "PLACE", "---", "---" };
      l1 = el[c];
    }
    if (gSub == 0) {             // F1 full page: the four option knobs
      static const char *fl[4] = { "TONES", "RNG", "LOW", "ALGO" };
      l1 = fl[c];
    } else if (gSub != 0xFF) {   // F2..F4 mod pages: which knob binds
      static const char *mOc[4] = { "---", "OCT", "MAP", "---" };
      static const char *mBr[4] = { "---", "---", "BRW", "---" };
      static const char *mPb[4] = { "---", "---", "---", "PROB" };
      l1 = (gSub == 1) ? mOc[c] : (gSub == 2) ? mBr[c] : mPb[c];
    }
    u8g2.drawStr(x, 22, l1);
    if (genOv) {
      // SHIFT/TRANS row: ENC1 = the rotation position, ENC2 = the track's
      // semitone transpose, the rest stay idle
      if (c == 0) snprintf(val, sizeof(val), "%u", seqGenShiftPos() + 1);
      else if (c == 1) {
        int tv = seqTrans();
        if (tv == 0) snprintf(val, sizeof(val), "0");
        else snprintf(val, sizeof(val), "%+d", tv);
      } else snprintf(val, sizeof(val), "");
    } else if (seqTrOv) {
      // held-step TRANS row: ENC1 = that step's offset (0 = "0")
      if (c == 0) {
        int8_t tv = stepTrAt((uint8_t)seqTrStepSelGet());
        if (tv == 0) snprintf(val, sizeof(val), "0");
        else snprintf(val, sizeof(val), "%+d", tv);
      } else snprintf(val, sizeof(val), "");
    } else if (skankStepOv) {
      // SKANK lock row: that step's override, "inherit" = the page knob
      int8_t ss = seqTrStepSelGet();
      int16_t lk = (ss >= 0) ? stepSkankLockAt((uint8_t)ss, (uint8_t)c) : -1;
      if (lk < 0) snprintf(val, sizeof(val), "inherit");
      else snprintf(val, sizeof(val), "%d", lk);
    } else if (gSub == 0) {
      // F1 full options page: ENC1 TONES, ENC2 RANGE, ENC3 LOW, ENC4 ALGO
      if (c == 0) {
        snprintf(val, sizeof(val), "%s",
                 seqGenTonesName(seqGenTonesOf(genSeq)));
      } else if (c == 1) {
        snprintf(val, sizeof(val), "%uoct", seqGenRangeOf(genSeq));
      } else if (c == 2) {
        snprintf(val, sizeof(val), "%s",
                 seqGenLowName(seqGenLowOf(genSeq)));
      } else {
        snprintf(val, sizeof(val), "%s",
                 seqGenAlgoName(seqGenAlgoOf(genSeq)));
      }
    } else if (gSub != 0xFF) {
      // F2..F4 mod pages: the selected step's value (nothing picked yet
      // reads "pick"); MAP on F2/ENC3 needs no selection
      int8_t ss = genStepSelStep();
      if (gSub == 1) {
        if (c == 1) {
          if (ss < 0) snprintf(val, sizeof(val), "pick");
          else {
            int ov = stepOctAt((uint8_t)ss);
            if (ov == 0) snprintf(val, sizeof(val), "0");
            else snprintf(val, sizeof(val), "%+d", ov);
          }
        } else if (c == 2) {
          snprintf(val, sizeof(val), "%s", seqMapName(seqMap()));
        } else snprintf(val, sizeof(val), "");
      } else if (gSub == 2) {
        if (c == 2) {
          if (ss < 0) snprintf(val, sizeof(val), "pick");
          else if (stepBrwAt((uint8_t)ss) == 0)
            snprintf(val, sizeof(val), "off");
          else snprintf(val, sizeof(val), "%u", stepBrwAt((uint8_t)ss));
        } else snprintf(val, sizeof(val), "");
      } else {
        if (c == 3) {
          if (ss < 0) snprintf(val, sizeof(val), "pick");
          else snprintf(val, sizeof(val), "%u%%", stepModPAt((uint8_t)ss));
        } else snprintf(val, sizeof(val), "");
      }
    } else if (skankP && c == 0) {
      snprintf(val, sizeof(val), "%u", skankPrm(0));         // ENC1 = TES
    } else if (skankP && c == 1) {
      snprintf(val, sizeof(val), "%u", skankPrm(1));         // ENC2 = MOT
    } else if (skankP && c == 2) {
      snprintf(val, sizeof(val), "%s",
               skankShpName(skankPrm(2)));                   // ENC3 = SHP
    } else if (skankP && c == 3) {
      snprintf(val, sizeof(val), "%u", skankPrm(3));         // ENC4 = THN
    } else if (modeBase == MODE_SEQ && c == 0) {
      snprintf(val, sizeof(val), "%d", seqOctaveShift()); // ENC1 = octave shift
    } else if (genP && c == 1) {
      snprintf(val, sizeof(val), "%u%%", seqGenFillOf(genSeq));   // ENC2
    } else if (genP && c == 2) {
      snprintf(val, sizeof(val), "%u%%", seqGenAccOf(genSeq));    // ENC3
    } else if (genP && c == 3) {
      snprintf(val, sizeof(val), "%u%%", seqGenGlideOf(genSeq));  // ENC4
    } else if (arpP && c == 1) {
      snprintf(val, sizeof(val), "%u",
               seqArpSpanOf(arpSeq));           // ENC2 = span (octaves)
    } else if (arpP && c == 2) {
      snprintf(val, sizeof(val), "%s",
               seqArpSkipOf(arpSeq) ? "ON" : "OFF");   // ENC3 = skip rests
    } else if (arpP && c == 3) {
      snprintf(val, sizeof(val), "%s",
               seqArpModeName(seqArpModeOf(arpSeq)));  // ENC4 = mode
    } else if (leadP && c == 1) {
      snprintf(val, sizeof(val), "%s",
               seqLeadContourName(seqLeadContourOf(leadSeq))); // ENC2 contour
    } else if (leadP && c == 2) {
      snprintf(val, sizeof(val), "%s",
               seqGenLowName(seqGenLowOf(leadSeq)));          // ENC3 = LOW
    } else if (leadP && c == 3) {
      snprintf(val, sizeof(val), "%uoct",
               seqGenRangeOf(leadSeq));                       // ENC4 = RANGE
    } else if (harmP && c == 0) {
      snprintf(val, sizeof(val), "%s", k4GenreShort());       // ENC1 = genre
    } else if (harmP && c == 1) {
      snprintf(val, sizeof(val), "%u%%",
               seqGenFillOf((uint8_t)(seqEditChannel() - 1))); // ENC2 = fill
    } else if (harmP && c == 2) {
      snprintf(val, sizeof(val), "%s", k4HarmInputName());    // ENC3 = src
    } else if (harmP && c == 3) {
      snprintf(val, sizeof(val), "%s", k4HarmHoldName());     // ENC4 = hold
    } else if (compP && c == 0) {
      int h = seqCompHits();                                  // ENC1 = hits
      if (h <= 1) snprintf(val, sizeof(val), "off");
      else snprintf(val, sizeof(val), "%d", h);
    } else if (seq3 && c == 1) {
      snprintf(val, sizeof(val), "%d", seqPatLen());      // ENC2 = steps
    } else if (seq3 && c == 2) {
      snprintf(val, sizeof(val), "%s", seqFollow() ? "ON" : "OFF");
    } else if (seq3 && c == 3) {
      snprintf(val, sizeof(val), "%s",
               seqSpeedName(seqSpeedIdxOf((uint8_t)(seqEditChannel() - 1))));
    } else if (key4 && c == 0) {
      snprintf(val, sizeof(val), "%s", k4GenreShort()); // ENC1 = genre
    } else if (key4 && c == 1) {
      snprintf(val, sizeof(val), "%u", k4Knob(0));      // ENC2 = MOOD
    } else if (key4 && c == 2) {
      snprintf(val, sizeof(val), "%u", k4Knob(1));      // ENC3 = TENS
    } else if (key4 && c == 3) {
      snprintf(val, sizeof(val), "%u", k4Knob(2));      // ENC4 = MOTION
    } else if (key4Ov && c == 3) {
      snprintf(val, sizeof(val), "%s",
               chordFScale() ? "ON" : "OFF");       // overlay ENC4 = FScale
    } else if (key4Ov && c == 4) {
      snprintf(val, sizeof(val), "%u", k4EmbSubProb());  // overlay EN5 = SUB
    } else if (key4Ov && c == 5) {
      snprintf(val, sizeof(val), "%u", k4EmbInvBias());  // overlay EN6 = INV
    } else if (key4Ov && c == 6) {
      snprintf(val, sizeof(val), "%u", k4EmbPassAmt());  // overlay EN7 = PAS
    } else if (key4Ov && c == 7) {
      snprintf(val, sizeof(val), "%u", k4EmbColorAmt()); // overlay EN8 = COL
    } else if (key4Emb && c == 0) {
      snprintf(val, sizeof(val), "%s", k4EmbModeName());  // ENC1 = EMB
    } else if (key4Emb && c == 1) {
      snprintf(val, sizeof(val), "%s", k4EmbPlaceName()); // ENC2 = PLACE
    } else if (key4Emb) {
      snprintf(val, sizeof(val), "");                   // ENC3..8 idle
    } else if (key4Step) {
      // held-step values: the step's own OCT / degree / inversion / spread
      int8_t ss = k4StepSelGet();
      char dgs[8], ivs[8];
      bool has = (ss >= 0 &&
                  k4StepLabels((uint8_t)ss, dgs, sizeof(dgs),
                               ivs, sizeof(ivs)));
      if (!has) {
        snprintf(val, sizeof(val), "");                 // no step / empty
      } else if (c == 0) {
        int ov = stepOctAt((uint8_t)ss);                   // ENC1 = octave
        if (ov == 0) snprintf(val, sizeof(val), "0");
        else snprintf(val, sizeof(val), "%+d", ov);
      } else if (c == 1) {
        snprintf(val, sizeof(val), "%s", dgs);             // ENC2 = degree
      } else if (c == 2) {
        snprintf(val, sizeof(val), "%s", ivs);             // ENC3 = inversion
      } else {
        // ENC4 = voice: octave spread between bass and top ("close" when
        // they sit inside one octave)
        uint8_t n = 0;
        const uint8_t *nt = stepChordAt((uint8_t)ss, &n);
        uint8_t lo = 0, hi = 0;
        for (uint8_t i = 0; i < n; i++) {
          if (i == 0 || nt[i] < lo) lo = nt[i];
          if (i == 0 || nt[i] > hi) hi = nt[i];
        }
        uint8_t sp = (uint8_t)((hi - lo) / 12);
        if (sp == 0) snprintf(val, sizeof(val), "close");
        else snprintf(val, sizeof(val), "%uoct", sp);
      }
    } else if (modeBase == MODE_KEY && c == 0) {
      snprintf(val, sizeof(val), "%d", keyOctave());         // ENC1 = octave
    } else if (modeBase == MODE_KEY && c == 1) {
      snprintf(val, sizeof(val), "%s", keyScaleName());      // ENC2 = scale
    } else if (modeBase == MODE_KEY &&
               (modeVariant == 1 || modeVariant == 2 || key4Ov) && c == 2) {
      snprintf(val, sizeof(val), "%s", keyRootName());       // ENC3 = root
    } else if (keyOv && c == 3) {
      snprintf(val, sizeof(val), "%s", chordLatch() ? "ON" : "OFF");
    } else if (key2 && c == 3) {
      snprintf(val, sizeof(val), "%s", chordFScale() ? "ON" : "OFF"); // ENC4
    } else if (modeBase == MODE_ENC) {
      snprintf(val, sizeof(val), "%u", encCcValue(c));       // CC value
    } else if (l1[0] == '-' && l1[1] == '-') {
      snprintf(val, sizeof(val), "");      // "--" legend: no position noise
    } else {
      snprintf(val, sizeof(val), "%ld", encPosition(c));
    }
    u8g2.drawStr(x, 30, val);
    // row-2 labels follow the step-edit bindings while the radio is on
    // (the idle default row would contradict the live values)
    const char* l2 = encLabel(b.enc, 4 + c);
    if (seqGe) {
      static const char* sl[4] = { "GATE", "VEL", "---", "PROB" };
      l2 = sl[c];
    } else if (seqDrumGe) {
      static const char* sl[4] = { "---", "---", "---", "PROB" };
      l2 = sl[c];
    } else if (modeBase == MODE_ENC) {
      // ENC5..8 follow the F1..F4 CC bank in the active M3 page group
      snprintf(ccL2, sizeof(ccL2), "CC%u", encCcNum(4 + c));
      l2 = ccL2;
    }
    if (c == 1 && f3AccentActive()) l2 = "ACC";   // F3 held: ENC6 = accent
    if (arpP && 4 + c == 6) l2 = "INST";          // arp page: ENC7 = insert
    if (genP && 4 + c == 6) l2 = "TIE";           // gen page: ENC7 = tie %
    if (leadP && 4 + c == 6) l2 = "DENS";         // lead page: ENC7 = density
    if (genOv) l2 = "---";                        // SHIFT/TRANS row: idle
    if (seqTrOv) l2 = "---";                      // held-step TRANS: idle
    if (seqDrumGe && 4 + c == 6 && stepEditSel() == 5)
      l2 = "FX";                                  // POLY edit: ENC7 = FX type
    if (seqDrumGe && 4 + c == 6 && stepEditSel() == 6)
      l2 = "TYPE";                                // POLY edit: ENC7 = TYPE arm
    if (seqDrumGe && 4 + c <= 5 && stepEditSel() == 7)
      l2 = (c == 0) ? "SUB" : (c == 1) ? "INV" : (c == 2) ? "PAS" : "COL"; // POLY edit: ENC5-8 = EMB params
    if (gSub != 0xFF) l2 = "---";            // option sub-page: row 2 idle
    if (skankStepOv) {          // lock row: EXT/SUB, TES% lock, TRANS (wins
      static const char *s2[4] = { "EXT", "SUB", "TES%", "TRANS" }; // over radio)
      l2 = s2[c];
    }
    if (skankP && !seqOv) {     // SKANK page: the whole second row follows
      static const char *s2[4] = { "EXT", "SUB", "TES%", "NOT%" };
      l2 = s2[c];
    }
    u8g2.drawStr(x, 38, l2);
    if (genOv) {
      snprintf(val, sizeof(val), "");              // SHIFT/TRANS row idle
    } else if (seqTrOv) {
      snprintf(val, sizeof(val), "");              // held-step TRANS idle
    } else if (skankStepOv) {
      // lock row: EXT / SUB / TES% override, ENC8 = the step's TRANS
      int8_t ss = seqTrStepSelGet();
      if (c <= 2) {
        int16_t lk = (ss >= 0)
                         ? stepSkankLockAt((uint8_t)ss, (uint8_t)(c + 4))
                         : -1;
        if (lk < 0) snprintf(val, sizeof(val), "inherit");
        else if (c == 2) snprintf(val, sizeof(val), "%d%%", lk);
        else snprintf(val, sizeof(val), "%d", lk);
      } else if (c == 3) {
        int8_t tv = (ss >= 0) ? stepTrAt((uint8_t)ss) : 0;
        if (tv == 0) snprintf(val, sizeof(val), "0");
        else snprintf(val, sizeof(val), "%+d", tv);
      } else snprintf(val, sizeof(val), "");
    } else if (gSub != 0xFF) {
      snprintf(val, sizeof(val), "");        // option sub-page: idle
    } else if (c == 1 && f3AccentActive()) {
      snprintf(val, sizeof(val), "%d", seqAccentVelocity()); // ENC6 = acc vel
    } else if (arpP && 4 + c == 6) {
      snprintf(val, sizeof(val), "%s",
               seqArpInstName(seqArpInstOf(arpSeq)));  // ENC7 = insert mode
    } else if (genP && 4 + c == 6) {
      snprintf(val, sizeof(val), "%u%%", seqGenTieOf(genSeq));  // ENC7 tie
    } else if (leadP && 4 + c == 6) {
      snprintf(val, sizeof(val), "%u%%", seqGenFillOf(leadSeq)); // ENC7 dens
    } else if (skankP && 4 + c == 4) {
      snprintf(val, sizeof(val), "%u%%", skankPrm(4));      // ENC5 = EXT
    } else if (skankP && 4 + c == 5) {
      snprintf(val, sizeof(val), "%u%%", skankPrm(5));      // ENC6 = SUB
    } else if (skankP && 4 + c == 6) {
      snprintf(val, sizeof(val), "%u%%", skankPrm(6));      // ENC7 = TES%
    } else if (skankP && 4 + c == 7) {
      snprintf(val, sizeof(val), "%u%%", skankPrm(7));      // ENC8 = NOT%
    } else if (seq3 && 4 + c == 4) {
      snprintf(val, sizeof(val), "%u", seqInputChannel()); // ENC5 = input ch
    } else if (seq3 && 4 + c == 5) {
      snprintf(val, sizeof(val), "%s", seqTrackName());   // ENC6 = TRACK
    } else if (seq3 && 4 + c == 6 && seqTrack() == SEQ_TRACK_MONO) {
      snprintf(val, sizeof(val), "%s", seqMonoTypeName()); // ENC7 = TYPE
    } else if (seq3 && 4 + c == 6 && seqTrack() == SEQ_TRACK_POLY) {
      snprintf(val, sizeof(val), "%s", seqPolyTypeName()); // ENC7 = TYPE
    } else if (seq3 && 4 + c == 7) {
      snprintf(val, sizeof(val), "%u", seqTempo());         // ENC8 = BPM
    } else if (seqGe && 4 + c == 4) {
      snprintf(val, sizeof(val), "%d%%", seqGatePct());     // ENC5 = gate
    } else if (seqGe && 4 + c == 5) {
      snprintf(val, sizeof(val), "%d", seqVelocity());      // ENC6 = vel
    } else if (seqGe && 4 + c == 6) {
      snprintf(val, sizeof(val), "");                    // ENC7 = free
    } else if (seqGe && 4 + c == 7) {
      snprintf(val, sizeof(val), "%d%%", seqProbPct());  // ENC8 = track probability
    } else if (seqDrumGe && 4 + c == 4) {
      snprintf(val, sizeof(val), "");                    // ENC5 free
    } else if (seqDrumGe && 4 + c == 5) {
      snprintf(val, sizeof(val), "");                    // ENC6 free
    } else if (seqDrumGe && 4 + c == 6) {
      if (stepEditSel() == 7) {
        // EMB target: show SUB/INV/PAS/COL for focused step
        static const char* embLabels[4] = { "SUB", "INV", "PAS", "COL" };
        uint8_t p = stepEmbGet(k4Focus(), c);
        if (p == 255) p = (c == 0) ? k4EmbSubProb() : (c == 1) ? k4EmbInvBias()
            : (c == 2) ? k4EmbPassAmt() : k4EmbColorAmt();
        snprintf(val, sizeof(val), "%s=%u", embLabels[c], p);
      } else {
        snprintf(val, sizeof(val), "%s",
                 stepEditSel() == 5 ? stepFxSelName()
                 : stepEditSel() == 6 ? stepTypeArmName() : ""); // ENC7
      }
    } else if (seqDrumGe && 4 + c == 7) {
      snprintf(val, sizeof(val), "%d%%", seqProbPct());  // ENC8 = track probability
    } else if (key4 && 4 + c == 4) {
      snprintf(val, sizeof(val), "%u", k4Knob(3));          // CAD
    } else if (key4 && 4 + c == 5) {
      snprintf(val, sizeof(val), "%u", k4Knob(4));          // CPX
    } else if (key4 && 4 + c == 6) {
      snprintf(val, sizeof(val), "%u", k4Knob(5));          // ERA
    } else if (key4 && 4 + c == 7) {
      snprintf(val, sizeof(val), "%u", k4Knob(6));          // SURP
    } else if (key4Ov || key4Emb) {
      snprintf(val, sizeof(val), "");          // overlay row 2 idle
    } else if (keyOv && 4 + c == 4) {
      snprintf(val, sizeof(val), "%s", chordKeyLock() ? "ON" : "OFF");
    } else if (keyOv && 4 + c == 5) {
      snprintf(val, sizeof(val), "%s", chordPerf() ? "ON" : "OFF");
    } else if (key2 && 4 + c == 4) {
      snprintf(val, sizeof(val), "%u", chordParam(1));      // ENC5 = bass
    } else if (key2 && 4 + c == 5) {
      snprintf(val, sizeof(val), "%u", chordParam(2));      // ENC6 = spread
    } else if (key2 && 4 + c == 6) {
      snprintf(val, sizeof(val), "%u", chordParam(3));      // ENC7 = density
    } else if (key2 && 4 + c == 7) {
      snprintf(val, sizeof(val), "%s", chordLatch() ? "ON" : "OFF"); // ENC8
    } else if (modeBase == MODE_ENC) {
      snprintf(val, sizeof(val), "%u", encCcValue(4 + c)); // CC value
    } else if (l2[0] == '-' && l2[1] == '-') {
      snprintf(val, sizeof(val), "");      // "--" legend: no position noise
    } else {
      snprintf(val, sizeof(val), "%ld", encPosition(4 + c));
    }
    u8g2.drawStr(x, 46, val);
  }

  for (uint8_t c = 0; c < 5; c++) {
    uint8_t x = c * 25 + 1;
    u8g2.drawStr(x, 54, ctrlLegend(c));
    const char* r2 = ctrlLegend(5 + c);
    if (r2[0]) u8g2.drawStr(x, 62, r2);
  }

  displayDirty = true;
}

void drawHeaderText() {
  u8g2.setFont(u8g2_font_6x12_tf);   // header always uses its own font
  clearHeader();
  // SEQMODE step-edit: while an attribute target is engaged, the left
  // header slot shows it instead of the mode name (TIE/GLIDE/ACCENT/PROB
  // on MONO, FX/TYPE/ACCENT/PROB on POLY); an open ARP page wins over both
  // so the rebind is always visible
  const char* left = headerLeft;
  if (genPageActive()) {
    left = "GNR8";               // the generator page owns the encoder row
  } else if (arpPageActive()) {
    left = "ARP";
  } else if (strumFRowActive()) {
    left = "EXT";               // strum ADDS row: the F row edits colors
  } else if (stepEditActive()) {
    static const char* ed[4] = { "TIE", "GLIDE", "ACCENT", "PROB" };
    if (stepEditSel() == 5) left = "FX";
    else if (stepEditSel() == 6) left = "TYPE";
    else if (stepEditSel() == 7) left = "EMB";
    else left = ed[stepEditSel() - 1];
  }
  // a transient message owns the whole strip: long strings (the KEY4
  // progression) right-align over the left slot, so drop the label
  // instead of drawing it underneath.  Without a transient the persistent
  // right text (the KEY4 progression) stays up; the left label joins it
  // only when the two fit side by side
  if (headerActive && headerText[0]) {
    int w = u8g2.getStrWidth(headerText);
    u8g2.drawStr(SCREEN_W - w - 2, 13, headerText);
  } else if (headerStickOn && headerStick[0]) {
    int w = u8g2.getStrWidth(headerStick);
    int r = SCREEN_W - w - 2;
    if (r < 0) r = 0;
    u8g2.drawStr(r, 13, headerStick);
    if (left[0] && 2 + u8g2.getStrWidth(left) + 2 <= r)
      u8g2.drawStr(2, 13, left);
  } else if (left[0]) {
    u8g2.drawStr(2, 13, left);
  }
}

void headerSetLeft(const char* text) {
  strncpy(headerLeft, text, sizeof(headerLeft) - 1);
  headerLeft[sizeof(headerLeft) - 1] = '\0';
  drawHeaderText();
  displayDirty = true;
}

// top strip only: the left slot tracks state that can change without a
// mode switch (ARP page closed, radio target reset on a channel change)
void refreshHeader() {
  drawHeaderText();
  displayDirty = true;
}

void headerShow(const char* name) {
  strncpy(headerText, name, sizeof(headerText) - 1);
  headerText[sizeof(headerText) - 1] = '\0';
  headerDeadline = millis() + HEADER_MS;
  headerActive = true;
  drawHeaderText();
  displayDirty = true;
}

void headerSetStick(const char* text) {
  strncpy(headerStick, text, sizeof(headerStick) - 1);
  headerStick[sizeof(headerStick) - 1] = '\0';
  headerStickOn = headerStick[0] != '\0';
  drawHeaderText();
  displayDirty = true;
}

void headerStickClear() {
  if (!headerStickOn) return;
  headerStickOn = false;
  headerStick[0] = '\0';
  drawHeaderText();
  displayDirty = true;
}

void headerHide() {
  if (!headerActive) return;
  headerActive = false;
  drawHeaderText();
  displayDirty = true;
}

void displayMarkDirty() {
  displayDirty = true;
}

void initDisplay() {
  u8g2.begin();
  Wire.setClock(400000);
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.setDrawColor(0);
  u8g2.drawBox(0, 0, SCREEN_W, SCREEN_H);
  u8g2.setDrawColor(1);
  refreshMainScreen();
  u8g2.sendBuffer();
  displayDirty = false;
}

// SSD1306 paged write: one 128-byte page (~4ms @400kHz) per call so the MIDI
// pump runs between pages — a monolithic sendBuffer stalls ~37ms, which is
// longer than one MIDI clock at 120BPM and bursts/poisons uClock's ext interval
static bool oledSending = false;
static uint8_t oledPage = 0;

static void sendOledPage(uint8_t page) {
  uint8_t addr = u8g2.getU8x8()->i2c_address >> 1;  // u8x8 stores 8-bit (0x78)
  uint8_t cmds[3] = { (uint8_t)(0xB0 | page), 0x00, 0x10 };  // page, col 0
  Wire.beginTransmission(addr);
  Wire.write(0x00);            // control byte: commands follow
  Wire.write(cmds, 3);
  Wire.endTransmission();
  Wire.beginTransmission(addr);
  Wire.write(0x40);            // control byte: GDDRAM data follows
  Wire.write(u8g2.getBufferPtr() + (uint16_t)page * 128, 128);
  Wire.endTransmission();
}

void displayUpdate() {
  if (headerActive && (long)(millis() - headerDeadline) >= 0) {
    headerHide();
  }
  if (!oledSending && displayDirty) {
    oledSending = true;
    oledPage = 0;
    displayDirty = false;
  }
  if (oledSending) {
    seqPumpMidi();             // drain any tick that queued during last page
    sendOledPage(oledPage);
    if (++oledPage >= 8) oledSending = false;
  }
}
