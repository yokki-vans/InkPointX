#!/usr/bin/env python3
"""Validate InkPoint X locale completeness and Inter UI subset coverage."""

from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Set, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_i18n import parse_yaml_file  # noqa: E402


PRINTF_PATTERN = re.compile(
    r"%(?:[-+0 #]*\d*(?:\.\d+)?(?:hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcspn%])"
)
INTERVAL_BLOCK_PATTERN = re.compile(
    r"static const EpdUnicodeInterval \w+Intervals\[\] = \{(.*?)\n\};",
    re.DOTALL,
)
INTERVAL_PATTERN = re.compile(
    r"\{\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,"
)
LEAKED_TOKEN_PATTERN = re.compile(
    r"(?:__IPX(?:FMT|TERM)\d+__|\[\[IPX\d+\]\]|⟦IPX[^⟧]*⟧)"
)
METADATA_KEYS = ("_language_name", "_language_code", "_order")


def printf_tokens(value: str) -> List[str]:
    return PRINTF_PATTERN.findall(value)


def edge_whitespace(value: str) -> Tuple[str, str]:
    leading = value[: len(value) - len(value.lstrip())]
    trailing = value[len(value.rstrip()) :]
    return leading, trailing


def font_intervals(path: Path) -> List[Tuple[int, int]]:
    source = path.read_text(encoding="utf-8")
    block = INTERVAL_BLOCK_PATTERN.search(source)
    if not block:
        raise ValueError(f"{path}: no EpdUnicodeInterval table")
    return [
        (int(start, 0), int(end, 0))
        for start, end in INTERVAL_PATTERN.findall(block.group(1))
    ]


def is_covered(codepoint: int, intervals: Sequence[Tuple[int, int]]) -> bool:
    return any(start <= codepoint <= end for start, end in intervals)


def is_rtl_script_codepoint(codepoint: int) -> bool:
    return (
        0x0590 <= codepoint <= 0x08FF
        or 0xFB1D <= codepoint <= 0xFDFF
        or 0xFE70 <= codepoint <= 0xFEFF
    )


def is_korean_codepoint(codepoint: int) -> bool:
    return (
        0x1100 <= codepoint <= 0x11FF
        or 0x3130 <= codepoint <= 0x318F
        or 0xA960 <= codepoint <= 0xA97F
        or 0xAC00 <= codepoint <= 0xD7AF
        or 0xD7B0 <= codepoint <= 0xD7FF
    )


def display_codepoints(codepoints: Iterable[int]) -> str:
    values = []
    for codepoint in sorted(codepoints):
        character = chr(codepoint)
        name = unicodedata.name(character, "UNNAMED")
        values.append(f"U+{codepoint:04X} {character!r} ({name})")
    return ", ".join(values)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--translations-dir", default="lib/I18n/translations", type=Path
    )
    parser.add_argument(
        "--fonts-dir", default="lib/EpdFont/builtinFonts", type=Path
    )
    parser.add_argument(
        "--arabic-shaper", default="lib/MiniBidi/ArabicShaper.cpp", type=Path
    )
    args = parser.parse_args()

    locale_paths = sorted(args.translations_dir.glob("*.yaml"))
    locales: Dict[str, Tuple[Path, Dict[str, str]]] = {}
    english_path: Path | None = None
    english: Dict[str, str] | None = None
    errors: List[str] = []

    for path in locale_paths:
        locale = parse_yaml_file(str(path))
        code = locale.get("_language_code", "").upper()
        if not code:
            errors.append(f"{path}: missing _language_code")
            continue
        if code in locales:
            errors.append(f"duplicate locale code {code}: {locales[code][0]}, {path}")
            continue
        locales[code] = (path, locale)
        if code == "EN":
            english_path = path
            english = locale

    if english is None or english_path is None:
        errors.append("English reference locale is missing")
        print("\n".join(errors), file=sys.stderr)
        return 1

    english_keys = [key for key in english if key.startswith("STR_")]
    english_key_set = set(english_keys)
    all_text = set(english.get("_language_name", ""))

    for code, (path, locale) in sorted(locales.items()):
        locale_keys = [key for key in locale if key.startswith("STR_")]
        locale_key_set = set(locale_keys)
        missing = english_key_set - locale_key_set
        extra = locale_key_set - english_key_set
        if missing:
            errors.append(f"{code}: missing keys: {', '.join(sorted(missing))}")
        if extra:
            errors.append(f"{code}: extra keys: {', '.join(sorted(extra))}")
        if locale_keys != english_keys:
            errors.append(f"{code}: key order differs from {english_path.name}")
        for metadata_key in METADATA_KEYS:
            if not locale.get(metadata_key, ""):
                errors.append(f"{code}: missing {metadata_key}")

        all_text.update(locale.get("_language_name", ""))
        for key in english_keys:
            source = english[key]
            value = locale.get(key, "")
            if not value.strip():
                errors.append(f"{code}:{key}: empty translation")
                continue
            source_printf = printf_tokens(source)
            if source_printf and printf_tokens(value) != source_printf:
                errors.append(
                    f"{code}:{key}: printf tokens {printf_tokens(value)!r} "
                    f"!= {printf_tokens(source)!r}"
                )
            if edge_whitespace(value) != edge_whitespace(source):
                errors.append(f"{code}:{key}: leading/trailing whitespace changed")
            if LEAKED_TOKEN_PATTERN.search(value):
                errors.append(f"{code}:{key}: leaked translation protection token")
            for character in value:
                category = unicodedata.category(character)
                if category == "Cc" and character not in "\n\r\t":
                    errors.append(
                        f"{code}:{key}: unsupported control character "
                        f"U+{ord(character):04X}"
                    )
            all_text.update(value)

    font_paths = sorted(
        path for path in args.fonts_dir.glob("ui_*.h")
        if "wordmark" not in path.name
    )
    if not font_paths:
        errors.append("no generated Inter UI subsets found")

    ignored_codepoints = {0x0A, 0x0D}
    required_codepoints: Set[int] = {
        ord(character)
        for character in all_text
        if ord(character) not in ignored_codepoints
    }
    # Dynamic Arabic names are shaped at runtime to presentation forms which
    # do not occur literally in translation YAML. Validate both the core
    # logical alphabet and every standard contextual form emitted by the
    # shaper so a static locale check cannot pass while dynamic titles fail.
    required_codepoints.update(range(0x0621, 0x063B))
    required_codepoints.update(range(0x0640, 0x0653))
    required_codepoints.update((0x060C, 0x061B, 0x061F))
    required_codepoints.update(range(0x0660, 0x066A))
    if args.arabic_shaper.exists():
        shaper_source = args.arabic_shaper.read_text(encoding="utf-8")
        required_codepoints.update(
            int(value, 16)
            for value in re.findall(r"0x([Ff][BbEeFf][0-9A-Fa-f]{2})", shaper_source)
        )
    else:
        errors.append(f"Arabic shaper source missing: {args.arabic_shaper}")
    for path in font_paths:
        intervals = font_intervals(path)
        # ui_script_16 is used only for the Home author line. Caveat has no RTL
        # face, so Home deliberately routes Hebrew/Arabic authors through the
        # existing UI_12 font instead of embedding a second ~0.5 MB Noto copy.
        # Its remaining locale coverage must still be complete.
        font_required_codepoints = required_codepoints
        # Korean uses the Medium face for both regular and emphasized text and
        # routes the two accent slots to the 16 px Medium family at runtime.
        # Keeping a second identical Hangul raster in every SemiBold/Caveat
        # face would exceed the ESP32-C3 OTA partition without changing the
        # visible Korean typography.
        if "semibold" in path.name or "script" in path.name:
            font_required_codepoints = {
                codepoint for codepoint in font_required_codepoints
                if not is_korean_codepoint(codepoint)
            }
        if path.name == "ui_script_16.h":
            font_required_codepoints = {
                codepoint for codepoint in font_required_codepoints
                if not is_rtl_script_codepoint(codepoint)
            }
        missing_codepoints = {
            codepoint
            for codepoint in font_required_codepoints
            if not is_covered(codepoint, intervals)
        }
        if missing_codepoints:
            errors.append(
                f"{path.name}: missing locale glyphs: "
                f"{display_codepoints(missing_codepoints)}"
            )

    # The boot wordmark renders in the shared UI_12 face now (uppercase,
    # letterspaced); the dedicated 36 px wordmark font was removed. Verify the
    # glyphs it needs are covered by the UI subsets like any other string.
    for character in "INKPOINT X":
        codepoint = ord(character)
        for path in font_paths:
            if not is_covered(codepoint, font_intervals(path)):
                errors.append(f"{path.name}: missing wordmark glyph U+{codepoint:04X}")

    print(
        f"Locales: {len(locales)}; strings per locale: {len(english_keys)}; "
        f"UI codepoints: {len(required_codepoints)}; fonts checked: {len(font_paths)}"
    )
    if errors:
        print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
        return 1
    print("i18n validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
