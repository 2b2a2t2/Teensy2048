#!/usr/bin/env python3
"""Literature-based genre knowledge for genres without (or beyond) corpora.

Two kinds of data live here:

1. Rhythm templates + weights + microVar for EVERY genre (hand-authored from
   genre-convention literature: groove analyses, production guides, style
   primers). These are judgement calls, flagged as such in GENRE_DATA.md.

2. Transition / quality / inversion adjustments for the eight hybrid genres
   (pop-family prior x style multipliers) and the four estimated genres
   (same prior, flagged estimated).

All multipliers are applied to a pooled empirical prior (POP+ROCK+FOLK),
then normalised to integer rows by generate.py (largest remainder).
"""

# Global rhythm templates, one bar = 16 sixteenths. Bit i = chord attack at
# sixteenth i. Shared by all genres; only the weights differ.
RHYTHM_TEMPLATES = [
    ("WHOLE",    0x0001),  # attack on 1, held
    ("HALF",     0x0101),  # attacks on 1 and 3
    ("QUART",    0x1111),  # attacks on every beat
    ("EIGHTH",   0x5555),  # eighth notes
    ("BACKBEAT", 0x1010),  # stabs on 2 and 4
    ("OFFBEAT",  0x4444),  # '&' of every beat (skank / pump)
    ("PUSH",     0x4001),  # anticipation on the last 16th + downbeat
    ("SYNCOP",   0x0909),  # 1, a-of-2, 3, a-of-4
]

# genre -> (rhythmW sum=100, microVar 0-255)
RHYTHM_W = {
    "POP":        [10, 15, 35, 10,  5, 10, 10,  5],
    "ROCK":       [ 5, 10, 25, 25,  5, 10, 15,  5],
    "JAZZ":       [ 5, 10, 15, 10,  5, 25, 10, 20],
    "CLASSICAL":  [40, 30, 15,  5,  0,  5,  3,  2],
    "FOLK":       [15, 25, 35, 10,  2,  5,  5,  3],
    "COUNTRY":    [ 8, 18, 35, 15,  4,  8,  9,  3],
    "BLUES":      [ 5, 15, 30, 20,  3, 12, 10,  5],
    "RNB":        [ 8, 14, 22,  8,  8, 18, 12, 10],
    "FUNK":       [ 3, 10, 15, 15, 10, 25, 10, 12],
    "METAL":      [ 2,  8, 22, 30,  8,  8, 14,  8],
    "HIPHOP":     [12, 18, 18,  6, 10, 14, 14,  8],
    "ELECTRONIC": [ 5, 15, 30, 15,  8, 15,  7,  5],
    "REGGAE":     [ 4, 10, 14,  6, 12, 38, 10,  6],
    "DUBTECHNO":  [30, 20, 10,  5,  5, 20,  5,  5],
    "GOSPEL":     [ 8, 15, 25, 10, 12, 15,  8,  7],
    "LATIN":      [ 4, 12, 22, 12,  8, 20, 10, 12],
    "CINEMATIC":  [55, 25, 10,  3,  1,  3,  2,  1],
    "MODAL":      [30, 25, 20,  8,  4,  6,  4,  3],
}

MICRO_VAR = {
    "POP": 50, "ROCK": 60, "JAZZ": 175, "CLASSICAL": 90, "FOLK": 70,
    "COUNTRY": 65, "BLUES": 120, "RNB": 110, "FUNK": 140, "METAL": 20,
    "HIPHOP": 155, "ELECTRONIC": 15, "REGGAE": 100, "GOSPEL": 105,
    "DUBTECHNO": 30,
    "LATIN": 130, "CINEMATIC": 85, "MODAL": 95,
}

# Degrees: 0=I 1=ii 2=iii 3=IV 4=V 5=vi 6=vii (snapped; bVII lands on 6).
# Edge multipliers on the pooled empirical prior.
TRANS_MULT = {
    "COUNTRY": {
        (0, 3): 1.4, (3, 0): 1.4, (0, 4): 1.15, (4, 0): 1.15,
        (6, 0): 1.5, (0, 6): 1.5, (6, 6): 1.6, (3, 6): 1.3,
        (2, 5): 1.3, (5, 1): 1.3, (5, 3): 1.3,
    },
    "BLUES": {  # 12-bar: I IV V + bVII, heavy V->IV, turnarounds
        (0, 0): 1.5, (0, 3): 2.0, (3, 0): 1.6, (0, 4): 1.7,
        (4, 3): 1.9, (3, 4): 1.5, (4, 4): 1.5, (4, 0): 1.4,
        (6, 6): 2.2, (0, 6): 1.6, (6, 0): 1.6, (6, 3): 1.4,
        (5, 4): 1.4, (5, 0): 1.3,
    },
    "RNB": {  # ii-V-I rich, extended cadences
        (1, 4): 1.9, (4, 0): 1.3, (5, 1): 1.6, (2, 5): 1.4,
        (0, 5): 1.3, (3, 2): 1.3, (0, 1): 1.3, (6, 0): 1.3,
        (1, 1): 1.3, (0, 0): 1.2,
    },
    "FUNK": {  # vamps on I7/IV7, bVII, static grooves
        (0, 0): 1.7, (0, 3): 1.5, (3, 0): 1.4, (6, 6): 1.6,
        (0, 6): 1.4, (6, 0): 1.4, (3, 3): 1.3, (0, 4): 1.15,
    },
    "METAL": {  # i - bVI - bVII riffs, power chords
        (0, 5): 1.5, (5, 0): 1.5, (0, 6): 1.7, (6, 0): 1.7,
        (5, 6): 1.8, (6, 5): 1.8, (0, 3): 1.3, (3, 0): 1.3,
        (0, 0): 1.3, (2, 6): 1.4, (4, 0): 1.3,
    },
    "HIPHOP": {  # minor-key loops i-bVII-bVI, vi-I turnarounds
        (0, 0): 1.6, (0, 6): 1.7, (6, 5): 1.7, (5, 0): 1.6,
        (5, 5): 1.5, (0, 5): 1.4, (5, 3): 1.3, (3, 4): 1.3,
        (4, 5): 1.4, (6, 6): 1.4,
    },
    "ELECTRONIC": {  # vi-IV-I-V loop and relatives
        (5, 3): 1.9, (3, 0): 1.9, (0, 4): 1.7, (4, 5): 1.8,
        (0, 0): 1.4, (5, 5): 1.4, (0, 3): 1.4, (4, 0): 1.3,
    },
    "REGGAE": {  # static skank, I-IV / I-bVII
        (0, 0): 1.4, (0, 3): 1.5, (3, 0): 1.5, (0, 6): 1.5,
        (6, 0): 1.5, (4, 0): 1.3, (3, 4): 1.3, (6, 6): 1.4,
    },
    # ---- estimated-from-literature genres (same machinery, flagged) ----
    "GOSPEL": {
        (1, 4): 1.6, (3, 0): 1.5, (0, 3): 1.4, (5, 1): 1.4,
        (6, 0): 1.4, (0, 5): 1.3, (4, 3): 1.4, (0, 1): 1.3,
        (0, 0): 1.2,
    },
    "LATIN": {
        (0, 3): 1.4, (3, 4): 1.5, (4, 0): 1.5, (0, 4): 1.3,
        (0, 5): 1.3, (5, 4): 1.4, (4, 5): 1.3, (0, 0): 1.2,
        (6, 0): 1.3,
    },
    "CINEMATIC": {
        (0, 5): 1.6, (5, 3): 1.5, (3, 4): 1.4, (0, 3): 1.4,
        (4, 5): 1.4, (0, 4): 1.3, (5, 5): 1.4, (0, 0): 1.3,
        (6, 5): 1.3,
    },
    "MODAL": {
        (0, 0): 2.5, (0, 6): 1.6, (6, 0): 1.6, (0, 4): 1.3,
        (6, 4): 1.3, (4, 0): 1.3, (0, 3): 1.3, (3, 0): 1.3,
        (6, 6): 1.4,
    },
}

# Quality bucket multipliers: [triad, 7th, 9th+, sus/alt]
QUAL_MULT = {
    "COUNTRY": [1.15, 0.9, 0.7, 0.8],
    "BLUES":   [1.05, 1.7, 0.8, 1.1],
    "RNB":     [0.7, 1.3, 1.8, 1.2],
    "FUNK":    [0.9, 1.4, 1.3, 1.6],
    "METAL":   [1.5, 0.7, 0.5, 1.1],
    "HIPHOP":  [1.2, 1.1, 1.0, 1.1],
    "ELECTRONIC": [1.3, 0.8, 0.9, 1.3],
    "REGGAE":  [1.1, 1.1, 0.9, 1.4],
    "GOSPEL":  [0.8, 1.3, 1.5, 1.5],
    "LATIN":   [1.1, 1.1, 0.9, 1.0],
    "CINEMATIC": [1.1, 1.0, 1.0, 1.3],
    "MODAL":   [1.2, 0.9, 0.8, 1.4],
}

# Inversion multipliers: [root, 1st, 2nd, 3rd-in-bass]
INV_MULT = {
    "COUNTRY": [1.1, 1.0, 1.1, 1.0],
    "BLUES":   [1.2, 0.9, 1.0, 1.0],
    "RNB":     [0.8, 1.2, 1.2, 1.2],
    "FUNK":    [1.1, 1.0, 1.1, 1.0],
    "METAL":   [1.4, 0.7, 0.7, 0.8],
    "HIPHOP":  [1.2, 0.9, 1.0, 0.9],
    "ELECTRONIC": [1.3, 0.8, 0.9, 0.8],
    "REGGAE":  [1.1, 1.0, 1.0, 1.0],
    "GOSPEL":  [0.8, 1.3, 1.2, 1.2],
    "LATIN":   [1.0, 1.1, 1.1, 1.0],
    "CINEMATIC": [1.0, 1.1, 1.1, 1.0],
    "MODAL":   [1.1, 1.0, 1.0, 1.0],
}

# Genres with no chord corpus: flagged estimated in output/docs.
ESTIMATED = {"GOSPEL", "LATIN", "CINEMATIC", "MODAL"}
# Genres synthesised from pop-family prior + style multipliers.
HYBRID = {"COUNTRY", "BLUES", "RNB", "FUNK", "METAL", "HIPHOP",
          "ELECTRONIC", "REGGAE"}
# Genres with real corpora (see process.py / sources.json).
EMPIRICAL = {"POP": ["POP_billboard", "POP_isophonics"],
             "ROCK": ["ROCK_isophonics"],
             "JAZZ": ["JAZZ_weimar"],
             "CLASSICAL": ["CLASSICAL_tavern"],
             "FOLK": ["FOLK_nottingham"]}

# Genres whose tables are authored in full here, straight from genre theory
# (no corpus, no prior mixing).  Rows are raw weights; generate.py keeps
# them as-is (each row already sums to 100) and flags them estimated.
#
# DUBTECHNO — Basic Channel / Deepchord school: near-static single-chord
# vamp with the classic parallel formulas layered on top:
#   static vamp        i -> i              (diag 60 in the i row)
#   step-down drift    i <-> bVII          (-2 semitones, rows 0<->6)
#   classic fourth     i <-> iv            (+5, rows 0<->3)
#   shadow fifth       i <-> v             (+7, rows 0<->4)
#   hypnotic third     i <-> bIII          (+3, rows 0<->2)
# Quality: m7 standard, m9 blueprint (bucket 2 = m7+9 over minor via
# addExtensions), sus4 pivot strongest on i (bucket 3, x2 at TENS>=67).
# Bass stays anchored: root-position inversions (the inversion pivot is
# voiced by the sus bucket, not by moving the bass).
HAND = {
    "DUBTECHNO": {
        "trans": [
            # I   ii  bIII  iv   v   bVI bVII
            [60,  1,  8, 12, 10,  1,  8],   # i: vamp + fourth + fifth + third
            [45, 15,  8, 12, 10,  5,  5],   # ii
            [50,  2, 15,  8,  7,  8, 10],   # bIII
            [55,  2,  5, 20,  8,  5,  5],   # iv
            [55,  2,  5,  8, 20,  5,  5],   # v
            [45,  2, 15,  8,  7, 15,  8],   # bVI
            [52,  2,  8,  8,  7,  5, 18],   # bVII: drift partner, back to i
        ],
        "qual": [
            # triad  7th  9th+ sus/alt
            [ 5, 40, 45, 10],   # i: m9 blueprint + sus pivot
            [ 8, 47, 40,  5],
            [ 8, 47, 40,  5],
            [ 8, 47, 40,  5],
            [ 8, 47, 40,  5],
            [ 8, 47, 40,  5],
            [ 8, 47, 40,  5],
        ],
        "inv": [
            [96, 2, 1, 1],
            [96, 2, 1, 1],
            [96, 2, 1, 1],
            [96, 2, 1, 1],
            [96, 2, 1, 1],
            [96, 2, 1, 1],
            [96, 2, 1, 1],
        ],
    },
}

GENRE_ORDER = ["POP", "ROCK", "JAZZ", "CLASSICAL", "FOLK", "COUNTRY",
               "BLUES", "RNB", "FUNK", "METAL", "HIPHOP", "ELECTRONIC",
               "REGGAE", "DUBTECHNO", "GOSPEL", "LATIN", "CINEMATIC",
               "MODAL"]


def check():
    for g, w in RHYTHM_W.items():
        assert sum(w) == 100, (g, sum(w))
    assert set(RHYTHM_W) == set(MICRO_VAR) == set(GENRE_ORDER)
    assert set(HYBRID) | set(ESTIMATED) | set(EMPIRICAL) | set(HAND) == set(GENRE_ORDER)
    assert not (set(HAND) & (set(HYBRID) | set(ESTIMATED) | set(EMPIRICAL)))
    for g, d in HAND.items():
        for field, rows in d.items():
            cols = len(rows[0])
            assert len(rows) == 7 and all(len(r) == cols for r in rows), (g, field)
            assert all(sum(r) == 100 for r in rows), (g, field)
    return True


if __name__ == "__main__":
    check()
    print("literature.py OK:", len(GENRE_ORDER), "genres,",
          len(RHYTHM_TEMPLATES), "rhythm templates")
