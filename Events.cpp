#include "Events.h"
#include <EEPROM.h>
#include "Buttons.h"
#include "Display.h"
#include "Modes.h"
#include "Encoders.h"
#include "Bindings.h"
#include "SeqClock.h"
#include "Keyboard.h"
#include "Chords.h"
#include "Key4.h"       // KEYMODE4 controls (focus/apply/undo/locks)
#include "Gen.h"        // GNR8 generator params / produce / rotate
#include "Skank.h"      // SKANK page: shape name for the header
#include "Drum.h"       // DRUM page: genre/var/lane params + produce

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

// PREV overlay (KEYMODE2) is held down - clear its slot on release
static bool prevOvActive = false;

// --- SEQMODE step-edit radio (all track types) -----------------------------
// M1 toggles the edit on/off, F1-F4 pick the target (0=STEPS, 1=TIE,
// 2=GLIDE, 3=ACCENT, 4=PROB; POLY adds 5=FX).  The state persists across
// mode switches; while ON, the step pads edit attributes instead of
// recording (STEPS target = normal recording) and ENC5/6/8 shape
// gate/velocity/probability.  DRUM/POLY take ACCENT/PROB from F3/F4 only,
// POLY takes FX from F1 (M1 keeps its part-select hold on DRUM).
// ENCMODE keeps its CCs, KEYMODE its keys - never there
static bool    stepEditOn = true;     // boot: pads record (STEPS)
static uint8_t stepEditTarget = 0;    // 0 = STEPS (normal step recording)
// which FX type the pads/ENC7 edit while target 5 is engaged (StepFxType)
static uint8_t stepFxType = SEQ_FX_SUS4;

static const char* stepFxName(uint8_t t) {
  static const char* n[7] = { "sus2", "sus4", "6", "m7",
                              "M7", "9", "color" };
  return n[t % 7];
}

// active in SEQMODE variants 1/2/4 while the edit track is MONO
static bool stepEditCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_MONO;
}

// DRUM/POLY variant: accent (F3) and probability (F4) only - TIE/GLIDE
// make no sense for one-shot drums or per-chord playback
static bool stepEditAccCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         (seqTrack() == SEQ_TRACK_DRUM || seqTrack() == SEQ_TRACK_POLY);
}

// true while an ATTRIBUTE target is engaged for the current track: MONO =
// TIE/GLIDE/ACCENT/PROB (1..4), POLY = ACCENT/PROB plus the FX target (5)
// and the TYPE target (6), EMB target (7), DRUM = ACCENT/PROB (3,4).  A target never
// matches a track that cannot edit it (a POLY FX/TYPE/EMB selection does not
// arm MONO/DRUM pads).  Drives the header label, the step pulse and the
// attribute pad mode
bool stepEditActive() {
  if (!stepEditOn || modeBase != MODE_SEQ || modeVariant == 3) return false;
  uint8_t trk = seqTrack();
  if (trk == SEQ_TRACK_MONO) return stepEditTarget >= 1 && stepEditTarget <= 4;
  if (trk == SEQ_TRACK_POLY)
    return stepEditTarget == 5 || stepEditTarget == 6 || stepEditTarget == 7 ||
           (stepEditTarget >= 3 && stepEditTarget <= 4);
  return stepEditTarget >= 3 && stepEditTarget <= 4;
}

static void stepEditSet(bool on, uint8_t target) {
  stepEditOn = on;
  stepEditTarget = target;
  stepChainDisarm();        // a target switch kills a TIE span arm
}

bool    stepEditIsOn() { return stepEditOn; }
uint8_t stepEditSel()  { return stepEditTarget; }
uint8_t stepFxSel()    { return stepFxType; }
const char* stepFxSelName() { return stepFxName(stepFxType); }

// which per-step TYPE the F2/arm flow stamps on a POLY track while the
// TYPE target is engaged (0..2 = SEQ_POLY_STEP / STRUM / HAND).  Stays
// across takes like the FX selection (both live in RAM only)
static uint8_t stepTypeArmV = SEQ_POLY_STEP;

void stepTypeArmAdjust(int16_t counts) {
  int t = (int)stepTypeArmV;
  int steps = counts > 0 ? (int)counts : -(int)counts;
  if (steps > 0) {
    if (counts > 0) t = (t + steps) % 3;
    else t = ((t - steps) % 3 + 3) % 3;
    stepTypeArmV = (uint8_t)t;
  }
}
uint8_t stepTypeArm() { return stepTypeArmV; }
const char* stepTypeArmName() {
  static const char *const n[3] = { "STEP", "STRUM", "HAND" };
  return n[stepTypeArmV % 3];
}

// --- SEQMODE M2 "ARP" page (variants 1/2/4) -------------------------------
// Clicking M2 rebinds the encoder row while the edit track is a MONO
// arp.  Like the other overlays the active check is a composite, so a
// mid-hold channel/TYPE/mode switch can never leave a stale page behind.
static bool arpPageOn = false;
// armed F sub-page (0..3, 0xFF = the plain 8-knob row).  Like the GNR8
// page's genOptSel this is UI state and dies with the page (toggle and
// dismiss both clear it), so a reopen always starts on the plain ARP row
static uint8_t arpOptSel = 0xFF;

static bool arpTrackCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_MONO && seqMonoType() == SEQ_MONO_ARP;
}

void arpPageToggle() { arpPageOn = !arpPageOn; arpOptSel = 0xFF; }
void arpPageDismiss() { arpPageOn = false; arpOptSel = 0xFF; }
bool arpPageActive()  { return arpPageOn && arpTrackCtx(); }
// the composite check already gates the armed page: a mid-hold channel /
// TRACK / TYPE / mode switch reads 0xFF here without needing a dismiss
uint8_t arpOptionSel() { return arpPageActive() ? arpOptSel : 0xFF; }
void arpOptionSet(uint8_t sel) { arpOptSel = (sel < 4) ? sel : 0xFF; }

const char *arpOptionName(uint8_t sel) {
  static const char *const n[4] = { "ARP ORD", "ARP MOT", "ARP GEN",
                                    "ARP OPT" };
  return (sel < 4) ? n[sel] : "ARP";
}

// Which ARP_P_* each sub-page puts on which encoder.  -1 keeps the plain
// ARP binding, so the base row's knobs stay reachable from inside every
// sub-page.  Encoder dispatch, the label row and the value column all
// read this one table, so a knob can never show one thing and move another
static const int8_t arpSlotT[4][8] = {
  // F1 ORDER & SHAPE
  { ARP_P_ORDER, ARP_P_STEP, ARP_P_PIVOT, ARP_P_WEIGHT, -1, -1, -1, -1 },
  // F2 MOTION
  { ARP_P_MUTATE, ARP_P_INSERT, ARP_P_CYCLE, ARP_P_MORPH, -1, -1, -1, -1 },
  // F3 GENERATIVE (ENC1 octave / ENC8 prob stay the base knobs)
  { -1, ARP_P_ENGINE, ARP_P_RATE, ARP_P_DEPTH, ARP_P_MEM, ARP_P_BIAS,
    ARP_P_SEED, -1 },
  // F4 ENGINE OPTS (the armed engine's own four, then target + clock)
  { ARP_P_OPT1, ARP_P_OPT2, ARP_P_OPT3, ARP_P_OPT4, ARP_P_TARGET,
    ARP_P_CLOCK, -1, -1 },
};

int8_t arpSlotAt(uint8_t page, uint8_t enc) {
  if (page > 3 || enc > 7) return -1;
  return arpSlotT[page][enc];
}

// --- SEQMODE M2 strum ADDS row (variants 1/2/4) ----------------------------
// Clicking M2 on a POLY STRUM/HAND edit track swaps the F row to the Le
// Strum ADDS colors: F1 = sus4, F2 = add6, F3 = add9 (independent RAM
// toggles), F4 clears all.  Composite active check like the arp page - a
// mid-hold track/TYPE/mode switch drops the row by itself
static bool strumFRowOn = false;

static bool strumFRowCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_POLY &&
         (seqPolyType() == SEQ_POLY_STRUM || seqPolyType() == SEQ_POLY_HAND);
}

void strumFRowToggle() { strumFRowOn = !strumFRowOn; }
void strumFRowDismiss() { strumFRowOn = false; }
bool strumFRowActive() { return strumFRowOn && strumFRowCtx(); }

// --- SEQMODE M2 "GNR8" page (variants 1/2/4) ------------------------------
// Clicking M2 on a MONO+BASS edit track rebinds ENC2/3/4/7 to the acid
// generator's knobs (fill/accent/glide/tie).  Like the arp page the
// active check is a composite, so a mid-hold channel/TYPE/mode switch can
// never leave a stale page behind.  ROOT/ARP/STRUM never take it
static bool genPageOn = false;
// armed F radio of that page (0..3, 0xFF = none): F1 (sel 0) owns the
// FULL four-knob options page (ENC1 TONES, ENC2 RANGE, ENC3 LOW, ENC4
// ALGO), F2 (sel 1) = STEP OCT (ENC2 of the tapped step + MAP mode on
// ENC3), F3 (sel 2) = BORROW (ENC3), F4 (sel 3) = MOD PROB (ENC4);
// it dies with the page - toggle and dismiss both clear it, so a reopen
// always starts with the plain GNR8 row
static uint8_t genOptSel = 0xFF;
// step armed as the dial target on the F2/F3/F4 mod pages (tap = select);
// dies with the page like genOptSel, validated on read
static int8_t genStepSel = -1;
// 4 encoder counts = 1 value on the mod dials (clamped, no wrap): one
// residual each for OCT / BRW / PROB (MAP keeps its own in seqMapAdjust)
static int16_t genModRes[3];

static bool genTrackCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_MONO && seqMonoType() == SEQ_MONO_BASS;
}

// SEQMODE non-DRUM: a held step pad arms the per-step TRANS overlay (ENC1)
// for that step; release closes it.  Kept separate from the GNR8-page
// genStepSel (that one lives only while the GNR8 F-radio sub-page is up)
static int8_t seqTrStepSel = -1;

void genPageToggle() {
  genPageOn = !genPageOn;
  genOptSel = 0xFF;
  genStepSel = -1;
}
void genPageDismiss() {
  genPageOn = false;
  genOptSel = 0xFF;
  genStepSel = -1;
}
bool genPageActive()  { return genPageOn && genTrackCtx(); }
uint8_t genOptionSel() { return genPageActive() ? genOptSel : 0xFF; }

// --- SEQMODE M2 "LEAD" page (variants 1/2/4) -------------------------------
// Clicking M2 on a MONO+LEAD edit track rebinds ENC2/3/4/7 to the melody
// generator's knobs (contour/low/range/density).  Like the ARP/GNR8 pages
// the active check is composite, so a mid-hold channel/TYPE/mode switch
// can never leave a stale page behind.  ROOT/BASS/ARP/STRUM never take it.
static bool leadPageOn = false;

static bool leadTrackCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_MONO && seqMonoType() == SEQ_MONO_LEAD;
}

void leadPageToggle() { leadPageOn = !leadPageOn; }
void leadPageDismiss() { leadPageOn = false; }
bool leadPageActive()  { return leadPageOn && leadTrackCtx(); }

// --- SEQMODE M2 "HARM" page (variants 1/2/4) --------------------------------
// Clicking M2 on a POLY+HARM edit track rebinds the encoder row to the
// harmonizer: ENC1 = GENRE (the harmonic grammar), ENC2 = FILL (density
// gate of the baked triads), ENC3 = SRC (bass/arp source), ENC4 = HOLD
// (force hold on a repeated bass).  M3 bakes the progression inferred from
// the FOLLOW feeder's bassline into the track's steps, M3 hold undoes it.
// Composite active check like the other pages.
static bool harmPageOn = false;

static bool harmTrackCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_POLY && seqPolyType() == SEQ_POLY_HARM;
}

void harmPageToggle() { harmPageOn = !harmPageOn; }
void harmPageDismiss() { harmPageOn = false; }
bool harmPageActive()  { return harmPageOn && harmTrackCtx(); }

// --- SEQMODE M2 "COMP" page (variants 1/2/4) --------------------------------
// Clicking M2 on a POLY+STEP edit track rebinds the encoder row to the
// comping: ENC1 = HITS (chord stabs per step on the Euclid on-positions of
// 8 slots; 0 or 1 = today's single block chord).  ENC2..4 stay free
// (rotation / per-stab gate are reserved).  Composite active check like
// the other pages - a mid-turn channel / TYPE / mode switch drops it
static bool compPageOn = false;

static bool compTrackCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_POLY && seqPolyType() == SEQ_POLY_STEP;
}

void compPageToggle() { compPageOn = !compPageOn; }
void compPageDismiss() { compPageOn = false; }
bool compPageActive()  { return compPageOn && compTrackCtx(); }

// --- SEQMODE M2 "SKANK" page (variants 1/2/4) ------------------------------
// Clicking M2 on a POLY+SKANK edit track rebinds the encoder row to the
// derivation: ENC1 = TES (tessitura anchor), ENC2 = MOT (motion), ENC3 =
// SHP (shape), ENC4 = THN (thinning), ENC5 = EXT (extension %), ENC6 =
// SUB (substitution %), ENC7 = LEN, ENC8 = SPEED - the last two mirror
// SEQMODE3's pattern knobs so the walk fits the track without a mode hop.
// Composite active check like the other pages - a mid-turn channel /
// TYPE / mode switch drops it
static bool skankPageOn = false;

static bool skankTrackCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_POLY && seqPolyType() == SEQ_POLY_SKANK;
}

void skankPageToggle() { skankPageOn = !skankPageOn; }
void skankPageDismiss() { skankPageOn = false; }
bool skankPageActive()  { return skankPageOn && skankTrackCtx(); }

// --- SEQMODE M2 "DRUM" page (variants 1/2/4) --------------------------------
// Clicking M2 on a DRUM edit track opens the pattern page (indigo pad): the
// base row is eight knobs - ENC1 GENRE, ENC2 VAR, ENC3 FILL %, ENC4 MOT %,
// ENC5..8 the KICK/SNARE/HAT/PERC line selectors - and the F row picks one
// of four knob SETS that zoom the same four encoders: GROOVE (genre/var/
// fill/mot), LINES, FILL% and MOT% per lane.  Clicking the armed set again
// returns to the base row.  Every turn rebuilds the pattern live through
// Drum.cpp; M3 re-rolls, M3 hold undoes.  Composite active check like the
// other pages - a mid-turn channel / TRACK / mode switch drops it
static bool drumPageOn = false;
static uint8_t drumSetSel = 0xFF;      // 0..3 = armed F set, 0xFF = base row

static bool drumTrackCtx() {
  return modeBase == MODE_SEQ && modeVariant != 3 &&
         seqTrack() == SEQ_TRACK_DRUM;
}

void drumPageToggle() { drumPageOn = !drumPageOn; drumSetSel = 0xFF; }
void drumPageDismiss() { drumPageOn = false; drumSetSel = 0xFF; }
bool drumPageActive()  { return drumPageOn && drumTrackCtx(); }
uint8_t drumSetSelGet() { return drumPageActive() ? drumSetSel : 0xFF; }
void drumSetSelSet(uint8_t s) { drumSetSel = (s <= 3) ? s : 0xFF; }

// selected step for the mod dials (F2/F3/F4), or -1: nothing picked yet,
// or the pattern shrank / the step lost its content under the selection
int8_t genStepSelStep() {
  if (!genPageActive() || genStepSel < 0) return -1;
  uint8_t st = (uint8_t)genStepSel;
  if (st >= seqPatLenOf((uint8_t)(seqEditChannel() - 1)) || !stepIsActive(st)) {
    genStepSel = -1;
    return -1;
  }
  return genStepSel;
}

// the held step for the SEQMODE per-step TRANS overlay; a mid-hold mode
// switch drops it by itself (composite check like the page accessors)
int8_t seqTrStepSelGet() {
  if (seqTrStepSel < 0 || modeBase != MODE_SEQ) return -1;
  return seqTrStepSel;
}

// 4 counts = 1 value step of dial i (0 = OCT, 1 = BRW, 2 = PROB)
static int16_t genModStep(uint8_t i, int16_t counts) {
  genModRes[i] += counts;
  int16_t s = 0;
  while (genModRes[i] >= 4) { genModRes[i] -= 4; s++; }
  while (genModRes[i] <= -4) { genModRes[i] += 4; s--; }
  return s;
}

// header line for an armed option (F1..F4 press) or a sub-page dial
// change.  On the F1 page sel names the knob that moved (0 TONES /
// 1 LOW / 2 RANGE / 3 ALGO): "TONES all" / "LOW C1" / "RANGE 2oct" /
// "ALGO CLASSIC".  On the mod pages sel is the armed radio itself:
// "step 5 oct +1" / "step 5 brw 3" / "step 5 prob 60%" (nothing picked:
// "step oct" / "step brw" / "step prob" - the dial itself flashes
// "pick step" instead)
static void genOptionHeader(uint8_t sel) {
  uint8_t ch = (uint8_t)(seqEditChannel() - 1);
  char hb[24];
  if (genOptSel == 0) {
    if (sel == 0)
      snprintf(hb, sizeof(hb), "TONES %s", seqGenTonesName(seqGenTonesOf(ch)));
    else if (sel == 1)
      snprintf(hb, sizeof(hb), "LOW %s", seqGenLowName(seqGenLowOf(ch)));
    else if (sel == 2)
      snprintf(hb, sizeof(hb), "RANGE %uoct", seqGenRangeOf(ch));
    else
      snprintf(hb, sizeof(hb), "ALGO %s", seqGenAlgoName(seqGenAlgoOf(ch)));
  } else if (sel == 1) {
    int8_t ss = genStepSelStep();
    if (ss < 0)
      snprintf(hb, sizeof(hb), "step oct");
    else if (stepOctAt((uint8_t)ss))
      snprintf(hb, sizeof(hb), "step %u oct %+d", ss + 1,
               (int)stepOctAt((uint8_t)ss));
    else
      snprintf(hb, sizeof(hb), "step %u oct 0", ss + 1);
  } else if (sel == 2) {
    int8_t ss = genStepSelStep();
    if (ss < 0)
      snprintf(hb, sizeof(hb), "step brw");
    else if (stepBrwAt((uint8_t)ss) == 0)
      snprintf(hb, sizeof(hb), "step %u brw off", ss + 1);
    else
      snprintf(hb, sizeof(hb), "step %u brw %u", ss + 1,
               stepBrwAt((uint8_t)ss));
  } else {
    int8_t ss = genStepSelStep();
    if (ss < 0)
      snprintf(hb, sizeof(hb), "step prob");
    else
      snprintf(hb, sizeof(hb), "step %u prob %u%%", ss + 1,
               stepModPAt((uint8_t)ss));
  }
  headerShow(hb);
  if (logRoom(48)) {
    Serial.print("  ");
    Serial.println(hb);
  }
}

// --- GNR8 TIE span fill (hold-arm gesture) --------------------------------
// With the step-edit radio on TIE on the GNR8 track (MONO+BASS), HOLD a
// step to arm the span start (header "chain from N" - the touch-down tap
// of that same press is reverted first, so holding never changes the
// flag); the next tap on ANOTHER step stamps TIE across start..end-1
// (wrapping inside the pattern length - the span rings over rests).  Tap
// the armed start itself = cancel; 5 s without execution disarms.
// Plain taps NEVER arm: TIE and the other attributes (GLIDE/ACCENT/PROB
// never span at all) stay ordinary per-step toggles, like in every other
// track type.
static bool     chainArmed = false;
static uint8_t  chainStart = 0;
static uint32_t chainArmAt = 0;
// the step this press toggled (0 = none, else step+1): a HOLD arriving on
// the same press reverts it, so hold-to-arm leaves the flag untouched
static uint8_t  pressToggled = 0;

void stepChainDisarm() { chainArmed = false; }
bool stepChainArmed()  { return chainArmed && genTrackCtx() &&
                                 stepEditTarget == 1; }

// HOLD F3 in SEQMODE: ENC6 rebinds from the track's default velocity to
// its ACCENT velocity (the quick tap still picks the ACCENT target)
static bool f3AccentSel = false;

void f3AccentHold(bool on) { f3AccentSel = on; }

// composite: the flag can outlive a mid-hold mode switch - the rebind
// only applies while the panel is still in SEQMODE variants 1/2/4 (=
// everywhere the hold is offered).  SEQMODE3 owns ENC6 = TRACK, so the
// rebind must NOT reach it - hence "!= 3", not "== 3": with "== 3" the
// hold did nothing in 1/2/4 (no ACC label, no value, dead knob) and a
// stale flag then painted ACC over SEQMODE3's TRACK row.
bool f3AccentActive() {
  return f3AccentSel && modeBase == MODE_SEQ && modeVariant != 3;
}

// a SEQ channel switch always lands back in step edit on the new channel:
// M1 (STEPS target) selected, radio on, and every page overlay of the OLD
// track dismissed (the new track may be any type)
static void seqEditChannelLand(uint8_t ch) {
  seqSetEditChannel(ch);
  stepEditSet(true, 0);
  arpPageDismiss();       // the new track may not be an arp
  strumFRowDismiss();     // ...and it may not strum either
  genPageDismiss();       // nor a BASS track for the GNR8 page
  leadPageDismiss();      // nor a LEAD track for the LEAD page
  harmPageDismiss();      // nor a HARM track for the HARM page
  compPageDismiss();      // nor a POLY/STEP track for the COMP page
  skankPageDismiss();     // nor a POLY/SKANK track for the SKANK page
  drumPageDismiss();      // nor a DRUM track for the drum pattern page
  stepChainDisarm();      // ...nor an armed chain span (old ch)
  refreshAllLeds();       // M1 + F-row follow the radio again
  refreshMainScreen();
  refreshHeader();
}

// KEYMODE3 ENC3/ENC4 channel link (ENCOD / KBOARD, free or locked): the
// SEQ edit channel is the hub.  A LOCKED family follows every hub change;
// a change in a locked family drives the hub, and the other locked family
// follows transitively.  Free families stay independent - exactly the
// pre-link behavior.  Only the variant-3 channel overlays call this
// (EEPROM loads / internal focus swaps must never propagate)
static void channelLinkPropagate(uint8_t from) {  // 0 = KEY, 1 = ENC, 2 = SEQ
  static bool busy;
  if (busy) return;
  bool kL = chanKeyLocked(), eL = chanEncLocked();
  if (!kL && !eL) return;                 // both free: nothing to do
  if (from == 0 && !kL) return;           // a free family moved: local only
  if (from == 1 && !eL) return;
  busy = true;
  uint8_t ch = (from == 0) ? keyChannel()
             : (from == 1) ? encChannel()
                           : seqEditChannel();   // already 1-based
  if (from != 2 && ch != seqEditChannel())
    seqEditChannelLand(ch);               // the hub takes the new channel
  if (from != 1 && eL) encSetChannel(ch);
  if (from != 0 && kL) keySetChannel(ch);
  if (logRoom(48)) {
    Serial.print("  channel link -> ");
    Serial.println(ch);
  }
  busy = false;
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

    // pad classification (LED numbers): keyboard chromatic group, step group
    int8_t kl = ledForPad[pad];
    uint8_t kidx = 255, sidx = 255;
    for (uint8_t i = 0; i < KEYBOARD_NUM_G; i++)
      if (groupKeyboard[i] == kl) kidx = i;
    for (uint8_t i = 0; i < STEPS_NUM_G; i++)
      if (groupSteps[i] == kl) sidx = i;

    // step pads (all modes, behavior owned by SEQMODE):
    //  press   -> trigger the step with its chord (or the last keyboard chord)
    //  hold    -> assign mode: play keys to replace this step's chord
    //  release -> note-off
    if (sidx != 255) {
      // variant-3 channel overlays: steps select a MIDI channel
      // (KEY = keyboard output, SEQ = sequencer edit, ENC = encoder CCs).
      // SEQ3's selector is gone once an encoder moved - the pads are back
      // on the sequencer until SEQ is held into SEQMODE3 again
      bool chOverlay = (modeVariant == 3 &&
                        (modeBase == MODE_KEY || modeBase == MODE_ENC ||
                         (modeBase == MODE_SEQ && seq3ChSelActive())));
      // DRUM track: step press = toggle the selected part (voice)
      bool drum = (modeBase == MODE_SEQ && modeVariant != 3 &&
                   seqTrack() == SEQ_TRACK_DRUM);
      // hold M1 = part-select overlay: steps choose the drum voice
      bool partSel = drum && drumPartSelectActive();
      // hold M2 in ENC mode = channel-mute overlay: steps toggle channel mutes
      bool encMute = encMuteActive();
      // hold M1 in ENC mode = drum-voice mute overlay: steps toggle the
      // muted drum voices (note 36+N) across all drum-track sequences
      bool drumVoice = drumVoiceMuteActive();
      // attribute mode: an attribute target is engaged - presses toggle it
      // on ACTIVE steps (empty steps stay inert)
      bool attrMode = stepEditActive();
      // edit off: the step pads are inert on a MONO track (no accidental
      // recording; release still runs for defensive cleanup)
      bool editLocked = stepEditCtx() && !stepEditOn;
      // pads address the edit WINDOW (one bar of 16): map pad -> absolute
      // step of the sequence (overlays above keep using the raw pad index)
      uint8_t stp = (uint8_t)(sidx + seqStepViewBase());
      if (encMute) {
        if (ev == EV_PRESSED) {
          seqMuteToggle(sidx);
          char mb[12];
          snprintf(mb, sizeof(mb), "ch%u %s", sidx + 1,
                   seqIsMuted(sidx) ? "mute" : "on");
          headerShow(mb);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(mb);
          }
        }
      } else if (drumVoice) {
        if (ev == EV_PRESSED) {
          drumVoiceMuteToggle(36 + sidx);
          char db[12];
          snprintf(db, sizeof(db), "%s %s", drumVoiceName(36 + sidx),
                   drumVoiceMuted(36 + sidx) ? "mute" : "on");
          headerShow(db);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(db);
          }
        }
      } else if (chOverlay) {
        if (ev == EV_PRESSED) {
          // each press moves its OWN family first, then the KEYMODE3
          // channel link (ENCOD/KBOARD locked) propagates the change to
          // the hub and the other locked families
          if (modeBase == MODE_KEY) {
            keySetChannel(sidx + 1);
            channelLinkPropagate(0);
          } else if (modeBase == MODE_SEQ) {
            seqEditChannelLand(sidx + 1);
            channelLinkPropagate(2);
          } else {
            encSetChannel(sidx + 1);
            channelLinkPropagate(1);
          }
          if (modeBase == MODE_ENC)
            refreshMainScreen();   // CC values belong to the new channel
          if (logRoom(48)) {
            Serial.print(modeBase == MODE_KEY ? "  kbd channel " :
                         modeBase == MODE_SEQ ? "  seq channel " :
                                                "  enc channel ");
            Serial.println(modeBase == MODE_KEY ? keyChannel() :
                            modeBase == MODE_SEQ ? seqEditChannel() :
                                                   encChannel());
          }
        } else if (ev == EV_RELEASED) {
          stepAssignSet(-1);   // defensive: never leave assign mode armed
        }
      } else if (partSel) {
        if (ev == EV_PRESSED) {
          drumPartSet(sidx);
          if (logRoom(48)) {
            Serial.print("  part ");
            Serial.println(drumPartName());
          }
        }
      } else if (!seqStepUsed(stp) && (ev == EV_PRESSED || ev == EV_CLICKED ||
                                       ev == EV_HOLD)) {
        // past the pattern length: the pad is dark AND inert - no record,
        // no toggle, no assign, no attribute edit (release still falls
        // through to the defensive cleanup below)
        if (ev == EV_PRESSED && logRoom(48)) Serial.println("  step unused");
      } else if (attrMode && (ev == EV_PRESSED || ev == EV_CLICKED ||
                              ev == EV_HOLD)) {
        // attribute edit: a tap on an ACTIVE step toggles the selected
        // attribute; empty steps and clicks do nothing here.  HOLD (GNR8
        // track, TIE target only) arms the span fill: the touch-down
        // toggle of this same press is reverted first, the header
        // announces the start, and the next tap on ANOTHER step stamps
        // TIE across start..end-1 (wrapping in the pattern length - the
        // span rings over rests).  Tap the armed start = cancel, 5 s
        // disarms; every plain tap just toggles, GLIDE/ACCENT/PROB never
        // span at all
        static const uint8_t attOf[4] = { SEQ_ATT_TIE, SEQ_ATT_GLIDE,
                                          SEQ_ATT_ACCENT, SEQ_ATT_PROB };
        static const char *const nmOf[4] = { "tie", "glide", "accent",
                                             "prob" };
        bool chainCtx = genTrackCtx() && stepEditTarget == 1;  // TIE only
        char ab[24];
        // a forgotten arm expires after 5 s (plain toggles again)
        if (chainArmed && (uint32_t)(millis() - chainArmAt) >= 5000u)
          chainArmed = false;
        if (ev == EV_PRESSED) pressToggled = 0;   // fresh for this press
        if (ev == EV_HOLD && chainCtx) {
          // hold = arm the span start; undo this press's touch-down
          // toggle first, so holding never changes the flag itself
          if (pressToggled == (uint8_t)(stp + 1))
            stepAttrToggle(stp, SEQ_ATT_TIE);
          pressToggled = 0;
          chainArmed = true;
          chainStart = stp;
          chainArmAt = millis();
          snprintf(ab, sizeof(ab), "chain from %u", stp + 1);
          headerShow(ab);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(ab);
          }
        } else if (ev == EV_PRESSED && chainCtx && chainArmed &&
                   stp == chainStart) {
          // tap the armed start itself = cancel the span
          chainArmed = false;
          headerShow("chain off");
          if (logRoom(48)) Serial.println("  chain off");
        } else if (ev == EV_PRESSED && chainCtx && chainArmed &&
            stp != chainStart) {
          uint8_t att = attOf[stepEditTarget - 1];
          uint8_t n = stepAttrChain(chainStart, stp, att);
          chainArmed = false;
          if (n)
            snprintf(ab, sizeof(ab), "%u steps %s", n,
                     nmOf[stepEditTarget - 1]);
          else
            snprintf(ab, sizeof(ab), "chain empty");
          headerShow(ab);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(ab);
          }
          refreshAllLeds();   // the F-row blink mask follows the new flags
        } else if (ev == EV_PRESSED && stepIsActive(stp)) {
          bool on = false;
          if (stepEditTarget == 5) {
            // POLY FX target: the pad arms the SELECTED FX type on it
            on = stepFxToggle(stp, stepFxType);
            snprintf(ab, sizeof(ab), "%u %s %s", stp + 1,
                     stepFxName(stepFxType), on ? "on" : "off");
          } else if (stepEditTarget == 6) {
            // POLY TYPE target: the pad stamps the ARMED override on it
            // (a second tap clears back to inherit-the-track)
            on = stepTypeToggle(stp, stepTypeArmV);
            snprintf(ab, sizeof(ab), "%u %s %s", stp + 1,
                     on ? stepTypeArmName() : "type",
                     on ? "on" : "inherit");
          } else if (stepEditTarget == 7) {
            // POLY EMB target: cycle through SUB/INV/PAS/COL for this step
            // ENC7 selects which param to edit, tap cycles the value
            on = stepEmbToggle(stp);
            snprintf(ab, sizeof(ab), "%u %s", stp + 1, on ? "emb" : "emb");
          } else {
            uint8_t att = attOf[stepEditTarget - 1];
            on = stepAttrToggle(stp, att);
            if (seqTrack() == SEQ_TRACK_DRUM) {
              // per voice: name the SELECTED PART and its short attribute
              snprintf(ab, sizeof(ab), "%u %s %s %s", stp + 1,
                       drumPartName(), att == SEQ_ATT_ACCENT ? "acc" : "prb",
                       on ? "on" : "off");
            } else {
              snprintf(ab, sizeof(ab), "%u %s %s", stp + 1,
                       nmOf[stepEditTarget - 1], on ? "on" : "off");
            }
          }
          headerShow(ab);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(ab);
          }
          if (chainCtx) pressToggled = (uint8_t)(stp + 1);
          // (a HOLD arriving on this same press reverts that toggle and
          // arms the span from here instead)
        }
      } else if (genPageActive() && genOptSel >= 1 && genOptSel <= 3 &&
                 ev == EV_PRESSED && stepIsActive(stp)) {
        // GNR8 mod pages (F2 OCT / F3 BRW / F4 PROB): a tap on an ACTIVE
        // step selects it as the dial target (header shows its value,
        // the pad lights up); empty steps fall through to editLocked and
        // stay inert.  The selection dies with the page - F1 / none-armed
        // taps never record here (editLocked catches those below)
        genStepSel = (int8_t)stp;
        genOptionHeader(genOptSel);
        if (logRoom(48)) {
          Serial.print("  gen step ");
          Serial.println(stp + 1);
        }
        refreshAllLeds();
      } else if (editLocked && (ev == EV_PRESSED || ev == EV_CLICKED ||
                                ev == EV_HOLD)) {
        // edit off: pads inert (no record, no assign mode, no disable)
        if (ev == EV_PRESSED && logRoom(48))
          Serial.println("  step edit off");
      } else if (ev == EV_PRESSED) {
        // KEYMODE4: a step pad press moves the write target there first,
        // then the usual trigger/assign behavior runs on it
        if (modeBase == MODE_KEY && modeVariant == 4) k4FocusSet(stp);
        if (drum) {
          int r = stepDrumToggle(stp);
          if (r == -2 && logRoom(48)) {
            Serial.println("  part off");
          } else if (r >= 0 && logRoom(48)) {
            Serial.print("  part on ");
            Serial.println(drumPartName());
          }
        } else {
          int n = stepChordPress(stp);
          if (n > 0 && logRoom(48)) {
            Serial.print("  step record notes=");
            Serial.println(n);
          }
        }
      } else if (ev == EV_CLICKED) {
        // KEYMODE4 AUDITION: a tap never disables a step - disable only
        // lands in WRITE (AUDITION promises nothing is written here)
        bool k4 = (modeBase == MODE_KEY && modeVariant == 4);
        if (k4 && !k4Write()) {
          if (logRoom(48)) Serial.println("  k4 audition: step kept");
        } else if (!drum && stepChordClick(stp) == 0 && logRoom(48))
          Serial.println("  step disabled");
      } else if (ev == EV_HOLD) {
        if (modeBase == MODE_KEY && modeVariant == 4) {
          // KEYMODE4: holding a step swaps ENC1..ENC4 onto OCT/DEG/INV/
          // VOICE for THAT step (release restores the mode row, or the EMB
          // menu if it was open); WRITE keeps the assign gesture on top
          k4StepSelSet((int8_t)stp);
          setOverlay(G_ENC, ENC_KEY4_STEP);
          if (k4Write()) stepAssignSet((int8_t)stp);
          refreshMainScreen();    // the encoder labels are the overlay's now
          char sb[24];
          snprintf(sb, sizeof(sb), "step %u", stp + 1);
          headerShow(sb);
          if (logRoom(48)) {
            Serial.print("  step hold ");
            Serial.println(stp + 1);
          }
        } else if (!drum) {       // DRUM steps are toggles, no assign mode
          stepAssignSet((int8_t)stp);
          // SEQMODE: the hold ALSO swaps ENC1 onto this step's TRANS
          // offset (the other encoders keep their idle row; DRUM tracks
          // never take it - their steps are voice toggles)
          if (modeBase == MODE_SEQ) {
            seqTrStepSel = (int8_t)stp;
            // a POLY+SKANK track swaps the row onto its LOCK knobs instead
            // (encoders 1..6 = per-step derivation overrides, 8 = TRANS)
            bool skankRow = modeVariant != 3 &&
                            seqTrack() == SEQ_TRACK_POLY &&
                            seqPolyType() == SEQ_POLY_SKANK;
            setOverlay(G_ENC, skankRow ? ENC_SEQ_SKANK_STEP
                                       : ENC_SEQ_STEPOV);
            refreshMainScreen();  // the encoder labels are the overlay's
            char tb[24];
            if (skankRow) {
              snprintf(tb, sizeof(tb), "step %u locks", stp + 1);
            } else {
              int8_t tv = stepTrAt(stp);
              if (tv >= 0)
                snprintf(tb, sizeof(tb), "step %u trans +%d", stp + 1, tv);
              else
                snprintf(tb, sizeof(tb), "step %u trans %d", stp + 1, tv);
            }
            headerShow(tb);
            if (logRoom(48)) {
              Serial.print("  ");
              Serial.println(tb);
            }
          } else if (logRoom(48)) {
            Serial.print("  step assign ");
            Serial.println(stp + 1);
          }
        }
      } else if (ev == EV_RELEASED) {
        if (k4StepSelGet() >= 0) {
          // end the per-step row (never clobber an overlay someone else
          // took over in the meantime, e.g. PREV pressed mid-hold)
          k4StepSelSet(-1);
          if (currentBinding().enc == ENC_KEY4_STEP) {
            clearOverlay(G_ENC);
            if (k4EmbMenu()) setOverlay(G_ENC, ENC_KEY4_EMB);
          }
          refreshMainScreen();    // labels back to the mode's own row
        }
        if (seqTrStepSel >= 0) {
          // SEQMODE step-trans row lets go (same never-clobber guard);
          // both held-step rows (plain TRANS, SKANK locks) share the sel
          seqTrStepSel = -1;
          if (currentBinding().enc == ENC_SEQ_STEPOV ||
              currentBinding().enc == ENC_SEQ_SKANK_STEP)
            clearOverlay(G_ENC);
          refreshMainScreen();
        }
        stepAssignSet(-1);
      }
      refreshAllLeds();   // recording may have toggled the step's dim-green LED
    }

    // PART SELECT (DRUM track): press M1 -> orange overlay on the steps to
    // pick which drum voice the steps sequence (release closes it; the
    // overlay comes up on press so it appears immediately, no 300ms wait).
    // ENCMODE: press M1 instead opens the drum-VOICE mute overlay
    if (ledForPad[pad] == groupControls[2]) {
      if (ev == EV_PRESSED && modeBase == MODE_SEQ && modeVariant != 3 &&
          seqTrack() == SEQ_TRACK_DRUM) {
        drumPartSelectHold(true);
        headerShow("PART SELECT");
        if (logRoom(48)) {
          Serial.print("  part select (");
          Serial.print(drumPartName());
          Serial.println(")");
        }
        refreshAllLeds();
      } else if (ev == EV_RELEASED && drumPartSelectActive()) {
        drumPartSelectHold(false);
        refreshAllLeds();
      } else if (ev == EV_PRESSED && modeBase == MODE_ENC) {
        drumMuteHold(true);
        headerShow("DRUM MUTE");
        if (logRoom(48)) Serial.println("  drum voice mute");
        refreshAllLeds();
      } else if (ev == EV_RELEASED && drumVoiceMuteActive()) {
        drumMuteHold(false);
        refreshAllLeds();
      } else if (ev == EV_CLICKED && stepEditCtx()) {
        // SEQMODE + MONO: CLICK on M1 cycles the edit radio - OFF -> ON
        // (STEPS target), ON+STEPS -> OFF, ON+attribute -> back to STEPS.
        // Turning it ON closes the M2 GNR8/ARP page first - the keys are
        // one radio (each page's own toggle does the reverse)
        bool droppedPage = false;
        if (!stepEditOn) {
          if (genPageActive()) {      // M1 wins: the GNR8 page leaves
            genPageDismiss();
            droppedPage = true;
            if (logRoom(48)) Serial.println("  gen page off (edit on)");
          }
          if (arpPageActive()) {      // ...and so does the ARP page
            arpPageDismiss();
            droppedPage = true;
            if (logRoom(48)) Serial.println("  arp page off (edit on)");
          }
          if (leadPageActive()) {     // ...and the LEAD page
            leadPageDismiss();
            droppedPage = true;
            if (logRoom(48)) Serial.println("  lead page off (edit on)");
          }
          if (harmPageActive()) {     // ...and the HARM page
            harmPageDismiss();
            droppedPage = true;
            if (logRoom(48)) Serial.println("  harm page off (edit on)");
          }
          if (compPageActive()) {     // ...and the COMP page
            compPageDismiss();
            droppedPage = true;
            if (logRoom(48)) Serial.println("  comp page off (edit on)");
          }
          if (skankPageActive()) {    // ...and the SKANK page
            skankPageDismiss();
            droppedPage = true;
            if (logRoom(48)) Serial.println("  skank page off (edit on)");
          }
          if (drumPageActive()) {     // ...and the DRUM pattern page
            drumPageDismiss();
            droppedPage = true;
            if (logRoom(48)) Serial.println("  drum page off (edit on)");
          }
          stepEditSet(true, 0);
          headerShow("EDIT ON");
          if (logRoom(48)) Serial.println("  step edit on (steps)");
        } else if (stepEditTarget != 0) {
          stepEditSet(true, 0);
          headerShow("STEPS");
          if (logRoom(48)) Serial.println("  edit target steps");
        } else {
          stepEditSet(false, 0);
          headerShow("EDIT OFF");
          if (logRoom(48)) Serial.println("  step edit off");
        }
        refreshAllLeds();
        if (droppedPage) refreshMainScreen();   // the encoder row changes
      } else if (ev == EV_CLICKED && stepEditAccCtx()) {
        // SEQMODE + DRUM/POLY: a QUICK tap of M1 toggles the edit master only
        // (a HOLD still opens the part select - hold suppresses the click)
        if (stepEditOn) {
          stepEditSet(false, 0);
          headerShow("EDIT OFF");
          if (logRoom(48)) Serial.println("  step edit off");
        } else {
          // master on: reuse the last attribute, default to ACCENT
          uint8_t t = (stepEditTarget >= 3) ? stepEditTarget : 3;
          if (t == 5 && seqTrack() != SEQ_TRACK_POLY) t = 3; // FX is POLY-only
          if (t == 6 && seqTrack() != SEQ_TRACK_POLY) t = 3; // TYPE is POLY-only
          stepEditSet(true, t);
          if (t == 3) headerShow("ACCENT");
          else if (t == 4) headerShow("PROB");
          else {
            char hb[16];
            snprintf(hb, sizeof(hb), "%s %s", t == 6 ? "TYPE" : "FX",
                     t == 6 ? stepTypeArmName() : stepFxName(stepFxType));
            headerShow(hb);
          }
          if (logRoom(80)) {
            Serial.print("  step edit on (");
            Serial.println(t == 3 ? "accent)" : t == 4 ? "prob)"
                           : t == 6 ? "type)" : "fx)");
          }
        }
        refreshAllLeds();
      }
    }

    // HOLD F3 in SEQMODE: ENC6 becomes the track's ACCENT velocity knob
    // (the quick tap below still picks the ACCENT target - a hold ends in
    // EV_RELEASED and never reaches EV_CLICKED).  The strum ADDS row owns
    // F3 while it is open (add9), the GNR8 page while it is open (RANGE)
    // and the ARP page (F3 = the generative sub-page) - the accent hold
    // stays out of all of their ways
    if (modeBase == MODE_SEQ && modeVariant != 3 && !strumFRowActive() &&
        !genPageActive() && !drumPageActive() && !arpPageActive() &&
        kl >= groupControls[5] && kl <= groupControls[8] &&
        (uint8_t)(kl - groupControls[5]) == 2) {
      if (ev == EV_PRESSED) {
        f3AccentHold(true);
        headerShow("ACCENT VEL");
        refreshMainScreen();
        if (logRoom(48)) {
          Serial.print("  accent vel ");
          Serial.println(seqAccentVelocity());
        }
      } else if (ev == EV_RELEASED && f3AccentSel) {
        f3AccentHold(false);
        refreshMainScreen();
        if (logRoom(48)) {
          Serial.print("  accent vel done ");
          Serial.println(seqAccentVelocity());
        }
      }
    }

    // STRUM ADDS row (open via M2): F1..F3 toggle sus4 / add6 / add9,
    // F4 clears the selection - independent bits, combinable, RAM only
    if (ev == EV_CLICKED && strumFRowActive() &&
        kl >= groupControls[5] && kl <= groupControls[8]) {
      uint8_t idx = (uint8_t)(kl - groupControls[5]);      // 0..3
      static const char *const xn[3] = { "sus4", "add6", "add9" };
      if (idx == 3) {
        strumExtSet(0);
        headerShow("ADDS cleared");
        if (logRoom(48)) Serial.println("  adds cleared");
      } else {
        strumExtSet((uint8_t)(strumExtGet() ^ (1u << idx)));
        char hb[16];
        snprintf(hb, sizeof(hb), "%s %s", xn[idx],
                 (strumExtGet() >> idx) & 1 ? "on" : "off");
        headerShow(hb);
        if (logRoom(48)) {
          Serial.print("  adds ");
          Serial.println(hb);
        }
      }
      refreshAllLeds();
    } else
    // ARP page (open via M2 on a MONO/ARP track): F1..F4 arm the encoder
    // SUB-PAGES (order & shape / motion / generative / engine options).
    // Click the armed one again to fall back to the plain 8-knob row, click
    // another to switch.  The claim sits BEFORE the step-edit target radio
    // below - that radio would otherwise dismiss the page - so the open
    // page owns the F row, exactly like the GNR8 page does
    if (ev == EV_CLICKED && arpPageActive() &&
        kl >= groupControls[5] && kl <= groupControls[8]) {
      uint8_t idx = (uint8_t)(kl - groupControls[5]);      // 0..3
      if (arpOptionSel() == idx) arpOptionSet(0xFF);       // tap again = plain row
      else                       arpOptionSet(idx);
      headerShow(arpOptionName(arpOptionSel()));
      refreshAllLeds();
      refreshMainScreen();
      if (logRoom(48)) {
        Serial.print("  arp sub-page ");
        Serial.println(arpOptionSel() == 0xFF ? 0 : idx + 1);
      }
    } else
    // GNR8 page (open via M2): F1..F4 are RADIO switches for the option
    // sub-pages (values live in the SEQ EEPROM block / the step table) -
    // F1 arms the FULL page (ENC1 TONES, ENC2 RANGE, ENC3 LOW, ENC4
    // ALGO), F2 arms STEP OCT (+ MAP on ENC3), F3 BORROW, F4 MOD PROB
    // (tap a step first, the armed page dials it).  Click the armed
    // one again releases, click another switches.  The F-pad LEDs follow
    // via refreshAllLeds, the row via refreshMain
    if (ev == EV_CLICKED && genPageActive() &&
        kl >= groupControls[5] && kl <= groupControls[8]) {
      uint8_t idx = (uint8_t)(kl - groupControls[5]);      // 0..3
      if (genOptSel == idx) {
        genOptSel = 0xFF;
        headerShow("GNR8");
        if (logRoom(48)) Serial.println("  gen option off");
      } else {
        genOptSel = idx;
        if (idx == 0) {                // F1 = the four-knob options page
          headerShow("GNR8 opts");
          if (logRoom(48)) Serial.println("  gen opts page");
        } else {
          genOptionHeader(idx);
          if (logRoom(48)) {
            Serial.print("  gen option arm ");
            Serial.println(idx);
          }
        }
      }
      refreshAllLeds();
      refreshMainScreen();
    } else
    // DRUM page (open via M2): F1..F4 pick the knob SET - GROOVE (genre /
    // VAR / FILL / MOT on ENC1..4), LINES (the four lane selectors on
    // ENC1..4), FILL% and MOT% (the per-lane values on ENC1..4).  Clicking
    // the armed set again returns to the base eight-knob row
    if (ev == EV_CLICKED && drumPageActive() &&
        kl >= groupControls[5] && kl <= groupControls[8]) {
      uint8_t idx = (uint8_t)(kl - groupControls[5]);      // 0..3
      uint8_t want = (drumSetSelGet() == idx) ? 0xFF : idx;
      drumSetSelSet(want);
      static const char *const sn[4] = { "GROOVE", "LINES", "FILL%", "MOT%" };
      headerShow(want == 0xFF ? "DRUMS" : sn[idx]);
      if (logRoom(48)) {
        Serial.print("  drum set ");
        Serial.println(want == 0xFF ? "all" : sn[idx]);
      }
      refreshAllLeds();
      refreshMainScreen();
    } else
    // F1-F4 in SEQMODE (MONO): pick the edit target and switch the radio
    // on (TIE/GLIDE/ACCENT/PROB).  DRUM/POLY tracks only take ACCENT/PROB
    // from F3/F4 - clicking the selected one again closes the radio there;
    // POLY adds F1 = FX and F2 = TYPE (STEP/STRUM/HAND override per step).
    // SEQMODE only: ENCMODE/KEYMODE keep their F-pad jobs.  The strum
    // ADDS row above takes the F row first while it is open, then the
    // GNR8 page's option radio row (OPTS/OCT/BRW/PROB)
    if (ev == EV_CLICKED && !strumFRowActive() && !genPageActive() &&
        (stepEditCtx() || stepEditAccCtx()) &&
        kl >= groupControls[5] && kl <= groupControls[8]) {
      uint8_t idx = (uint8_t)(kl - groupControls[5]);      // 0..3
      static const char *const tn[4] = { "TIE", "GLIDE", "ACCENT", "PROB" };
      bool droppedArp = false;
      if (stepEditCtx()) {
        // arming a target wins over the M2 ARP page (the radio is one
        // group - same exclusivity the GNR8 page has with its F row)
        if (arpPageActive()) {
          arpPageDismiss();
          droppedArp = true;
          if (logRoom(48)) Serial.println("  arp page off (edit target)");
        }
        if (leadPageActive()) {     // ...same exclusivity for the LEAD page
          leadPageDismiss();
          droppedArp = true;
          if (logRoom(48)) Serial.println("  lead page off (edit target)");
        }
        if (harmPageActive()) {     // ...and the HARM page
          harmPageDismiss();
          droppedArp = true;
          if (logRoom(48)) Serial.println("  harm page off (edit target)");
        }
        if (compPageActive()) {     // ...and the COMP page
          compPageDismiss();
          droppedArp = true;
          if (logRoom(48)) Serial.println("  comp page off (edit target)");
        }
        if (skankPageActive()) {    // ...and the SKANK page
          skankPageDismiss();
          droppedArp = true;
          if (logRoom(48)) Serial.println("  skank page off (edit target)");
        }
        if (drumPageActive()) {     // ...and the DRUM pattern page
          drumPageDismiss();
          droppedArp = true;
          if (logRoom(48)) Serial.println("  drum page off (edit target)");
        }
        stepEditSet(true, (uint8_t)(idx + 1));
        headerShow(tn[idx]);
        if (logRoom(48)) {
          Serial.print("  edit target ");
          Serial.println(tn[idx]);
        }
      } else if (idx >= 2) {               // DRUM/POLY: ACCENT (F3) / PROB (F4)
        if (stepEditOn && stepEditTarget == idx + 1) {
          stepEditSet(false, 0);
          headerShow("EDIT OFF");
          if (logRoom(48)) Serial.println("  step edit off");
        } else {
          stepEditSet(true, (uint8_t)(idx + 1));
          headerShow(tn[idx]);
          if (logRoom(48)) {
            Serial.print("  edit target ");
            Serial.println(tn[idx]);
          }
        }
      } else if (idx == 0 && seqTrack() == SEQ_TRACK_POLY) {
        // POLY: F1 cycles FX (5) -> EMB (7) -> off
        if (stepEditOn && stepEditTarget == 5) {
          stepEditSet(true, 7);
          char hb[24];
          snprintf(hb, sizeof(hb), "EMB SUB=%u INV=%u", k4EmbSubProb(), k4EmbInvBias());
          headerShow(hb);
          if (logRoom(48)) { Serial.print("  edit target emb "); Serial.println(hb); }
        } else if (stepEditOn && stepEditTarget == 7) {
          stepEditSet(false, 0);
          headerShow("EDIT OFF");
          if (logRoom(48)) Serial.println("  step edit off");
        } else {
          stepEditSet(true, 5);
          char hb[16];
          snprintf(hb, sizeof(hb), "FX %s", stepFxName(stepFxType));
          headerShow(hb);
          if (logRoom(48)) {
            Serial.print("  edit target fx ");
            Serial.println(stepFxName(stepFxType));
          }
        }
      } else if (idx == 1 && seqTrack() == SEQ_TRACK_POLY) {
        // POLY: F2 = the TYPE target (STEP/STRUM/HAND per step); ENC7 picks
        // the armed value, a tap on a step stamps it (tap again = inherit)
        if (stepEditOn && stepEditTarget == 6) {
          stepEditSet(false, 0);
          headerShow("EDIT OFF");
          if (logRoom(48)) Serial.println("  step edit off");
        } else {
          stepEditSet(true, 6);
          char hb[16];
          snprintf(hb, sizeof(hb), "TYPE %s", stepTypeArmName());
          headerShow(hb);
          if (logRoom(48)) {
            Serial.print("  edit target type ");
            Serial.println(stepTypeArmName());
          }
        }
      }
      refreshAllLeds();
      if (droppedArp) refreshMainScreen();  // the encoder row changes
    }

    // M3 in ENCMODE: toggles the page group of the ENC5..8 banks (off =
    // F1..F4 = CC1..16, on = CC21..36; ENC1..4 always send CC17..20).
    // The pad LED follows (dim green = lower group, purple = upper), the
    // encoder labels/values repaint with the group
    if (ev == EV_CLICKED && modeBase == MODE_ENC &&
        kl == groupControls[4]) {
      bool on = !encM3Page();
      encM3PageSet(on);
      headerShow(on ? "CC21-36" : "CC1-16");
      if (logRoom(48)) {
        Serial.print("  enc page group ");
        Serial.println(on ? "2 (CC21-36)" : "1 (CC1-16)");
      }
      refreshAllLeds();
      refreshMainScreen();
    }

    // F1-F4 in ENCMODE: radio switches for the ENC5..8 CC bank (F1 =
    // CC1..4 ... F4 = CC13..16 in the lower M3 page group, F1 = CC21..24
    // ... F4 = CC33..36 in the upper one; ENC1..4 always send CC17..20).
    // RAM only and always one bank armed - re-clicking the selected one
    // keeps it.  The pad LEDs follow via refreshAllLeds, the encoder
    // labels/values via refreshMainScreen
    if (ev == EV_CLICKED && modeBase == MODE_ENC &&
        kl >= groupControls[5] && kl <= groupControls[8]) {
      uint8_t idx = (uint8_t)(kl - groupControls[5]);      // 0..3
      encBankSet(idx);
      uint8_t c1 = encCcNum(4);          // first CC of the armed block
      char hb[20];
      snprintf(hb, sizeof(hb), "F%u CC%u-%u", idx + 1, c1, c1 + 3);
      headerShow(hb);
      if (logRoom(48)) {
        Serial.print("  enc bank ");
        Serial.println(hb);
      }
      refreshAllLeds();
      refreshMainScreen();
    }

    // MUTE overlay (ENC mode): hold M2 -> the steps show the channel mute
    // state (dim orange-red = playing, bright orange-red = muted), tapping
    // one toggles that channel's playback; release closes the overlay.
    // SEQMODE3: a CLICK on M2 instead erases the selected track
    if (ledForPad[pad] == groupControls[3]) {
      if (ev == EV_PRESSED && modeBase == MODE_ENC) {
        encMuteHold(true);
        headerShow("MUTE");
        if (logRoom(48)) Serial.println("  mute overlay");
        refreshAllLeds();
      } else if (ev == EV_RELEASED && encMuteActive()) {
        encMuteHold(false);
        refreshAllLeds();
      } else if (ev == EV_CLICKED && modeBase == MODE_SEQ && modeVariant == 3) {
        seqErase((uint8_t)(seqEditChannel() - 1));
        char eb[14];
        snprintf(eb, sizeof(eb), "ch%u erased", seqEditChannel());
        headerShow(eb);
        if (logRoom(48)) {
          Serial.print("  ");
          Serial.println(eb);
        }
        refreshAllLeds();   // step LEDs dim: that pattern is gone
      } else if (ev == EV_CLICKED && strumFRowCtx()) {
        // SEQMODE 1/2/4 + POLY STRUM/HAND: M2 clicks the strum ADDS row
        // (F1..F4 = sus4/add6/add9/clear) on/off - POLY had no M2 job yet,
        // so this never collides with the ARP page (MONO) or erase (mode 3)
        strumFRowToggle();
        headerShow(strumFRowActive() ? "ADDS" : "ADDS OFF");
        if (logRoom(48)) {
          Serial.print("  strum adds row ");
          Serial.println(strumFRowActive() ? "on" : "off");
        }
        refreshAllLeds();
        refreshMainScreen();
      } else if (ev == EV_CLICKED && arpTrackCtx()) {
        // SEQMODE 1/2/4 + MONO/ARP: M2 clicks the ARP encoder page on/off.
        // Opening it drops the M1 edit radio - the two keys are mutually
        // exclusive (M1's toggle closes this page instead)
        arpPageToggle();
        if (arpPageActive()) {
          if (stepEditIsOn()) {
            stepEditSet(false, 0);
            if (logRoom(48)) Serial.println("  edit off (arp open)");
          }
        } else {
          stepChainDisarm();           // the F row leaves too
        }
        headerShow(arpPageActive() ? "ARP" : "ARP OFF");
        if (logRoom(48)) {
          Serial.print("  arp page ");
          Serial.println(arpPageActive() ? "on" : "off");
        }
        refreshAllLeds();
        refreshMainScreen();
      } else if (ev == EV_CLICKED && genTrackCtx()) {
        // SEQMODE 1/2/4 + MONO/BASS: M2 clicks the GNR8 encoder page on/off.
        // Opening it drops the M1 edit radio - the two keys are mutually
        // exclusive (M1's toggle closes this page instead)
        genPageToggle();
        if (genPageActive()) {
          if (stepEditIsOn()) {
            stepEditSet(false, 0);
            if (logRoom(48)) Serial.println("  edit off (gnr8 open)");
          }
        } else {
          stepChainDisarm();           // the F row leaves too
        }
        headerShow(genPageActive() ? "GNR8" : "GNR8 OFF");
        if (logRoom(48)) {
          Serial.print("  gen page ");
          Serial.println(genPageActive() ? "on" : "off");
        }
        refreshAllLeds();
        refreshMainScreen();
      } else if (ev == EV_CLICKED && leadTrackCtx()) {
        // SEQMODE 1/2/4 + MONO/LEAD: M2 clicks the LEAD encoder page on/off.
        // Opening it drops the M1 edit radio - the two keys are mutually
        // exclusive (M1's toggle closes this page instead)
        leadPageToggle();
        if (leadPageActive()) {
          if (stepEditIsOn()) {
            stepEditSet(false, 0);
            if (logRoom(48)) Serial.println("  edit off (lead open)");
          }
        } else {
          stepChainDisarm();           // the F row leaves too
        }
        headerShow(leadPageActive() ? "LEAD" : "LEAD OFF");
        if (logRoom(48)) {
          Serial.print("  lead page ");
          Serial.println(leadPageActive() ? "on" : "off");
        }
        refreshAllLeds();
        refreshMainScreen();
      } else if (ev == EV_CLICKED && harmTrackCtx()) {
        // SEQMODE 1/2/4 + POLY/HARM: M2 clicks the HARM encoder page on/off.
        // Opening it drops the M1 edit radio - the two keys are mutually
        // exclusive (M1's toggle closes this page instead)
        harmPageToggle();
        if (harmPageActive()) {
          if (stepEditIsOn()) {
            stepEditSet(false, 0);
            if (logRoom(48)) Serial.println("  edit off (harm open)");
          }
        } else {
          stepChainDisarm();           // the F row leaves too
        }
        headerShow(harmPageActive() ? "HARM" : "HARM OFF");
        if (logRoom(48)) {
          Serial.print("  harm page ");
          Serial.println(harmPageActive() ? "on" : "off");
        }
        refreshAllLeds();
        refreshMainScreen();
      } else if (ev == EV_CLICKED && compTrackCtx()) {
        // SEQMODE 1/2/4 + POLY/STEP: M2 clicks the COMP encoder page on/off
        compPageToggle();
        if (compPageActive()) {
          if (stepEditIsOn()) {
            stepEditSet(false, 0);
            if (logRoom(48)) Serial.println("  edit off (comp open)");
          }
        } else {
          stepChainDisarm();           // the F row leaves too
        }
        headerShow(compPageActive() ? "COMP" : "COMP OFF");
        if (logRoom(48)) {
          Serial.print("  comp page ");
          Serial.println(compPageActive() ? "on" : "off");
        }
        refreshAllLeds();
        refreshMainScreen();
      } else if (ev == EV_CLICKED && skankTrackCtx()) {
        // SEQMODE 1/2/4 + POLY/SKANK: M2 clicks the SKANK encoder page on/
        // off.  Opening it drops the M1 edit radio - the two keys are
        // mutually exclusive (M1's toggle closes this page instead)
        skankPageToggle();
        if (skankPageActive()) {
          if (stepEditIsOn()) {
            stepEditSet(false, 0);
            if (logRoom(48)) Serial.println("  edit off (skank open)");
          }
        } else {
          stepChainDisarm();           // the F row leaves too
        }
        headerShow(skankPageActive() ? "SKANK" : "SKANK OFF");
        if (logRoom(48)) {
          Serial.print("  skank page ");
          Serial.println(skankPageActive() ? "on" : "off");
        }
        refreshAllLeds();
        refreshMainScreen();
      } else if (ev == EV_CLICKED && drumTrackCtx()) {
        // SEQMODE 1/2/4 + DRUM: M2 clicks the DRUM pattern page on/off
        // (indigo pad).  Opening it drops the M1 edit radio - the two keys
        // are mutually exclusive (M1's toggle closes this page instead)
        drumPageToggle();
        if (drumPageActive()) {
          if (stepEditIsOn()) {
            stepEditSet(false, 0);
            if (logRoom(48)) Serial.println("  edit off (drum open)");
          }
        } else {
          stepChainDisarm();           // the F row leaves too
        }
        headerShow(drumPageActive() ? "DRUMS" : "DRUMS OFF");
        if (logRoom(48)) {
          Serial.print("  drum page ");
          Serial.println(drumPageActive() ? "on" : "off");
        }
        refreshAllLeds();
        refreshMainScreen();
      }
    }

    // SEQMODE3 + REC: erase the COMPLETE pattern (all 16 tracks) - allowed
    // while the transport runs; every sounding note is cut at once
    if (ev == EV_PRESSED && ledForPad[pad] == groupTransport[1] &&
        modeBase == MODE_SEQ && modeVariant == 3) {
      seqEraseAll();
      headerShow("ALL ERASED");
      if (logRoom(48)) Serial.println("  erase all tracks");
      refreshAllLeds();
    }

    // ENCMODE4 + REC: save the knob values + channel to EEPROM (restored
    // on the next boot; DAW feedback still wins live) - save only, no push
    if (ev == EV_PRESSED && ledForPad[pad] == groupTransport[1] &&
        modeBase == MODE_ENC && modeVariant == 4) {
      encCcSave();
      headerShow("CC SAVED");
      if (logRoom(48)) Serial.println("  cc values saved to eeprom");
    }

    // SEQMODE4 + REC: save every SEQ setting (track type, mono type, octave,
    // input channel, velocity, accent, gate, mutes, probability, pattern
    // length, FOLLOW, arp options, edit channel).  Notes, step contents and
    // the tie/glide/accent/prob attributes are NOT stored, they stay RAM
    if (ev == EV_PRESSED && ledForPad[pad] == groupTransport[1] &&
        modeBase == MODE_SEQ && modeVariant == 4) {
      seqCfgSave();
      headerShow("SEQ SAVED");
      if (logRoom(48)) Serial.println("  seq config saved to eeprom");
    }

    // SEQMODE1/2 and KEYMODE1/2/4 + REC: live record - arm one pass over
    // the edit track's pattern from its next 1st bar (auto-off after one
    // pass; the keyboard channel must match the edit track or arming is
    // refused).  KEYMODE2 records the whole chord the engine builds
    if (ev == EV_PRESSED && ledForPad[pad] == groupTransport[1] &&
        ((modeBase == MODE_SEQ && (modeVariant == 1 || modeVariant == 2)) ||
         (modeBase == MODE_KEY &&
          (modeVariant == 1 || modeVariant == 2 || modeVariant == 4)))) {
      uint8_t r = seqRecToggle();
      if (r == 0) headerShow("kbd ch mismatch");
      else if (r == 1) headerShow("rec armed");
      else headerShow("rec off");
      refreshAllLeds();
      if (logRoom(48)) {
        Serial.print("  rec ");
        Serial.println(r == 0 ? "refused (kbd ch)" : (r == 1 ? "armed" : "off"));
      }
    }

    // KEYMODE2 chord buttons (momentary): M1/M2/M3/NEXT select the quality
    // Maj/Min/Dim/Sus, F1..F4 the extensions 6/m7/M7/9 - press sets,
    // release clears, a sounding chord re-triggers.  M1 keeps its DRUM
    // part-select / ENCMODE voice-mute priority, M2 its ENCMODE mute /
    // SEQMODE3 erase above the quality keys
    if ((ev == EV_PRESSED || ev == EV_RELEASED) && keyBehaviorVariant() == 2) {
      int8_t ci = -1;
      for (uint8_t i = 0; i < CONTROLS_NUM_G; i++)
        if (groupControls[i] == kl) ci = (int8_t)i;
      bool m1SeqRadio = (ci == 2 && modeBase == MODE_SEQ &&
                         modeVariant != 3);               // edit radio M1
      bool m1Enc = (ci == 2 && modeBase == MODE_ENC);
      bool m2Enc = (ci == 3 && modeBase == MODE_ENC);
      bool m2Seq3 = (ci == 3 && modeBase == MODE_SEQ && modeVariant == 3);
      bool fSeq = (ci >= 5 && modeBase == MODE_SEQ && modeVariant != 3);
      bool fEnc = (ci >= 5 && modeBase == MODE_ENC);
      bool m3Enc = (ci == 4 && modeBase == MODE_ENC);
      // F1-F4 belong to the SEQMODE edit radio (target / accent / prob)
      // plus the F3 accent-velocity hold - they never touch the chords
      // there, on any track type; in ENCMODE they are the CC-bank radio
      // and M3 the page-group toggle
      if (ci > 0 && !m1SeqRadio && !m1Enc && !m2Enc && !m2Seq3 &&
          !m3Enc && !fSeq && !fEnc) {                     // ci 0 = PREV
        bool on = (ev == EV_PRESSED);
        static const uint8_t extOf[4] = { CHORD_EXT_6, CHORD_EXT_m7,
                                          CHORD_EXT_M7, CHORD_EXT_9 };
        if (ci >= 5) {
          chordSetExt(extOf[ci - 5], on);
        } else {
          static const uint8_t qOf[5] = { CHORD_Q_NONE, CHORD_Q_SUS,
                                          CHORD_Q_MAJ, CHORD_Q_MIN, CHORD_Q_DIM };
          // order on the panel: NEXT=1 Sus, M1=2 Maj, M2=3 Min, M3=4 Dim
          chordSetQuality(on ? qOf[ci] : CHORD_Q_NONE);
        }
        refreshAllLeds();               // sounding pitch classes may change
        if (chordName()[0]) headerShow(chordName());
        if (logRoom(48)) {
          Serial.print(on ? "  chord set " : "  chord clear ");
          Serial.println(chordName());
        }
      }
    }

    // KEYMODE4 control row: NEXT tap = the EMB menu (sticky: the encoder
    // row becomes EMB/PLACE, tap again to close), NEXT hold = focus+1,
    // M1 = write latch (WRITE lands
    // the proposal on the focused step / AUDITION plays it only), M2 =
    // auto-advance after every write, M3 tap = apply the generator to every
    // unlocked step / M3 hold = undo, F1 = variation (reseed + generate),
    // F2 = lock the focused step (apply skips it), F3 = VOICE-LEAD
    // inversions, F4 held = audition only (no write, no advance)
    int8_t k4ci = -1;
    for (uint8_t i = 0; i < CONTROLS_NUM_G; i++)
      if (groupControls[i] == kl) k4ci = (int8_t)i;
    if (modeBase == MODE_KEY && modeVariant == 4) {
      if (k4ci == 1 && ev == EV_CLICKED) {
        bool on = !k4EmbMenu();
        k4EmbMenuSet(on);
        if (on) setOverlay(G_ENC, ENC_KEY4_EMB);
        else clearOverlay(G_ENC);
        headerShow(on ? "EMB MENU" : "MENU OFF");
        refreshMainScreen();   // the row swaps to EMB/PLACE labels
        if (logRoom(48))
          Serial.println(on ? "  emb menu on" : "  emb menu off");
      } else if (k4ci == 1 && ev == EV_HOLD) {
        k4FocusStep(1);            // announces focus + progression itself
        refreshAllLeds();
        if (logRoom(48)) Serial.println("  focus next");
      } else if (k4ci == 2 && ev == EV_CLICKED) {
        bool on = k4WriteToggle();
        headerShow(on ? "KEY WRITE" : "KEY AUDITION");
        if (logRoom(48))
          Serial.println(on ? "  key write" : "  key audition");
      } else if (k4ci == 3 && ev == EV_CLICKED) {
        bool on = k4AutoToggle();
        headerShow(on ? "AUTO ADV" : "AUTO OFF");
        if (logRoom(48)) Serial.println(on ? "  auto on" : "  auto off");
      } else if (k4ci == 4 && ev == EV_CLICKED) {
        int w = k4ApplyAll();
        if (w > 0) k4AnnouncePattern();   // the progression that was written
        else headerShow("APPLIED 0");
        refreshAllLeds();
        refreshMainScreen();   // focus/locks may have been re-read
        if (logRoom(48)) Serial.println(w > 0 ? "  applied" : "  applied 0");
      } else if (k4ci == 4 && ev == EV_HOLD) {
        int u = k4Undo();
        if (u == 0) k4AnnouncePattern();   // the restored progression
        else headerShow("nothing to undo");
        refreshAllLeds();
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  undo");
      } else if (k4ci == 5 && ev == EV_CLICKED) {
        int w = k4Variation();
        if (w > 0) k4AnnouncePattern();   // the reseeded progression
        else headerShow("VAR 0");
        refreshAllLeds();
        if (logRoom(48)) Serial.println(w > 0 ? "  variation" : "  var 0");
      } else if (k4ci == 6 && ev == EV_CLICKED) {
        bool on = k4LockToggle(k4Focus());
        char lb[14];
        snprintf(lb, sizeof(lb), "%s %u", on ? "lock" : "unlock",
                 (unsigned)(k4Focus() + 1));
        headerShow(lb);
        refreshAllLeds();       // amber focus/lock pads follow
        if (logRoom(48)) { Serial.print("  "); Serial.println(lb); }
      } else if (k4ci == 7 && ev == EV_CLICKED) {
        bool on = k4LeadToggle();
        headerShow(on ? "LEAD ON" : "LEAD OFF");
        if (logRoom(48)) Serial.println(on ? "  lead on" : "  lead off");
      } else if (k4ci == 8 && ev == EV_PRESSED) {
        k4HoldSet(true);
        if (logRoom(48)) Serial.println("  hold audition");
      }
    }
    // F4 release, ungated by mode: the momentary hold flag clears even if
    // the mode changed while the key was down (never a stuck audition)
    if (k4ci == 8 && ev == EV_RELEASED && k4Hold()) {
      k4HoldSet(false);
      if (logRoom(48)) Serial.println("  hold off");
    }

    // GNR8 (SEQMODE 1/2/4 + MONO/BASS): M3 tap = generate the pattern
    // from the page's parameters (fill/acc/glide/tie + the F-row options,
    // notes inside the KEY + SCALE), M3 hold = undo it again - the shared
    // one-level step snapshot, mirroring KEYMODE4's apply/undo pair
    if (modeBase == MODE_SEQ && modeVariant != 3 && genTrackCtx() &&
        k4ci == 4) {
      if (ev == EV_CLICKED) {
        int w = seqGenProduce();
        char gb[24];
        snprintf(gb, sizeof(gb), "gen %d steps", w > 0 ? w : 0);
        headerShow(gb);
        refreshAllLeds();       // the step LEDs follow the new pattern
        refreshMainScreen();
        if (logRoom(48)) {
          Serial.print("  ");
          Serial.println(gb);
        }
      } else if (ev == EV_HOLD) {
        int u = stepSnapshotRestore();
        headerShow(u == 0 ? "undo" : "nothing to undo");
        refreshAllLeds();
        refreshMainScreen();
        if (logRoom(48))
          Serial.println(u == 0 ? "  undo" : "  nothing to undo");
      }
    }

    // LEAD (SEQMODE 1/2/4 + MONO/LEAD): M3 tap = generate the melody from
    // the page's contour + LOW/RANGE/density (notes inside the KEY + SCALE,
    // stored as a skeleton LEAD + FOLLOW + MAP re-resolves onto the feeder
    // chord), M3 hold = undo - the shared one-level step snapshot, exactly
    // like the GNR8 generate/undo pair
    if (modeBase == MODE_SEQ && modeVariant != 3 && leadTrackCtx() &&
        k4ci == 4) {
      if (ev == EV_CLICKED) {
        int w = seqLeadProduce();
        char lb[24];
        snprintf(lb, sizeof(lb), "lead %d steps", w > 0 ? w : 0);
        headerShow(lb);
        refreshAllLeds();       // the step LEDs follow the new melody
        refreshMainScreen();
        if (logRoom(48)) {
          Serial.print("  ");
          Serial.println(lb);
        }
      } else if (ev == EV_HOLD) {
        int u = stepSnapshotRestore();
        headerShow(u == 0 ? "undo" : "nothing to undo");
        refreshAllLeds();
        refreshMainScreen();
        if (logRoom(48))
          Serial.println(u == 0 ? "  undo" : "  nothing to undo");
      }
    }

    // HARM (SEQMODE 1/2/4 + POLY/HARM): M3 tap = bake the chord progression
    // inferred from the FOLLOW feeder's bassline/arp into this track's steps
    // (k4Harm: Viterbi over source-fit + the KEYMODE4 genre grammar), M3
    // hold = undo - the shared one-level step snapshot, like GNR8/LEAD
    if (modeBase == MODE_SEQ && modeVariant != 3 && harmTrackCtx() &&
        k4ci == 4) {
      if (ev == EV_CLICKED) {
        int w = k4Harm();
        char hb[24];
        snprintf(hb, sizeof(hb), "harm %d steps", w > 0 ? w : 0);
        headerShow(hb);
        refreshAllLeds();       // the step LEDs follow the new chords
        refreshMainScreen();
        if (logRoom(48)) {
          Serial.print("  ");
          Serial.println(hb);
        }
      } else if (ev == EV_HOLD) {
        int u = stepSnapshotRestore();
        headerShow(u == 0 ? "undo" : "nothing to undo");
        refreshAllLeds();
        refreshMainScreen();
        if (logRoom(48))
          Serial.println(u == 0 ? "  undo" : "  nothing to undo");
      }
    }

    // DRUM (SEQMODE 1/2/4 + DRUM track): M3 tap = re-roll the pattern from
    // the page's genre / variation / lanes / FILL / MOT settings, M3 hold =
    // undo - the shared one-level step snapshot, like GNR8/LEAD/HARM.  The
    // knob turns already rebuild live; this is the "new roll" gesture (the
    // FILL ghosts, the MOT accents and the bar-end fill are re-rolled)
    if (modeBase == MODE_SEQ && modeVariant != 3 && drumTrackCtx() &&
        k4ci == 4) {
      if (ev == EV_CLICKED) {
        int w = drumProduce();
        char rb[24];
        snprintf(rb, sizeof(rb), "drums %d steps", w > 0 ? w : 0);
        headerShow(rb);
        refreshAllLeds();       // the step LEDs follow the new pattern
        refreshMainScreen();
        if (logRoom(48)) { Serial.print("  "); Serial.println(rb); }
      } else if (ev == EV_HOLD) {
        int u = stepSnapshotRestore();
        headerShow(u == 0 ? "undo" : "nothing to undo");
        refreshAllLeds();
        refreshMainScreen();
        if (logRoom(48))
          Serial.println(u == 0 ? "  undo" : "  nothing to undo");
      }
    }

    // PREV = momentary encoder overlay: KEYMODE2 remaps the row to
    // Oct/Scale/Root/Latch/KLock/Perf, KEYMODE4 to Oct/Scale/Key; release
    // restores it.  A quick KEYMODE4 tap (no hold) steps the focus back
    if (kl == groupControls[0]) {
      if (ev == EV_PRESSED && modeBase == MODE_SEQ) {
        // SEQMODE: PREV held = the SHIFT/TRANS row (ENC1 = pattern rotate,
        // ENC2 = semitone transpose of the sequence's playback).  The GNR8
        // page's old rotate row lives on this same overlay (ENC1 still =
        // rotate).  Checked FIRST - the KEYMODE2 quality row claims PREV
        // via keyBehaviorVariant() no matter which mode is up, so it must
        // not steal the hold in SEQMODE
        setOverlay(G_ENC, ENC_SEQ_ROT_OV);
        prevOvActive = true;
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  overlay SEQ SHIFT/TRANS");
      } else if (ev == EV_PRESSED && keyBehaviorVariant() == 2) {
        setOverlay(G_ENC, ENC_KEY2_PREV);
        prevOvActive = true;
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  overlay KEY2 PREV");
      } else if (ev == EV_PRESSED && modeBase == MODE_KEY &&
                 modeVariant == 4) {
        setOverlay(G_ENC, ENC_KEY4_PREV);
        prevOvActive = true;
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  overlay KEY4 PREV");
      } else if (ev == EV_RELEASED && prevOvActive) {
        // PREV lets go: back to the EMB menu row when it is still open
        // (the menu is sticky, the PREV row above was only borrowed -
        // and only a live KEYMODE4 owns that menu, never a stale flag)
        if (modeBase == MODE_KEY && modeVariant == 4 && k4EmbMenu())
          setOverlay(G_ENC, ENC_KEY4_EMB);
        else clearOverlay(G_ENC);
        prevOvActive = false;
        refreshMainScreen();
        if (logRoom(48)) Serial.println("  overlay off");
      } else if (ev == EV_CLICKED && modeBase == MODE_KEY &&
                 modeVariant == 4) {
        k4FocusStep(-1);           // announces focus + progression itself
        refreshAllLeds();
        if (logRoom(48)) Serial.println("  focus prev");
      }
    }

    // PLAY in SEQMODE: Start (or Stop while the INT transport runs).
    // ENCMODE3: PLAY = toggle the clock source instead (frees ENC4 for
    // the pattern speed) - it never starts the transport there
    if (ev == EV_PRESSED && ledForPad[pad] == groupTransport[0]) {
      if (modeBase == MODE_ENC && modeVariant == 3) {
        seqSetSyncInternal(!seqIsSyncInternal());
        headerShow(seqIsSyncInternal() ? "clock INT" : "clock EXT");
        if (logRoom(48)) {
          Serial.print("  clock ");
          Serial.println(seqIsSyncInternal() ? "INT" : "EXT");
        }
      } else if (modeBase == MODE_SEQ ||
                 (modeBase == MODE_KEY && modeVariant == 4)) {
        seqTogglePlay();
        if (logRoom(48)) Serial.println("  -> MIDI Play toggle");
      }
    }

    // keyboard pads -> MIDI notes in every mode (behavior defined by KEYMODE,
    // inherited by SEQMODE/ENCMODE); release always clears the key state
    // (even if the mode changed while it was held: no stuck notes).
    // SEQMODE3 intercepts C D E F first: they pick the edit window (bar)
    // and press the selected bar again to loop it - swallowed presses are
    // remembered so the matching release never sends a stray note-off
    if (kidx != 255) {
      static uint8_t barHeld = 0;          // kidx bits eaten as pattern bars
      bool isBar = (ev == EV_PRESSED && modeBase == MODE_SEQ &&
                    modeVariant == 3 && kl >= 25 && kl <= 28);
      if (isBar) {
        uint8_t bar = (uint8_t)(kl - 25);  // 25,26,27,28 -> bar 0..3
        barHeld |= (uint8_t)(1 << kidx);
        uint8_t r = seqBarPress(bar);
        char bb[20];
        if (r == 0)
          snprintf(bb, sizeof(bb), "steps %u-%u",
                   bar * 16 + 1, bar * 16 + 16);
        else if (r == 1)
          snprintf(bb, sizeof(bb), "loop %u-%u", bar * 16 + 1, bar * 16 + 16);
        else
          snprintf(bb, sizeof(bb), "loop off");
        headerShow(bb);
        if (logRoom(48)) {
          Serial.print("  bar ");
          Serial.println(bb);
        }
      } else if (ev == EV_PRESSED) {
        int n = keyPadPress(kidx);
        if (n >= 0 && logRoom(48)) {
          Serial.print("  note on ");
          Serial.println(n);
        }
        // chord engine variants: the header shows the chord that started
        if (n >= 0 && keyChordMode() && chordName()[0])
          headerShow(chordName());
      } else if (ev == EV_RELEASED) {
        if (barHeld & (1 << kidx)) barHeld &= (uint8_t)~(1 << kidx);
        else keyPadRelease(kidx);
      }
      refreshAllLeds();   // first assign key can activate the held step's LED
    }

    if (ev == EV_PRESSED || ev == EV_CLICKED || ev == EV_HOLD || ev == EV_DOUBLE)
      logSets();
  }
}

// ENCMODE knob positions: ENC1..ENC4 always send CC17..CC20; ENC5..ENC8
// send the F1..F4 bank's block inside the active M3 page group (off =
// F1 = CC1..4 ... F4 = CC13..16; on = F1 = CC21..4 ... F4 = CC33..36)
// on the encoder channel (ENCMODE3).  Values are stored PER CC NUMBER
// (36 CCs x 16 channels): switching channels loads that channel's own
// value instead of inheriting the previous channel's (no jump); DAW
// feedback updates the slots via encCcIncoming()
static int16_t ccVal[36][16] = {{0}};

// F1..F4 bank radio for ENC5..8 - RAM only, remembered PER ENCODER
// CHANNEL (the page last used on each channel is recalled when the
// channel changes or ENCMODE is entered - encPageRecall)
static uint8_t encBankSel = 0;
static uint8_t encBankMem[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0};

// M3 page group: off = the banks live in CC1..16, on = CC21..36 (the
// upper group; CC17..20 stay reserved for ENC1..4).  RAM only, likewise
// remembered per encoder channel
static bool encM3Sel = false;
static bool encM3Mem[16] = {false, false, false, false, false, false,
                            false, false, false, false, false, false,
                            false, false, false, false};

uint8_t encBank() { return (encBankSel < 4) ? encBankSel : 0; }
void encBankSet(uint8_t b) {
  if (b >= 4) return;
  encBankSel = b;
  encBankMem[encChannel() - 1] = b;   // every change lands in this channel
}

bool encM3Page() { return encM3Sel; }
void encM3PageSet(bool on) {
  encM3Sel = on;
  encM3Mem[encChannel() - 1] = on;    // ...so the switch can recall it
}

// encoder channel changed (or ENCMODE re-entered): take the page the new
// channel last used (setters store, so this just replays them)
void encPageRecall() {
  encBankSet(encBankMem[encChannel() - 1]);
  encM3PageSet(encM3Mem[encChannel() - 1]);
}

// EEPROM accessors (the SEQ block stores the per-channel page memory)
uint8_t encBankMemOf(uint8_t ch) {
  if (ch < 1 || ch > 16) return 0;
  return (encBankMem[ch - 1] < 4) ? encBankMem[ch - 1] : 0;
}

void encBankMemSet(uint8_t ch, uint8_t v) {
  if (ch < 1 || ch > 16) return;
  encBankMem[ch - 1] = (v < 4) ? v : 0;
}

bool encM3MemOf(uint8_t ch) {
  return ch >= 1 && ch <= 16 && encM3Mem[ch - 1];
}

void encM3MemSet(uint8_t ch, bool on) {
  if (ch >= 1 && ch <= 16) encM3Mem[ch - 1] = on;
}

// CC number encoder ei (0..7) sends: ENC1..4 = CC17..20 (fixed), the
// rest = the current bank's block of four in the active page group
uint8_t encCcNum(uint8_t ei) {
  if (ei < 4) return (uint8_t)(17 + ei);
  uint8_t base = encM3Sel ? 20 : 0;
  return (uint8_t)(base + encBank() * 4 + (ei - 4) + 1);
}

uint8_t encCcValue(uint8_t idx) {
  if (idx >= 8) return 0;
  return (uint8_t)ccVal[encCcNum(idx) - 1][encChannel() - 1];
}

// incoming CC from the DAW: sync the knob positions of CC1..CC36 on any
// channel (store only - never re-sent, so no feedback loop; deduped so an
// echo of our own move doesn't repaint the screen)
void encCcIncoming(uint8_t ch, uint8_t cc, uint8_t val) {
  if (ch < 1 || ch > 16 || cc < 1 || cc > 36) return;
  if (ccVal[cc - 1][ch - 1] == (int16_t)(val & 0x7F)) return;
  ccVal[cc - 1][ch - 1] = val & 0x7F;
  if (modeBase == MODE_ENC) refreshMainScreen();   // knobs are on screen
}

// EEPROM persistence (ENCMODE4 + REC saves, boot loads).  Layout at
// address 0: magic | encCh | 576 values (cc*16+ch) | checksum = 579 of
// 4096 bytes; a blank or corrupt block (including a save from an older
// 8- or 20-CC layout) just keeps the defaults (zeros)
static const uint8_t CC_EE_MAGIC = 0xCE;
static const uint16_t CC_EE_LEN = 579;

void encCcSave() {
  uint8_t b[CC_EE_LEN];
  b[0] = CC_EE_MAGIC;
  b[1] = encChannel();
  for (uint8_t e = 0; e < 36; e++)
    for (uint8_t c = 0; c < 16; c++)
      b[2 + e * 16 + c] = (uint8_t)ccVal[e][c] & 0x7F;
  uint8_t sum = 0;
  for (uint16_t i = 0; i < CC_EE_LEN - 1; i++) sum += b[i];
  b[CC_EE_LEN - 1] = sum;
  EEPROM.put(0, b);   // put() writes only the bytes that changed
}

void encCcLoad() {
  uint8_t b[CC_EE_LEN];
  EEPROM.get(0, b);
  uint8_t sum = 0;
  for (uint16_t i = 0; i < CC_EE_LEN - 1; i++) sum += b[i];
  if (b[0] != CC_EE_MAGIC || sum != b[CC_EE_LEN - 1]) return;
  for (uint8_t e = 0; e < 36; e++)
    for (uint8_t c = 0; c < 16; c++)
      ccVal[e][c] = b[2 + e * 16 + c] & 0x7F;
  encSetChannel(b[1]);
}

void handleEncoderEvents() {
  uint8_t ei;
  int16_t ed;
  bool any = false;
  for (uint8_t n = 0; n < MAX_EVENTS_PER_LOOP && popEncEvent(ei, ed); n++) {
    // SEQMODE3: ANY encoder movement drops the step-pad channel selector
    // for the rest of the hold - the pads go back to the sequencer (so the
    // playhead, recorded steps and the pattern length read again) and only
    // holding SEQ into SEQMODE3 brings the selector up
    if (ed != 0 && modeBase == MODE_SEQ && modeVariant == 3 &&
        seq3ChSelDismiss())
      refreshAllLeds();

    // KEYMODE: ENC1 = octave -2..8 (clamped, no wrap; 4 counts = 1 octave).
    // KEYMODE4: the main row owns ENC1 for GENRE - the octave lives on the
    // PREV overlay row (same composite guard the scale row uses)
    if (modeBase == MODE_KEY && ei == 0 && ed != 0 &&
        (modeVariant != 4 || currentBinding().enc == ENC_KEY4_PREV)) {
      keyOctaveAdjust(ed);
      if (logRoom(48)) {
        Serial.print("  octave ");
        Serial.println(keyOctave());
      }
      any = true;
    }

    // KEYMODE: ENC2 = scale (every variant, and the PREV overlay row);
    // 4 counts = 1 scale, clamped 0..12, no wrap.  Selecting a scale
    // flashes its mood ("Happy", "Sad", ...) in the temporary header -
    // in KEYMODE4 the persistent progression behind it re-ranks live
    if (modeBase == MODE_KEY && ei == 1 && ed != 0 &&
        (modeVariant != 4 || currentBinding().enc == ENC_KEY4_PREV)) {
      keyScaleAdjust(ed);
      refreshAllLeds();             // in-scale / out-of-scale keys repaint
      if (modeVariant == 4) {
        k4Refresh();                // the framework follows the scale
        k4Announce();               // the sticky header shows the ranking
      }
      headerShow(keyScaleMood());   // the mood flashes over it (all variants)
      if (logRoom(48)) {
        Serial.print("  scale ");
        Serial.println(keyScaleName());
      }
      any = true;
    }

    // KEYMODE1/2: ENC3 = KEY (the key/root) - shared by both variants: it
    // roots the scale mask in KEYMODE1 and the chord engine in KEYMODE2.
    // KEYMODE4: same, but only while its PREV overlay row is up (the main
    // row uses ENC3 for TENS)
    if (modeBase == MODE_KEY &&
        (modeVariant == 1 || modeVariant == 2 ||
         currentBinding().enc == ENC_KEY4_PREV) &&
        ei == 2 && ed != 0) {
      keyRootAdjust(ed);
      refreshAllLeds();           // the scale mask is rooted here too
      if (modeVariant == 4) {     // the framework follows the root
        k4Refresh();
        k4Announce();
      }
      if (logRoom(48)) {
        Serial.print("  root ");
        Serial.println(keyRootName());
      }
      any = true;
    }

    // KEYMODE4 + step held: the overlay row edits THAT step - ENC1 OCT
    // (playback transpose, GNR8's -2..+2 stored separately), ENC2 DEG
    // (shift every tone one scale degree in the key), ENC3 INV (cycle the
    // bass up / top down an octave), ENC4 VOICE (spread/close the upper
    // voices around the bass).  Every turn echoes into the header
    if (modeBase == MODE_KEY && modeVariant == 4 && ed != 0 &&
        currentBinding().enc == ENC_KEY4_STEP && ei <= 3 &&
        k4StepSelGet() >= 0) {
      uint8_t stp = (uint8_t)k4StepSelGet();
      char hb[24];
      char dgs[8], ivs[8];
      if (ei == 0) {
        stepOctSet(stp, (int8_t)(stepOctAt(stp) + ((ed > 0) ? 1 : -1)));
        int8_t nv = stepOctAt(stp);
        if (nv == 0) snprintf(hb, sizeof(hb), "%u oct 0", stp + 1);
        else snprintf(hb, sizeof(hb), "%u oct %+d", stp + 1, nv);
        headerShow(hb);
        if (logRoom(48)) { Serial.print("  "); Serial.println(hb); }
      } else if (ei == 1) {
        int8_t dd = (ed > 12) ? 12 : (ed < -12) ? -12 : (int8_t)ed;
        stepDegAdjust(stp, dd);
        if (k4StepLabels(stp, dgs, sizeof(dgs), NULL, 0)) {
          snprintf(hb, sizeof(hb), "%u deg %s", stp + 1, dgs);
          headerShow(hb);
          if (logRoom(48)) { Serial.print("  "); Serial.println(hb); }
        }
      } else if (ei == 2) {
        stepInvAdjust(stp, (int8_t)((ed > 0) ? 1 : -1));
        if (k4StepLabels(stp, NULL, 0, ivs, sizeof(ivs))) {
          snprintf(hb, sizeof(hb), "%u inv %s", stp + 1, ivs);
          headerShow(hb);
          if (logRoom(48)) { Serial.print("  "); Serial.println(hb); }
        }
      } else {
        stepVoiAdjust(stp, (int8_t)((ed > 0) ? 1 : -1));
        uint8_t n = 0;
        const uint8_t *nt = stepChordAt(stp, &n);
        if (n) {
          uint8_t lo = nt[0], hi = nt[0];
          for (uint8_t i = 1; i < n; i++) {
            if (nt[i] < lo) lo = nt[i];
            if (nt[i] > hi) hi = nt[i];
          }
          snprintf(hb, sizeof(hb), "%u voice %uoct", stp + 1,
                   (uint8_t)((hi - lo) / 12));
          headerShow(hb);
          if (logRoom(48)) { Serial.print("  "); Serial.println(hb); }
        }
      }
      any = true;
    }

    // KEYMODE4: the main context row = ENC1 GENRE (1 count = 1 genre,
    // wraps), ENC2..ENC8 = MOOD/TENS/MOTION/CAD/CPX/ERA/SURP (1 count =
    // 1 unit, clamped 0..100).  Every change re-ranks the proposals AND
    // re-generates every unlocked step live, so the progression playing
    // in the sequencer follows the knobs in real time (locked steps are
    // never touched; the first count of a tweak burst claims the undo
    // slot, so M3-hold restores the pre-tweak progression)
    if (modeBase == MODE_KEY && modeVariant == 4 && ed != 0 &&
        currentBinding().enc == ENC_KEY4) {
      if (ei == 0) {
        k4GenreAdjust(ed);
        if (logRoom(48)) {
          Serial.print("  genre ");
          Serial.println(k4GenreName());
        }
      } else {
        k4KnobAdjust((uint8_t)(ei - 1), ed);
        if (logRoom(48)) {
          static const char* kn[7] = { "mood ", "tens ", "motion ", "cad ",
                                       "cpx ", "era ", "surp " };
          Serial.print(kn[ei - 1]);
          Serial.println(k4Knob((uint8_t)(ei - 1)));
        }
      }
      k4Regenerate();       // the written progression follows the knobs
      refreshAllLeds();   // the keyboard repaints the re-ranked proposals
      k4Announce();       // ...and the header names them (I V vi IV)
      any = true;
    }

    // KEYMODE4: the PREV overlay row = ENC1 Octave / ENC2 Scale / ENC3 Key
    // / ENC4 = Force-to-scale (same global ON/OFF as KEYMODE2's ENC4: with
    // it on, sequencer playback snaps stored notes to the current scale,
    // so turning the scale recolors recorded progressions live)
    // EN5-EN8 = embellishment encoders: SUB prob / INV bias / PAS amt / COL amt
    if (modeBase == MODE_KEY && modeVariant == 4 && ei >= 3 && ei <= 7 && ed != 0 &&
        currentBinding().enc == ENC_KEY4_PREV) {
      if (ei == 3) {
        chordFScaleAdjust(ed);
        if (logRoom(48)) {
          Serial.print("  fscale ");
          Serial.println(chordFScale() ? "ON" : "OFF");
        }
      } else if (ei == 4) {       // EN5 = SUB prob
        k4EmbSubProbAdjust(ed);
        if (logRoom(48)) {
          Serial.print("  sub% ");
          Serial.println(k4EmbSubProb());
        }
      } else if (ei == 5) {       // EN6 = INV bias
        k4EmbInvBiasAdjust(ed);
        if (logRoom(48)) {
          Serial.print("  inv ");
          Serial.println(k4EmbInvBias());
        }
      } else if (ei == 6) {       // EN7 = PAS amt
        k4EmbPassAmtAdjust(ed);
        if (logRoom(48)) {
          Serial.print("  pas% ");
          Serial.println(k4EmbPassAmt());
        }
      } else if (ei == 7) {       // EN8 = COL amt
        k4EmbColorAmtAdjust(ed);
        if (logRoom(48)) {
          Serial.print("  col% ");
          Serial.println(k4EmbColorAmt());
        }
      }
      any = true;
    }

    // KEYMODE4: the sticky EMB menu row = ENC1 = embellish mode (off ->
    // pass -> pass+sub, wraps), ENC2 = passing placement (odd / distance,
    // two-state: CW one way, CCW the other).  The other six are "--"
    if (modeBase == MODE_KEY && modeVariant == 4 && k4EmbMenu() && ed != 0 &&
        currentBinding().enc == ENC_KEY4_EMB) {
      if (ei == 0) {
        k4EmbModeAdjust(ed);
        char eb[8];
        snprintf(eb, sizeof(eb), "EMB %s", k4EmbModeName());
        headerShow(eb);
        if (logRoom(48)) { Serial.print("  "); Serial.println(eb); }
      } else if (ei == 1) {
        k4EmbPlaceAdjust(ed);
        char pb[8];
        snprintf(pb, sizeof(pb), "PLC %s", k4EmbPlaceName());
        headerShow(pb);
        if (logRoom(48)) { Serial.print("  "); Serial.println(pb); }
      }
      any = true;
    }

    // KEYMODE2: ENC4 = Force-to-scale (FScale), ENC5..ENC7 = bass/spread/
    // density, ENC8 = latch.  With PREV held the row remaps: ENC4 = latch,
    // ENC5 = KeyLock, ENC6 = Perf (state only)
    if (modeBase == MODE_KEY && modeVariant == 2 && ei != 2 && ed != 0) {
      ModeBinding b = currentBinding();
      if (b.enc == ENC_KEY2_PREV) {
        if (ei == 3) chordLatchAdjust(ed);
        else if (ei == 4) chordKeyLockAdjust(ed);
        else if (ei == 5) chordPerfAdjust(ed);
        if (ei >= 3 && ei <= 5) {
          refreshAllLeds();         // latch off silences the chord's tint
          if (logRoom(48)) {
            Serial.print(ei == 3 ? "  latch " : ei == 4 ? "  klock " : "  perf ");
            Serial.println(ei == 3 ? (chordLatch() ? "ON" : "OFF") :
                           ei == 4 ? (chordKeyLock() ? "ON" : "OFF") :
                                     (chordPerf() ? "ON" : "OFF"));
          }
          any = true;
        }
      } else if (ei == 7) {
        chordLatchAdjust(ed);       // main row: ENC8 = latch
        refreshAllLeds();
        if (logRoom(48)) {
          Serial.print("  latch ");
          Serial.println(chordLatch() ? "ON" : "OFF");
        }
        any = true;
      } else if (ei == 3) {
        chordFScaleAdjust(ed);      // main row: ENC4 = Force-to-scale
        refreshAllLeds();
        if (logRoom(48)) {
          Serial.print("  fscale ");
          Serial.println(chordFScale() ? "ON" : "OFF");
        }
        any = true;
      } else if (ei >= 4 && ei <= 6) {
        chordParamAdjust(ei - 3, ed);   // BASS / SPRD / DENS
        if (logRoom(48)) {
          static const char* pn[4] = { "voic ", "bass ", "sprd ", "dens " };
          Serial.print(pn[ei - 3]);
          Serial.println(chordParam(ei - 3));
        }
        any = true;
      }
    }

    // KEYMODE3: ENC3 = ENCOD, ENC4 = KBOARD - channel-link switches,
    // states free/locked (two-state, NO wrap: CW saturates at locked,
    // CCW at free - a turn never flips past the end).  LOCKED = this
    // family's channel moves with the SEQ edit channel (the hub) and with
    // the other locked family (see channelLinkPropagate); FREE = each mode
    // keeps its own independent channel, as always
    if (modeBase == MODE_KEY && modeVariant == 3 && ed != 0 &&
        (ei == 2 || ei == 3)) {
      bool want = (ed > 0);          // CW = locked, CCW = free
      if (ei == 2) {
        if (want != chanEncLocked()) {
          chanEncLinkSet(want);
          headerShow(want ? "ENCOD locked" : "ENCOD free");
          if (logRoom(48)) {
            Serial.print("  encod link ");
            Serial.println(want ? "locked" : "free");
          }
          any = true;
        }
      } else {
        if (want != chanKeyLocked()) {
          chanKeyLinkSet(want);
          headerShow(want ? "KBOARD locked" : "KBOARD free");
          if (logRoom(48)) {
            Serial.print("  kboard link ");
            Serial.println(want ? "locked" : "free");
          }
          any = true;
        }
      }
    }

    // ENCMODE: ENC1..ENC4 send CC17..CC20, ENC5..ENC8 send the F bank's
    // block (F1 = CC1..4 ... F4 = CC13..16) on the encoder channel
    // (ENCMODE3); 1 count = 1 value step, clamped 0..127, no wrap;
    // positions persist across mode changes like a real knob
    if (modeBase == MODE_ENC && ei < 8 && ed != 0) {
      uint8_t cch = encChannel();
      uint8_t cc = encCcNum(ei);
      int16_t v = ccVal[cc - 1][cch - 1] + ed;
      if (v < 0) v = 0;
      if (v > 127) v = 127;
      if (v != ccVal[cc - 1][cch - 1]) {
        ccVal[cc - 1][cch - 1] = v;
        usbMIDI.sendControlChange(cc, (uint8_t)v, cch);
        if (logRoom(48)) {
          Serial.print("  CC ");
          Serial.print(cc);
          Serial.print("=");
          Serial.println(v);
        }
      }
      any = true;
    }

    // SEQMODE: ENC1 = octave shift of the selected sequence (all variants)
    // SEQMODE3: ENC4 = pattern SPEED of the edit sequence (9 rungs, 6 =
    //           normal; sync moved to ENCMODE3+PLAY)
    //           ENC5 = input channel of the edit sequence, 1..16
    //           ENC6 = TRACK DRUM/MONO/POLY (three-state, no wrap)
    //           ENC7 = TYPE ROOT/BASS/ARP (MONO sequences only, no wrap)
    //           ENC8 = tempo 40..240 (moved from ENC1)
    if (modeBase == MODE_SEQ) {
      if (ed != 0 && currentBinding().enc == ENC_SEQ_ROT_OV) {
        // SHIFT/TRANS overlay (PREV held): ENC1 = rotate the pattern,
        // 4 counts = 1 step (GNR8 page's old ROT row, same knob); ENC2 =
        // semitone TRANS of the sequence's playback, 1 count = 1 st,
        // -12..+12 (DRUM tracks refuse the turn).  The other six idle
        if (ei == 0) {
          seqGenShiftAdjust(ed);
          char rb[12];
          snprintf(rb, sizeof(rb), "rot %u", seqGenShiftPos() + 1);
          headerShow(rb);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(rb);
          }
        } else if (ei == 1) {
          seqTransAdjust(ed);
          char tb[16];
          int8_t tv = (int8_t)seqTrans();
          if (tv >= 0) snprintf(tb, sizeof(tb), "trans +%d", (int)tv);
          else         snprintf(tb, sizeof(tb), "trans %d", (int)tv);
          headerShow(tb);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(tb);
          }
        } else if (ei == 2) {
          // ENC3 = per-track scale-lock (Klock): snap the TRANS result back
          // into the current scale so the semitone knob stays diatonic.
          // Two-state, NO wrap (like ENCOD/KBOARD): CW = ON, CCW = OFF - a
          // continuous turn saturates at the end instead of toggling on
          // every count; only a change repaints/echoes
          if (seqKlockSetDir(ed)) {
            headerShow(seqKlock() ? "klock ON" : "klock OFF");
            if (logRoom(48)) {
              Serial.print("  klock ");
              Serial.println(seqKlock() ? "ON" : "OFF");
            }
          }
        }
        any = true;
      } else if (ed != 0 && currentBinding().enc == ENC_SEQ_STEPOV &&
                 seqTrStepSel >= 0) {
        // held-step overlay in SEQMODE (non-DRUM): ENC1 = the held step's
        // TRANS offset, 1 count = 1 st, -12..+12 - a playback-only
        // modifier like the step's octave, the stored chord never changes
        if (ei == 0) {
          int8_t cur = stepTrAt((uint8_t)seqTrStepSel);
          int16_t nv = (int16_t)cur + ed;
          if (nv < -12) nv = -12;
          if (nv > 12) nv = 12;
          stepTrSet((uint8_t)seqTrStepSel, (int8_t)nv);
          char tb[20];
          if (nv >= 0)
            snprintf(tb, sizeof(tb), "step %d trans +%d", seqTrStepSel + 1, nv);
          else
            snprintf(tb, sizeof(tb), "step %d trans %d", seqTrStepSel + 1, nv);
          headerShow(tb);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(tb);
          }
        }
        any = true;
      } else if (ed != 0 && currentBinding().enc == ENC_SEQ_SKANK_STEP &&
                 seqTrStepSel >= 0) {
        // the SKANK lock row (a step held on a POLY+SKANK track): ENC1..7
        // override THAT step's derivation knob - turning while "inherit"
        // grabs the track's page value and nudges it, turning under 0
        // clears the lock back to inherit (0 is a legal value, the lock
        // storage keeps it distinct); ENC8 = the step's TRANS like the
        // plain held-step row (NOT% stays page-level, no lock slot)
        int8_t stp = seqTrStepSel;
        if (ei < 7) {
          static const char *nm[7] = { "tes", "mot", "shp", "thn",
                                       "ext", "sub", "tes" };
          int16_t cur = stepSkankLockAt((uint8_t)stp, (uint8_t)ei);
          if (cur < 0) cur = skankPrm((uint8_t)ei);   // inherit: grab the knob
          int16_t nv = cur + ed;
          if (nv > 100) nv = 100;
          if (nv < -1) nv = -1;
          stepSkankLockSet((uint8_t)stp, (uint8_t)ei, nv);
          char tb[24];
          if (nv < 0)
            snprintf(tb, sizeof(tb), "step %d %s inherit", stp + 1, nm[ei]);
          else if (ei == 2)
            snprintf(tb, sizeof(tb), "step %d shp %s", stp + 1,
                     skankShpName((uint8_t)nv));
          else if (ei == 6)
            snprintf(tb, sizeof(tb), "step %d tes %d%%", stp + 1, nv);
          else
            snprintf(tb, sizeof(tb), "step %d %s %d", stp + 1, nm[ei], nv);
          headerShow(tb);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(tb);
          }
        } else if (ei == 7) {      // ENC8 = the step's TRANS (shared knob)
          int8_t cur = stepTrAt((uint8_t)stp);
          int16_t nv = (int16_t)cur + ed;
          if (nv < -12) nv = -12;
          if (nv > 12) nv = 12;
          stepTrSet((uint8_t)stp, (int8_t)nv);
          char tb[24];
          if (nv >= 0)
            snprintf(tb, sizeof(tb), "step %d trans +%d", stp + 1, nv);
          else
            snprintf(tb, sizeof(tb), "step %d trans %d", stp + 1, nv);
          headerShow(tb);
          if (logRoom(48)) {
            Serial.print("  ");
            Serial.println(tb);
          }
        }
        any = true;
      } else if (genPageActive() && genOptSel != 0xFF) {
        // armed F radio: the sub-page swallows the whole row.  F1 = the
        // FULL options page - ENC1 TONES, ENC2 RANGE, ENC3 LOW, ENC4
        // ALGO (4 counts = 1 detent = 1 value, clamped, no wrap).  F2 =
        // STEP OCT on ENC2 (the tapped step) + MAP mode on ENC3, F3 =
        // BORROW on ENC3, F4 = MOD PROB on ENC4; ENC5-8 idle
        if (genOptSel == 0) {
          if (ei == 0) {
            seqGenTonesAdjust(ed);
            genOptionHeader(0);
          } else if (ei == 1) {
            seqGenRangeAdjust(ed);
            genOptionHeader(2);
          } else if (ei == 2) {
            seqGenLowAdjust(ed);
            genOptionHeader(1);
          } else if (ei == 3) {
            seqGenAlgoAdjust(ed);
            genOptionHeader(3);
          }
        } else if (genOptSel == 1 && (ei == 1 || ei == 2)) {
          if (ei == 1) {                    // ENC2: the step's octave
            int8_t ss = genStepSelStep();
            if (ss < 0) headerShow("pick step");
            else {
              int v = (int)stepOctAt((uint8_t)ss) + genModStep(0, ed);
              if (v < -2) v = -2;
              if (v > 2) v = 2;
              stepOctSet((uint8_t)ss, (int8_t)v);
              genOptionHeader(1);
            }
          } else {                          // ENC3: mapping mode
            seqMapAdjust(ed);
            char mb[16];
            snprintf(mb, sizeof(mb), "map %s", seqMapName(seqMap()));
            headerShow(mb);
            if (logRoom(48)) {
              Serial.print("  ");
              Serial.println(mb);
            }
          }
        } else if (genOptSel == 2 && ei == 2) {
          int8_t ss = genStepSelStep();     // ENC3: borrowed chord tone
          if (ss < 0) headerShow("pick step");
          else {
            int v = (int)stepBrwAt((uint8_t)ss) + genModStep(1, ed);
            if (v < 0) v = 0;
            if (v > 8) v = 8;
            stepBrwSet((uint8_t)ss, (uint8_t)v);
            genOptionHeader(2);
          }
        } else if (genOptSel == 3 && ei == 3) {
          int8_t ss = genStepSelStep();     // ENC4: modifier probability
          if (ss < 0) headerShow("pick step");
          else {
            int v = (int)stepModPAt((uint8_t)ss) + genModStep(2, ed);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            stepModPSet((uint8_t)ss, (uint8_t)v);
            genOptionHeader(3);
          }
        }
        any = true;
      } else if (ei == 0 && ed != 0 && !harmPageActive() && !compPageActive() &&
                 !skankPageActive() && !drumPageActive() &&
                 arpSlotAt(arpOptionSel(), 0) < 0) {
        seqOctaveShiftAdjust(ed);           // octave shift of the edit seq
        // (the open HARM page takes ENC1 for its GENRE, the COMP page for
        //  its HITS, the SKANK page for its TES and an armed ARP sub-page
        //  that owns ENC1 for its own knob - handled below)
        if (logRoom(48)) {
          Serial.print("  octshift ");
          Serial.println(seqOctaveShift());
        }
        any = true;
      } else if (modeVariant != 3) {
        // GNR8 page: ENC2 = fill %, ENC3 = accent %, ENC4 = glide %,
        // ENC7 = tie % - the page's four own knobs; ENC1 (octave), ENC5/6
        // (gate/vel) and ENC8 (prob) keep the bindings they had, and a
        // channel/TYPE/mode change drops the page (composite check)
        if (genPageActive() && (ei == 1 || ei == 2 || ei == 3 || ei == 6)) {
          uint8_t gs = (uint8_t)(seqEditChannel() - 1);
          if (ei == 1) {
            seqGenFillAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  fill ");
              Serial.print(seqGenFillOf(gs));
              Serial.println("%");
            }
          } else if (ei == 2) {
            seqGenAccAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  acc ");
              Serial.print(seqGenAccOf(gs));
              Serial.println("%");
            }
          } else if (ei == 3) {
            seqGenGlideAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  glide ");
              Serial.print(seqGenGlideOf(gs));
              Serial.println("%");
            }
          } else {
            seqGenTieAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  tie ");
              Serial.print(seqGenTieOf(gs));
              Serial.println("%");
            }
          }
        // M2 ARP page with an F sub-page armed: that page owns its slots
        // (see arpSlotT).  Slots it does not own fall through to the plain
        // page branch below, so SPAN/SKIP/MODE/INST/GATE/VEL/OCT/PROB stay
        // reachable from inside every sub-page
        } else if (ed != 0 && arpPageActive() &&
                   arpSlotAt(arpOptionSel(), ei) >= 0) {
          uint8_t slot = (uint8_t)arpSlotAt(arpOptionSel(), ei);
          uint8_t seq = (uint8_t)(seqEditChannel() - 1);
          seqArpPAdjust(slot, ed);
          if (logRoom(56)) {
            Serial.print("  ");
            Serial.print(seqArpPName(slot));
            Serial.print(' ');
            Serial.println(seqArpPValue(slot, seqArpPOf(seq, slot)));
          }
        // M2 ARP page: GATE / VEL / PROB are the plain row's knobs and the
        // page always turns the step-edit radio off, which is what those
        // three encoders are normally gated on - so they get their own
        // path here, or the slots read a value a turn can never move.  A
        // sub-page that owns the slot (F3's MEM / BIAS) is handled above
        } else if (ed != 0 && arpPageActive() &&
                   (ei == 4 || ei == 5 || ei == 7) &&
                   arpSlotAt(arpOptionSel(), ei) < 0) {
          if (ei == 4) {
            seqGateAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  gate ");
              Serial.print(seqGatePct());
              Serial.println("%");
            }
          } else if (ei == 5) {
            seqVelocityAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  vel ");
              Serial.println(seqVelocity());
            }
          } else {
            seqProbAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  prob ");
              Serial.print(seqProbPct());
              Serial.println("%");
            }
          }
        // M2 ARP page: ENC2 = span (1..4 octaves), ENC3 = skip rests,
        // ENC4 = mode, ENC7 = insert mode.  These four are the page's own
        // knobs; ENC1 (octave), ENC5/6 (gate/vel) and ENC8 (prob) keep the
        // bindings they had, and a TYPE/channel/mode change drops the page
        } else if (ed != 0 && arpPageActive() &&
            (ei == 1 || ei == 2 || ei == 3 || ei == 6)) {
          uint8_t seq = (uint8_t)(seqEditChannel() - 1);
          if (ei == 1) {
            seqArpSpanAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  span ");
              Serial.print(seqArpSpanOf(seq));
              Serial.println(" oct");
            }
          } else if (ei == 2) {
            seqArpSkipToggle(ed);
            if (logRoom(48)) {
              Serial.print("  skip ");
              Serial.println(seqArpSkipOf(seq) ? "ON" : "OFF");
            }
          } else if (ei == 3) {
            seqArpModeAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  arpmode ");
              Serial.println(seqArpModeName(seqArpModeOf(seq)));
            }
          } else {
            seqArpInstAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  inst ");
              Serial.println(seqArpInstName(seqArpInstOf(seq)));
            }
          }
        // M2 LEAD page: ENC2 = contour (ARCH/DESC/WALK, live), ENC3 = LOW,
        // ENC4 = RANGE (1..5 oct), ENC5 = CT (chord-tone vs scale blend),
        // ENC6 = GD (guide-tone set depth), ENC7 = density % (the shared
        // FILL slot).  ENC1 (octave) and ENC8 (prob) keep their bindings;
        // GATE/VEL live in the M1 edit menu, and a TYPE/channel/mode change
        // drops the page
        } else if (ed != 0 && leadPageActive() &&
            (ei == 1 || ei == 2 || ei == 3 || ei == 4 || ei == 5 ||
             ei == 6)) {
          uint8_t seq = (uint8_t)(seqEditChannel() - 1);
          if (ei == 1) {
            // CONTOUR is live: turning it reshapes the stored sequence in
            // place (the old contour's version is cached, the target's
            // last version restored - or reshaped fresh the first time),
            // no GEN press needed
            bool changed = seqLeadContourTurn(ed);
            if (changed) {
              refreshAllLeds();       // the step LEDs follow the melody
              refreshMainScreen();
            }
            if (logRoom(48)) {
              Serial.print("  contour ");
              Serial.println(seqLeadContourName(seqLeadContourOf(seq)));
            }
          } else if (ei == 2) {
            seqGenLowAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  low ");
              Serial.println(seqGenLowName(seqGenLowOf(seq)));
            }
          } else if (ei == 3) {
            seqGenRangeAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  range ");
              Serial.print(seqGenRangeOf(seq));
              Serial.println(" oct");
            }
          } else if (ei == 4) {
            seqLeadCtAdjust(ed);          // LEAD CT blend %
            if (logRoom(48)) {
              Serial.print("  ct blend ");
              Serial.print(seqLeadCtOf(seq));
              Serial.println("%");
            }
          } else if (ei == 5) {
            seqLeadGdAdjust(ed);          // LEAD GD set depth
            if (logRoom(48)) {
              Serial.print("  gd set ");
              Serial.println(seqLeadGdName(seqLeadGdOf(seq)));
            }
          } else {
            seqGenFillAdjust(ed);          // LEAD density % (shared FILL)
            if (logRoom(48)) {
              Serial.print("  dens ");
              Serial.print(seqGenFillOf(seq));
              Serial.println("%");
            }
          }
        // M2 HARM page: ENC1 = GENRE (the harmonic grammar), ENC2 = FILL
        // (the HARM track's FILL% - the same per-track roll gate the
        // generators use; M3 lands chords on the rolled steps and clears
        // the rest), ENC3 = SRC (bass/arp source for the feeder read),
        // ENC4 = HOLD (force hold on a repeated bass).  M3 bakes; ENC5..8
        // keep their bindings, and a TYPE/channel/mode change drops the page
        } else if (ed != 0 && harmPageActive() &&
            (ei == 0 || ei == 1 || ei == 2 || ei == 3)) {
          if (ei == 0) {
            k4GenreAdjust(ed);
            headerShow(k4GenreShort());   // announce the picked genre
            if (logRoom(48)) {
              Serial.print("  genre ");
              Serial.println(k4GenreName());
            }
          } else if (ei == 1) {
            seqGenFillAdjust(ed);         // FILL% of the HARM track itself
            char fb[10];
            snprintf(fb, sizeof(fb), "FILL %u%%",
                     seqGenFillOf((uint8_t)(seqEditChannel() - 1)));
            headerShow(fb);
            if (logRoom(48)) { Serial.print("  "); Serial.println(fb); }
          } else if (ei == 2) {
            k4HarmInputAdjust(ed);
            char sb[10];
            snprintf(sb, sizeof(sb), "SRC %s", k4HarmInputName());
            headerShow(sb);
            if (logRoom(48)) { Serial.print("  "); Serial.println(sb); }
          } else {
            k4HarmHoldAdjust(ed);
            char hh[12];
            snprintf(hh, sizeof(hh), "HOLD %s", k4HarmHoldName());
            headerShow(hh);
            if (logRoom(48)) { Serial.print("  "); Serial.println(hh); }
          }
        } else if (ed != 0 && drumPageActive()) {
          // DRUM page (M2 on a DRUM track): the armed F set picks which
          // parameter each encoder drives - 0xFF = the base row (GENRE /
          // VAR / FILL / MOT on ENC1..4, the four lane line selectors on
          // ENC5..8), GROOVE = the first four, LINES / FILL% / MOT% = the
          // same four encoders for the per-lane values.  Every turn
          // rebuilds the pattern in place (Drum.cpp takes the undo snapshot
          // once per burst), so a continuous turn undoes as one gesture
          uint8_t set = drumSetSelGet();
          int8_t prm = -1;
          if (set == 0xFF) prm = (ei <= 3) ? (int8_t)ei : (int8_t)(10 + ei - 4);
          else if (set == 0 && ei <= 3) prm = (int8_t)ei;
          else if (set == 1 && ei <= 3) prm = (int8_t)(10 + ei);
          else if (set == 2 && ei <= 3) prm = (int8_t)(20 + ei);
          else if (set == 3 && ei <= 3) prm = (int8_t)(30 + ei);
          static const char *const lane[4] = { "kick", "snare", "hat", "perc" };
          char db[26];
          db[0] = 0;
          if (prm == 0) {
            drumGenreAdjust(ed);
            snprintf(db, sizeof(db), "drums %s", drumGenreName(drumGenre()));
          } else if (prm == 1) {
            drumVarAdjust(ed);
            snprintf(db, sizeof(db), "var %s", drumVarName(drumVar()));
          } else if (prm == 2) {
            drumFillAdjust(ed);
            snprintf(db, sizeof(db), "fill %u%%", (unsigned)drumFill());
          } else if (prm == 3) {
            drumMotAdjust(ed);
            snprintf(db, sizeof(db), "mot %u%%", (unsigned)drumMot());
          } else if (prm >= 10 && prm <= 13) {
            uint8_t ln = (uint8_t)(prm - 10);
            drumLineAdjust(ln, ed);
            snprintf(db, sizeof(db), "%s %s", lane[ln],
                     drumLineName(drumLine(ln)));
          } else if (prm >= 20 && prm <= 23) {
            uint8_t ln = (uint8_t)(prm - 20);
            drumLaneFillAdjust(ln, ed);
            snprintf(db, sizeof(db), "%s fill %u%%", lane[ln],
                     (unsigned)drumLaneFill(ln));
          } else if (prm >= 30 && prm <= 33) {
            uint8_t ln = (uint8_t)(prm - 30);
            drumLaneMotAdjust(ln, ed);
            snprintf(db, sizeof(db), "%s mot %u%%", lane[ln],
                     (unsigned)drumLaneMot(ln));
          }
          if (prm >= 0) {
            drumProduce();            // live rebuild of the track's steps
            headerShow(db);
            refreshAllLeds();         // the step LEDs follow the pattern
            refreshMainScreen();
            if (logRoom(48)) { Serial.print("  "); Serial.println(db); }
          }
          any = true;
        } else if (ed != 0 && compPageActive() && ei == 0) {
          seqCompHitsAdjust(ed);            // ENC1 = HITS (stabs, 0..8,
                                            // 4 counts = 1)
          char cb[10];
          if (seqCompHits() <= 1) {
            headerShow("comp off");
          } else {
            snprintf(cb, sizeof(cb), "comp %d", seqCompHits());
            headerShow(cb);
          }
          if (logRoom(48)) {
            Serial.print("  comp hits ");
            Serial.println(seqCompHits());
          }
        } else if (ed != 0 && skankPageActive()) {
          // the SKANK row: ENC1 TES, ENC2 MOT, ENC3 SHP, ENC4 THN, ENC5
          // EXT, ENC6 SUB, ENC7 TES%, ENC8 NOT% (0..100, 4 counts = 1
          // unit each) - the last two are the per-hit randomizers: TES%
          // rolls the inversion fully at random instead of the walk,
          // NOT% mutates one voicing tone to a random in-scale note
          if (ei < 8) {
            skankPrmAdjust((uint8_t)ei, ed);
            char sb[14];
            if (ei == 2) {
              snprintf(sb, sizeof(sb), "shp %s", skankShpName(skankPrm(2)));
            } else if (ei == 4 || ei == 5 || ei == 6 || ei == 7) {
              static const char *nm[4] = { "ext", "sub", "tes", "not" };
              snprintf(sb, sizeof(sb), "%s %u%%", nm[ei - 4],
                       skankPrm((uint8_t)ei));
            } else {
              static const char *nm[6] = { "tes", "mot", "shp", "thn",
                                           "ext", "sub" };
              snprintf(sb, sizeof(sb), "%s %u", nm[ei], skankPrm((uint8_t)ei));
            }
            headerShow(sb);
            if (logRoom(48)) { Serial.print("  "); Serial.println(sb); }
          }
        // F3 held: ENC6 = ACCENT velocity of the edit track (every track
        // type).  Otherwise the step-edit value encoders: ENC5 = gate %,
        // ENC6 = default velocity (MONO only), ENC7 = the FX type or the
        // armed step TYPE while those targets are engaged (POLY),
        // ENC8 = the probability of the edit track itself - live, so every
        // PROB step of that track re-rolls at the new rate
        } else if (ed != 0 && ei == 4 && seqTrack() == SEQ_TRACK_DRUM) {
          // SEQMODE ENC5 on a DRUM track = the kit MAPPING: CUSTOM (this
          // kit, boot default) or GM (General MIDI percussion).  It is a
          // track setting, so it works with the step-edit radio on or off;
          // the M2 DRUM page keeps ENC5 = KICK while it is open.  Two-state,
          // NO wrap (CW = GM, CCW = CUSTOM, like KLOCK); a change re-bakes
          // the pattern onto the new map (one undo burst)
          if (drumMapSetDir(ed)) {
            drumProduce();
            char mb[16];
            snprintf(mb, sizeof(mb), "map %s", drumMapName(drumMap()));
            headerShow(mb);
            refreshAllLeds();
            refreshMainScreen();
            if (logRoom(48)) { Serial.print("  "); Serial.println(mb); }
          }
          any = true;
        } else if (ei == 5 && ed != 0 && f3AccentActive()) {
          seqAccentVelocityAdjust(ed);
          if (logRoom(48)) {
            Serial.print("  accvel ");
            Serial.println(seqAccentVelocity());
          }
        } else if (ed != 0 && stepEditOn) {
          if (ei == 4 && stepEditCtx()) {
            seqGateAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  gate ");
              Serial.print(seqGatePct());
              Serial.println("%");
            }
          } else if (ei == 5 && stepEditCtx()) {
            seqVelocityAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  vel ");
              Serial.println(seqVelocity());
            }
          } else if (ei == 6 && stepEditTarget == 5 &&
                     seqTrack() == SEQ_TRACK_POLY) {
            // FX type of the pads: sus2 sus4 6 m7 M7 9 color (wrap, 7)
            uint8_t t = (uint8_t)((stepFxType + (ed > 0 ? 1 : 6)) % 7);
            if (t != stepFxType) {
              stepFxType = t;
              char hb[16];
              snprintf(hb, sizeof(hb), "FX %s", stepFxName(stepFxType));
              headerShow(hb);
              if (logRoom(48)) {
                Serial.print("  fx type ");
                Serial.println(stepFxName(stepFxType));
              }
            }
          } else if (ei == 6 && stepEditTarget == 6 &&
                     seqTrack() == SEQ_TRACK_POLY) {
            // armed step TYPE of the pads: STEP STRUM HAND (wrap)
            stepTypeArmAdjust(ed);
            char hb[16];
            snprintf(hb, sizeof(hb), "TYPE %s", stepTypeArmName());
            headerShow(hb);
            if (logRoom(48)) {
              Serial.print("  type arm ");
              Serial.println(stepTypeArmName());
            }
          } else if (ei == 7 &&
                     (stepEditCtx() || stepEditAccCtx())) {
            seqProbAdjust(ed);
            if (logRoom(48)) {
              Serial.print("  prob ");
              Serial.print(seqProbPct());
              Serial.println("%");
            }
          }
        }
      } else if (ei == 1 && ed != 0) {
        // SEQMODE3: ENC2 = pattern length of the edit sequence (0..64
        // steps, boot default 16; 0 = silent track; per-track RAM only)
        seqPatLenAdjust(ed);
        refreshAllLeds();       // playhead / step view follows the length
        if (logRoom(48)) {
          Serial.print("  len ");
          Serial.println(seqPatLen());
        }
        any = true;
      } else if (ei == 2 && ed != 0) {
        // SEQMODE3: ENC3 = FOLLOW, three-state (OFF / ON / FREEZE): OFF =
        // own steps play the stored line, ON = notes re-resolve live from
        // the feeder, FREEZE = hold the chord the feeder is playing at the
        // moment you switch.  4 counts = 1 state, clamped at both ends like
        // every other SEQMODE dial (no wrap - a turn past FREEZE used to
        // drop straight back to OFF)
        static int16_t folRes = 0;
        folRes += ed;
        int8_t st = 0;
        while (folRes >= 4) { folRes -= 4; st++; }
        while (folRes <= -4) { folRes += 4; st--; }
        if (st) {
          static const char *folN[3] = { "OFF", "ON", "FREEZE" };
          int m = (int)seqFollowMode() + st;
          if (m < 0) m = 0;
          if (m > 2) m = 2;
          if ((uint8_t)m != seqFollowMode()) seqFollowModeSet((uint8_t)m);
          if (logRoom(48)) {
            Serial.print("  follow ");
            Serial.println(folN[m]);
          }
        }
        any = true;
      } else if (ei == 7 && ed != 0) {
        long bpm = (long)seqTempo() + ed;
        if (bpm < 40) bpm = 40;
        if (bpm > 240) bpm = 240;
        seqSetTempo((uint16_t)bpm);
        if (logRoom(48)) {
          Serial.print("  tempo ");
          Serial.println(seqTempo());
        }
        any = true;
      } else if (ei == 3 && ed != 0) {
        seqSpeedAdjust(ed);               // SPEED: 1/64 .. 1 .. 4 (no wrap)
        if (logRoom(48)) {
          Serial.print("  speed ");
          Serial.println(seqSpeedName(seqSpeedIdxOf(
              (uint8_t)(seqEditChannel() - 1))));
        }
        any = true;
      } else if (ei == 4 && ed != 0) {
        seqInputChannelAdjust(ed);         // input channel of the edit seq
        if (logRoom(48)) {
          Serial.print("  in ch ");
          Serial.println(seqInputChannel());
        }
        any = true;
      } else if (ei == 5 && ed != 0) {
        seqTrackAdjust(ed);                // TRACK: DRUM/MONO/POLY (no wrap)
        if (seqTrack() != SEQ_TRACK_MONO) arpPageDismiss();  // no arp left
        if (!strumFRowCtx()) strumFRowDismiss();   // nor a strum ADDS row
        genPageDismiss();                  // the GNR8 page needs MONO+BASS
        leadPageDismiss();                 // ...the LEAD page needs MONO+LEAD
        harmPageDismiss();                 // ...the HARM page needs POLY+HARM
        compPageDismiss();                 // ...and COMP needs POLY+STEP
        skankPageDismiss();                // ...and SKANK needs POLY+SKANK
        drumPageDismiss();                 // ...and the DRUM page needs a DRUM track
        stepChainDisarm();                 // ...and a chain arm outlives nothing
        refreshAllLeds();                  // the step-edit radio follows the track type
        if (logRoom(48)) {
          Serial.print("  track ");
          Serial.println(seqTrackName());
        }
        any = true;
      } else if (ei == 6 && ed != 0 &&
                 (seqTrack() == SEQ_TRACK_MONO ||
                  seqTrack() == SEQ_TRACK_POLY)) {
        if (seqTrack() == SEQ_TRACK_MONO) {
          seqMonoTypeAdjust(ed);           // TYPE: ROOT/BASS/ARP/STRUM/LEAD
          if (seqMonoType() != SEQ_MONO_ARP) arpPageDismiss();
          strumFRowDismiss();     // a MONO track never takes the ADDS row
          genPageDismiss();       // only TYPE = BASS takes the GNR8 page
          leadPageDismiss();      // ...and only TYPE = LEAD takes the LEAD page
          compPageDismiss();      // ...and COMP needs a POLY track
          skankPageDismiss();     // ...and so does the SKANK page
          stepChainDisarm();      // ...and the chain arm belongs to BASS
          if (logRoom(48)) {
            Serial.print("  mono ");
            Serial.println(seqMonoTypeName());
          }
        } else {
          seqPolyTypeAdjust(ed);     // TYPE: STEP/STRUM/HAND/HARM (no wrap)
          if (!strumFRowCtx()) strumFRowDismiss();   // left STRUM/HAND
          if (seqPolyType() != SEQ_POLY_HARM) harmPageDismiss();  // left HARM
          if (seqPolyType() != SEQ_POLY_STEP) compPageDismiss();  // left STEP
          if (seqPolyType() != SEQ_POLY_SKANK)
            skankPageDismiss();   // left SKANK
          if (logRoom(48)) {
            Serial.print("  poly ");
            Serial.println(seqPolyTypeName());
          }
        }
        any = true;
      }
    }

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
