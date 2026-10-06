#include "Key4.h"
#include "genre_data.h"   // the ONLY translation unit that includes this
#include "Chords.h"       // chord engine build path, scale/root state
#include "Keyboard.h"     // step writes, snapshots, edit sequence access
#include "SeqClock.h"     // pattern length + edit window follow
#include "Display.h"      // headerShow: the progression announcement
#include <stdio.h>
#include <string.h>

// the keyboard only offers this many proposals at a time - rank 0 is the
// next chord from the highest probability, the rest of the keyboard is
// dead (the engine proposes, not the scale)
#define K4_PROPOSE 4

// ---------------------------------------------------------------------------
// state (RAM only - a reboot starts from the defaults below)
// ---------------------------------------------------------------------------
static uint8_t  sGenre = 0;                              // boot = genre 0
static uint8_t  sKnob[7] = {50, 50, 50, 50, 50, 50, 50}; // MOOD..SURP
static bool     sWrite = false;  // M1: boots in AUDITION, land = opt-in
static bool     sAuto = false;   // M2: boots with auto-advance off
static bool     sLead = false;   // F3: minimal-bass-movement inversions
static bool     sHold = false;   // F4 held: audition only
static uint8_t  sFocus = 0;      // write target, inside the pattern
static uint64_t sLock = 0;       // step lock bits (apply skips them)
static uint32_t sSeed = 0x9E3779B9u;

// proposal cache - k4Refresh() rebuilds it; the LED hook only reads it
static int8_t   sRank[12];        // rank per pitch class, -1 = not proposed
static bool     sBorrow[12];      // proposed but outside the framework
static uint8_t  sQual[12];        // CHORD_Q_* of that proposal
static uint8_t  sExtBits[12];     // CHORD_EXT_* of that proposal
static uint8_t  sInv[12];         // bass index 0..3 (buildNotes param)

// framework: the 7 pcs the progression moves between - the current scale
// when it has exactly 7 notes, else major intervals from the current root
// (chromatic / pentatonic / 5-note scales still get a coherent grammar)
static uint8_t  sFw[7];

// audition chain: the proposal context follows the LAST key pressed, so the
// keyboard walks a progression by ear even when nothing is written (WRITE
// off, HOLD, locked focus).  Cleared when the focus moves or the generator
// rewrites the pattern; degrees are key-relative, so a scale/root change
// keeps the chain valid
static bool     sChain = false;
static uint8_t  sChainDeg = 0;
static uint8_t  sChainBass = 0;
static uint8_t  sChainRoot = 0;
static uint8_t  sChainQ = CHORD_Q_MAJ;

// progression history for the header: the first chord (the chord played in
// KEYMODE2 on entry, or the stored step before the focus) + every chord
// picked on the keyboard, oldest dropped past K4_HIST
#define K4_HIST 4
static uint8_t  sHistRoot[K4_HIST];
static uint8_t  sHistQ[K4_HIST];
static uint8_t  sHistN = 0;

static void histPush(uint8_t r, uint8_t q) {
  if (sHistN && sHistRoot[sHistN - 1] == r && sHistQ[sHistN - 1] == q)
    return;                           // re-press of the same chord: no new step
  if (sHistN == K4_HIST) {
    for (uint8_t i = 1; i < K4_HIST; i++) {
      sHistRoot[i - 1] = sHistRoot[i];
      sHistQ[i - 1] = sHistQ[i];
    }
    sHistN--;
  }
  sHistRoot[sHistN] = r;
  sHistQ[sHistN] = q;
  sHistN++;
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

// weighted pick over 4 buckets (0..3)
static uint8_t pick4(const int32_t *w, uint32_t h) {
  int32_t total = w[0] + w[1] + w[2] + w[3];
  if (total <= 0) return 0;
  int32_t r = (int32_t)(h % (uint32_t)total);
  for (uint8_t i = 0; i < 4; i++) {
    if (r < w[i]) return i;
    r -= w[i];
  }
  return 3;
}

static int circDist(uint8_t a, uint8_t b) {
  int df = (int)a - (int)b;
  if (df < 0) df = -df;
  return df > 6 ? 12 - df : df;
}

static bool hasPc(const uint8_t *nt, uint8_t n, uint8_t pc) {
  for (uint8_t i = 0; i < n; i++)
    if (nt[i] % 12 == pc) return true;
  return false;
}

// ---------------------------------------------------------------------------
// framework + degree helpers (all local: no Chords.cpp exports needed)
// ---------------------------------------------------------------------------
static void buildFw() {
  uint8_t pcs[12];
  uint8_t n = 0;
  uint8_t root = (uint8_t)keyRoot();
  for (uint8_t pc = 0; pc < 12; pc++)
    if (keyIsInScale(pc)) pcs[n++] = pc;
  int ri = -1;
  for (uint8_t i = 0; i < n; i++)
    if (pcs[i] == root) { ri = (int)i; break; }
  if (n == 7 && ri >= 0) {
    for (uint8_t d = 0; d < 7; d++) sFw[d] = pcs[(ri + d) % 7];
  } else {
    static const uint8_t maj[7] = {0, 2, 4, 5, 7, 9, 11};
    for (uint8_t d = 0; d < 7; d++)
      sFw[d] = (uint8_t)((root + maj[d]) % 12);
  }
}

static bool fwHas(uint8_t pc) {
  for (uint8_t d = 0; d < 7; d++)
    if (sFw[d] == pc) return true;
  return false;
}

static uint8_t fwDeg(uint8_t pc) {
  int best = 0, bd = 99;
  for (uint8_t d = 0; d < 7; d++) {
    int df = circDist(sFw[d], pc);
    if (df < bd) { bd = df; best = d; }
  }
  return (uint8_t)best;
}

// quality of a framework degree: from the framework's own interval sizes,
// so a Dorian/minor-flavored scale proposes the chords it actually has
static uint8_t fwQuality(uint8_t d) {
  int i3 = (int)sFw[(d + 2) % 7] - (int)sFw[d];
  int i5 = (int)sFw[(d + 4) % 7] - (int)sFw[d];
  if (i3 < 0) i3 += 12;
  if (i5 < 0) i5 += 12;
  if (i3 == 4 && i5 == 7) return CHORD_Q_MAJ;
  if (i3 == 3 && i5 == 7) return CHORD_Q_MIN;
  if (i3 == 3 && i5 == 6) return CHORD_Q_DIM;
  return CHORD_Q_MAJ;
}

// does this chord have a triad (or sus) on candidate root r?  Inversions
// work because every tone of the built chord is tested, not the bass
static bool rootTriadQ(const uint8_t *nt, uint8_t n, uint8_t r, uint8_t *q) {
  bool m3 = hasPc(nt, n, (uint8_t)((r + 3) % 12));
  bool M3 = hasPc(nt, n, (uint8_t)((r + 4) % 12));
  bool P5 = hasPc(nt, n, (uint8_t)((r + 7) % 12));
  bool d5 = hasPc(nt, n, (uint8_t)((r + 6) % 12));
  bool s4 = hasPc(nt, n, (uint8_t)((r + 5) % 12));
  if (M3 && P5) { *q = CHORD_Q_MAJ; return true; }
  if (m3 && d5) { *q = CHORD_Q_DIM; return true; }
  if (m3 && P5) { *q = CHORD_Q_MIN; return true; }
  if (s4 && P5) { *q = CHORD_Q_SUS; return true; }
  return false;
}

// which framework degree does a stored chord belong to?  Its triad (or
// sus) tones are all in the set - works for inversions because the root
// tone stays in the stored notes; borrowed roots (outside the framework)
// still find their quality, their degree is the nearest scale degree.
// *root/*q are optional outs; falls back to the first tone's nearest
// degree for bare dyads and singles
static uint8_t decodeChord(const uint8_t *nt, uint8_t n,
                           uint8_t *root, uint8_t *q) {
  if (n == 0) { if (root) *root = 0; if (q) *q = CHORD_Q_MAJ; return 0; }
  uint8_t found = 0xFF;
  for (uint8_t pass = 0; pass < 2 && found == 0xFF; pass++) {
    for (uint8_t i = 0; i < n; i++) {
      uint8_t c = (uint8_t)(nt[i] % 12);
      if ((pass == 0) != fwHas(c)) continue;   // pass0: in framework, pass1: borrowed
      uint8_t qq;
      if (rootTriadQ(nt, n, c, &qq)) { found = c; if (q) *q = qq; break; }
    }
  }
  if (found != 0xFF) {
    if (root) *root = found;
    return fwDeg(found);
  }
  // dyad / single: bass's nearest degree, triad quality of that degree
  if (root) *root = (uint8_t)(nt[0] % 12);
  if (q) *q = fwQuality(fwDeg((uint8_t)(nt[0] % 12)));
  return fwDeg((uint8_t)(nt[0] % 12));
}

// the chord BEFORE the focus, walked like the apply chain: locked and
// empty steps are skipped.  Degree feeds the transition matrix, bass pc
// feeds the VOICE-LEAD inversion choice; root/quality feed the header.
// Returns false when it defaulted to the tonic (no real first chord)
static bool prevContext(uint8_t *deg, uint8_t *bass,
                        uint8_t *root, uint8_t *q) {
  if (sChain) {                    // audition: follow the last chord heard
    *deg = sChainDeg;
    *bass = sChainBass;
    *root = sChainRoot;
    *q = sChainQ;
    return true;
  }
  for (int s = (int)sFocus - 1; s >= 0; s--) {
    uint8_t n = 0;
    const uint8_t *nt = stepChordAt((uint8_t)s, &n);
    if (k4Locked((uint8_t)s) || n == 0) continue;
    *bass = (uint8_t)(nt[0] % 12);
    *deg = decodeChord(nt, n, root, q);
    return true;
  }
  *deg = 0;
  *bass = sFw[0];
  *root = sFw[0];
  *q = fwQuality(0);
  return false;
}

// re-anchor the header's progression on the stored context (focus moves,
// apply and undo all land here - the chain was cleared before us)
static void histRebuild() {
  sHistN = 0;
  uint8_t d, bass, r, q;
  if (prevContext(&d, &bass, &r, &q)) histPush(r, q);
}

// ---------------------------------------------------------------------------
// scoring: trans x MOTION x MOOD x CADENCE x SURP (all knobs 0..100)
// ---------------------------------------------------------------------------
static bool barEnd(uint8_t pos) {
  int L = seqPatLen();
  if (L <= 0) return false;
  if ((int)pos >= L) pos = (uint8_t)(L - 1);
  return (pos % 16) >= 12 || pos == (uint8_t)(L - 1);
}

static uint32_t scorePc(uint8_t prev, uint8_t pc, uint8_t pos) {
  const GenreData *g = &genreTable[sGenre];
  bool in = fwHas(pc);
  uint8_t d = fwDeg(pc);
  uint32_t s = (uint32_t)g->trans[prev][d] * 1000u;
  if (in) {
    if (s == 0) s = 1000u;          // a scale degree always speaks
  } else {
    uint8_t t = sKnob[1];           // TENS: the borrowed window
    if (t <= 33) return 0;
    uint32_t bf = (uint32_t)(t - 33);
    if (t >= 67) bf += (uint32_t)(t - 67) / 2u;
    s = s * bf / 100u;
    if (s == 0) s = 1;
  }

  uint8_t m = sKnob[2];             // MOTION: stay vs change around 50
  uint32_t mot = (d == prev) ? (uint32_t)(150 - m) : (uint32_t)(50 + m);
  s = s * mot / 100u;

  int32_t bias = (int32_t)sKnob[0] - 50;      // MOOD: bright vs dark degrees
  bool majDeg = (d == 0 || d == 3 || d == 4);
  int32_t mf = 100 + (majDeg ? bias : -bias) / 2;
  s = (uint32_t)((int32_t)s * mf / 100);

  if (barEnd(pos) && (d == 0 || d == 4)) {    // CADENCE: cadence points
    s = s * (100u + (uint32_t)sKnob[3]) / 100u;
  }

  uint8_t sp = sKnob[6];                      // SURP: seeded temperature
  if (sp) {
    int32_t nz = (int32_t)(hash32(sSeed ^ ((uint32_t)prev << 8) ^ pc ^
                                  ((uint32_t)pos << 16) ^ 0x51u) % 81u) - 40;
    int32_t f = 100 + (nz * (int32_t)sp) / 100;
    s = (uint32_t)((int32_t)s * f / 100);
  }
  if (s == 0) s = 1;
  return s;
}

// ---------------------------------------------------------------------------
// proposal derivation: quality bucket (CPX x genre table), extension,
// inversion (ERA x genre table, or VOICE-LEAD's minimal movement)
// ---------------------------------------------------------------------------
static void deriveProposal(uint8_t prev, uint8_t pc, uint8_t prevBass,
                           uint8_t *oQ, uint8_t *oExt, uint8_t *oInv) {
  const GenreData *g = &genreTable[sGenre];
  bool in = fwHas(pc);
  uint8_t d = fwDeg(pc);
  uint8_t cpx = sKnob[4], era = sKnob[5];

  int32_t wq[4];
  wq[0] = ((int32_t)g->qual[d][0] * cpx + 100 * (100 - cpx)) / 100;
  for (uint8_t i = 1; i < 4; i++)
    wq[i] = ((int32_t)g->qual[d][i] * cpx) / 100;
  if (sKnob[1] >= 67) wq[3] *= 2;            // TENS high: sus/alt weight
  uint32_t h = hash32(sSeed ^ (prev * 0x9E3779B9u) ^ (pc * 0x85EBCA6Bu) ^
                      0xA3u);
  uint8_t bucket = pick4(wq, h);

  uint8_t q;
  if (bucket == 3) q = CHORD_Q_SUS;
  else if (in) q = fwQuality(d);
  else q = (sKnob[0] >= 50) ? CHORD_Q_MAJ : CHORD_Q_MIN;   // borrowed: MOOD

  uint8_t ext = 0;
  if (bucket == 1)
    ext = (q == CHORD_Q_MIN || q == CHORD_Q_DIM) ? CHORD_EXT_m7
                                                 : CHORD_EXT_M7;
  else if (bucket == 2)
    ext = (q == CHORD_Q_DIM) ? (CHORD_EXT_m7 | CHORD_EXT_9) : CHORD_EXT_9;

  uint8_t inv;
  if (sLead) {
    // minimal bass movement from the previous chord's bass
    static const uint8_t ivM[4][5] = {
      {0, 4, 7, 11, 14},   // Maj
      {0, 3, 7, 10, 14},   // Min
      {0, 3, 6, 10, 14},   // Dim
      {0, 5, 7, 11, 14}    // Sus
    };
    uint8_t row = (q == CHORD_Q_MAJ) ? 0 : (q == CHORD_Q_MIN) ? 1
               : (q == CHORD_Q_DIM) ? 2 : 3;
    uint8_t tones = (bucket == 2) ? 5 : (bucket == 1) ? 4 : 3;
    int bestD = 99;
    inv = 0;
    for (uint8_t k = 0; k < 4 && k < tones; k++) {
      uint8_t bassPc = (uint8_t)((pc + ivM[row][k]) % 12);
      int df = circDist(bassPc, prevBass);
      if (df < bestD) { bestD = df; inv = k; }
    }
  } else {
    int32_t wi[4];
    wi[0] = ((int32_t)g->inv[d][0] * era + 100 * (100 - era)) / 100;
    for (uint8_t i = 1; i < 4; i++)
      wi[i] = ((int32_t)g->inv[d][i] * era) / 100;
    uint32_t hi = hash32(sSeed ^ (prev * 0x9E3779B9u) ^ (pc * 0x85EBCA6Bu) ^
                         0x57u);
    inv = pick4(wi, hi);
  }

  *oQ = q;
  *oExt = ext;
  *oInv = inv;
}

// weighted pick of the next root pc during generation (0xFF = nobody)
static uint8_t pickPc(uint8_t prev, uint8_t pos) {
  uint32_t sc[12];
  uint32_t total = 0;
  for (uint8_t pc = 0; pc < 12; pc++) {
    sc[pc] = scorePc(prev, pc, pos);
    total += sc[pc];
  }
  if (total == 0) return 0xFF;
  uint32_t h = hash32(sSeed ^ (prev * 0x27D4EB2Fu) ^ (pos * 0x165667B1u) ^
                      0x9Du);
  uint32_t r = h % total;
  for (uint8_t pc = 0; pc < 12; pc++) {
    if (r < sc[pc]) return pc;
    r -= sc[pc];
  }
  return 0xFF;
}

// ---------------------------------------------------------------------------
// refresh: rank every proposed pc for the current context + cache buckets
// ---------------------------------------------------------------------------
void k4Refresh() {
  buildFw();
  uint8_t prev, bass, r0, q0;
  prevContext(&prev, &bass, &r0, &q0);

  uint32_t sc[12];
  uint8_t order[12];
  uint8_t n = 0;
  for (uint8_t pc = 0; pc < 12; pc++) {
    sc[pc] = scorePc(prev, pc, sFocus);
    sBorrow[pc] = !fwHas(pc);
    if (sc[pc] > 0) order[n++] = pc;
  }
  for (uint8_t i = 1; i < n; i++) {     // insertion sort desc (ties keep pc)
    uint8_t key = order[i];
    int j = (int)i - 1;
    while (j >= 0 && sc[order[j]] < sc[key]) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }
  for (uint8_t pc = 0; pc < 12; pc++) sRank[pc] = -1;
  // every scored pitch class gets its rank - the whole degree set sits on
  // the keyboard, brightness carries the probability (rank 0 hottest); the
  // header only spells out the top K4_PROPOSE
  for (uint8_t i = 0; i < n; i++) {
    uint8_t pc = order[i];
    sRank[pc] = (int8_t)i;
    deriveProposal(prev, pc, bass, &sQual[pc], &sExtBits[pc], &sInv[pc]);
  }
}

// ---------------------------------------------------------------------------
// context encoders
// ---------------------------------------------------------------------------
void k4GenreAdjust(int16_t counts) {
  int g = (int)sGenre + (int)counts;
  if (g < 0) g = 0;                       // clamp: the genre knob does
  if (g >= GENRE_COUNT) g = GENRE_COUNT - 1;   // not wrap around
  if ((uint8_t)g != sGenre) {
    sGenre = (uint8_t)g;
    k4Refresh();
  }
}

uint8_t k4Genre() { return sGenre; }
const char* k4GenreName() { return genreNames[sGenre]; }

const char* k4GenreShort() {
  static char b[8];
  snprintf(b, sizeof(b), "%.7s", genreNames[sGenre]);
  return b;
}

void k4KnobAdjust(uint8_t idx, int16_t counts) {
  if (idx >= 7) return;
  int v = (int)sKnob[idx] + (int)counts;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  if ((uint8_t)v != sKnob[idx]) {
    sKnob[idx] = (uint8_t)v;
    k4Refresh();
  }
}

uint8_t k4Knob(uint8_t idx) { return idx < 7 ? sKnob[idx] : 0; }

// ---------------------------------------------------------------------------
// control buttons
// ---------------------------------------------------------------------------
bool k4WriteToggle() { sWrite = !sWrite; return sWrite; }
bool k4Write()       { return sWrite; }
bool k4AutoToggle()  { sAuto = !sAuto; return sAuto; }
bool k4Auto()        { return sAuto; }
bool k4LeadToggle()  { sLead = !sLead; k4Refresh(); return sLead; }
bool k4Lead()        { return sLead; }
void k4HoldSet(bool on) { sHold = on; }
bool k4Hold()        { return sHold; }

// ---------------------------------------------------------------------------
// focus and locks
// ---------------------------------------------------------------------------
uint8_t k4Focus() { return sFocus; }

void k4FocusSet(uint8_t step) {
  int L = seqPatLen();
  if (L > 0 && (int)step >= L) step = (uint8_t)(L - 1);
  if (step > 63) step = 63;
  sFocus = step;
  sChain = false;              // a focus move re-anchors on the stored steps
  histRebuild();
  seqStepViewFollow(sFocus);
  k4Refresh();
  k4Announce();
}

void k4FocusStep(int8_t delta) {
  int L = seqPatLen();
  if (L <= 0) return;
  int f = (int)sFocus + (int)delta;
  f %= L;
  if (f < 0) f += L;
  sFocus = (uint8_t)f;
  sChain = false;
  histRebuild();
  seqStepViewFollow(sFocus);
  k4Refresh();
  k4Announce();
}

bool k4Locked(uint8_t step) {
  if (step >= 64) return false;
  return (sLock >> step) & 1u;
}

bool k4LockToggle(uint8_t step) {
  if (step >= 64) return false;
  sLock ^= (uint64_t)1 << step;
  k4Refresh();       // the walk skips locks: the proposals can shift
  return k4Locked(step);
}

// ---------------------------------------------------------------------------
// gesture: press sounds the proposal, commit writes the focus
// ---------------------------------------------------------------------------
int k4KeyOn(uint8_t kidx) {
  if (kidx >= 12 || sRank[kidx] < 0) return -1;
  uint8_t root = (uint8_t)(12 * keyOctave() + 24 + kidx);
  int r = chordKey4Press(kidx, root, sQual[kidx], sExtBits[kidx], sInv[kidx]);
  if (r >= 0) {
    // chain the progression: from here the proposals follow THIS chord, so
    // repeated presses walk a sequence by ear (audition, HOLD or locked)
    const uint8_t *nt = chordNotes();
    uint8_t n = chordCount();
    if (n) {
      sChainDeg = decodeChord(nt, n, &sChainRoot, &sChainQ);
      sChainBass = (uint8_t)(nt[0] % 12);
      sChain = true;
      histPush(sChainRoot, sChainQ);   // the walk the header shows
    }
  }
  return r;
}

void k4BucketFor(uint8_t kidx) {
  if (kidx >= 12 || sRank[kidx] < 0) return;
  chordKey4Bucket(sQual[kidx], sExtBits[kidx], sInv[kidx]);
}

void k4GestureEnd() {
  chordKey4Clear();
  k4Refresh();      // the chain now points past the chord just heard
  k4Announce();
}

static void focusAdvance() {
  int L = seqPatLen();
  if (L <= 0) return;
  uint8_t start = sFocus;
  do {
    sFocus = (uint8_t)((sFocus + 1) % L);
  } while (k4Locked(sFocus) && sFocus != start);
  sChain = false;    // the written chord sits before the focus again
  seqStepViewFollow(sFocus);
}

int k4KeyCommitted() {
  if (!sWrite || sHold) return 1;           // audition only
  if (k4Locked(sFocus)) return -1;          // locked: nothing lands
  const uint8_t *nt = chordNotes();
  uint8_t n = chordCount();
  if (n == 0) return -1;
  stepSnapshotTake();                       // undo remembers this gesture
  if (stepChordWrite(sFocus, nt, n) < 0) return -1;
  if (sAuto) focusAdvance();
  k4Refresh();
  return 0;
}

// ---------------------------------------------------------------------------
// generation: M3 applies it, F1 reseeds + applies, M3 held undoes
// ---------------------------------------------------------------------------
int k4ApplyAll() {
  int L = seqPatLen();
  if (L <= 0) return 0;
  stepSnapshotTake();
  sChain = false;                         // the pattern itself is the context
  buildFw();
  uint8_t prev = 0;                         // start: the tonic
  uint8_t prevBass = sFw[0];
  int written = 0;
  for (uint8_t st = 0; st < (uint8_t)L; st++) {
    if (k4Locked(st)) continue;
    uint8_t pc = pickPc(prev, st);
    if (pc > 11) continue;
    uint8_t q, ext, inv;
    deriveProposal(prev, pc, prevBass, &q, &ext, &inv);
    uint8_t root = (uint8_t)(12 * keyOctave() + 24 + pc);
    uint8_t notes[8];
    uint8_t n = chordBuild(root, q, ext, inv, notes);
    if (n == 0) continue;
    if (stepChordWrite(st, notes, n) < 0) continue;
    written++;
    prev = fwDeg(pc);
    prevBass = (uint8_t)(notes[0] % 12);    // the bass the build chose
  }
  histRebuild();      // the context is the stored pattern again
  k4Refresh();
  return written;
}

int k4Variation() {
  sSeed = hash32(sSeed ^ (uint32_t)millis() ^ 0xB5297A4Du);
  if (sSeed == 0) sSeed = 0x9E3779B9u;
  return k4ApplyAll();
}

int k4Undo() {
  if (stepSnapshotRestore() < 0) return -1;
  sChain = false;
  histRebuild();
  k4Refresh();
  return 0;
}

void k4Stop() {
  chordKey4Clear();
  sChain = false;
  sHistN = 0;
  sHold = false;
}

// entering KEYMODE4: the first chord of the progression is the chord
// played in KEYMODE2 (the engine's kept notes seed the chain and the
// header); otherwise the progression re-anchors on the stored step before
// the focus
// entering KEYMODE4: the progression starts from the chord the engine
// still holds - the G#Dim just played in KEYMODE2 is degree 1 here ("C=
// G#Dim").  No held chord: re-anchor on the stored step before the focus
void k4Enter() {
  sChain = false;
  if (chordCount() > 0) {
    const uint8_t *nt = chordNotes();
    uint8_t r, q;
    uint8_t d = decodeChord(nt, chordCount(), &r, &q);
    sChainDeg = d;
    sChainBass = (uint8_t)(nt[0] % 12);
    sChainRoot = r;
    sChainQ = q;
    sChain = true;
    sHistN = 0;
    histPush(r, q);
  } else {
    histRebuild();
  }
  k4Refresh();
  k4Announce();
}

// ---------------------------------------------------------------------------
// header announcements: roman numerals, e.g. "I", "vi", "bVII", "Vsus".
// Borrowed roots are named from the scale degree they sit below
// (Db->bII, Ab->bVI, F#->bV ...); minor/diminished are lowercase
// ---------------------------------------------------------------------------
static void numeral(uint8_t pc, uint8_t q, char *out, size_t cap) {
  static const char *up[7] = { "I", "II", "III", "IV", "V", "VI", "VII" };
  uint8_t d;
  char acc = 0;
  if (fwHas(pc)) {
    d = fwDeg(pc);
  } else {
    // borrowed root: name it from the NEAREST degree above it (Ab->bVI,
    // Db->bII, G#/Ab dim -> bvi) - smallest positive interval wins
    acc = 'b';
    d = 0;
    uint8_t best = 7;
    for (uint8_t i = 0; i < 7; i++) {
      uint8_t iv = (uint8_t)((sFw[i] - pc + 12) % 12);
      if (iv >= 1 && iv <= 6 && iv < best) { best = iv; d = i; }
    }
  }
  uint8_t n = 0;
  if (acc && n < cap - 1) out[n++] = acc;
  const char *base = up[d];
  bool lower = (q == CHORD_Q_MIN || q == CHORD_Q_DIM);
  while (*base && n < cap - 4) {
    out[n++] = lower ? (char)(*base + ('a' - 'A')) : *base;
    base++;
  }
  if (q == CHORD_Q_DIM && n < cap - 1) {
    out[n++] = '\xB0';               // degree sign as Latin-1 byte: drawStr
  } else if (q == CHORD_Q_SUS && n < cap - 4) {   // is 8-bit, not UTF-8
    out[n++] = 's';
    out[n++] = 'u';
    out[n++] = 's';
  }
  out[n] = '\0';
}

// append a whole token (" V", " > ") only if it fits - numerals are never
// cut in half, the header just stops growing
static int annAppend(char *buf, int off, size_t cap, const char *s) {
  size_t len = strlen(s);
  if (off < 0 || (size_t)off + len >= cap) return off;
  memcpy(buf + off, s, len);
  off += (int)len;
  buf[off] = '\0';
  return off;
}

// the header's progression: focus, the walk so far (first chord = the
// KEYMODE2 chord on entry / the stored step before the focus), then the
// proposals the lit keys offer - "f3 V vi > I IV"
void k4Announce() {
  char buf[24];
  int off = snprintf(buf, sizeof(buf), "f%u", (unsigned)(sFocus + 1));
  for (uint8_t i = 0; i < sHistN; i++) {
    char sym[8], piece[10];
    numeral(sHistRoot[i], sHistQ[i], sym, sizeof(sym));
    snprintf(piece, sizeof(piece), " %s", sym);
    off = annAppend(buf, off, sizeof(buf), piece);
  }
  if (sHistN) off = annAppend(buf, off, sizeof(buf), " >");   // pieces
  for (uint8_t r = 0; r < K4_PROPOSE; r++) {                   // carry " "

    int pc = -1;
    for (uint8_t i = 0; i < 12; i++)
      if (sRank[i] == (int8_t)r) { pc = (int)i; break; }
    if (pc < 0) continue;
    char sym[8], piece[10];
    numeral((uint8_t)pc, sQual[pc], sym, sizeof(sym));
    snprintf(piece, sizeof(piece), " %s", sym);
    off = annAppend(buf, off, sizeof(buf), piece);
  }
  headerShow(buf);
}

// the WRITTEN progression: the pattern's chords in order, first four
// numerals + how many follow ("I V vi IV +8") - what M3/F1/undo produced
void k4AnnouncePattern() {
  char buf[24];
  int off = 0;
  buf[0] = '\0';
  uint8_t shown = 0, rest = 0;
  int L = seqPatLen();
  for (uint8_t st = 0; st < (uint8_t)L && st < 64; st++) {
    uint8_t n = 0;
    const uint8_t *nt = stepChordAt(st, &n);
    if (n == 0) continue;
    uint8_t root, q;
    decodeChord(nt, n, &root, &q);
    if (shown < 4) {
      char sym[8];
      numeral(root, q, sym, sizeof(sym));
      if (off) off += snprintf(buf + off, sizeof(buf) - (size_t)off, " ");
      off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%s", sym);
      if (off >= (int)sizeof(buf) - 1) { shown++; break; }
      shown++;
    } else {
      rest++;
    }
  }
  if (off && rest && off < (int)sizeof(buf) - 5)
    snprintf(buf + off, sizeof(buf) - (size_t)off, " +%u", (unsigned)rest);
  headerShow(buf[0] ? buf : "no chords");
}

// ---------------------------------------------------------------------------
// proposals for the LED hook
// ---------------------------------------------------------------------------
int8_t k4KeyRank(uint8_t pc) {
  if (pc >= 12) return -1;
  return sRank[pc];
}

bool k4KeyBorrowed(uint8_t pc) {
  if (pc >= 12) return false;
  return sBorrow[pc];
}
