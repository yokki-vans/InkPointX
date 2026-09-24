#!/usr/bin/env python3
"""Fill missing InkPoint X translation strings without touching existing work.

The script uses Google or Bing translation in small batches, protects printf
placeholders and firmware terminology, validates every restored string, and
rewrites each locale in the canonical English key order.

Run without --apply for an audit. Network access is only used with --apply.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import http.cookiejar
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_i18n import parse_yaml_file  # noqa: E402


TRANSLATION_TARGETS = {
    "AR": "ar",
    "BE": "be",
    "CA": "ca",
    "CAV": "ca",  # Google has no separate Valencian target.
    "CS": "cs",
    "DA": "da",
    "DE": "de",
    "ES": "es",
    "FI": "fi",
    "FR": "fr",
    "HE": "he",
    "HU": "hu",
    "IT": "it",
    "KK": "kk",
    "KO": "ko",
    "LT": "lt",
    "NL": "nl",
    "PL": "pl",
    "PT": "pt",
    "RO": "ro",
    "RU": "ru",
    "SI": "sl",
    "SK": "sk",
    "SV": "sv",
    "TR": "tr",
    "UK": "uk",
    "VI": "vi",
}

PROTECTED_TERMS = (
    "KOReader",
    "InkPoint X",
    "XTEINK X4",
    "Noto Serif",
    "Noto Sans",
    "Ink Sans",
    "Inter",
    "Noto Naskh Arabic",
    "Calibre",
    "Wi-Fi",
    "firmware.bin",
    "OPDS",
    "EPUB",
    "FB2",
    "PDF",
    "XTC",
    "HTTP",
    "MAC",
    "UTC",
    "QR",
    "SD",
    "PWR",
    "OK",
    "Ubuntu",
    "Lyra Extended",
    "RoundedRaff",
    "Lyra",
)

PRINTF_PATTERN = re.compile(
    r"%(?:[-+0 #]*\d*(?:\.\d+)?(?:hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcspn%])"
)
MARKER_PATTERN = re.compile(r"\[\[IPX(\d{3})\]\]")
LEADING_SPACE_PATTERN = re.compile(r"^\s*")
TRAILING_SPACE_PATTERN = re.compile(r"\s*$")

_BING_OPENER = None
_BING_IG = ""
_BING_KEY = ""
_BING_TOKEN = ""
_BING_REQUEST_COUNT = 0


def yaml_quote(value: str) -> str:
    return (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
    )


def printf_tokens(value: str) -> List[str]:
    return PRINTF_PATTERN.findall(value)


def align_edge_whitespace(value: str, source: str) -> str:
    """Match source edge spacing used when the firmware concatenates labels."""
    leading = LEADING_SPACE_PATTERN.match(source).group(0)
    trailing = TRAILING_SPACE_PATTERN.search(source).group(0)
    return leading + value.strip() + trailing


def protect_text(value: str) -> Tuple[str, Dict[str, str], str, str]:
    leading = LEADING_SPACE_PATTERN.match(value).group(0)
    trailing = TRAILING_SPACE_PATTERN.search(value).group(0)
    end = len(value) - len(trailing) if trailing else len(value)
    protected = value[len(leading) : end]
    replacements: Dict[str, str] = {}

    def replace_printf(match: re.Match[str]) -> str:
        token = f"__IPXFMT{len(replacements)}__"
        replacements[token] = match.group(0)
        return token

    protected = PRINTF_PATTERN.sub(replace_printf, protected)

    for term in sorted(PROTECTED_TERMS, key=len, reverse=True):
        if term not in protected:
            continue
        token = f"__IPXTERM{len(replacements)}__"
        replacements[token] = term
        protected = protected.replace(term, token)

    return protected, replacements, leading, trailing


def restore_text(
    translated: str,
    replacements: Dict[str, str],
    leading: str,
    trailing: str,
    source: str,
) -> str:
    restored = translated.strip()
    for token, original in replacements.items():
        if token not in restored:
            raise ValueError(f"protected token {token} disappeared: {translated!r}")
        restored = restored.replace(token, original)
    restored = leading + restored + trailing
    expected_printf = printf_tokens(source)
    if expected_printf and printf_tokens(restored) != expected_printf:
        raise ValueError(
            f"printf placeholders changed: {source!r} -> {restored!r}"
        )
    if not restored.strip():
        raise ValueError(f"empty translation for {source!r}")
    return restored


def request_google_translation(text: str, target: str, retries: int = 6) -> str:
    url = "https://translate.googleapis.com/translate_a/single"
    payload = urllib.parse.urlencode(
        {"client": "gtx", "sl": "en", "tl": target, "dt": "t", "q": text}
    ).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=payload,
        headers={
            "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
            "User-Agent": "InkPointX-i18n/1.0",
        },
    )
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                data = json.loads(response.read().decode("utf-8"))
            return "".join(part[0] for part in data[0] if part and part[0])
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
            if attempt == retries - 1:
                raise RuntimeError(f"translation request failed: {exc}") from exc
            time.sleep(min(8.0, 0.5 * (2**attempt)))
    raise AssertionError("unreachable")


def refresh_bing_session() -> None:
    global _BING_OPENER, _BING_IG, _BING_KEY, _BING_TOKEN, _BING_REQUEST_COUNT
    cookie_jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(cookie_jar)
    )
    headers = {
        "User-Agent": (
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/537.36 Chrome/138.0 Safari/537.36"
        ),
        "Accept-Language": "en-US,en;q=0.9",
    }
    request = urllib.request.Request(
        "https://www.bing.com/translator",
        headers=headers,
    )
    with opener.open(request, timeout=30) as response:
        html = response.read().decode("utf-8", errors="replace")
    ig_match = re.search(r'IG:"([A-Z0-9]+)"', html)
    token_match = re.search(
        r'params_AbusePreventionHelper\s*=\s*\[(\d+),"([^"]+)",(\d+)\]',
        html,
    )
    if not ig_match or not token_match:
        raise RuntimeError("could not initialize Bing translation session")
    _BING_OPENER = opener
    _BING_IG = ig_match.group(1)
    _BING_KEY = token_match.group(1)
    _BING_TOKEN = token_match.group(2)
    _BING_REQUEST_COUNT = 0


def request_bing_translation(text: str, target: str, retries: int = 6) -> str:
    global _BING_REQUEST_COUNT
    for attempt in range(retries):
        try:
            if _BING_OPENER is None or _BING_REQUEST_COUNT >= 45:
                refresh_bing_session()
            url = (
                "https://www.bing.com/ttranslatev3"
                f"?isVertical=1&IG={_BING_IG}&IID=translator.5023.1"
            )
            payload = urllib.parse.urlencode(
                {
                    "fromLang": "en",
                    "to": target,
                    "text": text,
                    "token": _BING_TOKEN,
                    "key": _BING_KEY,
                    "tryFetchingGenderDebiasedTranslations": "true",
                }
            ).encode("utf-8")
            request = urllib.request.Request(
                url,
                data=payload,
                headers={
                    "Content-Type": "application/x-www-form-urlencoded",
                    "Origin": "https://www.bing.com",
                    "Referer": "https://www.bing.com/translator",
                    "User-Agent": (
                        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                        "AppleWebKit/537.36 Chrome/138.0 Safari/537.36"
                    ),
                    "Accept-Language": "en-US,en;q=0.9",
                },
            )
            with _BING_OPENER.open(request, timeout=30) as response:
                data = json.loads(response.read().decode("utf-8"))
            _BING_REQUEST_COUNT += 1
            if not isinstance(data, list) or not data:
                raise ValueError(f"unexpected Bing response: {data!r}")
            return data[0]["translations"][0]["text"]
        except (
            urllib.error.URLError,
            TimeoutError,
            json.JSONDecodeError,
            KeyError,
            IndexError,
            ValueError,
        ) as exc:
            refresh_bing_session()
            if attempt == retries - 1:
                raise RuntimeError(f"Bing translation request failed: {exc}") from exc
            time.sleep(min(8.0, 0.5 * (2**attempt)))
    raise AssertionError("unreachable")


def request_translation(text: str, target: str, provider: str) -> str:
    if provider == "bing":
        return request_bing_translation(text, target)
    return request_google_translation(text, target)


def split_batch_result(translated: str, count: int) -> List[str]:
    matches = list(MARKER_PATTERN.finditer(translated))
    if len(matches) != count:
        raise ValueError(
            f"expected {count} translation markers, received {len(matches)}"
        )
    values: List[str] = []
    for index, match in enumerate(matches):
        if int(match.group(1)) != index:
            raise ValueError(f"unexpected marker order in {translated!r}")
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(translated)
        value = translated[start:end].strip("\r\n ")
        values.append(value)
    return values


def translate_batch(
    sources: Sequence[str],
    target: str,
    provider: str,
) -> List[str]:
    protected_rows = [protect_text(source) for source in sources]
    if len(sources) == 1:
        protected, replacements, leading, trailing = protected_rows[0]
        translated = request_translation(protected, target, provider)
        return [
            restore_text(
                translated,
                replacements,
                leading,
                trailing,
                sources[0],
            )
        ]

    payload = "\n".join(
        f"[[IPX{index:03d}]] {row[0]}"
        for index, row in enumerate(protected_rows)
    )
    try:
        translated_rows = split_batch_result(
            request_translation(payload, target, provider), len(sources)
        )
        return [
            restore_text(translated, replacements, leading, trailing, source)
            for translated, source, (_, replacements, leading, trailing) in zip(
                translated_rows, sources, protected_rows
            )
        ]
    except ValueError:
        midpoint = len(sources) // 2
        return translate_batch(sources[:midpoint], target, provider) + translate_batch(
            sources[midpoint:], target, provider
        )


def chunks(values: Sequence[str], size: int) -> Iterable[Sequence[str]]:
    for offset in range(0, len(values), size):
        yield values[offset : offset + size]


def load_cache(path: Path) -> Dict[str, str]:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save_cache(path: Path, cache: Dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(
        json.dumps(cache, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def cache_key(target: str, source: str) -> str:
    return f"{target}\0{source}"


def translate_missing(
    missing_keys: Sequence[str],
    english: Dict[str, str],
    target: str,
    cache: Dict[str, str],
    cache_path: Path,
    batch_size: int,
    provider: str,
) -> Dict[str, str]:
    translated: Dict[str, str] = {}
    pending_sources: List[str] = []

    for key in missing_keys:
        source = english[key]
        cached = cache.get(cache_key(target, source))
        if cached is not None:
            translated[key] = cached
        elif source not in pending_sources:
            pending_sources.append(source)

    completed = 0
    for batch in chunks(pending_sources, batch_size):
        results = translate_batch(batch, target, provider)
        for source, result in zip(batch, results):
            cache[cache_key(target, source)] = result
        completed += len(batch)
        save_cache(cache_path, cache)
        print(
            f"    translated {completed}/{len(pending_sources)} unique strings",
            flush=True,
        )
        time.sleep(0.08)

    for key in missing_keys:
        translated[key] = cache[cache_key(target, english[key])]
    return translated


def write_locale(
    path: Path,
    locale: Dict[str, str],
    english: Dict[str, str],
    english_keys: Sequence[str],
) -> None:
    metadata = ("_language_name", "_language_code", "_order")
    lines = [f'{key}: "{yaml_quote(locale[key])}"' for key in metadata]
    lines.append("")
    lines.extend(
        f'{key}: "{yaml_quote(align_edge_whitespace(locale[key], english[key]))}"'
        for key in english_keys
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("--batch-size", type=int, default=20)
    parser.add_argument(
        "--provider",
        choices=("google", "bing"),
        default="google",
    )
    parser.add_argument(
        "--translations-dir",
        default="lib/I18n/translations",
    )
    parser.add_argument(
        "--cache",
        default="build/i18n-translation-cache.json",
    )
    args = parser.parse_args()

    translations_dir = Path(args.translations_dir)
    english = parse_yaml_file(str(translations_dir / "english.yaml"))
    english_keys = [key for key in english if key.startswith("STR_")]
    requested = {code.upper() for code in args.target}
    cache_path = Path(args.cache)
    cache = load_cache(cache_path)

    total_missing = 0
    locales: List[Tuple[Path, Dict[str, str], List[str], str]] = []
    for path in sorted(translations_dir.glob("*.yaml")):
        locale = parse_yaml_file(str(path))
        code = locale["_language_code"].upper()
        if code == "EN":
            continue
        if requested and code not in requested:
            continue
        if code not in TRANSLATION_TARGETS:
            raise ValueError(f"no translation target configured for {code}")
        missing = [key for key in english_keys if not locale.get(key, "").strip()]
        locales.append((path, locale, missing, code))
        total_missing += len(missing)
        print(f"{code:3} {path.name:20} missing {len(missing):3}")

    print(f"Total missing strings: {total_missing}")
    if not args.apply:
        return 0

    for path, locale, missing, code in locales:
        if missing:
            print(f"\n{code}: filling {len(missing)} strings")
            locale.update(
                translate_missing(
                    missing,
                    english,
                    TRANSLATION_TARGETS[code],
                    cache,
                    cache_path,
                    args.batch_size,
                    args.provider,
                )
            )
        write_locale(path, locale, english, english_keys)

    print("\nAll selected locales are complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
