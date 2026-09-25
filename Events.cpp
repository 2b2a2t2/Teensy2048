#include "Events.h"
#include "Buttons.h"
#include "Display.h"
#include "Modes.h"
#include "Encoders.h"
#include "Bindings.h"

static bool padInGroup(uint8_t pad, const byte* group, uint8_t n) {
  int8_t led = ledForPad[pad];
  if (led < 0) return false;
  for (uint8_t i = 0; i < n; i++) {
    if (group[i] == led) return true;
  }
  return false;
}

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

    // demo overlay: holding any step pad in SEQMODE switches encoders
    bool isStep = padInGroup(pad, groupSteps, STEPS_NUM_G);
    if (isStep && modeBase == MODE_SEQ) {
      if (ev == EV_PRESSED) {
        setOverlay(G_ENC, ENC_SEQ_STEP_HELD);
        if (logRoom(48)) Serial.println("  -> overlay ENC_SEQ_STEP_HELD");
      } else if (ev == EV_RELEASED) {
        clearOverlay(G_ENC);
        if (logRoom(48)) Serial.println("  -> overlay cleared");
      }
    }

    if (ev == EV_PRESSED || ev == EV_CLICKED || ev == EV_HOLD || ev == EV_DOUBLE)
      logSets();
  }
}

void handleEncoderEvents() {
  uint8_t ei;
  int16_t ed;
  bool any = false;
  for (uint8_t n = 0; n < MAX_EVENTS_PER_LOOP && popEncEvent(ei, ed); n++) {
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
