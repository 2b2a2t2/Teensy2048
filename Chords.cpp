#include "Chords.h"
#include "Keyboard.h"   // keyChannel(), keyPendingRefresh()

#define CHORD_VEL   100
#define CHORD_N     8      // notes sounding at once
#define CHORD_ENC   4      // encoder counts per param step

// 39 scales; patterns are 12-bit masks rooted at C (bit0 = C) so the
// keyRoot rotation lands on the right pitch classes
static const char* const kScaleName[KEY_SCALE_COUNT] = {
  // diatonic modes
  "Chr", "Major", "Minor", "HarmMin", "MelMin", "Dorian", "Phryg",
  "Lydian", "Mixoly", "Locrian",
  // pentatonic & blues
  "MajPent", "MinPent", "Blues",
  // symmetric & exotic
  "WhoTone", "DimHW", "DimWH", "DbHarm", "PhrDom", "HunMin", "Mishbrk",
  "LydDom", "Altered", "BebopD", "BebopM", "LydAug", "Mixb6", "LocN2",
  "Dor#4", "Lyd#2", "Ultra", "Persian", "NeaMin", "NeaMaj", "Augmnt",
  "Promth", "Enigm", "Insen", "Yo", "Hirajo"
};
// one-word mood shown in the header when a scale is selected
static const char* const kScaleMood[KEY_SCALE_COUNT] = {
  "All Notes", "Happy", "Sad", "Exotic", "Dramatic", "Jazzy", "Spanish",
  "Dreamy", "Groovy", "Tense",
  "Open", "Soulful", "Bluesy",
  "Floating", "Spooky", "Unstable", "Majestic", "Fiery", "Wild", "Lively",
  "Bluesy", "Restless", "Swinging", "Breezy", "Shimmer", "Moody", "Eerie",
  "Mystic", "Bright", "Bizarre", "Mystical", "Lament", "Cinematic",
  "Surreal", "Airy", "Cryptic", "Haunting", "Gentle", "Tranquil"
};
static const uint16_t kScalePat[KEY_SCALE_COUNT] = {
  0xFFF,  // Chr       {0..11}
  0xAB5,  // Major     {0,2,4,5,7,9,11}
  0x5AD,  // Minor     {0,2,3,5,7,8,10}
  0x9AD,  // HarmMin   {0,2,3,5,7,8,11}
  0xABD,  // MelMin    {0,2,3,5,7,9,11}
  0x6AD,  // Dorian    {0,2,3,5,7,9,10}
  0x5AB,  // Phryg     {0,1,3,5,7,8,10}
  0xAE5,  // Lydian    {0,2,4,6,7,9,11}
  0x6B5,  // Mixoly    {0,2,4,5,7,9,10}
  0x56B,  // Locrian   {0,1,3,5,6,8,10}
  0x295,  // MajPent   {0,2,4,7,9}
  0x4A9,  // MinPent   {0,3,5,7,10}
  0x4E9,  // Blues     {0,3,5,6,7,10}
  0x555,  // WhoTone   {0,2,4,6,8,10}
  0x6DB,  // DimHW     {0,1,3,4,6,7,9,10}
  0xB6D,  // DimWH     {0,2,3,5,6,8,9,11}
  0xB13,  // DbHarm    {0,1,4,5,7,8,11}
  0x5B3,  // PhrDom    {0,1,4,5,7,8,10}
  0xB2D,  // HunMin    {0,2,3,6,7,8,11}
  0x6F1,  // Mishbrk   {0,4,5,6,7,9,10}
  0x6D5,  // LydDom    {0,2,4,6,7,9,10}
  0x55B,  // Altered   {0,1,3,4,6,8,10}
  0xEB5,  // BebopD    {0,2,4,5,7,9,10,11}
  0xBB5,  // BebopM    {0,2,4,5,7,8,9,11}
  0xB55,  // LydAug    {0,2,4,6,8,9,11}
  0x5B5,  // Mixb6     {0,2,4,5,7,8,10}
  0x56D,  // LocN2     {0,2,3,5,6,8,10}
  0x6CD,  // Dor#4     {0,2,3,6,7,9,10}
  0xAE9,  // Lyd#2     {0,3,4,6,7,9,11}
  0x2DB,  // Ultra     {0,1,3,4,6,7,9}
  0x973,  // Persian   {0,1,4,5,6,8,11}
  0x9AB,  // NeaMin    {0,1,3,5,7,8,11}
  0xABB,  // NeaMaj    {0,1,3,5,7,9,11}
  0x999,  // Augmnt    {0,3,4,7,8,11}
  0x655,  // Promth    {0,2,4,6,9,10}
  0xD53,  // Enigm     {0,1,4,6,8,10,11}
  0x4A3,  // Insen     {0,1,5,7,10}
  0x4A9,  // Yo        {0,3,5,7,10} (as listed)
  0x1A3,  // Hirajo    {0,1,5,7,8}
};
static const char* const kRootName[12] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// quality -> row in the triad table below (NONE plays no triad of its own;
// it only appears through the diatonic/KeyLock paths)
static const uint8_t kQualityRow[5] = { 0, 0, 1, 2, 5 };
static const char* const kQualityName[5] = { "", "Maj", "Min", "Dim", "Sus" };
static const uint8_t kTriad[6][3] = {
  {0, 4, 7},  // 0: Major
  {0, 3, 7},  // 1: Minor
  {0, 3, 6},  // 2: Diminished
  {0, 4, 8},  // 3: Augmented
  {0, 2, 7},  // 4: Sus2
  {0, 5, 7},  // 5: Sus4
};

// scale state (shared by every KEY variant)
static uint8_t sScale = 0;            // 0 = Chrom: no filter, no LED change
static uint8_t sRoot  = 0;            // 0 = C
static int16_t sScaleRes = 0, sRootRes = 0;

// chord engine state
static uint8_t sQuality = CHORD_Q_NONE;   // momentary quality button
static uint8_t sExt = 0;                  // extension bitmask (F1..F4)
static int8_t  sParam[4] = {0, 0, 0, 0};  // voicing, bass, spread, density
static int16_t sParamRes[4] = {0};
static bool    sLatch = false, sLock = false, sPerf = false;
static bool    sFScale = false;      // KEYMODE2 ENC4: force chord tones to scale

static uint16_t sHeld = 0;                // keys held during the gesture
static uint8_t  sHeldCount = 0;
static int16_t  sRootNote = -1;           // root of the current gesture
static uint8_t  sPressNote[12];           // note each key was pressed with

static uint8_t  sNotes[CHORD_N];          // notes currently sounding
static uint8_t  sCount = 0;
static int16_t  sPlayedRoot = -1;         // root of the built chord (name)
static uint8_t  sPlayed = CHORD_Q_NONE;   // quality actually built
static bool     sCut = false;             // silenced by chordCut()
static bool     sMute = false;            // live MONO-track voice owns sound

static uint16_t rotPattern() {
  uint16_t p = kScalePat[sScale];
  return (uint16_t)(((p << sRoot) | (p >> (12 - sRoot))) & 0xFFF);
}

static uint8_t clampNote(int n) {
  if (n < 0) n = 0;
  if (n > 127) n = 127;
  return (uint8_t)n;
}

// ---------------------------------------------------------------------------
// scale API
// ---------------------------------------------------------------------------

void keyScaleAdjust(int16_t counts) {
  sScaleRes += counts;
  int8_t step = 0;
  while (sScaleRes >= CHORD_ENC) { sScaleRes -= CHORD_ENC; step++; }
  while (sScaleRes <= -CHORD_ENC) { sScaleRes += CHORD_ENC; step--; }
  if (!step) return;
  int v = (int)sScale + step;
  if (v < 0) v = 0;
  if (v > KEY_SCALE_COUNT - 1) v = KEY_SCALE_COUNT - 1;
  sScale = (uint8_t)v;
}

int keyScale() { return sScale; }
const char* keyScaleName() { return kScaleName[sScale]; }
const char* keyScaleMood() { return kScaleMood[sScale]; }

void keyRootAdjust(int16_t counts) {
  sRootRes += counts;
  int8_t step = 0;
  while (sRootRes >= CHORD_ENC) { sRootRes -= CHORD_ENC; step++; }
  while (sRootRes <= -CHORD_ENC) { sRootRes += CHORD_ENC; step--; }
  if (!step) return;
  int v = (int)sRoot + step;
  if (v < 0) v = 0;
  if (v > 11) v = 11;
  sRoot = (uint8_t)v;
}

int keyRoot() { return sRoot; }
const char* keyRootName() { return kRootName[sRoot]; }

bool keyIsInScale(uint8_t kidx) {
  if (sScale == 0) return true;
  return (rotPattern() & (uint16_t)(1u << (kidx % 12))) != 0;
}

// ---------------------------------------------------------------------------
// internals: quality resolution
// ---------------------------------------------------------------------------

// diatonic quality of a root pitch class: I/IV/V major, ii/iii/vi minor,
// vii diminished (the pressed quality button overrides this upstream)
static uint8_t diatonicQuality(uint8_t rootPC) {
  if (sScale == 0) return CHORD_Q_MAJ;
  uint16_t rot = rotPattern();
  int dist = ((int)rootPC - (int)sRoot + 12) % 12;
  int deg = 0;
  for (int s = 0; s <= dist && s < 12; s++) {
    if (rot & (uint16_t)(1u << ((sRoot + s) % 12))) {
      if (s == dist) break;
      deg++;
    }
  }
  if (deg == 0 || deg == 3 || deg == 4) return CHORD_Q_MAJ;
  if (deg == 6) return CHORD_Q_DIM;
  return CHORD_Q_MIN;
}

// KeyLock: nearest scale degree of the played pitch class
static int keylockDegree(uint8_t pc) {
  uint16_t rot = rotPattern();
  int bestDeg = 0, bestDist = 12, deg = 0;
  for (int s = 0; s < 12; s++) {
    int tpc = (sRoot + s) % 12;
    if (rot & (uint16_t)(1u << tpc)) {
      int d = (int)pc - tpc;
      if (d < 0) d = -d;
      if (d > 6) d = 12 - d;
      if (d < bestDist) { bestDist = d; bestDeg = deg; }
      deg++;
    }
  }
  return bestDeg;
}

// KeyLock: the triad quality built on that degree
static uint8_t keylockQuality(int deg) {
  if (sScale == 0) return CHORD_Q_MAJ;
  uint16_t rot = rotPattern();
  int pcs[12], n = 0;
  for (int s = 0; s < 12; s++) {
    int tpc = (sRoot + s) % 12;
    if (rot & (uint16_t)(1u << tpc)) pcs[n++] = tpc;
  }
  if (n < 3) return CHORD_Q_MAJ;
  int d = deg % n;
  int r = pcs[d], i3 = (pcs[(d + 2) % n] - r + 12) % 12,
                 i5 = (pcs[(d + 4) % n] - r + 12) % 12;
  if (i3 == 4 && i5 == 7) return CHORD_Q_MAJ;
  if (i3 == 3 && i5 == 7) return CHORD_Q_MIN;
  if (i3 == 3 && i5 == 6) return CHORD_Q_DIM;
  return CHORD_Q_MAJ;
}

// ---------------------------------------------------------------------------
// internals: play / stop
// ---------------------------------------------------------------------------

static void stopNotes() {
  if (!sMute)                            // muted: the notes never went out
    for (uint8_t i = 0; i < sCount; i++)
      usbMIDI.sendNoteOff(sNotes[i], 0, keyChannel());
  sCount = 0;
}

static void sendOut() {
  if (sMute) return;
  for (uint8_t i = 0; i < sCount; i++)
    usbMIDI.sendNoteOn(sNotes[i], CHORD_VEL, keyChannel());
}

// extensions above the triad - fixed intervals so every scale (chromatic,
// pentatonic, ...) gets proper 6/7/9 colors; the 9 implies a 7th when no
// 7th button is held
static void addExtensions(uint8_t *out, uint8_t &n, int root, uint8_t qType) {
  if (!sExt) return;
  uint8_t add[4];
  uint8_t an = 0;
  if (sExt & CHORD_EXT_6)
    add[an++] = (qType == CHORD_Q_MIN || qType == CHORD_Q_DIM) ? 8 : 9;
  if (sExt & CHORD_EXT_m7) add[an++] = 10;
  if (sExt & CHORD_EXT_M7) add[an++] = 11;
  if (sExt & CHORD_EXT_9) {
    if (!(sExt & (CHORD_EXT_m7 | CHORD_EXT_M7)))
      add[an++] = (qType == CHORD_Q_MIN) ? 10 : 11;
    add[an++] = 14;
  }
  for (uint8_t i = 0; i < an && n < CHORD_N; i++) {
    uint8_t v = clampNote(root + add[i]);
    bool dup = false;
    for (uint8_t j = 0; j < n; j++) if (out[j] == v) dup = true;
    if (!dup) out[n++] = v;
  }
}

// FScale: pull an out-of-scale chord tone onto the nearest scale pitch.
// Ties go DOWN first (a sharp tends to flatten), and chromatic has nothing
// to do so it passes everything through
static uint8_t forceScaleNote(uint8_t note) {
  if (!sFScale || keyIsInScale(note)) return note;
  for (int d = 1; d <= 6; d++) {
    int dn = (int)note - d;
    if (dn >= 0 && keyIsInScale((uint8_t)dn)) return (uint8_t)dn;
    int up = (int)note + d;
    if (up <= 127 && keyIsInScale((uint8_t)up)) return (uint8_t)up;
  }
  return note;
}

// build and play the chord on a root note with a fixed quality
static void playChordAt(int note, uint8_t qType) {
  stopNotes();
  int root = note;
  if (root < 36) root = 36;
  if (root > 84) root = 84;

  uint8_t out[CHORD_N];
  uint8_t n = 0;
  out[n++] = (uint8_t)root;

  uint8_t q = kQualityRow[qType];
  for (uint8_t t = 0; t < 3 && n < CHORD_N; t++) {
    uint8_t v = clampNote(root + kTriad[q][t]);
    bool dup = false;
    for (uint8_t j = 0; j < n; j++) if (out[j] == v) dup = true;
    if (!dup) out[n++] = v;
  }
  addExtensions(out, n, root, qType);

  // spread: raise everything above the root one octave per step
  if (sParam[2] > 0)
    for (uint8_t j = 1; j < n; j++)
      out[j] = clampNote((int)out[j] + sParam[2] * 12);

  // density: add octave copies of the chord until full
  if (sParam[3] > 0) {
    uint8_t base = n;
    for (uint8_t j = 0; j < base && n < CHORD_N; j++) {
      out[n] = clampNote((int)out[j] + 12);
      n++;
    }
  }

  // bass: move the idx-th note down an octave to the bottom
  if (sParam[1] > 0 && n > 1 && n < CHORD_N) {
    uint8_t src = out[sParam[1] % n];
    if (src >= 12) {
      uint8_t bass = (uint8_t)(src - 12);
      for (int j = n; j > 0; j--) out[j] = out[j - 1];
      out[0] = bass;
      n++;
    }
  }

  // voicing: rotate the top notes down (voicing times)
  if (sParam[0] > 0 && n > 3) {
    for (int8_t v = 0; v < sParam[0] && v < n - 1; v++) {
      uint8_t top = out[0];
      for (uint8_t j = 0; j + 1 < n; j++) out[j] = out[j + 1];
      out[n - 1] = top;
    }
  }

  // FScale: every tone lands inside the scale, collapsed to unique notes
  // (so a forced third can never double the root silently)
  if (sFScale) {
    uint8_t m = 0;
    for (uint8_t i = 0; i < n; i++) {
      uint8_t v = forceScaleNote(out[i]);
      bool dup = false;
      for (uint8_t j = 0; j < m; j++) if (out[j] == v) dup = true;
      if (!dup) out[m++] = v;
    }
    n = m;
  }

  for (uint8_t i = 0; i < n; i++) sNotes[i] = out[i];
  sCount = n;
  sPlayedRoot = (int16_t)root;
  sPlayed = qType;
  sendOut();
}

// chromatic + no quality: the pad alone (still through the engine so the
// pending chord, assign mode and the LEDs all see it)
static void playSingle(int note) {
  stopNotes();
  sNotes[0] = clampNote(note);
  sCount = 1;
  sPlayedRoot = (int16_t)sNotes[0];
  sPlayed = CHORD_Q_NONE;
  sendOut();
}

// KeyLock: snap the root to the nearest scale tone, then play - a held
// quality button wins over that degree's triad (KeyLock still quantizes
// the root, the quality row stays live)
static void playKeyLockAt(int note) {
  uint8_t pc = (uint8_t)(((note % 12) + 12) % 12);
  int deg = keylockDegree(pc);
  uint8_t q = (sQuality != CHORD_Q_NONE) ? sQuality : keylockQuality(deg);

  uint16_t rot = rotPattern();
  int bestDist = 12, snap = pc;
  for (int s = 0; s < 12; s++) {
    int tpc = (sRoot + s) % 12;
    if (rot & (uint16_t)(1u << tpc)) {
      int d = (int)pc - tpc;
      if (d < 0) d = -d;
      if (d > 6) d = 12 - d;
      if (d < bestDist) { bestDist = d; snap = tpc; }
    }
  }
  playChordAt(note - (int)pc + snap, q);
}

// the current gesture as a chord (press path; quality > diatonic > single)
static void triggerRoot() {
  if (sRootNote < 0) return;
  if (sLock) playKeyLockAt(sRootNote);
  else if (sQuality != CHORD_Q_NONE) playChordAt(sRootNote, sQuality);
  else if (sScale != 0) playChordAt(sRootNote, diatonicQuality((uint8_t)(sRootNote % 12)));
  else playSingle(sRootNote);
}

// re-trigger only while a chord is actually sounding (quality/ext/param
// change, octave shift)
static void retrigger() {
  if (sRootNote < 0 || sCount == 0) return;
  triggerRoot();
  keyPendingRefresh();
}

// ---------------------------------------------------------------------------
// gesture
// ---------------------------------------------------------------------------

int chordPadPress(uint8_t kidx, uint8_t note) {
  if (kidx >= 12) return -1;
  if (!(sHeld & (uint16_t)(1u << kidx))) {
    sHeld |= (uint16_t)(1u << kidx);
    sHeldCount++;
  }
  sPressNote[kidx] = note;
  sRootNote = note;
  triggerRoot();
  keyPendingRefresh();          // the press is the gesture snapshot
  return (sCount > 0) ? sNotes[0] : -1;
}

int chordPadRelease(uint8_t kidx) {
  if (kidx >= 12 || !(sHeld & (uint16_t)(1u << kidx))) return -1;
  sHeld &= (uint16_t)~(1u << kidx);
  if (sHeldCount) sHeldCount--;

  if (sHeld == 0) {
    if (sCount == 0) { sRootNote = -1; return 0; }
    if (sLatch) return sNotes[0];          // latch: keep sounding
    stopNotes();
    sRootNote = -1;
    return 0;
  }
  // keys remain: re-trigger from the first held key (the pending chord is
  // NOT re-fed - the gesture snapshot was taken on press, like the
  // single-note path)
  for (uint8_t k = 0; k < 12; k++) {
    if (sHeld & (uint16_t)(1u << k)) {
      sRootNote = sPressNote[k];
      triggerRoot();
      return (sCount > 0) ? sNotes[0] : -1;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// params
// ---------------------------------------------------------------------------

void chordSetQuality(uint8_t q) {
  if (q > CHORD_Q_SUS) q = CHORD_Q_NONE;
  if (q == sQuality) return;
  sQuality = q;
  retrigger();
}

void chordSetExt(uint8_t ext, bool on) {
  if (on) sExt |= ext;
  else sExt &= (uint8_t)~ext;
  retrigger();
}

void chordParamAdjust(uint8_t idx, int16_t counts) {
  if (idx >= 4) return;
  sParamRes[idx] += counts;
  int8_t step = 0;
  while (sParamRes[idx] >= CHORD_ENC) { sParamRes[idx] -= CHORD_ENC; step++; }
  while (sParamRes[idx] <= -CHORD_ENC) { sParamRes[idx] += CHORD_ENC; step--; }
  if (!step) return;
  int v = (int)sParam[idx] + step;
  if (v < 0) v = 0;
  if (v > 3) v = 3;
  if (v == sParam[idx]) return;
  sParam[idx] = (int8_t)v;
  retrigger();
}

void chordLatchAdjust(int16_t counts) {
  bool want = (counts > 0);          // CW = on, CCW = off (no wrap)
  if (want == sLatch) return;
  sLatch = want;
  // latch off with nobody holding a key = silence now (pending keeps the chord)
  if (!sLatch && sHeldCount == 0 && sCount > 0) {
    stopNotes();
    sRootNote = -1;
  }
}

void chordKeyLockAdjust(int16_t counts) {
  bool want = (counts > 0);
  if (want != sLock) sLock = want;   // takes effect on the next press
}

void chordPerfAdjust(int16_t counts) {
  bool want = (counts > 0);
  if (want != sPerf) sPerf = want;   // state only (the reference has no behavior)
}

void chordFScaleAdjust(int16_t counts) {
  bool want = (counts > 0);          // CW = on, CCW = off (no wrap)
  if (want == sFScale) return;
  sFScale = want;
  retrigger();                       // live: rebuild the chord it silences
}

bool chordFScale() { return sFScale; }

uint8_t chordParam(uint8_t idx) { return (idx < 4) ? (uint8_t)sParam[idx] : 0; }
bool chordLatch()  { return sLatch; }
bool chordKeyLock() { return sLock; }
bool chordPerf()   { return sPerf; }

// ---------------------------------------------------------------------------
// read accessors
// ---------------------------------------------------------------------------

const uint8_t* chordNotes() { return sNotes; }
uint8_t chordCount() { return sCount; }
bool chordSounding() { return sCount > 0; }

bool chordPlaysPitch(uint8_t pc) {
  pc %= 12;
  for (uint8_t i = 0; i < sCount; i++)
    if (sNotes[i] % 12 == pc) return true;
  return false;
}

const char* chordName() {
  static char buf[16];
  buf[0] = '\0';
  if (sPlayedRoot < 0) return buf;
  char ext[8] = "";
  if (sExt & CHORD_EXT_6)  strcat(ext, "6");
  if (sExt & CHORD_EXT_m7) strcat(ext, "m7");
  if (sExt & CHORD_EXT_M7) strcat(ext, "M7");
  if (sExt & CHORD_EXT_9)  strcat(ext, "9");
  snprintf(buf, sizeof(buf), "%s%s%s",
           kRootName[((sPlayedRoot % 12) + 12) % 12],
           kQualityName[sPlayed], ext);
  return buf;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

void chordShift(int semitones) {
  if (semitones == 0) return;
  for (uint8_t k = 0; k < 12; k++) {
    if (!(sHeld & (uint16_t)(1u << k))) continue;
    sPressNote[k] = clampNote((int)sPressNote[k] + semitones);
  }
  if (sRootNote < 0) return;
  sRootNote = (int16_t)clampNote((int)sRootNote + semitones);
  retrigger();
}

void chordCut() {
  if (sMute) { sCut = true; return; }   // muted: nothing is out to cut
  if (sCount == 0) return;
  for (uint8_t i = 0; i < sCount; i++)
    usbMIDI.sendNoteOff(sNotes[i], 0, keyChannel());
  sCut = true;
}

void chordRestore() {
  if (!sCut) return;
  sCut = false;
  sendOut();
}

// live keyboard through a MONO track: keep building the gesture chord
// (pending, LEDs, recording all see it) but silence the engine - the mono
// voice (ROOT/BASS/ARP) sounds on the same channel instead
void chordMute(bool on) {
  if (on == sMute) return;
  sMute = on;
  if (on) {
    if (!sCut) {                         // fresh engage: cut what is out
      for (uint8_t i = 0; i < sCount; i++)
        usbMIDI.sendNoteOff(sNotes[i], 0, keyChannel());
      sCut = true;
    }
  } else {
    sCut = false;
    sendOut();                           // re-send whatever sounds now
  }
}

void chordStopAll() {
  stopNotes();
  sCut = false;
  sHeld = 0;
  sHeldCount = 0;
  sRootNote = -1;
  // quality/extensions are momentary (cleared when their buttons release);
  // latch/KeyLock/Perf/voicing params persist like the reference's globals
}
