/*********************************************************
Teensy 4.1 4x MPR121 touch controller
48 pads / 42 WS2812 LEDs / SSD1306 header / 8 encoders
IRQ-driven, non-blocking button states and modes

Modules: Buttons, TouchChips, Display, Encoders, Modes
**********************************************************/
#include <Wire.h>

#include "Buttons.h"
#include "TouchChips.h"
#include "Display.h"
#include "Encoders.h"
#include "Modes.h"
#include "Events.h"
#include "SeqClock.h"

void setup() {
  Serial.begin(115200);
  Serial.println("Teensy 4x MPR121 Touch");

  Wire.begin();
  Wire.setClock(400000);

  initButtons();
  initDisplay();
  initEncoders();
  initModes();
  initSeqClock();

  if (!initTouchChips()) while (1);
}

void loop() {
  // pump USB MIDI FIRST: clockMe() only runs when usbMIDI.read() runs.
  // starving it freezes uClock's external phase-lock (the "green dot freeze").
  uint32_t t0 = micros();
  pollSeqClock();
  uint32_t t1 = micros();
  seqDiagNote(0, t1 - t0);

  pollTouchChips();
  btnUpdate();
  pollEncoders();
  pollModeLeds();
  uint32_t t2 = micros();
  seqDiagNote(1, t2 - t1);

  handleButtonEvents();
  handleEncoderEvents();
  uint32_t t3 = micros();
  seqDiagNote(2, t3 - t2);

  // drain ticks queued during touch/Serial/LED work, THEN block on the OLED
  // send (~25ms I2C); incoming ticks buffer in the USB RX ring meanwhile
  pollSeqClock();
  displayUpdate();
  uint32_t t4 = micros();
  seqDiagNote(3, t4 - t3);
}
