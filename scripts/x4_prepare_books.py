#!/usr/bin/env python3
"""Prepare books for CrossPoint on an XTEINK X4.

This X4 build reads EPUB, FB2, Markdown, TXT, BMP, XTC and XTCH directly. Other formats are
converted to a conservative EPUB 2 profile with calibre's ``ebook-convert``.
Prepared files can optionally be uploaded to the reader's HTTP file server.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path
from urllib.parse import quote

NATIVE_EXTENSIONS = {".epub", ".fb2", ".md", ".txt", ".bmp", ".xtc", ".xtch"}
CALIBRE_LOCATIONS = (
    "/opt/homebrew/bin/ebook-convert",
    "/usr/local/bin/ebook-convert",
    "/Applications/calibre.app/Contents/MacOS/ebook-convert",
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Copy CrossPoint-native books and convert other e-book formats "
            "to XTEINK X4-friendly EPUB 2 files."
        )
    )
    parser.add_argument("inputs", nargs="+", type=Path, help="Book files or directories")
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        default=Path("converted"),
        help="Destination directory (default: ./converted)",
    )
    parser.add_argument(
        "--normalize-epub",
        action="store_true",
        help="Rebuild existing EPUB files with the X4 conversion profile",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite existing prepared files",
    )
    parser.add_argument(
        "--upload-url",
        metavar="URL",
        help="Upload prepared books while CrossPoint File Transfer is open, e.g. http://crosspoint.local",
    )
    parser.add_argument(
        "--remote-path",
        default="/Books",
        help="Reader destination directory used with --upload-url (default: /Books)",
    )
    parser.add_argument(
        "--calibre-arg",
        action="append",
        default=[],
        help="Extra ebook-convert argument; may be repeated",
    )
    return parser


def find_ebook_convert() -> str | None:
    on_path = shutil.which("ebook-convert")
    if on_path:
        return on_path
    for candidate in CALIBRE_LOCATIONS:
        if Path(candidate).is_file():
            return candidate
    return None


def collect_files(inputs: list[Path]) -> tuple[list[Path], list[str]]:
    files: list[Path] = []
    errors: list[str] = []
    for raw_path in inputs:
        path = raw_path.expanduser()
        if path.is_file():
            files.append(path)
        elif path.is_dir():
            files.extend(
                item
                for item in sorted(path.rglob("*"))
                if item.is_file() and not any(part.startswith(".") for part in item.relative_to(path).parts)
            )
        else:
            errors.append(f"Не найдено: {path}")
    return files, errors


def destination_for(source: Path, output_dir: Path, normalize_epub: bool) -> Path:
    suffix = source.suffix.lower()
    if suffix in NATIVE_EXTENSIONS and not (normalize_epub and suffix == ".epub"):
        return output_dir / source.name
    return output_dir / f"{source.stem}.epub"


def prepare_one(
    source: Path,
    destination: Path,
    converter: str | None,
    normalize_epub: bool,
    force: bool,
    extra_args: list[str],
) -> str:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and not force:
        raise FileExistsError(f"уже существует (добавьте --force): {destination}")

    suffix = source.suffix.lower()
    copy_native = suffix in NATIVE_EXTENSIONS and not (normalize_epub and suffix == ".epub")
    if copy_native:
        if source.resolve() == destination.resolve():
            return "оставлен на месте"
        shutil.copy2(source, destination)
        return "скопирован без конвертации"

    if converter is None:
        raise RuntimeError(
            "ebook-convert не найден; установите calibre командой "
            "`brew install --cask calibre`"
        )

    command = [
        converter,
        str(source),
        str(destination),
        "--output-profile",
        "generic_eink",
        "--epub-version",
        "2",
        "--flow-size",
        "100",
        "--no-svg-cover",
    ]
    if suffix == ".md":
        command.extend(("--formatting-type", "markdown"))
    command.extend(extra_args)
    subprocess.run(command, check=True)
    return "конвертирован в EPUB 2"


def upload(prepared: Path, base_url: str, remote_path: str) -> None:
    curl = shutil.which("curl")
    if curl is None:
        raise RuntimeError("curl не найден")
    endpoint = f"{base_url.rstrip('/')}/upload?path={quote(remote_path, safe='/')}"
    subprocess.run(
        [curl, "--fail", "--show-error", "--silent", "--form", f"file=@{prepared}", endpoint],
        check=True,
    )


def main() -> int:
    args = build_parser().parse_args()
    files, failures = collect_files(args.inputs)
    if not files:
        failures.append("Не найдено ни одного файла для обработки")

    output_dir = args.output_dir.expanduser().resolve()
    converter = find_ebook_convert()
    prepared_count = 0
    uploaded_count = 0

    for source in files:
        destination = destination_for(source, output_dir, args.normalize_epub)
        try:
            action = prepare_one(
                source,
                destination,
                converter,
                args.normalize_epub,
                args.force,
                args.calibre_arg,
            )
            prepared_count += 1
            print(f"✓ {source.name}: {action} → {destination}")
            if args.upload_url:
                upload(destination, args.upload_url, args.remote_path)
                uploaded_count += 1
                print(f"  загружен в {args.remote_path}")
        except (FileExistsError, OSError, RuntimeError, subprocess.CalledProcessError) as exc:
            failures.append(f"{source}: {exc}")

    print(f"\nГотово: {prepared_count}; загружено: {uploaded_count}; ошибок: {len(failures)}")
    for failure in failures:
        print(f"✗ {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
