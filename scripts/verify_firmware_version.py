#!/usr/bin/env python3
"""Verify the ESP application descriptor embedded in a firmware image."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


APP_DESC_MAGIC = 0xABCD5432
VERSION_OFFSET = 16
VERSION_SIZE = 32
PROJECT_SIZE = 32


def read_c_string(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("expected_version")
    parser.add_argument("expected_project", nargs="?", default="InkPointX")
    args = parser.parse_args()

    image = args.image.read_bytes()
    magic = struct.pack("<I", APP_DESC_MAGIC)
    offsets = [offset for offset in range(len(image)) if image.startswith(magic, offset)]
    if len(offsets) != 1:
        raise SystemExit(f"expected one app descriptor, found {len(offsets)} at {offsets}")

    offset = offsets[0] + VERSION_OFFSET
    version = read_c_string(image[offset : offset + VERSION_SIZE])
    project_offset = offset + VERSION_SIZE
    project = read_c_string(image[project_offset : project_offset + PROJECT_SIZE])

    if version != args.expected_version:
        raise SystemExit(f"firmware version is {version!r}, expected {args.expected_version!r}")
    if project != args.expected_project:
        raise SystemExit(f"firmware project is {project!r}, expected {args.expected_project!r}")

    print(f"firmware descriptor: project={project} version={version} offset={offsets[0]}")


if __name__ == "__main__":
    main()
