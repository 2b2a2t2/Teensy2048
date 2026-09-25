#pragma once

#include <Arduino.h>

// which group an event routes to
enum Group : uint8_t { G_ENC, G_CTRL, G_STEP, G_KEY };

// one id per distinct set in the mode matrix; * cells reuse the base id
enum EncSet : uint8_t {
  ENC_SEQ, ENC_SEQ2, ENC_SEQ3, ENC_SEQ4,
  ENC_ENC, ENC_ENC2, ENC_ENC3, ENC_ENC4,
  ENC_KEY, ENC_KEY2, ENC_KEY3, ENC_KEY4,
  ENC_SEQ_STEP_HELD,           // overlay: step held in SEQMODE
  ENC_SET_COUNT
};
enum CtrlSet : uint8_t {
  CTRL_SEQ, CTRL_SEQ2, CTRL_SEQ3, CTRL_SEQ4,
  CTRL_ENC2, CTRL_ENC3, CTRL_ENC4,
  CTRL_SET_COUNT
};
enum StepSet : uint8_t {
  STEP_SEQ, STEP_SEQ3, STEP_SEQ4,
  STEP_ENC2, STEP_ENC3, STEP_ENC4,
  STEP_KEY3, STEP_KEY4,
  STEP_SET_COUNT
};
enum KeySet : uint8_t {
  KEYSET_BASE, KEYSET_2, KEYSET_3, KEYSET_4,
  KEY_SET_COUNT
};

struct ModeBinding {
  EncSet  enc;
  CtrlSet ctrl;
  StepSet step;
  KeySet  key;
};

const ModeBinding* baseBinding();            // static row for modeBase+variant
void setOverlay(Group g, uint8_t setId);     // momentary override (on press)
void clearOverlay(Group g);                  // restore (on release)
ModeBinding currentBinding();                // base + active overlays

const char* encLabel(EncSet s, uint8_t i);   // encoder i's name in that set
const char* encSetName(EncSet s);
const char* ctrlSetName(CtrlSet s);
const char* stepSetName(StepSet s);
const char* keySetName(KeySet s);
