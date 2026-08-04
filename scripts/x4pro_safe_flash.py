#!/usr/bin/env python3
"""Fail-closed first-install tool for the XTEINK X4 Pro.

The tool never writes the bootloader, partition table, NVS, factory app1, or
coredump/SPIFFS partitions. A complete backup and a second independent ROM
session verification are mandatory before app0 can be written.
"""

from __future__ import annotations

import argparse
import binascii
import datetime as dt
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

FLASH_SIZE = 0x1000000
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x1000
NVS_OFFSET = 0x9000
NVS_SIZE = 0x5000
OTADATA_OFFSET = 0xE000
OTADATA_SIZE = 0x2000
OTA_SECTOR_SIZE = 0x1000
APP0_OFFSET = 0x10000
APP0_SIZE = 0x7E0000
APP1_OFFSET = 0x7F0000
APP1_SIZE = 0x7E0000
ESP32S3_CHIP_ID = 0x0009

EXPECTED_PARTITIONS = {
    "nvs": (0x01, 0x02, NVS_OFFSET, NVS_SIZE),
    "otadata": (0x01, 0x00, OTADATA_OFFSET, OTADATA_SIZE),
    "app0": (0x00, 0x10, APP0_OFFSET, APP0_SIZE),
    "app1": (0x00, 0x11, APP1_OFFSET, APP1_SIZE),
    "spiffs": (0x01, 0x82, 0xFD0000, 0x014000),
    "coredump": (0x01, 0x03, 0xFE4000, 0x01C000),
}


class SafetyError(RuntimeError):
    pass


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def esptool_path() -> str:
    tool = shutil.which("esptool") or shutil.which("esptool.py")
    if not tool:
        raise SafetyError("esptool 4.x/5.x is required and was not found in PATH")
    return tool


def run_esptool(port: str, baud: int, *args: str, capture: bool = False) -> str:
    command = [esptool_path(), "--chip", "esp32s3", "--port", port, "--baud", str(baud), *args]
    print("+", " ".join(command))
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.STDOUT if capture else None, check=False)
    if capture and result.stdout:
        print(result.stdout, end="")
    if result.returncode != 0:
        raise SafetyError(f"esptool failed with exit code {result.returncode}")
    return result.stdout or ""


def read_flash(port: str, baud: int, offset: int, size: int, output: Path) -> None:
    run_esptool(port, baud, "read-flash", hex(offset), hex(size), str(output))
    if not output.is_file() or output.stat().st_size != size:
        raise SafetyError(f"short flash read: expected {size} bytes at {offset:#x}")


def parse_partitions(table: bytes) -> dict[str, tuple[int, int, int, int]]:
    result: dict[str, tuple[int, int, int, int]] = {}
    for pos in range(0, len(table), 32):
        entry = table[pos:pos + 32]
        if len(entry) != 32:
            break
        magic, part_type, subtype, offset, size, raw_label, _flags = struct.unpack("<HBBII16sI", entry)
        if magic == 0xFFFF:
            break
        if magic != 0x50AA:
            continue
        label = raw_label.split(b"\0", 1)[0].decode("ascii", "strict")
        result[label] = (part_type, subtype, offset, size)
    return result


def require_oem_layout(image: bytes) -> None:
    actual = parse_partitions(image[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE])
    if actual != EXPECTED_PARTITIONS:
        raise SafetyError(f"partition table is not the confirmed X4 Pro OEM layout:\n{actual}")


def image_chip_id(image: bytes) -> int:
    if len(image) < 24 or image[0] != 0xE9:
        raise SafetyError("firmware is not an ESP application image")
    return struct.unpack_from("<H", image, 12)[0]


def validate_firmware(path: Path) -> bytes:
    firmware = path.read_bytes()
    if len(firmware) < 64 * 1024 or len(firmware) > APP0_SIZE:
        raise SafetyError(f"firmware size {len(firmware)} does not fit OEM app0")
    chip_id = image_chip_id(firmware)
    if chip_id != ESP32S3_CHIP_ID:
        raise SafetyError(f"firmware target chip is {chip_id:#06x}, expected ESP32-S3 {ESP32S3_CHIP_ID:#06x}")
    return firmware


def ota_crc(seq: int) -> int:
    return binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF


def active_ota_entry(otadata: bytes) -> tuple[int, int, int]:
    valid: list[tuple[int, int, int]] = []
    for metadata_sector in range(2):
        pos = metadata_sector * OTA_SECTOR_SIZE
        seq, state, crc = struct.unpack_from("<I20xII", otadata, pos)
        if seq != 0xFFFFFFFF and crc == ota_crc(seq) and state not in (3, 4):
            valid.append((seq, metadata_sector, (seq - 1) % 2))
    if not valid:
        raise SafetyError("no valid OTA metadata entry found")
    return max(valid, key=lambda item: item[0])


def security_is_enabled(report: str) -> bool:
    risky = ("secure boot", "flash encryption", "flash_crypt")
    for line in report.lower().splitlines():
        if any(key in line for key in risky) and any(value in line for value in ("enabled", "true", "= 1")):
            return True
    return False


def mac_from_report(report: str) -> str:
    match = re.search(r"(?:MAC|BASE MAC)\s*:\s*([0-9a-f:]{17})", report, re.IGNORECASE)
    if not match:
        raise SafetyError("could not read the device MAC identity from esptool")
    return match.group(1).lower()


def load_manifest(directory: Path) -> dict:
    path = directory / "manifest.json"
    if not path.is_file():
        raise SafetyError(f"missing backup manifest: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def command_backup(args: argparse.Namespace) -> None:
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=False)
    identity = run_esptool(args.port, args.baud, "chip-id", capture=True)
    security = run_esptool(args.port, args.baud, "security-info", capture=True)
    flash_id = run_esptool(args.port, args.baud, "flash-id", capture=True)
    backup = root / "x4pro-full-16mb.bin"
    read_flash(args.port, args.baud, 0, FLASH_SIZE, backup)
    image = backup.read_bytes()
    require_oem_layout(image)
    if image[NVS_OFFSET:NVS_OFFSET + NVS_SIZE].count(0xFF) == NVS_SIZE:
        raise SafetyError("NVS is blank; per-device calibration cannot be protected")
    active_seq, metadata_sector, active_app = active_ota_entry(image[OTADATA_OFFSET:OTADATA_OFFSET + OTADATA_SIZE])
    if active_app != 1:
        raise SafetyError(f"factory app1 is not active (active OTA slot is app{active_app}); refusing this procedure")
    if security_is_enabled(security):
        raise SafetyError("Secure Boot or flash encryption is enabled; unsigned field build must not be installed")

    extracts = {
        "bootloader.bin": image[0x0000:0x8000],
        "partition-table.bin": image[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE],
        "nvs.bin": image[NVS_OFFSET:NVS_OFFSET + NVS_SIZE],
        "otadata.bin": image[OTADATA_OFFSET:OTADATA_OFFSET + OTADATA_SIZE],
        "factory-app1.bin": image[APP1_OFFSET:APP1_OFFSET + APP1_SIZE],
    }
    hashes = {backup.name: sha256_file(backup)}
    for name, data in extracts.items():
        (root / name).write_bytes(data)
        hashes[name] = sha256_bytes(data)
    (root / "esptool-identity.txt").write_text(identity + "\n" + flash_id, encoding="utf-8")
    (root / "esptool-security.txt").write_text(security, encoding="utf-8")
    manifest = {
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "device": "XTEINK X4 Pro",
        "chip": "ESP32-S3",
        "mac": mac_from_report(identity),
        "flash_size": FLASH_SIZE,
        "active_app": active_app,
        "active_seq": active_seq,
        "active_metadata_sector": metadata_sector,
        "partitions": EXPECTED_PARTITIONS,
        "sha256": hashes,
    }
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"\nBACKUP OK: {root}")
    print("Disconnect the device, re-enter ROM download mode, then run the verify command.")


def command_verify(args: argparse.Namespace) -> None:
    root = Path(args.backup).resolve()
    manifest = load_manifest(root)
    backup = root / "x4pro-full-16mb.bin"
    if backup.stat().st_size != FLASH_SIZE or sha256_file(backup) != manifest["sha256"][backup.name]:
        raise SafetyError("full-flash backup size/hash mismatch")
    identity = run_esptool(args.port, args.baud, "chip-id", capture=True)
    security = run_esptool(args.port, args.baud, "security-info", capture=True)
    if mac_from_report(identity) != manifest["mac"]:
        raise SafetyError("the second ROM session belongs to a different device")
    if security_is_enabled(security):
        raise SafetyError("security eFuses reject this unsigned installation")
    with tempfile.TemporaryDirectory(prefix="x4pro-verify-") as temp:
        first = Path(temp) / "first-64k.bin"
        app1_head = Path(temp) / "app1-head.bin"
        read_flash(args.port, args.baud, 0, 0x10000, first)
        read_flash(args.port, args.baud, APP1_OFFSET, 0x10000, app1_head)
        original = backup.read_bytes()
        if first.read_bytes() != original[:0x10000]:
            raise SafetyError("bootloader/partition/NVS/otadata changed since backup")
        if app1_head.read_bytes() != original[APP1_OFFSET:APP1_OFFSET + 0x10000]:
            raise SafetyError("factory app1 changed since backup")
    verification = {
        "verified_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "mac": manifest["mac"],
        "backup_sha256": manifest["sha256"][backup.name],
    }
    (root / "rom-session-2-verified.json").write_text(
        json.dumps(verification, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("\nSECOND ROM SESSION VERIFIED. The backup is usable and the device is unchanged.")


def require_verified_backup(root: Path) -> tuple[dict, bytes]:
    manifest = load_manifest(root)
    verification_path = root / "rom-session-2-verified.json"
    if not verification_path.is_file():
        raise SafetyError("second ROM-session verification is missing")
    verification = json.loads(verification_path.read_text(encoding="utf-8"))
    backup_path = root / "x4pro-full-16mb.bin"
    backup = backup_path.read_bytes()
    digest = sha256_bytes(backup)
    if len(backup) != FLASH_SIZE or digest != manifest["sha256"][backup_path.name]:
        raise SafetyError("backup no longer matches its manifest")
    if verification.get("backup_sha256") != digest or verification.get("mac") != manifest.get("mac"):
        raise SafetyError("ROM verification does not match this backup")
    require_oem_layout(backup)
    return manifest, backup


def command_install(args: argparse.Namespace) -> None:
    if args.confirm != "PRESERVE-FACTORY-APP1":
        raise SafetyError("install requires --confirm PRESERVE-FACTORY-APP1")
    root = Path(args.backup).resolve()
    firmware_path = Path(args.firmware).resolve()
    firmware = validate_firmware(firmware_path)
    manifest, backup = require_verified_backup(root)
    identity = run_esptool(args.port, args.baud, "chip-id", capture=True)
    security = run_esptool(args.port, args.baud, "security-info", capture=True)
    if mac_from_report(identity) != manifest["mac"]:
        raise SafetyError("install port is connected to a different device")
    if security_is_enabled(security):
        raise SafetyError("security eFuses reject this unsigned installation")

    with tempfile.TemporaryDirectory(prefix="x4pro-install-") as temp:
        temp_dir = Path(temp)
        current_first = temp_dir / "current-first-64k.bin"
        read_flash(args.port, args.baud, 0, 0x10000, current_first)
        if current_first.read_bytes() != backup[:0x10000]:
            raise SafetyError("protected bootloader/partition/NVS/otadata changed; make a new backup")

        # Write only inactive app0. esptool performs its own target compatibility
        # check and MD5 verification; --force and --erase-all are never used.
        run_esptool(args.port, args.baud, "write-flash", "--flash-mode", "keep", "--flash-freq", "keep",
                    "--flash-size", "keep", hex(APP0_OFFSET), str(firmware_path))
        readback = temp_dir / "app0-readback.bin"
        read_flash(args.port, args.baud, APP0_OFFSET, len(firmware), readback)
        if sha256_file(readback) != sha256_bytes(firmware):
            raise SafetyError("app0 read-back hash mismatch; otadata was NOT changed")

        active_seq, active_metadata_sector, active_app = active_ota_entry(
            backup[OTADATA_OFFSET:OTADATA_OFFSET + OTADATA_SIZE])
        if active_app != 1:
            raise SafetyError("backup does not select factory app1")
        new_seq = active_seq + 1
        while (new_seq - 1) % 2 != 0:
            new_seq += 1
        target_metadata_sector = 1 - active_metadata_sector
        sector = bytearray(b"\xFF" * OTA_SECTOR_SIZE)
        struct.pack_into("<I20sII", sector, 0, new_seq, b"\xFF" * 20, 0, ota_crc(new_seq))
        sector_path = temp_dir / "otadata-app0-sector.bin"
        sector_path.write_bytes(sector)
        metadata_address = OTADATA_OFFSET + target_metadata_sector * OTA_SECTOR_SIZE
        run_esptool(args.port, args.baud, "write-flash", "--flash-mode", "keep", "--flash-freq", "keep",
                    "--flash-size", "keep", hex(metadata_address), str(sector_path))
        metadata_readback = temp_dir / "otadata-readback.bin"
        read_flash(args.port, args.baud, metadata_address, OTA_SECTOR_SIZE, metadata_readback)
        if metadata_readback.read_bytes() != sector:
            raise SafetyError("otadata read-back mismatch")

    report = {
        "installed_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "mac": manifest["mac"],
        "firmware": firmware_path.name,
        "firmware_size": len(firmware),
        "firmware_sha256": sha256_bytes(firmware),
        "written_app": "app0",
        "preserved_factory_app": "app1",
        "preserved_nvs_sha256": manifest["sha256"]["nvs.bin"],
    }
    (root / "install-report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("\nINSTALL VERIFIED. app0 and one inactive otadata sector were written; factory app1/NVS were preserved.")


def command_restore(args: argparse.Namespace) -> None:
    if args.confirm != "RESTORE-FACTORY-BOOT":
        raise SafetyError("restore requires --confirm RESTORE-FACTORY-BOOT")
    root = Path(args.backup).resolve()
    manifest, backup = require_verified_backup(root)
    identity = run_esptool(args.port, args.baud, "chip-id", capture=True)
    security = run_esptool(args.port, args.baud, "security-info", capture=True)
    if mac_from_report(identity) != manifest["mac"]:
        raise SafetyError("restore port is connected to a different device")
    if security_is_enabled(security):
        raise SafetyError("security eFuses changed; automatic stock restore is unsafe")
    otadata = root / "otadata.bin"
    original_otadata = backup[OTADATA_OFFSET:OTADATA_OFFSET + OTADATA_SIZE]
    if not otadata.is_file() or otadata.read_bytes() != original_otadata:
        raise SafetyError("saved factory otadata does not match the immutable full backup")
    run_esptool(args.port, args.baud, "write-flash", "--flash-mode", "keep", "--flash-freq", "keep",
                "--flash-size", "keep", hex(OTADATA_OFFSET), str(otadata))
    with tempfile.TemporaryDirectory(prefix="x4pro-restore-") as temp:
        readback = Path(temp) / "otadata-readback.bin"
        read_flash(args.port, args.baud, OTADATA_OFFSET, OTADATA_SIZE, readback)
        if readback.read_bytes() != original_otadata:
            raise SafetyError("factory otadata read-back mismatch")
    print("Factory otadata restored and verified. app1 will be selected on the next reset.")


def parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", required=True, help="serial port in ESP32-S3 ROM download mode")
    common.add_argument("--baud", type=int, default=460800)
    root = argparse.ArgumentParser(description=__doc__)
    sub = root.add_subparsers(dest="command", required=True)
    backup = sub.add_parser("backup", parents=[common])
    backup.add_argument("--output", required=True, help="new empty backup directory")
    backup.set_defaults(func=command_backup)
    verify = sub.add_parser("verify", parents=[common])
    verify.add_argument("--backup", required=True)
    verify.set_defaults(func=command_verify)
    install = sub.add_parser("install", parents=[common])
    install.add_argument("--backup", required=True)
    install.add_argument("--firmware", required=True)
    install.add_argument("--confirm", required=True)
    install.set_defaults(func=command_install)
    restore = sub.add_parser("restore-stock", parents=[common])
    restore.add_argument("--backup", required=True)
    restore.add_argument("--confirm", required=True)
    restore.set_defaults(func=command_restore)
    return root


def main() -> int:
    try:
        args = parser().parse_args()
        args.func(args)
        return 0
    except (SafetyError, OSError, subprocess.SubprocessError) as error:
        print(f"\nSAFETY STOP: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
