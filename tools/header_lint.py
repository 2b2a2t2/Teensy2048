#!/usr/bin/env python3
"""Check every OLED header message for the 128 px band.

The header strip is 128 px wide at the 6x12 font: 21 characters.  A
transient that does not fit is pinned to the left edge by Display.cpp so
its head stays readable, but the tail is still lost - so this script

  * FAILS on the messages it can measure exactly: string literals passed
    to headerShow()/headerAnnounce(), and
  * FAILS on the shorthand codes the strip must never use again ("PROB",
    "mot", "klock", "GNR8", ...) - the header names the parameter in full
    words, even where the 4-character main-screen column cannot, and
  * NOTES the snprintf-built messages whose worst case would overflow.
    Those depend on the runtime value ("Step 16 Transpose +12" is fine,
    "Sustain Common on" is not), so they are reported, not failed.

Usage:  tools/header_lint.py [repo_root]        (exit 1 = findings)
"""

import os
import re
import sys

SCREEN_W = 128
CHAR_W = 6
MAX_CHARS = (SCREEN_W - 2) // CHAR_W          # 21

# Shorthand that must not reach the header: whole uppercase words only, so
# "Probability" and "Motion" pass and "PROB" / "MOT" are findings.
BANNED = [
    "ACC", "PROB", "MOT", "VAR", "SHP", "TES", "THN", "EXT", "SUB", "BRW",
    "DENS", "KLOCK", "ENCOD", "KBOARD", "GNR8", "INCH", "LEN", "SRC",
    "CT", "GD", "PLC", "EMB", "ILS",
]
BANNED_RE = re.compile(r"\b(" + "|".join(BANNED) + r")\b")

MESSAGE_CALL = re.compile(r'headerShow\(\s*"((?:[^"\\]|\\.)*)"')
ANNOUNCE_CALL = re.compile(
    r'headerAnnounce\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"'
)
SNPRINTF_ANY = re.compile(
    r'snprintf\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*[^,]+,\s*'
    r'"((?:[^"\\]|\\.)*)"'
)
HEADER_IDENT = re.compile(r"headerShow\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")

SPEC = re.compile(r"%[-+ #0]*[0-9]*(?:\.[0-9]+)?[a-zA-Z]")

# How far above a headerShow() its snprintf may sit (the messages are
# always built a few lines before they are shown).
LOOKBACK = 8


def measure_format(fmt):
    """Worst-case character count of a printf format.

    Numbers get 3 characters, %s gets 7 (a drum voice or genre name), %%
    is one.  Generous on purpose: a note here means "look at this".
    """
    total = 0
    i = 0
    for m in SPEC.finditer(fmt):
        total += len(fmt[i:m.start()])
        spec = m.group(0)[-1]
        total += 1 if spec == "%" else (7 if spec == "s" else 3)
        i = m.end()
    total += len(fmt[i:])
    return total


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    files = sorted(f for f in os.listdir(root)
                   if f.endswith(".cpp") or f.endswith(".h"))
    fails, notes = [], []
    exact = 0
    checked_formats = 0

    for name in files:
        path = os.path.join(root, name)
        with open(path, encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()

        # snprintf buffer -> (lineno, format) for the look-back
        pending = {}
        for lineno, line in enumerate(lines, 1):
            if line.lstrip().startswith("//"):
                continue

            for m in SNPRINTF_ANY.finditer(line):
                pending[m.group(1)] = (lineno, m.group(2))

            for m in MESSAGE_CALL.finditer(line):
                text = m.group(1)
                if "%" in text:
                    continue
                exact += 1
                check(name, lineno, text, fails)

            for m in ANNOUNCE_CALL.finditer(line):
                text = (m.group(1) + " " + m.group(2)).strip()
                if "%" in text:
                    continue
                exact += 1
                check(name, lineno, text, fails)

            for m in HEADER_IDENT.finditer(line):
                buf = m.group(1)
                built = pending.get(buf)
                if not built or lineno - built[0] > LOOKBACK:
                    continue
                checked_formats += 1
                fmt = built[1]
                width = measure_format(fmt)
                if width > MAX_CHARS:
                    notes.append((name, built[0], "wide", f"{width} chars", fmt))
                hit = BANNED_RE.search(fmt)
                if hit:
                    fails.append((name, built[0], "jargon", hit.group(1), fmt))

    for name, lineno, kind, detail, text in fails:
        print(f"{name}:{lineno}: {kind}: {detail}: {text!r}")
    for name, lineno, kind, detail, text in notes:
        print(f"{name}:{lineno}: note {kind}: {detail}: {text!r}")
    print(f"\n{exact} literal header messages checked exactly, "
          f"{checked_formats} format-built messages checked, "
          f"{len(fails)} finding(s), {len(notes)} note(s)")
    return 1 if fails else 0


def check(name, lineno, text, fails):
    if len(text) > MAX_CHARS:
        fails.append((name, lineno, "wide", f"{len(text)} chars > {MAX_CHARS}", text))
    hit = BANNED_RE.search(text)
    if hit:
        fails.append((name, lineno, "jargon", hit.group(1), text))


if __name__ == "__main__":
    sys.exit(main())
