#include "Display.h"
#include "Encoders.h"
#include "Bindings.h"
#include "SeqClock.h"
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
//  control buttons grid: row1 y48  PREV NEXT M1 M2 M3
//                        row2 y56  M1 M2 M3 M4
void refreshMainScreen() {
  clearMainScreen();
  u8g2.setFont(u8g2_font_4x6_tr);

  ModeBinding b = currentBinding();
  char val[12];

  for (uint8_t c = 0; c < 4; c++) {
    uint8_t x = c * 32 + 1;
    u8g2.drawStr(x, 22, encLabel(b.enc, c));
    snprintf(val, sizeof(val), "%ld", encPosition(c));
    u8g2.drawStr(x, 30, val);
    u8g2.drawStr(x, 38, encLabel(b.enc, 4 + c));
    snprintf(val, sizeof(val), "%ld", encPosition(4 + c));
    u8g2.drawStr(x, 46, val);
  }

  static const char* ctrlRow1[5] = { "PREV", "NEXT", "M1", "M2", "M3" };
  static const char* ctrlRow2[5] = { "M1", "M2", "M3", "M4", "" };
  for (uint8_t c = 0; c < 5; c++) {
    uint8_t x = c * 25 + 1;
    u8g2.drawStr(x, 54, ctrlRow1[c]);
    if (ctrlRow2[c][0]) u8g2.drawStr(x, 62, ctrlRow2[c]);
  }

  displayDirty = true;
}

void drawHeaderText() {
  u8g2.setFont(u8g2_font_6x12_tf);   // header always uses its own font
  clearHeader();
  if (headerLeft[0]) {
    u8g2.drawStr(2, 13, headerLeft);
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
