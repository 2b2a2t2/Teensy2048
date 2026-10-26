#include "Key4.h"
#include "genre_data.h"   // the ONLY translation unit that includes this
#include "Chords.h"       // chord engine build path, scale/root state
#include "Keyboard.h"     // step writes, snapshots, edit sequence access
#include "SeqClock.h"     // pattern length + edit window follow
#include "Gen.h"          // FILL/ALGO gate for the bake's rhythm roll
#include "bjorklund.h"    // Euclidean placement (same as the generators)
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

// embellishment encoders (PREV hold: EN5-8)
static uint8_t  sEmbSubProb  = 50;  // EN5: substitution probability %
static uint8_t  sEmbInvBias  = 0;   // EN6: inversion preference 0-3
static uint8_t  sEmbPassAmt  = 50;  // EN7: passing chord density %
static uint8_t  sEmbColorAmt = 50;  // EN8: extensions/sus/strum %

static bool     sEmbMenu = false; // NEXT tap: the EMB/PLACE row is open
static uint8_t  sEmbMode = 0;     // 0 = off, 1 = pass, 2 = pass + substitute
static uint8_t  sEmbPlace = 0;    // 0 = every odd step, 1 = by bracket gap
static bool     sHarmInput = false; // EMB ENC3: false = bass (Mode A),
                                    // true = arp (Mode B) source for HARM
static bool     sHarmHold  = false; // EMB ENC4: force hold when the bass
                                    // repeats a note (independent of MOTION)
static bool     sWrite = false;  // M1: boots in AUDITION, land = opt-in
static bool     sAuto = false;   // M2: boots with auto-advance off
static bool     sLead = false;   // F3: minimal-bass-movement inversions
static bool     sHold = false;   // F4 held: audition only
static uint8_t  sFocus = 0;      // write target, inside the pattern
static uint64_t sLock = 0;       // step lock bits (apply skips them)
static uint32_t sSeed = 0x9E3779B9u;
static int8_t   sStepSel = -1;   // step held: drives the OCT/DEG/INV/VOICE row
// undo slot already claimed for the current parameter-tweak burst (the
// first knob count snapshots, so M3-hold still lands on the pre-tweak
// pattern; later counts in the same burst keep that one slot)
static bool     sRegenSnap = false;

void   k4StepSelSet(int8_t step) { sStepSel = step; }
int8_t k4StepSelGet() { return sStepSel; }

// proposal cache - k4Refresh() rebuilds it; the LED hook only reads it
static int8_t   sRank[12];        // rank per pitch class, -1 = not proposed
static bool     sBorrow[12];      // proposed but outside the framework
static uint8_t  sQual[12];        // CHORD_Q_* of that proposal
static uint8_t  sExtBits[12];     // CHORD_EXT_* of that proposal
static uint8_t  sInv[12];         // bass index 0..3 (buildNotes param)
static uint8_t  sSd[12];          // secondary dominant: resolution target
                                  // degree 0..6, 255 = not an SD proposal

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
static uint8_t  sChainSd = 255;   // if the chained chord was a secondary
                                  // dominant, its resolution target degree
                                  // (0..6); 255 = the chain is not an SD
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

// circular distance in scale degrees (0..3): how far apart two main
// chords stand - the passing rule (PLACE = DIST) needs at least 2
static uint8_t fwDist7(uint8_t a, uint8_t b) {
  int d = (int)b - (int)a;
  if (d < 0) d += 7;
  int r = 7 - d;
  return (uint8_t)(d < r ? d : r);
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

// secondary-dominant target: a framework degree whose root sits a fifth
// ABOVE a non-diminished, non-tonic degree functions as V/x (V/V, V/ii,
// V/iii, V/IV, V/vi).  Returns the resolution target degree 0..6, or 255
// when this degree is not an SD root (the primary V, diminished targets,
// borrowed roots)
static uint8_t sdTargetOfPc(uint8_t rootPc) {
  if (!fwHas(rootPc)) return 255;
  uint8_t t = (uint8_t)((fwDeg(rootPc) + 3) % 7);  // a fifth below the root
  if (t == 0) return 255;                          // the primary V is not SD
  if (fwQuality(t) == CHORD_Q_DIM) return 255;
  return t;
}

// does this chord function as a secondary dominant (major triad + minor
// seventh on a qualifying root)?  Returns the resolution target degree,
// 255 = no.  Stored chords are detected from their tones
static uint8_t sdTgtOfNotes(const uint8_t *nt, uint8_t n, uint8_t rootPc) {
  if (n < 4) return 255;                       // a seventh needs 4 notes
  if (!hasPc(nt, n, (uint8_t)((rootPc + 4) % 12))) return 255;   // M3
  if (!hasPc(nt, n, (uint8_t)((rootPc + 10) % 12))) return 255;  // m7
  return sdTargetOfPc(rootPc);
}

// pitch-class set of the triad on rootPc in quality q (voice-leading uses
// the diatonic quality as an approximation - it only RANKS proposals,
// deriveProposal still picks the real quality for the built chord)
static uint8_t triadPcs(uint8_t rootPc, uint8_t q, uint8_t *out) {
  uint8_t iv3 = 4, iv5 = 7;                    // MAJ default
  if (q == CHORD_Q_MIN)      { iv3 = 3; iv5 = 7; }
  else if (q == CHORD_Q_DIM) { iv3 = 3; iv5 = 6; }
  else if (q == CHORD_Q_SUS) { iv3 = 5; iv5 = 7; }
  out[0] = rootPc;
  out[1] = (uint8_t)((rootPc + iv3) % 12);
  out[2] = (uint8_t)((rootPc + iv5) % 12);
  return 3;
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
// degree for bare dyads and singles (exported: the step-hold row's DEG
// transform reads the stored chord's root through it)
uint8_t decodeChord(const uint8_t *nt, uint8_t n,
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

// LIVE follow (playback, Keyboard.cpp): one feeder note that actually
// PLAYED - post-probability, post-fold/arp-walk - becomes the plain
// root-position triad of its nearest scale degree, in the same register
// the bake writes.  An out-of-scale note snaps to the nearest in-scale
// degree's chord (FScale philosophy); the diatonic quality is the degree's
// own (same voicing k4HarmBody bakes), so live chords match stored ones
uint8_t k4HarmLiveChord(uint8_t note, uint8_t *out) {
  buildFw();
  uint8_t rootPc = 0, q = CHORD_Q_MAJ;
  uint8_t deg = decodeChord(&note, 1, &rootPc, &q);
  uint8_t root = (uint8_t)(12 * keyOctave() + 24 + sFw[deg]);
  return chordBuild(root, q, 0, 0, out);   // plain triad, root pos
}

// SKANK ENC6 (SUB): sample the current genre's transition row from the
// degree nearest the sounding bass - one draw of the same matrix that
// drives KEYMODE4 / the HARM page, so the passing chord arrives in the
// language the progression speaks.  Returns the sampled degree's root
// pitch class + diatonic triad quality for the caller to voice.
bool k4BorrowSample(uint8_t bassPc, uint8_t *outPc, uint8_t *outQ) {
  if (!outPc || !outQ) return false;
  buildFw();
  uint8_t from = fwDeg(bassPc);
  const GenreData *g = &genreTable[sGenre];
  long roll = random(100), acc = 0;
  uint8_t to = from;
  for (uint8_t d = 0; d < 7; d++) {
    acc += g->trans[from][d];
    if (roll < acc) { to = d; break; }   // rows sum to 100: always lands
  }
  *outPc = (uint8_t)(sFw[to] % 12);
  *outQ = fwQuality(to);
  return true;
}

// the chord BEFORE the focus, walked like the apply chain: locked and
// empty steps are skipped.  Degree feeds the transition matrix, bass pc
// feeds the VOICE-LEAD inversion choice; root/quality feed the header;
// *sd is the previous chord's secondary-dominant resolution target (255 =
// not an SD) so scoring can pull toward what it resolves to.
// Returns false when it defaulted to the tonic (no real first chord)
static bool prevContext(uint8_t *deg, uint8_t *bass,
                        uint8_t *root, uint8_t *q, uint8_t *sd) {
  if (sChain) {                    // audition: follow the last chord heard
    *deg = sChainDeg;
    *bass = sChainBass;
    *root = sChainRoot;
    *q = sChainQ;
    *sd = sChainSd;
    return true;
  }
  for (int s = (int)sFocus - 1; s >= 0; s--) {
    uint8_t n = 0;
    const uint8_t *nt = stepChordAt((uint8_t)s, &n);
    if (k4Locked((uint8_t)s) || n == 0) continue;
    *bass = (uint8_t)(nt[0] % 12);
    *root = 0;
    *deg = decodeChord(nt, n, root, q);
    *sd = sdTgtOfNotes(nt, n, *root);
    return true;
  }
  *deg = 0;
  *bass = sFw[0];
  *root = sFw[0];
  *q = fwQuality(0);
  *sd = 255;
  return false;
}

// re-anchor the header's progression on the stored context (focus moves,
// apply and undo all land here - the chain was cleared before us)
static void histRebuild() {
  sHistN = 0;
  uint8_t d, bass, r, q, sd;
  if (prevContext(&d, &bass, &r, &q, &sd)) histPush(r, q);
}

// ---------------------------------------------------------------------------
// scoring: trans x MOTION x MOOD x CADENCE x SURP (all knobs 0..100),
// plus phrase/pair cadence terms, a voice-leading term and the
// secondary-dominant resolution pull (all gated by the same knobs)
// ---------------------------------------------------------------------------
static bool barEnd(uint8_t pos) {
  int L = seqPatLen();
  if (L <= 0) return false;
  if ((int)pos >= L) pos = (uint8_t)(L - 1);
  return (pos % 16) >= 12 || pos == (uint8_t)(L - 1);
}

// the pattern's final bar = the phrase end: the cadence terms stack there
// so generated progressions land (single-bar patterns: barEnd covers it)
static bool phraseEnd(uint8_t pos) {
  int L = seqPatLen();
  if (L <= 16) return barEnd(pos);
  return (int)pos >= L - 16;
}

static uint32_t scorePc(uint8_t prev, uint8_t pc, uint8_t pos,
                        uint8_t prevRoot, uint8_t prevQ, uint8_t prevSd) {
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

  uint8_t cad = sKnob[3];                      // CADENCE (all layers below)
  if (barEnd(pos) && (d == 0 || d == 4)) {     // cadence points: I / V
    s = s * (100u + (uint32_t)cad) / 100u;
  }
  // recognized cadence PAIRS at bar ends: ii-V and V-I are the moves a
  // cadence knob should hear (prev is the framework degree of the chord
  // before the focus)
  if (barEnd(pos)) {
    if (prev == 1 && d == 4) s = s * (100u + (uint32_t)cad) / 100u;  // ii-V
    if (prev == 4 && d == 0) s = s * (100u + (uint32_t)cad) / 100u;  // V-I
  }
  // phrase end (last bar of the pattern): the authentic cadence V-I lands
  // with an extra layer, plagal IV-I gets a softer one - the term that
  // makes generated progressions feel shaped rather than walked
  if (phraseEnd(pos)) {
    if (prev == 4 && d == 0)
      s = s * (100u + (uint32_t)cad / 2u) / 100u;    // authentic, lands
    else if (prev == 3 && d == 0)
      s = s * (100u + (uint32_t)cad / 4u) / 100u;    // plagal, softer
  }

  // secondary-dominant resolution: the previous chord was a V/x - its
  // target degree gets pulled (scaled by SURP, the knob that opened the
  // SD class in deriveProposal), so V/V really leads to V
  if (prevSd != 255 && d == prevSd)
    s = s * (100u + (uint32_t)sKnob[6] / 2u) / 100u;

  // voice leading: reward common tones and stepwise motion between the
  // previous chord's triad and the candidate's diatonic triad, penalize
  // parallel fifths/octaves (roots a fifth/fourth or octave apart while
  // both triads carry a perfect fifth).  Fixed conservative weight - the
  // genre transition tables stay dominant
  {
    uint8_t pt[3], ct[3];
    triadPcs(prevRoot, prevQ, pt);
    triadPcs(pc, fwQuality(d), ct);
    uint8_t common = 0;
    for (uint8_t i = 0; i < 3; i++)
      for (uint8_t j = 0; j < 3; j++)
        if (pt[i] == ct[j]) { common++; break; }
    if (common) s = s * (100u + (uint32_t)common * 15u) / 100u;
    uint8_t sum = 0;                       // stepwise motion: pair each
    for (uint8_t i = 0; i < 3; i++) {      // candidate tone to its nearest
      uint8_t best = 6;                    // previous tone (ties re-use)
      for (uint8_t j = 0; j < 3; j++) {
        uint8_t dd = (uint8_t)circDist(ct[i], pt[j]);
        if (dd < best) best = dd;
      }
      sum += best;
    }
    if (sum < 6) s = s * (100u + (uint32_t)(6 - sum) * 4u) / 100u;
    // root motion: circDist folds 7 (P5) to 5 (P4) - roots a fifth or
    // fourth apart while both triads carry a perfect fifth = the fifths
    // travel in parallel (same root = stationary, no parallels)
    uint8_t rm = (uint8_t)circDist(prevRoot, pc);
    bool p5a = (prevQ == CHORD_Q_MAJ || prevQ == CHORD_Q_MIN);
    bool p5b = (fwQuality(d) == CHORD_Q_MAJ || fwQuality(d) == CHORD_Q_MIN);
    if (p5a && p5b && rm == 5) s = s * 80u / 100u;
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
// inversion (ERA x genre table, or VOICE-LEAD's minimal movement),
// secondary-dominant class (SURP-gated roll: forced MAJ + m7 on a V/x
// degree, *oSD = the degree it resolves to, 255 = plain proposal)
// step = 255 for global (proposals), 0..63 for per-step override (apply)
// ---------------------------------------------------------------------------
static void deriveProposal(uint8_t prev, uint8_t pc, uint8_t prevBass,
                           uint8_t *oQ, uint8_t *oExt, uint8_t *oInv,
                           uint8_t step, uint8_t *oSD) {
  const GenreData *g = &genreTable[sGenre];
  bool in = fwHas(pc);
  uint8_t d = fwDeg(pc);
  uint8_t cpx = sKnob[4], era = sKnob[5];

  // secondary dominant: one extra proposal class (V/V, V/ii, V/iii,
  // V/IV, V/vi).  The roll hashes ONLY (seed, prev, pc) - never step -
  // so ranking and apply always derive the same quality; F1 reseeds and
  // reshuffles the class.  SURP = chance: 0 = never, 100 = 30%
  uint8_t sd = 255;
  uint8_t sdTgt = in ? sdTargetOfPc(pc) : 255;
  if (sdTgt != 255) {
    uint32_t hs = hash32(sSeed ^ ((uint32_t)prev << 8) ^ ((uint32_t)pc << 16) ^
                         0x5Eu);
    if ((hs % 100u) < (uint32_t)((uint16_t)sKnob[6] * 30u / 100u)) sd = sdTgt;
  }
  if (oSD) *oSD = sd;

  // Per-step embellishment overrides (255 = use global)
  uint8_t colorP = (step < 64) ? stepEmbGet(step, 3) : 255;
  uint8_t invBias = (step < 64) ? stepEmbGet(step, 1) : 255;
  if (colorP == 255) colorP = k4EmbColorAmt();
  if (invBias == 255) invBias = k4EmbInvBias();

  int32_t wq[4];
  wq[0] = ((int32_t)g->qual[d][0] * cpx + 100 * (100 - cpx)) / 100;
  for (uint8_t i = 1; i < 4; i++)
    wq[i] = ((int32_t)g->qual[d][i] * cpx) / 100;
  if (sKnob[1] >= 67) wq[3] *= 2;            // TENS high: sus/alt weight
  uint32_t h = hash32(sSeed ^ (prev * 0x9E3779B9u) ^ (pc * 0x85EBCA6Bu) ^
                      0xA3u);
  uint8_t bucket = pick4(wq, h);

  uint8_t q;
  uint8_t ext = 0;
  if (sd != 255) {
    // the V/x class: a dominant seventh - major triad + minor seventh,
    // whatever the genre quality table would say for this degree
    q = CHORD_Q_MAJ;
    ext = CHORD_EXT_m7;
  } else if (bucket == 3) {
    // sus probability gated by color amount
    uint32_t hs = hash32(sSeed ^ (prev * 0x12345678u) ^ (pc * 0x9ABCDEFu) ^ 0xF1u);
    q = (hs % 100u) < (uint32_t)colorP ? CHORD_Q_SUS : fwQuality(d);
  } else if (in) q = fwQuality(d);
  else q = (sKnob[0] >= 50) ? CHORD_Q_MAJ : CHORD_Q_MIN;   // borrowed: MOOD

  if (sd == 255) {
    if (bucket == 1) {
      ext = (q == CHORD_Q_MIN || q == CHORD_Q_DIM) ? CHORD_EXT_m7
                                                   : CHORD_EXT_M7;
      // 9th probability gated by color amount
      uint32_t h9 = hash32(sSeed ^ (prev * 0x11111111u) ^ (pc * 0x22222222u) ^ 0x33u);
      if ((h9 % 100u) < (uint32_t)colorP) ext |= CHORD_EXT_9;
    } else if (bucket == 2) {
      ext = (q == CHORD_Q_DIM) ? (CHORD_EXT_m7 | CHORD_EXT_9) : CHORD_EXT_9;
      // 11th/13th probability gated by color amount
      uint32_t h11 = hash32(sSeed ^ (prev * 0x44444444u) ^ (pc * 0x55555555u) ^ 0x66u);
      if ((h11 % 100u) < (uint32_t)colorP) ext |= CHORD_EXT_11;
      uint32_t h13 = hash32(sSeed ^ (prev * 0x77777777u) ^ (pc * 0x88888888u) ^ 0x99u);
      if ((h13 % 100u) < (uint32_t)colorP) ext |= CHORD_EXT_13;
    }
  }

  uint8_t inv;
  if (sLead) {
    // minimal bass movement from the previous chord's bass
    // with inversion bias preference
    static const uint8_t ivM[4][5] = {
      {0, 4, 7, 11, 14},   // Maj
      {0, 3, 7, 10, 14},   // Min
      {0, 3, 6, 10, 14},   // Dim
      {0, 5, 7, 11, 14}    // Sus
    };
    uint8_t row = (q == CHORD_Q_MAJ) ? 0 : (q == CHORD_Q_MIN) ? 1
               : (q == CHORD_Q_DIM) ? 2 : 3;
    uint8_t tones = (sd != 255) ? 4          // SD is always a 7th chord
                   : (bucket == 2) ? 5 : (bucket == 1) ? 4 : 3;
    int bestD = 99;
    inv = 0;
    for (uint8_t k = 0; k < 4 && k < tones; k++) {
      uint8_t bassPc = (uint8_t)((pc + ivM[row][k]) % 12);
      int df = circDist(bassPc, prevBass);
      // apply inversion bias: prefer the biased inversion
      if (k == invBias) df = df * 50 / 100;  // halve distance for preferred inv
      if (df < bestD) { bestD = df; inv = k; }
    }
  } else {
    int32_t wi[4];
    wi[0] = ((int32_t)g->inv[d][0] * era + 100 * (100 - era)) / 100;
    for (uint8_t i = 1; i < 4; i++)
      wi[i] = ((int32_t)g->inv[d][i] * era) / 100;
    // apply inversion bias: boost weight of preferred inversion
    if (invBias < 4) wi[invBias] = wi[invBias] * 3 / 2;
    uint32_t hi = hash32(sSeed ^ (prev * 0x9E3779B9u) ^ (pc * 0x85EBCA6Bu) ^
                         0x57u);
    inv = pick4(wi, hi);
  }

  *oQ = q;
  *oExt = ext;
  *oInv = inv;
}

// weighted pick of the next root pc during generation (0xFF = nobody)
static uint8_t pickPc(uint8_t prev, uint8_t pos, uint8_t prevRoot,
                      uint8_t prevQ, uint8_t prevSd) {
  uint32_t sc[12];
  uint32_t total = 0;
  for (uint8_t pc = 0; pc < 12; pc++) {
    sc[pc] = scorePc(prev, pc, pos, prevRoot, prevQ, prevSd);
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
  uint8_t prev, bass, r0, q0, sd0;
  prevContext(&prev, &bass, &r0, &q0, &sd0);

  uint32_t sc[12];
  uint8_t order[12];
  uint8_t n = 0;
  for (uint8_t pc = 0; pc < 12; pc++) {
    sc[pc] = scorePc(prev, pc, sFocus, r0, q0, sd0);
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
  for (uint8_t pc = 0; pc < 12; pc++) { sRank[pc] = -1; sSd[pc] = 255; }
  // every scored pitch class gets its rank - the whole degree set sits on
  // the keyboard, brightness carries the probability (rank 0 hottest); the
  // header only spells out the top K4_PROPOSE
  for (uint8_t i = 0; i < n; i++) {
    uint8_t pc = order[i];
    sRank[pc] = (int8_t)i;
    deriveProposal(prev, pc, bass, &sQual[pc], &sExtBits[pc], &sInv[pc],
                   255, &sSd[pc]);
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
// embellish menu (NEXT tap opens it, the encoder row shows EMB / PLACE)
// ---------------------------------------------------------------------------
void k4EmbMenuSet(bool on) { sEmbMenu = on; }
bool k4EmbMenu()           { return sEmbMenu; }

void k4EmbModeAdjust(int16_t counts) {
  if (!counts) return;
  int m = (int)sEmbMode + (int)counts;
  m %= 3;                       // off / pass / pass + sub, wraps
  if (m < 0) m += 3;
  sEmbMode = (uint8_t)m;
}

uint8_t k4EmbMode() { return sEmbMode; }

const char* k4EmbModeName() {
  static const char* n[3] = { "OFF", "PASS", "P+S" };
  return n[sEmbMode < 3 ? sEmbMode : 0];
}

void k4EmbPlaceAdjust(int16_t counts) {
  if (counts) sEmbPlace = (counts > 0) ? 1 : 0;   // CW = dist, CCW = odd
}

uint8_t k4EmbPlace() { return sEmbPlace; }

const char* k4EmbPlaceName() {
  return sEmbPlace ? "DIST" : "ODD";
}

// ---------------------------------------------------------------------------
// HARM settings (EMB menu ENC3/ENC4)
// ---------------------------------------------------------------------------
// ENC3 picks the HARM source: BASS = treat each step's lowest note as a bass
// line (Mode A, root-weighted consonance), ARP = treat the step notes as arp
// tones (Mode B, any chord tone fits).  ENC4 forces a hold whenever the bass
// repeats a note, independent of the MOTION knob.
void k4HarmInputAdjust(int16_t counts) {
  if (counts) sHarmInput = (counts > 0);        // CW = arp, CCW = bass
}
bool k4HarmInput() { return sHarmInput; }
const char* k4HarmInputName() { return sHarmInput ? "ARP" : "BASS"; }

void k4HarmHoldAdjust(int16_t counts) {
  if (counts) sHarmHold = (counts > 0);         // CW = on, CCW = off
}
bool k4HarmHold() { return sHarmHold; }
const char* k4HarmHoldName() { return sHarmHold ? "ON" : "OFF"; }

// ---------------------------------------------------------------------------
// embellishment encoders (PREV hold: EN5-8)
// ---------------------------------------------------------------------------
void k4EmbSubProbAdjust(int16_t counts) {
  int v = (int)sEmbSubProb + (int)counts;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  if ((uint8_t)v != sEmbSubProb) {
    sEmbSubProb = (uint8_t)v;
  }
}

uint8_t k4EmbSubProb() { return sEmbSubProb; }

void k4EmbInvBiasAdjust(int16_t counts) {
  int v = (int)sEmbInvBias + (int)counts;
  if (v < 0) v = 0;
  if (v > 3) v = 3;
  if ((uint8_t)v != sEmbInvBias) {
    sEmbInvBias = (uint8_t)v;
  }
}

uint8_t k4EmbInvBias() { return sEmbInvBias; }

void k4EmbPassAmtAdjust(int16_t counts) {
  int v = (int)sEmbPassAmt + (int)counts;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  if ((uint8_t)v != sEmbPassAmt) {
    sEmbPassAmt = (uint8_t)v;
  }
}

uint8_t k4EmbPassAmt() { return sEmbPassAmt; }

void k4EmbColorAmtAdjust(int16_t counts) {
  int v = (int)sEmbColorAmt + (int)counts;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  if ((uint8_t)v != sEmbColorAmt) {
    sEmbColorAmt = (uint8_t)v;
  }
}

uint8_t k4EmbColorAmt() { return sEmbColorAmt; }

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
      sChainSd = sSd[kidx];       // the proposal's SD target, if it was one
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
  sRegenSnap = false;                       // ...and ends any tweak burst
  if (stepChordWrite(sFocus, nt, n) < 0) return -1;
  if (sAuto) focusAdvance();
  k4Refresh();
  return 0;
}

// ---------------------------------------------------------------------------
// generation: M3 applies it, F1 reseeds + applies, M3 held undoes
// ---------------------------------------------------------------------------
// PASS+SUB substitution: one degree swapping another in the main
// progression - I->iii, ii->IV, iii->I, IV->ii, V->bVII, vi->IV,
// bVII->V (degree space; the swap lands on that degree's pitch class)
static const uint8_t subSw[7] = {2, 3, 0, 1, 6, 3, 4};

static int k4ApplyBody() {
  int L = seqPatLen();
  if (L <= 0) return 0;
  sChain = false;                         // the pattern itself is the context
  buildFw();
  uint8_t prev = 0;                       // start: the tonic
  uint8_t prevBass = sFw[0];
  uint8_t prevRoot = sFw[0];              // voice-leading context
  uint8_t prevQ = fwQuality(0);
  uint8_t prevSd = 255;                   // no secondary dominant yet
  int written = 0;
  // phase A: the main progression on every unlocked step (odd steps too -
  // they keep this main chord when the passing rule declines it)
  uint8_t mDeg[64], mBass[64];            // STEP_MAX: pattern max 64
  bool mOk[64];
  for (uint8_t st = 0; st < (uint8_t)L; st++) {
    mOk[st] = false;
    if (k4Locked(st)) {
      // the kept chord still brackets a passing slot (empty lock = no)
      uint8_t n = 0;
      const uint8_t *nt = stepChordAt(st, &n);
      if (n) {
        mDeg[st] = decodeChord(nt, n, 0, 0);
        mBass[st] = (uint8_t)(nt[0] % 12);
        mOk[st] = true;
      }
      continue;
    }
    uint8_t pc = pickPc(prev, st, prevRoot, prevQ, prevSd);
    if (pc > 11) continue;
    if (sEmbMode == 2 && fwHas(pc)) {      // PASS+SUB: swap the degree,
      uint32_t h = hash32(sSeed ^ ((uint32_t)st << 8) ^     // rolled once
                          ((uint32_t)pc << 16) ^ 0xC2u);
      uint8_t subP = stepEmbGet(st, 0);
      if (subP == 255) subP = k4EmbSubProb();
      if ((h % 100u) < (uint32_t)subP) pc = sFw[subSw[fwDeg(pc)]];
    }
    uint8_t q, ext, inv, sd;
    deriveProposal(prev, pc, prevBass, &q, &ext, &inv, st, &sd);
    uint8_t root = (uint8_t)(12 * keyOctave() + 24 + pc);
    uint8_t notes[8];
    uint8_t n = chordBuild(root, q, ext, inv, notes);
    if (n == 0) continue;
    if (stepChordWrite(st, notes, n) < 0) continue;
    written++;
    mDeg[st] = fwDeg(pc);
    mBass[st] = (uint8_t)(notes[0] % 12); // the bass the build chose
    mOk[st] = true;
    prev = mDeg[st];
    prevBass = mBass[st];
    prevRoot = pc;                        // the next step hears this chord
    prevQ = q;
    prevSd = sd;
  }
  // phase B (PASS/P+S): the odd steps become passing chords - a triad on
  // the scale degree one below the FOLLOWING main chord, quality and
  // inversion re-derived through the proposal scoring (genre + knobs +
  // voice-lead), so the passing chord belongs to the same grammar
  if (sEmbMode) {
    for (uint8_t st = 1; st < (uint8_t)L; st += 2) {
      if (k4Locked(st)) continue;
      uint8_t pr = st - 1;                     // bracketing main chords
      uint8_t nx = (st + 1 < (uint8_t)L) ? (uint8_t)(st + 1) : 0;
      if (!mOk[pr] || !mOk[nx]) continue;      // no bracket = keep main
      uint8_t passD = (uint8_t)((mDeg[nx] + 6) % 7);
      if (passD == mDeg[pr]) continue;         // the passing chord IS prev
      if (sEmbPlace == 1 && fwDist7(mDeg[pr], mDeg[nx]) < 2) continue;
      // passing chord probability gate (per-step override)
      uint8_t passP = stepEmbGet(st, 2);
      if (passP == 255) passP = k4EmbPassAmt();
      uint32_t h = hash32(sSeed ^ ((uint32_t)st << 8) ^ 0xE4u);
      if ((h % 100u) >= (uint32_t)passP) continue;
      uint8_t q, ext, inv;
      deriveProposal(mDeg[pr], sFw[passD], mBass[pr], &q, &ext, &inv, st,
                     0);
      uint8_t root = (uint8_t)(12 * keyOctave() + 24 + sFw[passD]);
      uint8_t notes[8];
      uint8_t n = chordBuild(root, q, ext, inv, notes);
      if (n == 0) continue;
      if (stepChordWrite(st, notes, n) < 0) continue;
      written++;
    }
  }
  histRebuild();      // the context is the stored pattern again
  k4Refresh();
  return written;
}

int k4ApplyAll() {
  stepSnapshotTake();
  sRegenSnap = false;
  return k4ApplyBody();
}

// ---------------------------------------------------------------------------
// HARM: inverse FOLLOW - harmonize a feeder bassline into chords
// ---------------------------------------------------------------------------
// Reads the FOLLOW feeder track's stored notes (a MONOPHONIC BASSLINE or an
// ARP, picked by the HARM page's SRC setting) and INFERS the chord that best
// explains each step: a Viterbi pass over the 7 framework degrees where the
// emission = how well the source fits as chord tones (bass: root > 5th >
// 3rd; arp: any chord tone) and the transition = the existing scorePc term
// (genre + motion + mood + cadence + voice-leading), so the source drives
// the harmony while the genre grammar smooths the path.  Plain root-position
// triads are baked into the HARM track's own steps (the feeder is only read,
// never written), a REST in the source carries the previous chord; M3-hold
// undoes the whole gesture.  Pairs with FOLLOW/LEAD: bass -> HARM -> chords
// -> FOLLOW -> LEAD.  Driven by the POLY TYPE=HARM page (M3), reusing the
// KEYMODE4 genre/knob grammar for the transitions.

// how well a bass pitch class sits as a chord tone of rootPc in quality q
// (root strongest, then the fifth, then the third; a non-chord-tone bass
// falls off with the circular distance to the nearest chord tone)
static uint8_t harmConsonance(uint8_t bassPc, uint8_t rootPc, uint8_t q) {
  uint8_t t[3];
  triadPcs(rootPc, q, t);
  if (bassPc == t[0]) return 100;             // root position
  if (bassPc == t[2]) return 85;              // fifth in the bass
  if (bassPc == t[1]) return 70;              // third in the bass
  uint8_t best = 12;
  for (uint8_t i = 0; i < 3; i++) {
    uint8_t d = (uint8_t)circDist(bassPc, t[i]);
    if (d < best) best = d;
  }
  return (best <= 6) ? (uint8_t)(40u - 6u * best) : 0;   // non-chord tone
}

// feeder channel + length for the current HARM run (set by k4Harm before
// the Viterbi; the harmonizer reads the FOLLOW feeder's stored bassline/arp)
static uint8_t sHarmFeederCh = 0;    // 1-based (0 = none yet)
static uint8_t sHarmFeederLen = 0;   // feeder pattern length

// per-step emission for degree d at step t (50 = rest, flat so the
// transitions decide).  Reads the FEEDER track's stored notes: arp = Mode B
// every note is an arp tone (all chord tones fit equally); bass = Mode A
// weights the lowest note root > fifth > third.  Read-only - writes happen
// after the whole Viterbi, so re-reading is safe
static uint8_t harmEmit(uint8_t t, uint8_t d, bool arp) {
  uint8_t n = 0;
  const uint8_t *nt = (t < sHarmFeederLen)
                        ? stepChordAtCh(sHarmFeederCh, t, &n) : 0;
  if (!nt || n == 0) return 50;                       // rest / past feeder
  uint8_t rootPc = sFw[d];
  uint8_t q = fwQuality(d);
  if (!arp) {                                      // Mode A: bass
    uint8_t low = nt[0];
    for (uint8_t i = 1; i < n; i++) if (nt[i] < low) low = nt[i];
    return harmConsonance((uint8_t)(low % 12), rootPc, q);
  }
  // Mode B: arp - every note should be a chord tone; average the fit
  uint8_t tri[3];
  triadPcs(rootPc, q, tri);
  uint16_t sum = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t pc = (uint8_t)(nt[i] % 12);
    if (pc == tri[0] || pc == tri[1] || pc == tri[2]) { sum += 100; continue; }
    uint8_t bestD = 12;
    for (uint8_t k = 0; k < 3; k++) {
      uint8_t dd = (uint8_t)circDist(pc, tri[k]);
      if (dd < bestD) bestD = dd;
    }
    sum += (bestD <= 6) ? (uint8_t)(40u - 6u * bestD) : 0;
  }
  return (uint8_t)(sum / n);
}

// emission scale: bass fit is the PRIMARY driver (a root-position fit must
// outweigh a genre transition), the transitions then break ties / smooth
#define HARM_EMIT_W 5000

// Infer the full chord path from the feeder: reads the input channel's stored
// bassline/arp, runs the Viterbi over the 7 framework degrees (emission = the
// Mode A bass / Mode B arp fit, transition = the KEYMODE4 scorePc grammar) and
// backtracks the best degree per step, then the hold-on-repeated-bass pass.
// Fills path[0..L-1] (degree) and has[0..L-1] (feeder had content there);
// returns the pattern length L (<=0 = nothing to infer).  Read-only: the
// caller decides what to do with the path (bake every step, or just one)
static int harmInferPath(uint8_t *path, bool *has) {
  int L = seqPatLen();
  if (L <= 0 || L > 64) return -1;
  buildFw();
  // FOLLOW gates the source: ON = read the INPUT channel (the bass/arp the
  // HARM page points at), OFF = ignore it and harmonize this track's own
  // notes in place.  The input channel is only HARM's generation source
  // (never its mirror), and only while FOLLOW is on
  sHarmFeederCh = seqFollow() ? seqFeederChannel() : seqEditChannel();
  if (sHarmFeederCh < 1) sHarmFeederCh = seqEditChannel();
  sHarmFeederLen = seqPatLenOf((uint8_t)(sHarmFeederCh - 1));
  // read the whole feeder bassline first (lowest note pc per step; empty or
  // past the feeder's length = rest)
  uint8_t bass[64];
  for (uint8_t st = 0; st < (uint8_t)L; st++) {
    uint8_t n = 0;
    const uint8_t *nt = (st < sHarmFeederLen)
                          ? stepChordAtCh(sHarmFeederCh, st, &n) : 0;
    if (!nt || n == 0) { has[st] = false; bass[st] = 0; continue; }
    uint8_t low = nt[0];
    for (uint8_t i = 1; i < n; i++) if (nt[i] < low) low = nt[i];
    bass[st] = (uint8_t)(low % 12);
    has[st] = true;
  }
  // Viterbi over the 7 framework degrees (emission = Mode A bass or Mode B
  // arp fit, chosen by the EMB ENC3 setting; transition = scorePc)
  static int32_t best[64][7];
  static int8_t  back[64][7];
  for (uint8_t d = 0; d < 7; d++)           // t = 0: emission only
    best[0][d] = (int32_t)harmEmit(0, d, sHarmInput) * HARM_EMIT_W;
  back[0][0] = -1;
  for (uint8_t d = 1; d < 7; d++) back[0][d] = -1;
  for (uint8_t t = 1; t < (uint8_t)L; t++) {
    uint8_t consT[7];
    for (uint8_t d = 0; d < 7; d++)
      consT[d] = harmEmit(t, d, sHarmInput);
    for (uint8_t d = 0; d < 7; d++) {
      int32_t em = (int32_t)consT[d] * HARM_EMIT_W;
      int32_t bestPrev = -1;
      int8_t bestD = 0;
      for (uint8_t dp = 0; dp < 7; dp++) {
        uint32_t tr = scorePc(dp, sFw[d], t, sFw[dp], fwQuality(dp), 255);
        int32_t cand = best[t - 1][dp] + (int32_t)tr;
        if (cand > bestPrev) { bestPrev = cand; bestD = (int8_t)dp; }
      }
      best[t][d] = em + bestPrev;
      back[t][d] = bestD;
    }
  }
  // backtrack the best degree path
  uint8_t dCur = 0;
  {
    int32_t m = -1;
    for (uint8_t d = 0; d < 7; d++)
      if (best[L - 1][d] > m) { m = best[L - 1][d]; dCur = d; }
  }
  for (int t = L - 1; t >= 0; t--) {
    path[t] = dCur;
    if (t > 0) dCur = (uint8_t)back[t][dCur];
  }
  // hold-on-repeated-bass (HARM page HOLD, bass mode only): a repeated bass
  // note keeps the same degree instead of re-voicing, independent of MOTION
  if (sHarmHold && !sHarmInput) {
    for (uint8_t t = 1; t < (uint8_t)L; t++) {
      if (!has[t] || !has[t - 1]) continue;
      if (bass[t] == bass[t - 1]) path[t] = path[t - 1];
    }
  }
  return L;
}

// shared bake body.  The manual M3 bake (automatic = false) claims the undo
// slot and re-anchors the KEYMODE4 context; the automatic re-bake that
// follows a feeder change (automatic = true) claims nothing - steps that
// already hold the inferred chord are skipped entirely (attributes survive,
// no sync churn) and the K4 context/proposal cache is left alone, so a
// feeder edit never disturbs the focus or the M3-hold undo
static int k4HarmBody(bool automatic) {
  uint8_t path[64];
  bool has[64];
  int L = harmInferPath(path, has);
  if (L <= 0) return -1;
  // rhythm of the manual bake: the track's own FILL% / ALGO, the same roll
  // gate the generators use (GNR8 page ENC2 / F1 ALGO) - a chord lands only
  // on the rolled steps, a fail turns the step to rest, so the baked
  // pattern keeps rests like a bassline instead of filling every step; M3
  // re-rolls the rhythm on every press (M3-hold undoes it).  The automatic
  // re-bake never rolls: it keeps the signature the last M3 roll left
  uint8_t fill = 100;
  bool euclid = false;
  uint64_t bits = 0;
  if (!automatic) {
    stepSnapshotTake();                     // M3 hold = undo the harmonize
    sRegenSnap = false;
    sChain = false;
    uint8_t idx = (uint8_t)(seqEditChannel() - 1);
    fill = seqGenFillOf(idx);
    euclid = seqGenAlgoOf(idx) == SEQ_GEN_EUCLID;
    if (euclid) {
      Bjorklund<uint64_t, 10> eu;
      int16_t pulses = (int16_t)(((uint32_t)L * fill + 50) / 100);  // round
      if (pulses > (int16_t)L) pulses = (int16_t)L;
      bits = eu.compute((int8_t)L, (int8_t)pulses);
    }
    genSeedOnce();
  }
  // bake the inferred chords into the HARM track's own steps (the feeder is
  // only read).  A REST step carries the previous chord, so the harmony
  // holds through a source gap rather than inventing a new chord there
  int written = 0;
  uint8_t carry = path[0];
  for (uint8_t st = 0; st < (uint8_t)L; st++) {
    if (has[st]) carry = path[st];
    if (automatic) {
      // the re-bake keeps the rhythmic signature: it refreshes already-
      // active steps only and never activates a disabled one
      if (!stepIsActive(st)) continue;
    } else {
      bool on = euclid ? (((bits >> st) & 1) != 0)
                       : ((uint8_t)random(100) < fill);
      if (!on) {
        if (stepIsActive(st)) stepGenClear(st);   // the roll says rest
        continue;
      }
    }
    uint8_t deg = has[st] ? path[st] : carry;
    uint8_t q = fwQuality(deg);
    uint8_t root = (uint8_t)(12 * keyOctave() + 24 + sFw[deg]);
    uint8_t notes[8];
    uint8_t n = chordBuild(root, q, 0, 0, notes);   // plain triad, root pos
    if (n == 0) continue;
    if (automatic && stepChordSame(st, notes, n)) { written++; continue; }
    if (stepChordWrite(st, notes, n) < 0) continue;
    written++;
  }
  if (!automatic) {
    histRebuild();        // M3 re-anchors the K4 context on the pattern...
    k4Refresh();          // ...and re-ranks the proposals
  }
  return written;
}

int k4Harm() { return k4HarmBody(false); }

// the deferred auto re-bake entry (Keyboard.cpp harmRebakePoll): same
// inference, no snapshot, no context rebuild
int k4HarmRebake() { return k4HarmBody(true); }

// The chord HARM infers for ONE step: the same Viterbi the M3 bake uses, so
// activating a HARM track's step (with no held gesture) records exactly the
// chord the bake would have written there - a chord that fits the feeder
// bass/arp, never the feeder's raw single note.  Returns the note count
// (0 = nothing to record: no feeder content yet, or step past the pattern)
int k4HarmStepChord(uint8_t step, uint8_t *notes) {
  if (!notes) return 0;
  uint8_t path[64];
  bool has[64];
  int L = harmInferPath(path, has);
  if (L <= 0 || step >= (uint8_t)L) return 0;
  uint8_t carry = path[0];
  bool any = false;
  uint8_t deg = path[0];
  for (uint8_t st = 0; st <= step; st++) {
    if (has[st]) { carry = path[st]; any = true; }
    deg = has[st] ? path[st] : carry;
  }
  if (!any) return 0;             // feeder empty up to here: keep pending
  uint8_t q = fwQuality(deg);
  uint8_t root = (uint8_t)(12 * keyOctave() + 24 + sFw[deg]);
  return chordBuild(root, q, 0, 0, notes);   // plain triad, root pos
}

// live re-apply while a GENRE/knob turn changes the parameters: same walk
// as M3, but the undo slot is claimed only once per tweak burst - the
// first count after any other gesture snapshots, so M3-hold still lands
// on the pre-tweak progression.  Locked steps are never touched
int k4Regenerate() {
  if (!sRegenSnap) {
    stepSnapshotTake();
    sRegenSnap = true;
  }
  return k4ApplyBody();
}

int k4Variation() {
  sSeed = hash32(sSeed ^ (uint32_t)millis() ^ 0xB5297A4Du);
  if (sSeed == 0) sSeed = 0x9E3779B9u;
  return k4ApplyAll();
}

int k4Undo() {
  if (stepSnapshotRestore() < 0) return -1;
  sRegenSnap = false;               // the next tweak claims a fresh slot
  sChain = false;
  histRebuild();
  k4Refresh();
  return 0;
}

void k4Stop() {
  chordKey4Clear();
  sChain = false;
  sChainSd = 255;
  sHistN = 0;
  sHold = false;
  sEmbMenu = false;            // the EMB row never outlives KEYMODE4
  sStepSel = -1;               // ...nor the held-step encoder row
  headerStickClear();          // nor the persistent progression header
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
  sChainSd = 255;
  if (chordCount() > 0) {
    const uint8_t *nt = chordNotes();
    uint8_t r, q;
    uint8_t d = decodeChord(nt, chordCount(), &r, &q);
    sChainDeg = d;
    sChainBass = (uint8_t)(nt[0] % 12);
    sChainRoot = r;
    sChainQ = q;
    sChainSd = sdTgtOfNotes(nt, chordCount(), r);   // V/x seeds the pull
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

// the held-step row's value cells: DEG = the stored chord's degree as the
// header names it, INV = which chord tone sits in the bass (root / 1st /
// 2nd / 3rd from the LOWEST tone's interval to the root - the transforms
// keep the array sorted, so the audible bass is always nt[0]... unless a
// generator reordered it, hence the explicit minimum)
bool k4StepLabels(uint8_t step, char *deg, size_t dcap,
                  char *inv, size_t icap) {
  uint8_t n = 0;
  const uint8_t *nt = stepChordAt(step, &n);
  if (n == 0) return false;
  uint8_t root = 0, q = CHORD_Q_MAJ;
  decodeChord(nt, n, &root, &q);
  if (deg && dcap) numeral(root, q, deg, dcap);
  uint8_t low = nt[0];
  for (uint8_t i = 1; i < n; i++) if (nt[i] < low) low = nt[i];
  uint8_t iv = (uint8_t)(((low % 12) + 12 - (root % 12)) % 12);
  const char *b;
  if (iv == 0) b = "root";
  else if (iv <= 4) b = "1st";          // m3/M3 (+ the odd m2/M2 edge)
  else if (iv <= 8) b = "2nd";          // P4/tri/P5/A5 (aug 2nd inv = A5)
  else b = "3rd";                       // m6/M6/m7 (7th chord, 3rd inv)
  if (inv && icap) snprintf(inv, icap, "%s", b);
  return true;
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

// the header's progression: the walk so far (first chord = the KEYMODE2
// chord on entry / the stored step before the focus), then the proposals
// the lit keys offer - "V vi > I IV".  The focused step itself needs no
// header prefix: the sequencer pads already mark it.  It STAYS in the
// header (persistent slot); only transient messages overlay it
void k4Announce() {
  char buf[24];
  int off = 0;
  buf[0] = '\0';
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
  headerSetStick(buf);
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
  headerShow(buf[0] ? buf : "No Chords");
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
