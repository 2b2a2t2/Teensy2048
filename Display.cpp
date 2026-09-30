#include "Display.h"
#include "Encoders.h"
#include "Bindings.h"
#include "SeqClock.h"
#include "Modes.h"
#include "Keyboard.h"
#include "Events.h"
#include "Chords.h"
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
    if (!key2) {
      static const char* row2[4] = { "M1", "M2", "M3", "M4" };
      return row2[i - 5];
    }
    static const char* ext[4] = { "6", "m7", "M7", "9" };
    return ext[i - 5];
  }
  if (i == 1) return key2 ? "Sus" : "NEXT";
  if (i == 2) {
    // hold M1 = DRUM part-select (beats the KEYMODE2 quality, like Events);
    // SEQMODE+MONO = step-edit radio; ENCMODE hold M1 = drum-VOICE mute
    if (modeBase == MODE_SEQ && modeVariant != 3 && seqTrack() == SEQ_TRACK_DRUM)
      return "PART";
    if (modeBase == MODE_SEQ && modeVariant != 3 && seqTrack() == SEQ_TRACK_MONO)
      return "EDIT";
    if (modeBase == MODE_ENC) return "DRUM";
    return key2 ? "Maj" : "M1";
  }
  if (i == 3) {
    // hold M2 = ENCMODE channel mute; SEQMODE3 click = erase the track
    // (both keep priority over the quality key, same as the handler)
    if (modeBase == MODE_ENC) return "MUTE";
    if (modeBase == MODE_SEQ && modeVariant == 3) return "ERASE";
    return key2 ? "Min" : "M2";
  }
  if (i == 4) return key2 ? "Dim" : "M3";
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
  // SEQMODE + DRUM + radio on: only ENC8 (probability clipboard) is bound
  bool seqDrumGe = (modeBase == MODE_SEQ && modeVariant != 3 &&
                    seqTrack() == SEQ_TRACK_DRUM && stepEditIsOn());
  // KEYMODE2 rows: the main chord row, or the PREV overlay row
  bool key2 = (modeBase == MODE_KEY && modeVariant == 2 && b.enc != ENC_KEY2_PREV);
  bool keyOv = (b.enc == ENC_KEY2_PREV);

  for (uint8_t c = 0; c < 4; c++) {
    uint8_t x = c * 32 + 1;
    u8g2.drawStr(x, 22, encLabel(b.enc, c));
    if (modeBase == MODE_SEQ && c == 0) {
      snprintf(val, sizeof(val), "%d", seqOctaveShift()); // ENC1 = octave shift
    } else if (seq3 && c == 3) {
      snprintf(val, sizeof(val), "%s", seqIsSyncInternal() ? "INT" : "EXT");
    } else if (modeBase == MODE_KEY && c == 0) {
      snprintf(val, sizeof(val), "%d", keyOctave());         // ENC1 = octave
    } else if (modeBase == MODE_KEY && c == 1) {
      snprintf(val, sizeof(val), "%s", keyScaleName());      // ENC2 = scale
    } else if ((key2 || keyOv) && c == 2) {
      snprintf(val, sizeof(val), "%s", keyRootName());       // ENC3 = root
    } else if (keyOv && c == 3) {
      snprintf(val, sizeof(val), "%s", chordLatch() ? "ON" : "OFF");
    } else if (key2 && c == 3) {
      snprintf(val, sizeof(val), "%u", chordParam(0));       // ENC4 = voicing
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
    u8g2.drawStr(x, 38, l2);
    if (seq3 && 4 + c == 4) {
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
      snprintf(val, sizeof(val), "%d%%", stepProbValue());  // ENC8 = prob
    } else if (seqDrumGe && 4 + c == 4) {
      snprintf(val, sizeof(val), "---");                    // ENC5 free
    } else if (seqDrumGe && 4 + c == 5) {
      snprintf(val, sizeof(val), "---");                    // ENC6 free
    } else if (seqDrumGe && 4 + c == 6) {
      snprintf(val, sizeof(val), "---");                    // ENC7 free
    } else if (seqDrumGe && 4 + c == 7) {
      snprintf(val, sizeof(val), "%d%%", stepProbValue());  // ENC8 = prob
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
  // header slot shows it instead of the mode name (TIE/GLIDE/ACCENT/PROB)
  const char* left = headerLeft;
  if (stepEditActive()) {
    static const char* ed[4] = { "TIE", "GLIDE", "ACCENT", "PROB" };
    left = ed[stepEditSel() - 1];
  }
  if (left[0]) {
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
