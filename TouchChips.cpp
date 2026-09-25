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

bool initTouchChips() {
  for (uint8_t i = 0; i < NUM_CHIPS; i++) {
    if (!beginChip(chips[i], i2cAddrs[i])) return false;
    Serial.print("MPR121 #");
    Serial.print(i + 1);
    Serial.println(" OK");
  }

  for (uint8_t i = 0; i < NUM_CHIPS; i++) {
    pinMode(irqPins[i], INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(irqPins[i]), isrs[i], FALLING);
  }
  return true;
}

void pollTouchChips() {
  for (uint8_t i = 0; i < NUM_CHIPS; i++) {
    if (chipIrq[i]) {
      chipIrq[i] = false;
      currTouched[i] = chips[i].touched();
      handleTouches(i, currTouched[i], lastTouched[i]);
    }
  }
}
