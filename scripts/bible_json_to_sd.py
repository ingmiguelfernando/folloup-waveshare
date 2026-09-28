#!/usr/bin/env python3
"""Convert a YouVersion-style Bible JSON snapshot into the SD-card layout the
FolloUp Bible reader expects.

Runs anywhere with Python 3.8+ (stdlib only). The Bible text itself is never
stored in this repo; point the script at a file you are entitled to use.

Input: a JSON file shaped like
  {"local_abbreviation": "RVR1960", "local_title": ..., "copyright": {"text": ...},
   "books": [{"usfm": "GEN", "human": "Génesis",
              "chapters": [{"usfm": "GEN.1",
                            "items": [{"type": "heading1", "lines": [...]},
                                      {"type": "verse", "verse_numbers": [1],
                                       "lines": [...], "np": true}]}]}]}

Output (copy the whole `bible/` folder to the root of the SD card):
  bible/<abbr>/meta.txt       key=value lines: format, abbreviation, title, language, copyright
  bible/<abbr>/index.tsv      one line per book: usfm, name, chapter_count, file
  bible/<abbr>/<USFM>.txt     UTF-8, one record per line, tab-separated:
                                C  <chapter>
                                H  <heading text>
                                V  <verse label, e.g. 3 or 3-4>  <1 if new paragraph else 0>  <text>
  bible/<abbr>/<USFM>.idx     one line per chapter: chapter, byte offset of its C line, verse count
  bible/<abbr>/charset.txt    every non-ASCII character used, for font generation

Usage:
  python3 bible_json_to_sd.py RVR1960_vid_149.json --out ./sd
"""

import argparse
import json
import re
import sys
import unicodedata
from collections import Counter
from pathlib import Path

FORMAT_VERSION = "1"

# Typographic punctuation the e-paper fonts may lack; accents, ñ, ¿ and ¡ are kept.
PUNCTUATION_MAP = {
    "\u2018": "'",
    "\u2019": "'",
    "\u201c": '"',
    "\u201d": '"',
    "\u2013": "-",
    "\u2014": "-",
    "\u2026": "...",
    "\u00a0": " ",
    "\u2009": " ",
    "\u200b": "",
}

WHITESPACE_RE = re.compile(r"\s+")


def clean_text(text: str, keep_punctuation: bool) -> str:
    text = unicodedata.normalize("NFC", text)
    if not keep_punctuation:
        for source, target in PUNCTUATION_MAP.items():
            text = text.replace(source, target)
    return WHITESPACE_RE.sub(" ", text).strip()


def join_lines(item: dict, keep_punctuation: bool) -> str:
    lines = item.get("lines") or []
    return clean_text(" ".join(str(line) for line in lines), keep_punctuation)


def verse_label(numbers) -> str:
    numbers = sorted(int(n) for n in numbers)
    if not numbers:
        return ""
    if numbers[0] == numbers[-1]:
        return str(numbers[0])
    return f"{numbers[0]}-{numbers[-1]}"


def chapter_number(chapter: dict):
    usfm = str(chapter.get("usfm", ""))
    tail = usfm.rsplit(".", 1)[-1]
    return int(tail) if tail.isdigit() else None


def safe_field(text: str) -> str:
    return text.replace("\t", " ").replace("\r", " ").replace("\n", " ")


def convert_book(book: dict, out_dir: Path, keep_punctuation: bool, charset: Counter,
                 skipped_types: Counter):
    usfm = str(book["usfm"]).upper()
    name = clean_text(str(book.get("human") or usfm), keep_punctuation)
    text_path = out_dir / f"{usfm}.txt"
    idx_lines = []
    chapter_count = 0

    with open(text_path, "wb") as text_file:
        for chapter in book.get("chapters", []):
            number = chapter_number(chapter)
            if number is None:
                continue
            chapter_count += 1
            offset = text_file.tell()
            text_file.write(f"C\t{number}\n".encode("utf-8"))
            verse_count = 0
            for item in chapter.get("items", []):
                item_type = str(item.get("type", ""))
                text = join_lines(item, keep_punctuation)
                if not text:
                    continue
                if item_type.startswith("heading"):
                    record = f"H\t{safe_field(text)}\n"
                elif item_type == "verse":
                    label = verse_label(item.get("verse_numbers") or [])
                    if not label:
                        continue
                    paragraph = "1" if item.get("np") else "0"
                    record = f"V\t{label}\t{paragraph}\t{safe_field(text)}\n"
                    verse_count += 1
                else:
                    skipped_types[item_type] += 1
                    continue
                charset.update(ch for ch in text if ord(ch) > 0x7E)
                text_file.write(record.encode("utf-8"))
            idx_lines.append(f"{number}\t{offset}\t{verse_count}\n")

    (out_dir / f"{usfm}.idx").write_text("".join(idx_lines), encoding="utf-8")
    charset.update(ch for ch in name if ord(ch) > 0x7E)
    return usfm, name, chapter_count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("input", type=Path, help="Bible JSON file")
    parser.add_argument("--out", type=Path, default=Path("sd"),
                        help="output root; the bible/ folder is created inside (default: ./sd)")
    parser.add_argument("--abbr", help="folder/abbreviation override (default: from JSON)")
    parser.add_argument("--keep-punctuation", action="store_true",
                        help="keep curly quotes, dashes and ellipsis instead of ASCII equivalents")
    args = parser.parse_args()

    with open(args.input, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    abbr = (args.abbr or data.get("local_abbreviation") or args.input.stem).strip()
    folder = re.sub(r"[^a-z0-9_-]", "", abbr.lower()) or "bible"
    out_dir = args.out / "bible" / folder
    out_dir.mkdir(parents=True, exist_ok=True)

    charset: Counter = Counter()
    skipped_types: Counter = Counter()
    index_lines = []
    for book in data.get("books", []):
        if "usfm" not in book:
            continue
        usfm, name, chapters = convert_book(book, out_dir, args.keep_punctuation, charset,
                                            skipped_types)
        if chapters == 0:
            continue
        index_lines.append(f"{usfm}\t{safe_field(name)}\t{chapters}\t{usfm}.txt\n")

    (out_dir / "index.tsv").write_text("".join(index_lines), encoding="utf-8")

    copyright_text = ""
    if isinstance(data.get("copyright"), dict):
        copyright_text = clean_text(str(data["copyright"].get("text", "")), True)
    language = ""
    if isinstance(data.get("language"), dict):
        language = str(data["language"].get("iso_639_1", ""))
    meta = {
        "format": FORMAT_VERSION,
        "abbreviation": abbr,
        "title": clean_text(str(data.get("local_title", abbr)), args.keep_punctuation),
        "language": language,
        "copyright": copyright_text,
    }
    (out_dir / "meta.txt").write_text(
        "".join(f"{key}={safe_field(value)}\n" for key, value in meta.items()), encoding="utf-8")

    charset.update(ch for ch in meta["title"] + copyright_text if ord(ch) > 0x7E)
    used = "".join(sorted(charset))
    (out_dir / "charset.txt").write_text(used + "\n", encoding="utf-8")

    print(f"Wrote {len(index_lines)} books to {out_dir}")
    print(f"Non-ASCII characters ({len(charset)}): {used}")
    if skipped_types:
        print("Skipped item types:", dict(skipped_types))
    print(f"Copy '{args.out / 'bible'}' to the root of the SD card.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
