#include "TouchChips.h"
#include "Buttons.h"
#include <Wire.h>

Adafruit_MPR121 chips[NUM_CHIPS];
uint16_t lastTouched[NUM_CHIPS] = {0, 0, 0, 0};
uint16_t currTouched[NUM_CHIPS] = {0, 0, 0, 0};

static const uint8_t irqPins[NUM_CHIPS] = {
  MPR121_IRQ1, MPR121_IRQ2, MPR121_IRQ3, MPR121_IRQ4
};
static const uint8_t i2cAddrs[NUM_CHIPS] = { 0x5A, 0x5B, 0x5C, 0x5D };

static volatile bool chipIrq[NUM_CHIPS] = {false, false, false, false};
static void isrChip0() { chipIrq[0] = true; }
static void isrChip1() { chipIrq[1] = true; }
static void isrChip2() { chipIrq[2] = true; }
static void isrChip3() { chipIrq[3] = true; }
static void (*isrs[NUM_CHIPS])() = { isrChip0, isrChip1, isrChip2, isrChip3 };

static bool beginChip(Adafruit_MPR121 &chip, uint8_t addr) {
  if (!chip.begin(addr, &Wire)) {
    Serial.print("MPR121 not found at 0x");
    Serial.println(addr, HEX);
    return false;
  }
  chip.setAutoconfig(true);
  chip.setThresholds(TOUCH_THRESH, TOUCH_REL);
  return true;
}

static void printBaselines() {
  for (uint8_t i = 0; i < NUM_CHIPS; i++) {
    Serial.print("Chip "); Serial.print(i); Serial.print(" baselines: ");
    for (uint8_t e = 0; e < 12; e++) {
      uint16_t bl = chips[i].baselineData(e);
      Serial.print(bl);
      if (e < 11) Serial.print(",");
    }
    Serial.println();
  }
}

// A pad whose baseline never came up (below 64 = register < 16, while a
// healthy resting pad sits near 520..800) reports nothing - the chip ACKs
// fine, its buttons are just dead
static bool chipBaselinesSane(uint8_t i) {
  for (uint8_t e = 0; e < 12; e++)
    if (chips[i].baselineData(e) < 64) return false;
  return true;
}

// stop -> autoconfig + thresholds -> restart, and clear a stale-low IRQ
// line by reading the touch status once
static void recalibrateChip(uint8_t i) {
  chips[i].writeRegister(0x5E, 0x00);  // ECR = 0 (stop mode)
  delay(10);
  chips[i].setAutoconfig(true);
  chips[i].setThresholds(TOUCH_THRESH, TOUCH_REL);
  chips[i].writeRegister(0x5E, 0x8C);  // ECR = 0x8C (CL=5, 12 electrodes)
  currTouched[i] = chips[i].touched();
  chipIrq[i] = false;
}

bool initTouchChips() {
  for (uint8_t i = 0; i < NUM_CHIPS; i++) {
    if (!beginChip(chips[i], i2cAddrs[i])) return false;
    Serial.print("MPR121 #");
    Serial.print(i + 1);
    Serial.println(" OK");
  }

  // Wait for autoconfig to stabilize baselines on all electrodes.
  // Autoconfig runs asynchronously after begin(); polling before it
  // completes leaves some electrodes at baseline 0 (never touch).
  delay(500);

  // Diagnostic: check baseline data for all electrodes
  printBaselines();

  // A chip can pass begin() yet come up with dead baselines (autoconfig
  // race - the last chip started loses this race most often).  Re-run
  // autoconfig per bad chip, bounded so boot can never hang.
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    bool anyBad = false;
    for (uint8_t i = 0; i < NUM_CHIPS; i++) {
      if (chipBaselinesSane(i)) continue;
      anyBad = true;
      Serial.print("MPR121 #"); Serial.print(i + 1);
      Serial.println(" bad baseline -> recalibrating");
      recalibrateChip(i);
    }
    if (!anyBad) break;
    delay(500);
    printBaselines();
  }

  for (uint8_t i = 0; i < NUM_CHIPS; i++)
    pinMode(irqPins[i], INPUT_PULLUP);

  // The IRQ line stays LOW until the touch status register is read.  Any
  // status change during the settle leaves it low, and attaching on
  // FALLING would then never fire again - the whole chip goes silent for
  // the session.  Read once to clear it; the value lands in currTouched so
  // the first poll still reports a pad that is already held down.
  for (uint8_t i = 0; i < NUM_CHIPS; i++) {
    if (digitalRead(irqPins[i]) == LOW) {
      Serial.print("MPR121 #"); Serial.print(i + 1);
      Serial.println(" IRQ low at boot -> cleared");
    }
    chipIrq[i] = false;
    currTouched[i] = chips[i].touched();
  }

  for (uint8_t i = 0; i < NUM_CHIPS; i++) {
    attachInterrupt(digitalPinToInterrupt(irqPins[i]), isrs[i], FALLING);
  }
  return true;
}

void pollTouchChips() {
  // heartbeat: recovers a chip whose edge was lost (also unsticks a line
  // left low, since reading the status clears it)
  static uint32_t lastBeat = 0;
  uint32_t now = millis();
  bool beat = (now - lastBeat) >= IRQ_HEARTBEAT_MS;
  if (beat) lastBeat = now;
  for (uint8_t i = 0; i < NUM_CHIPS; i++) {
    if (!chipIrq[i] && !beat && digitalRead(irqPins[i]) != LOW) continue;
    chipIrq[i] = false;
    currTouched[i] = chips[i].touched();
    // Diagnostic: print touch status when changed
    if (currTouched[i] != lastTouched[i]) {
      Serial.print("Chip "); Serial.print(i); Serial.print(" touched: 0x");
      Serial.println(currTouched[i], HEX);
    }
    handleTouches(i, currTouched[i], lastTouched[i]);
  }
}

void touchRecalibrate() {
  Serial.println("Forcing MPR121 recalibration...");
  for (uint8_t i = 0; i < NUM_CHIPS; i++) {
    recalibrateChip(i);
    Serial.print("MPR121 #"); Serial.print(i + 1); Serial.println(" recalibrated");
  }
  delay(500);
  // Print baselines after recalibration
  printBaselines();
}

void touchPrintDiag() {
  for (uint8_t i = 0; i < NUM_CHIPS; i++) {
    Serial.print("Chip "); Serial.print(i); Serial.print(" baselines: ");
    for (uint8_t e = 0; e < 12; e++) {
      uint16_t bl = chips[i].baselineData(e);
      Serial.print(bl);
      if (e < 11) Serial.print(",");
    }
    Serial.println();
    // Also print filtered data for reference
    Serial.print("Chip "); Serial.print(i); Serial.print(" filtered:  ");
    for (uint8_t e = 0; e < 12; e++) {
      uint16_t fd = chips[i].filteredData(e);
      Serial.print(fd);
      if (e < 11) Serial.print(",");
    }
    Serial.println();
  }
}
