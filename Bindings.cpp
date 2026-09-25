#include "Bindings.h"
#include "Modes.h"

// [MODE_ENC][MODE_SEQ][MODE_KEY] x variants 1..4 — from the mode matrix
static const ModeBinding table[3][4] = {
  // ENCMODE
  {
    { ENC_ENC,  CTRL_SEQ, STEP_SEQ,  KEYSET_BASE },  // 1: shared SEQ controls/steps, keyboard*
    { ENC_ENC2, CTRL_ENC2, STEP_ENC2, KEYSET_BASE }, // 2
    { ENC_ENC3, CTRL_ENC3, STEP_ENC3, KEYSET_3 },    // 3
    { ENC_ENC4, CTRL_ENC4, STEP_ENC4, KEYSET_BASE }, // 4: keyboard*
  },
  // SEQMODE
  {
    { ENC_SEQ,  CTRL_SEQ, STEP_SEQ,  KEYSET_BASE },  // 1
    { ENC_SEQ2, CTRL_SEQ2, STEP_SEQ, KEYSET_BASE },  // 2
    { ENC_SEQ3, CTRL_SEQ3, STEP_SEQ3, KEYSET_3 },    // 3
    { ENC_SEQ4, CTRL_SEQ4, STEP_SEQ, KEYSET_BASE },  // 4
  },
  // KEYMODE
  {
    { ENC_KEY,  CTRL_SEQ, STEP_SEQ,  KEYSET_BASE },  // 1
    { ENC_KEY2, CTRL_SEQ2, STEP_SEQ, KEYSET_2 },     // 2
    { ENC_KEY3, CTRL_SEQ3, STEP_SEQ3, KEYSET_3 },    // 3
    { ENC_KEY4, CTRL_SEQ4, STEP_SEQ4, KEYSET_4 },    // 4
  },
};

// placeholder encoder names — edit rows to real function names
static const char* encLabels[ENC_SET_COUNT][8] = {
  /* ENC_SEQ          */ { "SQ1", "SQ2", "SQ3", "SQ4", "SQ5", "SQ6", "SQ7", "SQ8" },
  /* ENC_SEQ2         */ { "S21", "S22", "S23", "S24", "S25", "S26", "S27", "S28" },
  /* ENC_SEQ3         */ { "S31", "S32", "S33", "S34", "S35", "S36", "S37", "S38" },
  /* ENC_SEQ4         */ { "S41", "S42", "S43", "S44", "S45", "S46", "S47", "S48" },
  /* ENC_ENC          */ { "EN1", "EN2", "EN3", "EN4", "EN5", "EN6", "EN7", "EN8" },
  /* ENC_ENC2         */ { "E21", "E22", "E23", "E24", "E25", "E26", "E27", "E28" },
  /* ENC_ENC3         */ { "E31", "E32", "E33", "E34", "E35", "E36", "E37", "E38" },
  /* ENC_ENC4         */ { "E41", "E42", "E43", "E44", "E45", "E46", "E47", "E48" },
  /* ENC_KEY          */ { "KY1", "KY2", "KY3", "KY4", "KY5", "KY6", "KY7", "KY8" },
  /* ENC_KEY2         */ { "K21", "K22", "K23", "K24", "K25", "K26", "K27", "K28" },
  /* ENC_KEY3         */ { "K31", "K32", "K33", "K34", "K35", "K36", "K37", "K38" },
  /* ENC_KEY4         */ { "K41", "K42", "K43", "K44", "K45", "K46", "K47", "K48" },
  /* ENC_SEQ_STEP_HELD*/ { "HD1", "HD2", "HD3", "HD4", "HD5", "HD6", "HD7", "HD8" },
};

static const char* encSetNames[ENC_SET_COUNT] = {
  "SEQENC", "SEQENC2", "SEQENC3", "SEQENC4",
  "ENCENC", "ENCENC2", "ENCENC3", "ENCENC4",
  "KEYENC", "KEYENC2", "KEYENC3", "KEYENC4",
  "SEQENC_STEP_HELD"
};
static const char* ctrlSetNames[CTRL_SET_COUNT] = {
  "SEQCTRL", "SEQCTRL2", "SEQCTRL3", "SEQCTRL4",
  "ENCCTRL2", "ENCCTRL3", "ENCCTRL4"
};
static const char* stepSetNames[STEP_SET_COUNT] = {
  "SEQSTEPS", "SEQSTEPS3", "SEQSTEPS4",
  "ENCSTEPS2", "ENCSTEPS3", "ENCSTEPS4",
  "KEYSTEPS3", "KEYSTEPS4"
};
static const char* keySetNames[KEY_SET_COUNT] = {
  "KEYBTNS", "KEYBTNS2", "KEYBTNS3", "KEYBTNS4"
};

// single momentary override slot per group
static bool    overlayOn[4] = { false, false, false, false };
static uint8_t overlaySet[4] = { 0, 0, 0, 0 };

const ModeBinding* baseBinding() {
  uint8_t row = 0;
  if (modeBase == MODE_ENC) row = 0;
  else if (modeBase == MODE_SEQ) row = 1;
  else if (modeBase == MODE_KEY) row = 2;
  else row = 1;  // MODE_NONE -> SEQ row is irrelevant; variant lookup still safe

  uint8_t col = (modeVariant >= 1 && modeVariant <= 4) ? modeVariant - 1 : 0;
  return &table[row][col];
}

void setOverlay(Group g, uint8_t setId) {
  overlayOn[g] = true;
  overlaySet[g] = setId;
}

void clearOverlay(Group g) {
  overlayOn[g] = false;
}

ModeBinding currentBinding() {
  ModeBinding b = *baseBinding();
  if (overlayOn[G_ENC])  b.enc  = (EncSet)overlaySet[G_ENC];
  if (overlayOn[G_CTRL]) b.ctrl = (CtrlSet)overlaySet[G_CTRL];
  if (overlayOn[G_STEP]) b.step = (StepSet)overlaySet[G_STEP];
  if (overlayOn[G_KEY])  b.key  = (KeySet)overlaySet[G_KEY];
  return b;
}

const char* encLabel(EncSet s, uint8_t i) {
  if (s >= ENC_SET_COUNT || i >= 8) return "?";
  return encLabels[s][i];
}
const char* encSetName(EncSet s)   { return (s < ENC_SET_COUNT)  ? encSetNames[s]  : "?"; }
const char* ctrlSetName(CtrlSet s) { return (s < CTRL_SET_COUNT) ? ctrlSetNames[s] : "?"; }
const char* stepSetName(StepSet s) { return (s < STEP_SET_COUNT) ? stepSetNames[s] : "?"; }
const char* keySetName(KeySet s)   { return (s < KEY_SET_COUNT)  ? keySetNames[s]  : "?"; }
