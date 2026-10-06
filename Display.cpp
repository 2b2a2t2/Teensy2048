#include "Display.h"
#include "Encoders.h"
#include "Bindings.h"
#include "SeqClock.h"
#include "Modes.h"
#include "Keyboard.h"
#include "Events.h"
#include "Chords.h"
#include "Key4.h"       // KEYMODE4 genre/knob values + control legend
#include <Wire.h>

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

static char headerLeft[16] = "";
static char headerText[24] = "";
static unsigned long headerDeadline = 0;
static bool headerActive = false;
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
    // SEQMODE + POLY: F1 = FX (sus/ext/color/strum), F2 stays free,
    // F3/F4 edit accent / probability
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        seqTrack() == SEQ_TRACK_POLY) {
      static const char* ed[4] = { "FX", "", "ACC", "PROB" };
      return ed[i - 5];
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
  if (i == 1) return key2 ? "Sus" : "NEXT";   // KEYMODE4 keeps NEXT
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
    // SEQMODE 1/2/4 + MONO/ARP = the ARP encoder page
    // (both keep priority over the quality key, same as the handler)
    if (modeBase == MODE_ENC) return "MUTE";
    if (modeBase == MODE_SEQ && modeVariant == 3) return "ERASE";
    if (modeBase == MODE_SEQ && modeVariant != 3 &&
        seqTrack() == SEQ_TRACK_MONO && seqMonoType() == SEQ_MONO_ARP)
      return "ARP";
    if (key4) return "AUTO";
    return key2 ? "Min" : "M2";
  }
  if (i == 4) {
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
  // SEQMODE3 encoders show state, not the raw shared counter
  bool seq3 = (modeBase == MODE_SEQ && modeVariant == 3);
  // SEQMODE + MONO + radio on: ENC5/6/8 shape the track (gate/vel/prob)
  bool seqGe = (modeBase == MODE_SEQ && modeVariant != 3 &&
                seqTrack() == SEQ_TRACK_MONO && stepEditIsOn());
  // SEQMODE + M2 arp page: ENC2/3/4/7 take span/skip/mode/insert, the
  // other four keep their knobs (ENC1 octave, 5/6/8 gate/vel/prob)
  bool arpP = arpPageActive();
  uint8_t arpSeq = (uint8_t)(seqEditChannel() - 1);
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

  for (uint8_t c = 0; c < 4; c++) {
    uint8_t x = c * 32 + 1;
    const char *l1 = encLabel(b.enc, c);
    if (arpP && c >= 1 && c <= 3) {
      static const char *al[4] = { "OCT", "SPAN", "SKIP", "MODE" };
      l1 = al[c];
    }
    u8g2.drawStr(x, 22, l1);
    if (modeBase == MODE_SEQ && c == 0) {
      snprintf(val, sizeof(val), "%d", seqOctaveShift()); // ENC1 = octave shift
    } else if (arpP && c == 1) {
      snprintf(val, sizeof(val), "%u",
               seqArpSpanOf(arpSeq));           // ENC2 = span (octaves)
    } else if (arpP && c == 2) {
      snprintf(val, sizeof(val), "%s",
               seqArpSkipOf(arpSeq) ? "ON" : "OFF");   // ENC3 = skip rests
    } else if (arpP && c == 3) {
      snprintf(val, sizeof(val), "%s",
               seqArpModeName(seqArpModeOf(arpSeq)));  // ENC4 = mode
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
    } else if (key4Emb && c == 0) {
      snprintf(val, sizeof(val), "%s", k4EmbModeName());  // ENC1 = EMB
    } else if (key4Emb && c == 1) {
      snprintf(val, sizeof(val), "%s", k4EmbPlaceName()); // ENC2 = PLACE
    } else if (key4Emb) {
      snprintf(val, sizeof(val), "---");                   // ENC3/4 idle
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
    } else {
      snprintf(val, sizeof(val), "%ld", encPosition(c));
    }
    u8g2.drawStr(x, 30, val);
    // row-2 labels follow the step-edit bindings while the radio is on
    // (the raw SQ5/SQ6/SQ8 set names would contradict the values)
    const char* l2 = encLabel(b.enc, 4 + c);
    if (seqGe) {
      static const char* sl[4] = { "GATE", "VEL", "---", "PROB" };
      l2 = sl[c];
    } else if (seqDrumGe) {
      static const char* sl[4] = { "---", "---", "---", "PROB" };
      l2 = sl[c];
    }
    if (c == 1 && f3AccentActive()) l2 = "ACC";   // F3 held: ENC6 = accent
    if (arpP && 4 + c == 6) l2 = "INST";          // arp page: ENC7 = insert
    if (seqDrumGe && 4 + c == 6 && stepEditSel() == 5)
      l2 = "FX";                                  // POLY edit: ENC7 = FX type
    u8g2.drawStr(x, 38, l2);
    if (c == 1 && f3AccentActive()) {
      snprintf(val, sizeof(val), "%d", seqAccentVelocity()); // ENC6 = acc vel
    } else if (arpP && 4 + c == 6) {
      snprintf(val, sizeof(val), "%s",
               seqArpInstName(seqArpInstOf(arpSeq)));  // ENC7 = insert mode
    } else if (seq3 && 4 + c == 4) {
      snprintf(val, sizeof(val), "%u", seqInputChannel()); // ENC5 = input ch
    } else if (seq3 && 4 + c == 5) {
      snprintf(val, sizeof(val), "%s", seqTrackName());   // ENC6 = TRACK
    } else if (seq3 && 4 + c == 6 && seqTrack() == SEQ_TRACK_MONO) {
      snprintf(val, sizeof(val), "%s", seqMonoTypeName()); // ENC7 = TYPE
    } else if (seq3 && 4 + c == 7) {
      snprintf(val, sizeof(val), "%u", seqTempo());         // ENC8 = BPM
    } else if (seqGe && 4 + c == 4) {
      snprintf(val, sizeof(val), "%d%%", seqGatePct());     // ENC5 = gate
    } else if (seqGe && 4 + c == 5) {
      snprintf(val, sizeof(val), "%d", seqVelocity());      // ENC6 = vel
    } else if (seqGe && 4 + c == 6) {
      snprintf(val, sizeof(val), "---");                    // ENC7 = free
    } else if (seqGe && 4 + c == 7) {
      snprintf(val, sizeof(val), "%d%%", seqProbPct());  // ENC8 = track probability
    } else if (seqDrumGe && 4 + c == 4) {
      snprintf(val, sizeof(val), "---");                    // ENC5 free
    } else if (seqDrumGe && 4 + c == 5) {
      snprintf(val, sizeof(val), "---");                    // ENC6 free
    } else if (seqDrumGe && 4 + c == 6) {
      snprintf(val, sizeof(val), "%s",
               stepEditSel() == 5 ? stepFxSelName() : "---"); // ENC7 = FX type
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
      snprintf(val, sizeof(val), "---");          // overlay row 2 idle
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
  // header slot shows it instead of the mode name (TIE/GLIDE/ACCENT/PROB);
  // an open ARP page wins over both so the rebind is always visible
  const char* left = headerLeft;
  if (arpPageActive()) {
    left = "ARP";
  } else if (stepEditActive()) {
    static const char* ed[4] = { "TIE", "GLIDE", "ACCENT", "PROB" };
    left = ed[stepEditSel() - 1];
  }
  // a transient message owns the whole strip: long strings (the KEY4
  // progression) right-align over the left slot, so drop the label
  // instead of drawing it underneath
  if (left[0] && !(headerActive && headerText[0])) {
    u8g2.drawStr(2, 13, left);
  }
  if (headerActive && headerText[0]) {
    int w = u8g2.getStrWidth(headerText);
    u8g2.drawStr(SCREEN_W - w - 2, 13, headerText);
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
