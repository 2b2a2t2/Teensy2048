#pragma once

#include <Arduino.h>
#include <U8g2lib.h>

// SSD1306 128x64, hardware I2C (shared Wire bus @ 400kHz)
// Header: y 0-15 | Main screen: y 16-63
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

#define SCREEN_W    128
#define SCREEN_H    64
#define HEADER_H    16
#define HEADER_MS   2500

void initDisplay();
void headerSetLeft(const char* text);           // permanent, top-left (e.g. mode)
void headerShow(const char* name);              // temporary, right side, HEADER_MS
// "<Parameter> <value>" - the temporary strip names WHAT changed and its
// value in full words, so no knob turn is silent and no message is a code
// ("Pattern Length 16", not "LEN 16").  Too-wide text is pinned left, so
// the start of a message is always readable
void headerAnnounce(const char* name, const char* value);
void headerAnnounceInt(const char* name, int value, const char* suffix = "");
void headerSetStick(const char* text);          // persistent, right side (KEY4 progression)
void headerStickClear();                        // erase the persistent right text
void headerHide();                              // erase temporary right text now
void clearHeader();                  // erase header region in buffer
void clearMainScreen();              // erase main region in buffer
void refreshMainScreen();            // clear + redraw encoder/control grids
void refreshHeader();                // redraw the top strip (mode/ARP/radio)
void displayMarkDirty();             // schedule a buffer send
void displayUpdate();                // call every loop: timeout + send if dirty
