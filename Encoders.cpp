#include "Encoders.h"
#include <Encoder.h>

Encoder enc1(40, 39);
Encoder enc2(36, 35);
Encoder enc3(34, 33);
Encoder enc4(31, 32);
Encoder enc5(38, 37);
Encoder enc6(26, 25);
Encoder enc7(27, 28);
Encoder enc8(29, 30);

static Encoder* encoders[NUM_ENCODERS] = {
  &enc1, &enc2, &enc3, &enc4, &enc5, &enc6, &enc7, &enc8
};
static long encPrev[NUM_ENCODERS];

#define ENCQ_SIZE 16
struct EncEvent { uint8_t idx; int16_t delta; };
static EncEvent encq[ENCQ_SIZE];
static uint8_t encqHead = 0, encqTail = 0;

static void pushEncEvent(uint8_t idx, int16_t delta) {
  uint8_t next = (encqHead + 1) % ENCQ_SIZE;
  if (next == encqTail) return;
  encq[encqHead].idx = idx;
  encq[encqHead].delta = delta;
  encqHead = next;
}

bool popEncEvent(uint8_t &idx, int16_t &delta) {
  if (encqTail == encqHead) return false;
  idx = encq[encqTail].idx;
  delta = encq[encqTail].delta;
  encqTail = (encqTail + 1) % ENCQ_SIZE;
  return true;
}

void initEncoders() {
  for (uint8_t i = 0; i < NUM_ENCODERS; i++) {
    encPrev[i] = encoders[i]->read();
  }
}

void pollEncoders() {
  for (uint8_t i = 0; i < NUM_ENCODERS; i++) {
    long pos = encoders[i]->read();
    long d = pos - encPrev[i];
    if (d != 0) {
      encPrev[i] = pos;
      if (d > 32767) d = 32767;
      if (d < -32767) d = -32767;
      pushEncEvent(i, (int16_t)d);
    }
  }
}

long encPosition(uint8_t idx) {
  if (idx >= NUM_ENCODERS) return 0;
  return encoders[idx]->read();
}
