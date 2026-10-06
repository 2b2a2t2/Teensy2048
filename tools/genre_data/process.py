#!/usr/bin/env python3
"""Parse every cached source, detect keys, map chords to scale degrees, count.

Outputs results.json (raw integer counts per genre) consumed by generate.py.

Run:  python3 tools/genre_data/process.py
"""
import json
import math
import re
import sqlite3
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
CACHE = HERE / "cache"
OUT = HERE / "results.json"

# ---------------------------------------------------------------- theory ----

PC = {"C": 0, "B#": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4,
      "Fb": 4, "E#": 5, "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8, "Ab": 8,
      "A": 9, "A#": 10, "Bb": 10, "B": 11, "Cb": 11}
MAJOR_SCALE = [0, 2, 4, 5, 7, 9, 11]
MINOR_SCALE = [0, 2, 3, 5, 7, 8, 10]  # natural minor / aeolian

PROF_MAJ = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88]
PROF_MIN = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]


def note_pc(tok: str):
    if not tok:
        return None
    tok = tok.strip().replace("-", "b")
    m = re.match(r"^([A-Ga-g])([#b]{0,2})$", tok)
    if not m:
        return None
    return (PC[m.group(1).upper()] + m.group(2).count("#") - m.group(2).count("b")) % 12


def scale_of(mode: str):
    return MAJOR_SCALE if mode == "maj" else MINOR_SCALE


def degree_of(root: int, tonic: int, mode: str):
    """Snap a pitch class to a diatonic degree 0..6. Returns (deg, chromatic)."""
    pcs = [(tonic + s) % 12 for s in scale_of(mode)]
    if root in pcs:
        return pcs.index(root), False
    best, bestd = 0, 99
    for i, p in enumerate(pcs):
        d = min((root - p) % 12, (p - root) % 12)
        if d < bestd:
            best, bestd = i, d
    return best, True


# ------------------------------------------------------- chord symbols -----

NOCHORD = {"N", "X", "x", "NC", "nc", "N.C.", "n.c.", "N.C", "none", "-"}


def is_nochord(s: str) -> bool:
    s = s.strip().strip("()")
    return s in NOCHORD or bool(re.fullmatch(r"\d+", s))


def quality_bucket(q: str) -> int:
    """0 triad, 1 7th, 2 9th+, 3 sus/alt."""
    ql = q.lower().replace(" ", "")
    if not ql:
        return 0
    if "sus" in ql:
        return 3
    if re.search(r"1[13]|9", ql):
        return 2
    if "7" in ql:
        return 1
    if re.search(r"[#b](5|9|11|13)|alt|\+", ql):
        return 3
    return 0


def degree_bass_interval(b: str, quality: str):
    """Harte degree-bass ('/3', '/5', '/b7') -> semitone interval above root."""
    ql = quality.lower()
    third = 3 if ("min" in ql or "dim" in ql) else 4
    fifth = 6 if "dim" in ql else (8 if ("aug" in ql or "+" in ql) else 7)
    seventh = 9 if "dim7" in ql else 10
    table = {"1": 0, "3": third, "b3": 3, "#3": third, "5": fifth, "b5": 6,
             "#5": 8, "7": seventh, "b7": 10, "#7": 11}
    return table.get(b)


def parse_chord(sym: str):
    """(root_pc, quality_str, bass_pc|None) or None for N/X/annotations."""
    s = sym.strip().strip("()").strip()  # optional-chord parens
    if not s or s in ("N", "X", "x", "n.c.", "N.C.", "NC", "nc", "N.C"):
        return None
    if re.fullmatch(r"\d+", s):  # rehearsal marks, not chords
        return None
    if s.startswith("/"):  # mangled prefix e.g. '/@<.5D7'
        m0 = re.search(r"[A-Ga-g]", s)
        s = s[m0.start():] if m0 else s
    if s[0] in "^_<>@+%&=" or s.startswith("%"):
        return None
    if ":" in s:  # Harte: root:quality[/bass]
        head, rest = s.split(":", 1)
        bass = None  # absent -> Harte default: root position
        if "/" in rest:
            rest, b = rest.split("/", 1)
            b = b.strip()
            bass = -1  # explicit but non-chord-tone bass (/9 etc.): skip
            if re.fullmatch(r"[b#]?\d+", b):  # degree bass (relative to root)
                iv = degree_bass_interval(b, rest)
                if iv is not None:
                    r0 = note_pc(head)
                    if r0 is not None:
                        bass = (r0 + iv) % 12
            else:
                pc = note_pc(b)
                if pc is not None:
                    bass = pc
        root = note_pc(head)
        return (root, rest, bass) if root is not None else None
    m = re.match(r"^([A-Ga-g][#b]?)([^/]*)(?:/(.+))?$", s)
    if not m:
        return None
    root = note_pc(m.group(1))
    if root is None:
        return None
    bass = None  # absent -> root position
    if m.group(3):
        bass = note_pc(m.group(3)) if re.match(r"^[A-Ga-g]", m.group(3)) else -1
        if bass is None:
            bass = -1
    return (root, m.group(2), bass)


def inversion_bucket(root: int, quality: str, bass):
    """0 root, 1 first, 2 second, 3 third-in-bass; None if unknown."""
    if bass is None:
        return None
    iv = (bass - root) % 12
    ql = quality.lower()
    dim = any(t in ql for t in ("dim", "°", "ø", "m7b5"))
    if iv == 0:
        return 0
    if iv in (3, 4):
        return 1
    if iv == 7 or (iv == 6 and dim) or iv == 8:
        return 2
    if iv in (9, 10):
        return 3
    return None


# ----------------------------------------------------------- K-S key -------

def pearson(x, y):
    n = len(x)
    mx, my = sum(x) / n, sum(y) / n
    num = sum((a - mx) * (b - my) for a, b in zip(x, y))
    dx = math.sqrt(sum((a - mx) ** 2 for a in x))
    dy = math.sqrt(sum((b - my) ** 2 for b in y))
    if dx == 0 or dy == 0:
        return -1.0
    return num / (dx * dy)


def ks_key(chroma12):
    """(tonic, mode, corr) best Krumhansl-Schmuckler fit, or None."""
    if sum(chroma12) <= 0:
        return None
    best = None
    for tonic in range(12):
        for mode, prof in (("maj", PROF_MAJ), ("min", PROF_MIN)):
            rotated = [chroma12[(tonic + i) % 12] for i in range(12)]
            r = pearson(rotated, prof)
            if best is None or r > best[2]:
                best = (tonic, mode, r)
    return best


KS_MIN_CORR = 0.55


def ks_or_none(chroma12):
    k = ks_key(chroma12)
    if k is None or k[2] < KS_MIN_CORR:
        return None
    return k


# ------------------------------------------------------------- accum -------

class Acc:
    def __init__(self, genre):
        self.genre = genre
        self.pieces = 0
        self.dropped = 0
        self.events = 0
        self.transitions = 0
        self.chromatic = 0
        self.trans = Counter()
        self.qual = Counter()
        self.inv = Counter()
        self.keys = Counter()
        self.unparsed = Counter()

    def add_piece(self, segments):
        """segments: [(tonic, mode, [(root, quality, bass), ...]), ...]"""
        if not segments or any(s[0] is None for s in segments):
            self.dropped += 1
            return
        self.pieces += 1
        prev = None
        for tonic, mode, chords in segments:
            for root, q, bass in chords:
                deg, chrom = degree_of(root, tonic, mode)
                if chrom:
                    self.chromatic += 1
                self.events += 1
                self.qual[(deg, quality_bucket(q))] += 1
                if bass is None:
                    bass = root  # Harte default: absent bass = root position
                if bass >= 0:
                    inv = inversion_bucket(root, q, bass)
                    if inv is not None:
                        self.inv[(deg, inv)] += 1
                if prev is not None:
                    self.transitions += 1
                    self.trans[(prev, deg)] += 1
                prev = deg
        t, m, _ = segments[0]
        self.keys[(t, m)] += 1

    def finish(self):
        return {
            "genre": self.genre,
            "pieces": self.pieces,
            "dropped": self.dropped,
            "events": self.events,
            "transitions": self.transitions,
            "chromatic_rate": round(self.chromatic / self.events, 4) if self.events else 0,
            "trans": {f"{a}->{b}": v for (a, b), v in self.trans.items()},
            "qual": {f"{a}->{b}": v for (a, b), v in self.qual.items()},
            "inv": {f"{a}->{b}": v for (a, b), v in self.inv.items()},
            "keys": {f"{a}:{b}": v for (a, b), v in self.keys.items()},
            "unparsed": dict(self.unparsed.most_common(20)),
        }


# --------------------------------------------------------- LAB parsing ------

def read_lab(path):
    rows = []
    for line in path.read_text(errors="replace").splitlines():
        f = line.split()  # LAB files are tab- or space-separated
        if len(f) < 3:
            continue
        try:
            rows.append((float(f[0]), float(f[1]), f[2], f[3:]))
        except ValueError:
            continue
    return rows


def parse_key_text(txt):
    """'A' -> (A,maj); 'A:minor' -> (A,min); 'G:mixolydian' -> (G,maj)."""
    if not txt or txt == "N":
        return None
    if ":" in txt:
        tonic_s, mode_s = txt.split(":", 1)
        mode_s = mode_s.lower().strip()
        if mode_s in ("minor", "min", "aeolian", "dorian", "phrygian"):
            mode = "min"
        elif mode_s in ("major", "maj", "ionian", "mixolydian", "lydian"):
            mode = "maj"
        else:
            return None
    else:
        tonic_s, mode = txt, "maj"
    t = note_pc(tonic_s)
    return (t, mode) if t is not None else None


def lab_segments(chords_path, key_path):
    chord_rows = []
    for s, e, label, rest in read_lab(chords_path):
        p = parse_chord(label)
        if p is not None:
            chord_rows.append((s, e, p))
    if not chord_rows:
        return None
    key_segs = []
    if key_path and key_path.exists():
        for s, e, label, rest in read_lab(key_path):
            if label == "Key":
                pk = parse_key_text(" ".join(rest).strip())
                if pk:
                    key_segs.append((s, e, pk))
    if not key_segs:
        chroma = [0.0] * 12
        for s, e, p in chord_rows:
            chroma[p[0]] += max(e - s, 0.01)
        k = ks_or_none(chroma)
        if k is None:
            return None
        return [(k[0], k[1], [p for _, _, p in chord_rows])]
    segments, cur, buf = [], None, []
    for s, e, p in chord_rows:
        mid = (s + e) / 2
        owner = None
        for ks, ke, pk in key_segs:
            if ks <= mid <= ke:
                owner = pk
                break
        if owner is None:
            owner = key_segs[-1][2] if mid > key_segs[-1][0] else key_segs[0][2]
        if owner != cur:
            if buf and cur:
                segments.append((cur[0], cur[1], buf))
            cur, buf = owner, []
        buf.append(p)
    if buf and cur:
        segments.append((cur[0], cur[1], buf))
    return segments or None


# ------------------------------------------------------- source loaders -----

def load_billboard(acc):
    lab_dir = CACHE / "billboard/lab/McGill-Billboard"
    for p in sorted(lab_dir.glob("*/full.lab")):
        segs = lab_segments(p, None)
        if segs is None:
            acc.dropped += 1
        else:
            acc.add_piece(segs)


def load_isophonics(acc, sub, genre_of_artist=None):
    root = CACHE / "isophonics" / sub
    for chordlab in root.rglob("chordlab"):
        for p in sorted(chordlab.rglob("*.lab")):
            rel = p.relative_to(chordlab)
            kl = root / "keylab" / rel
            segs = lab_segments(p, kl if kl.exists() else None)
            if segs is None:
                acc.dropped += 1
            else:
                acc.add_piece(segs)


def parse_wjd_key(keytxt):
    """'Bb-maj' / 'C-min' / 'D-dor' -> (tonic, mode) or None."""
    if not keytxt or "-" not in keytxt:
        return None
    t_s, m_s = keytxt.rsplit("-", 1)
    mode = {"maj": "maj", "min": "min", "dor": "min", "mix": "maj",
            "aeolian": "min", "ionian": "maj"}.get(m_s.lower())
    tonic = note_pc(t_s)
    if mode is None or tonic is None:
        return None
    return (tonic, mode)


def load_wjazzd(acc):
    db = CACHE / "weimar/wjazzd.db"
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    solos = dict(con.execute("select melid, key from solo_info"))
    rows = defaultdict(list)
    for melid, chord in con.execute(
            "select melid, chord from beats order by melid, beatid"):
        rows[melid].append(chord or "")
    for melid, chords_raw in rows.items():
        # carry forward, dedupe
        seq, last = [], ""
        for c in chords_raw:
            if c:
                last = c
            if last and (not seq or seq[-1] != last):
                seq.append(last)
        parsed = []
        for c in seq:
            if is_nochord(c):
                continue
            pc = parse_chord(c)
            if pc is None:
                acc.unparsed[c] += 1
            else:
                parsed.append(pc)
        if not parsed:
            acc.dropped += 1
            continue
        key = parse_wjd_key(solos.get(melid, ""))
        if key is None:
            chroma = [0.0] * 12
            for r, q, b in parsed:
                chroma[r] += 1.0
            key = ks_or_none(chroma)
            if key is None:
                acc.dropped += 1
                continue
        acc.add_piece([(key[0], key[1], parsed)])


KEYTOK = re.compile(r"^([A-G])([#b]?)(m|min)?$")


def load_nottingham(acc):
    root = CACHE / "nottingham/repo/nottingham-dataset-master/ABC_original"
    for p in sorted(root.glob("*.abc")):
        text = p.read_text(errors="replace")
        tunes = re.split(r"(?m)^X:\s*\d+\s*$", text)[1:]
        for tune in tunes:
            k = re.search(r"(?m)^K:\s*(\S+)", tune)
            if not k:
                continue
            m = KEYTOK.match(k.group(1))
            if not m:
                continue
            cur = (note_pc(m.group(1) + (m.group(2) or "")),
                   "min" if m.group(3) else "maj")
            chords = []
            for mm in re.finditer(r'"([^"]*)"', tune):
                sym = mm.group(1)
                inline = re.match(r"^\[K:([A-G][#b]?(?:m|min)?)\]", sym)
                if inline:
                    m2 = KEYTOK.match(inline.group(1))
                    if m2:
                        cur = (note_pc(m2.group(1) + (m2.group(2) or "")),
                               "min" if m2.group(3) else "maj")
                    continue
                if is_nochord(sym) or not sym.strip():
                    continue
                pc = parse_chord(sym)
                if pc is None:
                    acc.unparsed[sym] += 1
                    continue
                chords.append((cur[0], cur[1], pc))
            if len(chords) < 3:
                acc.dropped += 1
                continue
            segments, cur_key, buf = [], None, []
            for t2, m2, pc in chords:
                if (t2, m2) != cur_key:
                    if buf:
                        segments.append((cur_key[0], cur_key[1], buf))
                    cur_key, buf = (t2, m2), []
                buf.append(pc)
            if buf:
                segments.append((cur_key[0], cur_key[1], buf))
            acc.add_piece(segments)


# ------------------------------------------------------------ TAVERN --------

ROMAN = {"I": 1, "II": 2, "III": 3, "IV": 4, "V": 5, "VI": 6, "VII": 7}
KEYREC = re.compile(r"^\*([A-Ga-g][#b-]*):")
DURPREFIX = re.compile(r"^\d+(\.)?(\.)?")


def rn_to_chord(tok, tonic, mode):
    """TAVERN **chords token -> (root_pc, quality, bass_pc)."""
    t = tok.strip().strip("()").strip()
    if not t or t in (".", "*", "x", "X", "=", "-") or t.startswith("="):
        return None
    t = DURPREFIX.sub("", t).strip("()").strip()
    if not t or t in ("r", "=") or t.startswith("="):
        return None

    keyscale = scale_of(mode)

    def build(value, acc, qual, digits):
        inv = {"6": 1, "64": 2, "7": 0, "65": 1, "43": 2,
               "42": 3, "2": 3}.get(digits, 0)
        has7 = "7" in digits
        if has7 and qual in ("maj", "min"):
            qual += "7"
        root = (tonic + keyscale[value - 1] + acc) % 12
        bass = None
        if inv == 1:
            bass = (root + (3 if "min" in qual else 4)) % 12
        elif inv == 2:
            bass = (root + 7) % 12
        elif inv == 3 and has7:
            bass = (root + 10) % 12
        return (root, qual, bass)

    mC = re.match(r"^C(\d*)(?:/.*)?$", t)  # cadential six-four 'C64(/V)'
    if mC:
        return build(1, 0, "maj", mC.group(1))
    m6 = re.match(r"^(Gr|Fr|It)\+?(\d+)?", t)  # augmented sixths (built on bVI)
    if m6:
        return build(6, -1, "maj", m6.group(2) or "65")
    if t.startswith("N") and re.match(r"^N\d*$", t):  # Neapolitan (bII)
        return build(2, -1, "maj", t[1:])

    if t in ("T", "D", "P"):  # function-spine token leak
        return None
    if re.match(r"^[A-G][#b]?(?:maj|min|dim|aug|sus|m|\d|\+|\()", t):
        return parse_chord(t)  # absolute chord name written in the chords spine
    secondary = None
    if "/" in t:
        t, secondary = t.split("/", 1)
    acc = 0
    while t and t[0] in "b#_":
        acc += -1 if t[0] in "b_" else 1
        t = t[1:]
    m = re.match(r"^([ivIV]+)(.*)$", t)
    if not m:
        return None
    letters, rest = m.group(1), m.group(2)
    upper = letters.upper()
    if upper not in ROMAN:
        return None
    value = ROMAN[upper]
    minor = letters.islower()
    digits = ""
    dm = re.search(r"(\d+)$", rest)
    if dm:
        digits = dm.group(1)
        rest = rest[: dm.start()]
    if "dim" in rest or "ø" in rest:
        qual = "dim"
    elif "+" in rest:
        qual = "aug"
    elif minor:
        qual = "min"
    else:
        qual = "maj"
    if secondary:
        s_acc = 0
        while secondary and secondary[0] in "b#_":
            s_acc += -1 if secondary[0] in "b_" else 1
            secondary = secondary[1:]
        sm = re.match(r"^([ivIV]+)", secondary)
        if sm:
            sval = ROMAN[sm.group(1).upper()]
            target_root = (tonic + keyscale[sval - 1] + s_acc) % 12
            # secondary dominant: root a fifth above its target
            root = (target_root + 7) % 12
            qual7 = "7" if "7" in digits else "maj"
            bass = None
            if digits in ("6", "65"):
                bass = (root + 4) % 12
            elif digits == "64":
                bass = (root + 7) % 12
            return (root, qual7, bass)
    return build(value, acc, qual, digits)


def _first_key_record(path):
    """First '*X:' key record in a TAVERN .krn file -> (tonic, mode) or None."""
    for line in path.read_text(errors="replace").splitlines():
        if not line.startswith("*") or line.startswith("**"):
            continue
        mk = KEYREC.match(line)
        if mk:
            tok = mk.group(1)
            is_min = tok[0].islower()
            t = note_pc(tok[0].upper() + tok[1:].replace("-", "b"))
            if t is not None:
                return (t, "min" if is_min else "maj")
    return None


def _parse_enc(path):
    """-> (keyseq, chords, nlines).

    keyseq: [(line_idx, tonic, mode)]   key records with their line index
    chords: [(line_idx, token)]         **chords spine tokens with line index
    nlines: total lines in file (for proportional alignment)
    """
    lines = path.read_text(errors="replace").splitlines()
    ci = None
    keyseq, chords = [], []
    for i, line in enumerate(lines):
        if line.startswith(("!!!", "!")):
            continue
        if line.startswith("**"):
            spines = line.split("\t")
            ci = spines.index("**chords") if "**chords" in spines else None
            continue
        if ci is None:
            continue
        if line.startswith("*"):
            mk = KEYREC.match(line)
            if mk:
                tok = mk.group(1)
                is_min = tok[0].islower()
                t = note_pc(tok[0].upper() + tok[1:].replace("-", "b"))
                if t is not None:
                    keyseq.append((i, t, "min" if is_min else "maj"))
            continue
        if line.startswith("="):
            continue
        parts = line.split("\t")
        if ci >= len(parts):
            continue
        tok = parts[ci].strip()
        if tok:
            chords.append((i, tok))
    return keyseq, chords, len(lines)


def load_tavern(acc):
    root = CACHE / "tavern/repo"
    # group encodings by canonical phrase stem (encoders A/B/C of the same phrase)
    stems = {}
    for p in sorted(root.rglob("Encodings/*/*.krn")):
        canon = re.sub(r"_encoder[A-C]$", "", p.stem)
        stems.setdefault(str(p.parent.parent / canon), []).append(p)
    for canon, paths in sorted(stems.items()):
        parsed = {p: _parse_enc(p) for p in paths}
        # chord source: prefer Encoder_A, else any file that has a **chords spine
        chord_src = next((p for p in sorted(paths)
                          if p.name.endswith("encoderA.krn") and parsed[p][1]),
                         None) or next((p for p in sorted(paths) if parsed[p][1]), None)
        if chord_src is None:
            acc.dropped += 1
            continue
        # key source: prefer Encoder_A's key sequence, else any sibling's
        key_src = next((p for p in sorted(paths)
                        if p.name.endswith("encoderA.krn") and parsed[p][0]),
                       None) or next((p for p in sorted(paths) if parsed[p][0]), None)
        if key_src is None:
            acc.dropped += 1
            continue
        keyseq, chords, nlines = parsed[key_src]
        cseq, _, clen = parsed[chord_src]
        same = key_src is chord_src
        if not chords:
            acc.dropped += 1
            continue

        def key_at(idx):
            if same:
                k = (keyseq[0][1], keyseq[0][2])
                for li, t, m in keyseq:
                    if li <= idx:
                        k = (t, m)
                    else:
                        break
                return k
            pos = idx / max(clen - 1, 1)
            k = (keyseq[0][1], keyseq[0][2])
            for li, t, m in keyseq:
                if li / max(nlines - 1, 1) <= pos:
                    k = (t, m)
            return k

        segments, buf, seg_key = [], [], None
        for idx, tok in chords:
            if tok == "." or tok in ("*", "x", "X") or is_nochord(tok):
                continue
            if re.fullmatch(r"\d+(\.)?r|\d+\.+|\d+|\d+(\.)?[TDP]", tok):
                continue  # rests / holds / function-spine leaks
            k = key_at(idx)
            pc = rn_to_chord(tok, k[0], k[1])
            if pc is None:
                acc.unparsed[tok] += 1
                continue
            if seg_key is None:
                seg_key = k
            elif k != seg_key:
                segments.append((seg_key[0], seg_key[1], buf))
                buf, seg_key = [], k
            buf.append(pc)
        if buf and seg_key:
            segments.append((seg_key[0], seg_key[1], buf))
        if not segments:
            acc.dropped += 1
        else:
            acc.add_piece(segments)


# ------------------------------------------------------------- main --------

def main():
    stats = {}

    a = Acc("POP"); load_billboard(a); stats["POP_billboard"] = a.finish()
    a = Acc("POP"); load_isophonics(a, "caroleking")
    stats["POP_isophonics"] = a.finish()
    a = Acc("ROCK"); load_isophonics(a, "beatles"); load_isophonics(a, "queen")
    stats["ROCK_isophonics"] = a.finish()
    a = Acc("JAZZ"); load_wjazzd(a); stats["JAZZ_weimar"] = a.finish()
    a = Acc("CLASSICAL"); load_tavern(a); stats["CLASSICAL_tavern"] = a.finish()
    a = Acc("FOLK"); load_nottingham(a); stats["FOLK_nottingham"] = a.finish()

    OUT.write_text(json.dumps(stats, indent=1) + "\n")

    print(f"{'source':26} {'pieces':>7} {'drop':>5} {'events':>8} {'trans':>7} {'chrom%':>6}")
    for name, s in stats.items():
        print(f"{name:26} {s['pieces']:7d} {s['dropped']:5d} {s['events']:8d} "
              f"{s['transitions']:7d} {100 * s['chromatic_rate']:6.1f}")
    top = sorted(stats["POP_billboard"]["trans"].items(),
                 key=lambda kv: -kv[1])[:8]
    print("\nPOP top transitions:", ", ".join(f"{k}:{v}" for k, v in top))
    print(f"\n-> {OUT.relative_to(HERE.parent.parent)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
