#!/usr/bin/env python3
"""Verify genre_data.h + GENRE_DATA.md: row sums, flags, C++ syntax.

Usage:
  python3 verify.py            # static checks + g++ syntax check
  python3 verify.py --arduino  # additionally run arduino-cli compile
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HEADER = ROOT / "genre_data.h"

errors = []


def fail(msg):
    errors.append(msg)
    print("FAIL:", msg)


def parse_header(text):
    tables = re.findall(r"\{ // (\w+)\n(.*?)\n  \},", text, re.S)
    out = {}
    for name, body in tables:
        arrays = re.findall(r"\{((?:[^{}]|\{[^{}]*\})*)\}", body)

        def nested(s):
            rows = re.findall(r"\{([^{}]*)\}", s)
            return [[int(x) for x in r.split(",")] for r in rows]

        mv = re.search(r"(\d+),\s*0x([0-9A-Fa-f]{2})", body)
        if mv is None or len(arrays) < 4:
            fail(f"{name}: unparseable table body")
            continue
        out[name] = {
            "trans": nested(arrays[0]),
            "qual": nested(arrays[1]),
            "inv": nested(arrays[2]),
            "rhythmW": [int(x) for x in arrays[3].split(",")],
            "microVar": int(mv.group(1)),
            "flags": int(mv.group(2), 16),
        }
    return out


def check_static():
    if not HEADER.exists():
        fail("genre_data.h missing")
        return None
    if not (ROOT / "GENRE_DATA.md").exists():
        fail("GENRE_DATA.md missing")
    text = HEADER.read_text()
    m = re.search(r"#define GENRE_COUNT (\d+)", text)
    n = int(m.group(1)) if m else 0
    import literature as LIT
    if n != len(LIT.GENRE_ORDER):
        fail(f"GENRE_COUNT = {n}, expected {len(LIT.GENRE_ORDER)}")
    tables = parse_header(text)
    if len(tables) != n:
        fail(f"parsed {len(tables)} genre tables, expected {n}")
    for g, d in tables.items():
        if len(d["trans"]) != 7 or any(len(r) != 7 for r in d["trans"]):
            fail(f"{g}: trans not 7x7")
            continue
        for i, row in enumerate(d["trans"]):
            if sum(row) != 100:
                fail(f"{g}: trans row {i} sums {sum(row)}, expected 100")
        for field in ("qual", "inv"):
            if len(d[field]) != 7 or any(len(r) != 4 for r in d[field]):
                fail(f"{g}: {field} not 7x4")
            for i, row in enumerate(d[field]):
                if sum(row) != 100:
                    fail(f"{g}: {field} row {i} sums {sum(row)}, expected 100")
        if sum(d["rhythmW"]) != 100:
            fail(f"{g}: rhythmW sums {sum(d['rhythmW'])}")
        if len(d["rhythmW"]) != 8:
            fail(f"{g}: rhythmW not 8 entries")
        if d["microVar"] > 255:
            fail(f"{g}: microVar {d['microVar']} > 255")
        if not 0 <= d["flags"] <= 0xFF:
            fail(f"{g}: flags 0x{d['flags']:02X} out of range")
    return tables


def check_flags(tables):
    import literature as LIT
    for g in LIT.ESTIMATED:
        if not (tables[g]["flags"] & 0x01):
            fail(f"{g}: estimated genre missing ESTIMATED flag")
    for g in LIT.EMPIRICAL:
        if tables[g]["flags"] & 0x01:
            fail(f"{g}: empirical genre flagged estimated")
    for g in LIT.GENRE_ORDER:
        if sum(tables[g]["rhythmW"]) != sum(LIT.RHYTHM_W[g]):
            fail(f"{g}: rhythmW differs from literature.py")


def check_cpp():
    gxx = shutil.which("g++") or shutil.which("clang++")
    if not gxx:
        print("skip: no C++ compiler for syntax check")
        return
    r = subprocess.run([gxx, "-fsyntax-only", "-x", "c++", str(HEADER)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        fail("g++ -fsyntax-only failed:\n" + r.stderr)
    else:
        print("g++ syntax OK")


def check_arduino():
    if not shutil.which("arduino-cli"):
        print("skip: arduino-cli not on PATH")
        return
    r = subprocess.run(
        ["arduino-cli", "compile", "--fqbn", "teensy:avr:teensy41:usb=midi", "."],
        cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        fail("arduino-cli compile failed:\n" + (r.stderr or r.stdout)[-4000:])
    else:
        print("arduino-cli compile OK")


def main():
    tables = check_static()
    if tables:
        check_flags(tables)
        size = (49 * 3 + 8 + 2) * len(tables)
        print(f"{len(tables)} genres, ~{size} B table storage")
    check_cpp()
    if "--arduino" in sys.argv:
        check_arduino()
    if errors:
        print(f"{len(errors)} check(s) failed")
        sys.exit(1)
    print("verify.py OK")


if __name__ == "__main__":
    main()
