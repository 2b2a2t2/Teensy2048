#pragma once

#include <Arduino.h>
#include <Adafruit_MPR121.h>

#define NUM_CHIPS 4

#define MPR121_IRQ1 5
#define MPR121_IRQ2 2
#define MPR121_IRQ3 4
#define MPR121_IRQ4 3
#define TOUCH_THRESH 50
#define TOUCH_REL    30

extern Adafruit_MPR121 chips[NUM_CHIPS];
extern uint16_t lastTouched[NUM_CHIPS];
extern uint16_t currTouched[NUM_CHIPS];

bool initTouchChips();            // begin + autoconfig + IRQs; false if a chip missing
void pollTouchChips();            // read chips whose IRQ fired, feed handleTouches()
