#!/usr/bin/env python3
"""Download permissive sources into cache/ and record them in sources.json.

Run:  python3 tools/genre_data/fetch.py
Network required. Idempotent: skips files already present with matching size.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CACHE = HERE / "cache"
SOURCES_JSON = HERE / "sources.json"
UA = "Mozilla/5.0 (compatible; Teensy2048-genre-data/1.0)"

# name -> spec
DOWNLOADS = {
    "billboard_index": {
        "url": "https://www.dropbox.com/s/o0olz0uwl9z9stb/billboard-2.0-index.csv?dl=1",
        "dest": "billboard/billboard-2.0-index.csv",
        "genre": ["POP", "ROCK"],
        "license": "CC0-1.0",
        "license_url": "http://creativecommons.org/publicdomain/zero/1.0/",
        "note": "McGill Billboard Project index (song metadata, no keys).",
        "citation": "Aljanaki, Margoni, Muller. Studying Popular Music Composition with the McGill Billboard Dataset. ISMIR 2016.",
    },
    "billboard_lab": {
        "url": "https://www.dropbox.com/s/t390alzrkx0c9yt/billboard-2.0.1-lab.tar.gz?dl=1",
        "dest": "billboard/billboard-2.0.1-lab.tar.gz",
        "extract_to": "billboard/lab",
        "genre": ["POP", "ROCK"],
        "license": "CC0-1.0",
        "license_url": "http://creativecommons.org/publicdomain/zero/1.0/",
        "note": "890 songs, LAB chord annotations (Harte syntax, includes inversions and chord embellishments).",
        "citation": "Aljanaki, Margoni, Muller. Studying Popular Music Composition with the McGill Billboard Dataset. ISMIR 2016.",
    },
    "isophonics_beatles": {
        "url": "https://isophonics.net/files/annotations/The%20Beatles%20Annotations.tar.gz",
        "dest": "isophonics/beatles.tar.gz",
        "extract_to": "isophonics/beatles",
        "genre": ["ROCK"],
        "license": "no-formal-license (free research download)",
        "license_url": "https://isophonics.net/content/reference-annotations-beatles.html",
        "note": "Beatles reference annotations: chordlab + keylab + seglab LAB files (Harte). No formal license text; distributed openly for research. Only aggregate counts are redistributed.",
        "citation": "Mauch et al. A Correction to the Beatles Dataset. ISMIR 2011.",
    },
    "isophonics_queen": {
        "url": "https://isophonics.net/files/annotations/Queen%20Annotations.tar.gz",
        "dest": "isophonics/queen.tar.gz",
        "extract_to": "isophonics/queen",
        "genre": ["ROCK"],
        "license": "no-formal-license (free research download)",
        "license_url": "https://isophonics.net/content/reference-annotations-queen.html",
        "note": "Queen reference annotations: chordlab + keylab LAB files (Harte). Only aggregate counts are redistributed.",
        "citation": "Zanetti et al. A Fighter's Guide to Computational Music Analysis: Queen. ISMIR 2017.",
    },
    "isophonics_mj": {
        "url": "https://isophonics.net/files/annotations/Michael%20Jackson%20Annotations.tar.gz",
        "dest": "isophonics/mj.tar.gz",
        "extract_to": "isophonics/mj",
        "genre": ["POP"],
        "license": "no-formal-license (free research download)",
        "license_url": "https://isophonics.net/content/reference-annotations-michael-jackson.html",
        "note": "Michael Jackson reference annotations: chordlab + keylab LAB files (Harte). Only aggregate counts are redistributed.",
        "citation": "Mauch et al. The Michael Jackson pop corpus. ISMIR 2010.",
    },
    "isophonics_caroleking": {
        "url": "https://isophonics.net/files/annotations/Carole%20King%20Annotations.tar.gz",
        "dest": "isophonics/caroleking.tar.gz",
        "extract_to": "isophonics/caroleking",
        "genre": ["POP"],
        "license": "no-formal-license (free research download)",
        "license_url": "https://isophonics.net/content/reference-annotations-carole-king.html",
        "note": "Carole King reference annotations: chordlab + keylab LAB files (Harte). Only aggregate counts are redistributed.",
        "citation": "Mauch et al. A correction to the McGill Billboard dataset: Carole King. 2015.",
    },
    "wjazzd": {
        "url": "https://jazzomat.hfm-weimar.de/download/downloads/wjazzd.db",
        "dest": "weimar/wjazzd.db",
        "genre": ["JAZZ"],
        "license": "ODbL-1.0 (db_info table; contents under DbCL)",
        "license_url": "http://opendatacommons.org/licenses/odbl/1.0/",
        "note": "Weimar Jazz Database v2.1 SQLite: 456 jazz solos / 302 tunes, chord label per beat (beats.chord), composition-level tonality. ODbL share-alike honoured by attributing and only redistributing aggregate counts.",
        "citation": "Arndt, Pfleiderer, Muller. The Weimar Jazz Database. 2017.",
    },
    "nottingham": {
        "url": "https://github.com/jukedeck/nottingham-dataset/archive/refs/heads/master.zip",
        "dest": "nottingham/nottingham-dataset.zip",
        "extract_to": "nottingham/repo",
        "genre": ["FOLK"],
        "license": "GPL-3.0 (cleaning); original Nottingham Music Database is free/community",
        "license_url": "https://github.com/jukedeck/nottingham-dataset",
        "note": "1034 ABC lead sheets (melody + chords) with K: key fields. Chord notation normalised by JukeDeck. Used as read-only input; only aggregate counts are redistributed.",
        "citation": "Nottingham Music Database (abc.sourceforge.net/NMD/); cleaned by JukeDeck Ltd.",
    },
}

CLONES = {
    "tavern": {
        "url": "https://github.com/jcdevaney/TAVERN.git",
        "dest": "tavern/repo",
        "branch": None,
        "genre": ["CLASSICAL"],
        "license": "CC-BY-SA-4.0",
        "license_url": "https://github.com/jcdevaney/TAVERN/blob/master/LICENSE",
        "note": "27 Mozart/Beethoven theme-and-variation sets, 1060 phrases, Humdrum .krn Roman-numeral analyses (**chords spine + **function).",
        "citation": "Deevy, Devaney. TAVERN: Theme And Variation Encodings with Roman Numerals. 2020.",
    },
}

EXCLUDED = {
    "chordonomicon": {
        "license": "CC-BY-NC-4.0",
        "reason": "NonCommercial license; excluded by project decision.",
    },
    "casd": {
        "license": "CC-BY-NC-SA-4.0",
        "reason": "NonCommercial license.",
    },
    "dilemmadata": {
        "license": "CC-BY-NC-SA-4.0",
        "reason": "NonCommercial license (per its .zenodo.json).",
    },
    "dcmlab_corpora": {
        "license": "CC-BY-NC-SA-4.0",
        "reason": "NonCommercial license (DCML standard corpus license).",
    },
    "when_in_rome": {
        "license": "no license",
        "reason": "No repository license; it republishes third-party corpora of mixed licensing.",
    },
    "augmentednet": {
        "license": "MIT",
        "reason": "MIT, but checked-in data is only 199 MuseScore correction zips; its corpora are unlicensed/NC submodules. Dropped as data source (TAVERN covers classical).",
    },
}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(name: str, spec: dict) -> dict:
    dest = CACHE / spec["dest"]
    dest.parent.mkdir(parents=True, exist_ok=True)
    ok = dest.exists() and dest.stat().st_size > 0
    if not ok:
        print(f"[dl] {name}: {spec['url']}")
        # curl handles the system CA store; this python has no cert bundle.
        tmp = dest.with_suffix(dest.suffix + ".part")
        res = subprocess.run(
            ["curl", "-fsSL", "--retry", "3", "-A", UA, "-o", str(tmp), spec["url"]],
            capture_output=True, text=True,
        )
        if res.returncode != 0:
            tmp.unlink(missing_ok=True)
            raise RuntimeError(f"curl failed: {res.stderr[-300:]}")
        tmp.replace(dest)
        ok = True
    else:
        print(f"[ok] {name}: cached")
    entry = {
        "kind": "download",
        "url": spec["url"],
        "path": str(dest.relative_to(HERE)),
        "bytes": dest.stat().st_size,
        "sha256": sha256(dest),
        "genres": spec["genre"],
        "license": spec["license"],
        "license_url": spec["license_url"],
        "note": spec["note"],
        "citation": spec["citation"],
        "status": "ok",
    }
    if spec.get("extract_to") and ok:
        target = CACHE / spec["extract_to"]
        if not target.exists():
            extract(dest, target)
            entry["extracted_to"] = str(target.relative_to(HERE))
    return entry


def extract(archive: Path, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    print(f"[ex] {archive.name} -> {target.relative_to(HERE)}")
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(target)
    elif archive.name.endswith((".tar.gz", ".tgz")):
        with tarfile.open(archive) as t:
            t.extractall(target)
    else:
        print(f"  (no extractor for {archive.name})")


def clone(name: str, spec: dict) -> dict:
    dest = CACHE / spec["dest"]
    entry = {
        "kind": "clone",
        "url": spec["url"],
        "path": str(dest.relative_to(HERE)),
        "genres": spec["genre"],
        "license": spec["license"],
        "license_url": spec["license_url"],
        "note": spec["note"],
        "citation": spec["citation"],
    }
    if (dest / ".git").exists():
        print(f"[ok] {name}: cached clone")
    else:
        print(f"[cl] {name}: {spec['url']}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        cmd = ["git", "clone", "--depth", "1"]
        if spec.get("branch"):
            cmd += ["-b", spec["branch"]]
        cmd += [spec["url"], str(dest)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            entry["status"] = "failed"
            entry["error"] = res.stderr[-500:]
            return entry
    # drop vendored submodule dirs we are not licensed for
    raw = dest / "rawdata"
    if raw.is_dir():
        for sub in ("TAVERN", "When-in-Rome", "mozart_piano_sonatas", "haydn_op20_harm",
                    "functional-harmony-micchi", "key_modulation_dataset"):
            p = raw / sub
            if p.exists() and (p / ".git").exists():
                shutil.rmtree(p)
    files = sum(1 for _ in dest.rglob("*") if _.is_file())
    entry["files"] = files
    entry["status"] = "ok"
    return entry


def main() -> int:
    CACHE.mkdir(parents=True, exist_ok=True)
    sources = {"generated_by": "tools/genre_data/fetch.py", "included": {}, "excluded": EXCLUDED}
    rc = 0
    for name, spec in DOWNLOADS.items():
        try:
            sources["included"][name] = download(name, spec)
        except Exception as e:  # noqa: BLE001
            print(f"[ERR] {name}: {e}", file=sys.stderr)
            sources["included"][name] = {"status": "failed", "url": spec["url"], "error": str(e)}
            rc = 1
    for name, spec in CLONES.items():
        try:
            sources["included"][name] = clone(name, spec)
        except Exception as e:  # noqa: BLE001
            print(f"[ERR] {name}: {e}", file=sys.stderr)
            sources["included"][name] = {"status": "failed", "url": spec["url"], "error": str(e)}
            rc = 1
    SOURCES_JSON.write_text(json.dumps(sources, indent=2) + "\n")
    ok = sum(1 for v in sources["included"].values() if v.get("status") == "ok")
    print(f"\n{ok}/{len(sources['included'])} sources ready -> {SOURCES_JSON.relative_to(HERE.parent.parent)}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
