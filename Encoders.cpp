#include "Encoders.h"
#include "Events.h"    // encPageRecall - per-channel F bank + page group
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

// 0-100% dials: 1 COUNT = 1 unit, the same rate the ENCMODE CC knobs use,
// so a full sweep is about one turn instead of four (the old 4-counts rate
// was unreadable on the hardware - the polls come far faster than a detent,
// so a speed heuristic never saw more than one count and could not help).
// A spin still gets more: the library batches a fast turn into a bigger
// delta, so the step grows with the counts this poll saw.  `res` is kept
// only for the call sites' shape - there is nothing to carry below one
// count any more.
int16_t encDrain(int16_t counts, int16_t *res) {
  (void)res;
  if (counts > 64) counts = 64;            // a wild spin: no need to amplify
  if (counts < -64) counts = -64;
  int16_t a = (counts < 0) ? (int16_t)-counts : counts;
  // thresholds are in counts and one detent is 4, so a normal single-detent
  // turn never amplifies: 1 detent per poll = 1x, two = 2x, and so on
  int16_t mul = (a <= 4) ? 1 : (a <= 8) ? 2 : (a <= 16) ? 3 : 4;
  return (int16_t)(counts * mul);
}

// ENCMODE3: channel of the encoder CCs
static uint8_t encCh = 1;

void encSetChannel(uint8_t ch) {
  if (ch < 1 || ch > 16 || ch == encCh) return;
  encCh = ch;
  encPageRecall();   // the new channel takes its own last F bank + group
}

uint8_t encChannel() { return encCh; }
