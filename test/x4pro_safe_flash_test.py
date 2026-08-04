#!/usr/bin/env python3
"""Pure safety tests for the X4 Pro first-install tool."""

from __future__ import annotations

import argparse
import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("x4pro_safe_flash", ROOT / "scripts" / "x4pro_safe_flash.py")
assert SPEC is not None and SPEC.loader is not None
FLASH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FLASH)


def partition_entry(label: str, values: tuple[int, int, int, int]) -> bytes:
    part_type, subtype, offset, size = values
    raw_label = label.encode("ascii").ljust(16, b"\0")
    return struct.pack("<HBBII16sI", 0x50AA, part_type, subtype, offset, size, raw_label, 0)


def oem_flash_image() -> bytearray:
    image = bytearray(b"\xFF" * FLASH.FLASH_SIZE)
    table = b"".join(partition_entry(label, values) for label, values in FLASH.EXPECTED_PARTITIONS.items())
    image[FLASH.PARTITION_TABLE_OFFSET:FLASH.PARTITION_TABLE_OFFSET + len(table)] = table
    image[FLASH.NVS_OFFSET] = 0
    return image


def ota_sector(seq: int, state: int = 2) -> bytes:
    sector = bytearray(b"\xFF" * FLASH.OTA_SECTOR_SIZE)
    struct.pack_into("<I20sII", sector, 0, seq, b"\xFF" * 20, state, FLASH.ota_crc(seq))
    return bytes(sector)


class X4ProSafetyTest(unittest.TestCase):
    def test_exact_oem_partition_layout_is_accepted(self) -> None:
        FLASH.require_oem_layout(oem_flash_image())

    def test_any_partition_layout_change_is_rejected(self) -> None:
        image = oem_flash_image()
        struct.pack_into("<I", image, FLASH.PARTITION_TABLE_OFFSET + 8, FLASH.NVS_SIZE + 0x1000)
        with self.assertRaises(FLASH.SafetyError):
            FLASH.require_oem_layout(image)

    def test_active_factory_app1_and_new_app0_sequence(self) -> None:
        otadata = ota_sector(1) + ota_sector(2)
        seq, sector, app = FLASH.active_ota_entry(otadata)
        self.assertEqual((seq, sector, app), (2, 1, 1))
        new_seq = seq + 1
        while (new_seq - 1) % 2 != 0:
            new_seq += 1
        self.assertEqual((new_seq - 1) % 2, 0)

    def test_bad_ota_crc_is_rejected(self) -> None:
        otadata = bytearray(ota_sector(2) + b"\xFF" * FLASH.OTA_SECTOR_SIZE)
        otadata[28] ^= 1
        with self.assertRaises(FLASH.SafetyError):
            FLASH.active_ota_entry(otadata)

    def test_power_loss_during_inactive_metadata_write_keeps_previous_boot(self) -> None:
        # Install targets sector 0 while factory app1 remains valid in sector 1.
        interrupted_install = b"\xFF" * FLASH.OTA_SECTOR_SIZE + ota_sector(2)
        self.assertEqual(FLASH.active_ota_entry(interrupted_install)[2], 1)

        # Restore targets sector 1 while the installed app0 remains valid in sector 0.
        interrupted_restore = ota_sector(3) + b"\xFF" * FLASH.OTA_SECTOR_SIZE
        self.assertEqual(FLASH.active_ota_entry(interrupted_restore)[2], 0)

    def test_only_esp32s3_application_fits(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "firmware.bin"
            firmware = bytearray(b"\0" * (64 * 1024))
            firmware[0] = 0xE9
            struct.pack_into("<H", firmware, 12, FLASH.ESP32S3_CHIP_ID)
            path.write_bytes(firmware)
            digest = FLASH.sha256_bytes(firmware)
            self.assertEqual(FLASH.validate_firmware(path, digest, inspect_image=False), firmware)
            struct.pack_into("<H", firmware, 12, 0x0005)
            path.write_bytes(firmware)
            with self.assertRaises(FLASH.SafetyError):
                FLASH.validate_firmware(path, FLASH.sha256_bytes(firmware), inspect_image=False)
            struct.pack_into("<H", firmware, 12, FLASH.ESP32S3_CHIP_ID)
            path.write_bytes(firmware)
            with self.assertRaises(FLASH.SafetyError):
                FLASH.validate_firmware(path, "0" * 64, inspect_image=False)

    def test_only_audited_tool_series_is_accepted(self) -> None:
        accepted = mock.Mock(returncode=0, stdout="esptool v5.3.7\n")
        with mock.patch.object(FLASH.subprocess, "run", return_value=accepted):
            FLASH.require_tool_version("esptool", "esptool")
        for output in ("esptool v5.2.0\n", "esptool v5.4.0\n", "unknown\n"):
            rejected = mock.Mock(returncode=0, stdout=output)
            with mock.patch.object(FLASH.subprocess, "run", return_value=rejected):
                with self.assertRaises(FLASH.SafetyError):
                    FLASH.require_tool_version("esptool", "esptool")

    def test_security_report_is_fail_closed(self) -> None:
        safe = "Chip ID: 9\nSecure Boot: Disabled\nFlash Encryption: Disabled\n"
        FLASH.require_security_disabled(safe)
        for unsafe in (
            "Chip ID: 9\nSecure Boot: Enabled\nFlash Encryption: Disabled\n",
            "Chip ID: 9\nSecure Boot: Disabled\nFlash Encryption: Enabled\n",
            "Secure Boot: Disabled\nFlash Encryption: Disabled\n",
            "Chip ID: 9\nsecurity state unknown\n",
        ):
            with self.assertRaises(FLASH.SafetyError):
                FLASH.require_security_disabled(unsafe)

    def test_flash_size_is_fail_closed(self) -> None:
        FLASH.require_16mb_flash("Detected flash size: 16MB\n")
        for report in ("Detected flash size: 8MB\n", "Detected flash size: Unknown\n", ""):
            with self.assertRaises(FLASH.SafetyError):
                FLASH.require_16mb_flash(report)

    def test_ota_sequence_targets_requested_slot(self) -> None:
        self.assertEqual((FLASH.next_ota_sequence(2, 0) - 1) % 2, 0)
        self.assertEqual((FLASH.next_ota_sequence(3, 1) - 1) % 2, 1)
        with self.assertRaises(FLASH.SafetyError):
            FLASH.next_ota_sequence(0xFFFFFFFC, 0)

    def test_install_mutates_only_app0_and_one_inactive_otadata_sector(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            backup_dir = root / "backup"
            backup_dir.mkdir()
            flash = oem_flash_image()
            flash[FLASH.OTADATA_OFFSET:FLASH.OTADATA_OFFSET + FLASH.OTADATA_SIZE] = ota_sector(1) + ota_sector(2)
            original = bytes(flash)
            backup_path = backup_dir / "x4pro-full-16mb.bin"
            backup_path.write_bytes(original)
            digest = FLASH.sha256_bytes(original)
            manifest = {
                "mac": "02:00:00:00:00:01",
                "sha256": {backup_path.name: digest, "nvs.bin": FLASH.sha256_bytes(original[FLASH.NVS_OFFSET:FLASH.NVS_OFFSET + FLASH.NVS_SIZE])},
            }
            (backup_dir / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            (backup_dir / "rom-session-2-verified.json").write_text(
                json.dumps({"mac": manifest["mac"], "backup_sha256": digest}), encoding="utf-8"
            )

            firmware_path = root / "InkPointX-X4Pro.bin"
            firmware = bytearray(b"\0" * (64 * 1024))
            firmware[0] = 0xE9
            struct.pack_into("<H", firmware, 12, FLASH.ESP32S3_CHIP_ID)
            firmware_path.write_bytes(firmware)
            writes: list[tuple[int, int]] = []

            def fake_run(_port: str, _baud: int, *arguments: str, capture: bool = False) -> str:
                self.assertEqual(arguments[0], "write-flash")
                address = int(arguments[-2], 0)
                payload = Path(arguments[-1]).read_bytes()
                flash[address:address + len(payload)] = payload
                writes.append((address, len(payload)))
                return ""

            def fake_read(_port: str, _baud: int, offset: int, size: int, output: Path) -> None:
                output.write_bytes(flash[offset:offset + size])

            args = argparse.Namespace(
                confirm="PRESERVE-FACTORY-APP1",
                backup=str(backup_dir),
                firmware=str(firmware_path),
                firmware_sha256=FLASH.sha256_bytes(firmware),
                port="FAKE",
                baud=460800,
            )
            with mock.patch.object(FLASH, "require_supported_tools"), mock.patch.object(
                FLASH, "validate_image_info"
            ), mock.patch.object(
                FLASH, "check_live_security", return_value=("MAC: 02:00:00:00:00:01\n", "", "")
            ), mock.patch.object(FLASH, "run_esptool", side_effect=fake_run), mock.patch.object(
                FLASH, "read_flash", side_effect=fake_read):
                FLASH.command_install(args)

            self.assertEqual(writes, [(FLASH.APP0_OFFSET, len(firmware)), (FLASH.OTADATA_OFFSET, FLASH.OTA_SECTOR_SIZE)])
            self.assertEqual(flash[:FLASH.OTADATA_OFFSET], original[:FLASH.OTADATA_OFFSET])
            self.assertEqual(flash[FLASH.APP1_OFFSET:], original[FLASH.APP1_OFFSET:])
            self.assertEqual(flash[FLASH.APP0_OFFSET:FLASH.APP0_OFFSET + len(firmware)], firmware)

    def test_restore_writes_only_inactive_otadata_sector(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            backup_dir = root / "backup"
            backup_dir.mkdir()
            flash = oem_flash_image()
            factory_otadata = ota_sector(1) + ota_sector(2)
            flash[FLASH.OTADATA_OFFSET:FLASH.OTADATA_OFFSET + FLASH.OTADATA_SIZE] = factory_otadata
            original = bytes(flash)
            backup_path = backup_dir / "x4pro-full-16mb.bin"
            backup_path.write_bytes(original)
            digest = FLASH.sha256_bytes(original)
            (backup_dir / "manifest.json").write_text(json.dumps({
                "mac": "02:00:00:00:00:01", "sha256": {backup_path.name: digest}
            }), encoding="utf-8")
            (backup_dir / "rom-session-2-verified.json").write_text(json.dumps({
                "mac": "02:00:00:00:00:01", "backup_sha256": digest
            }), encoding="utf-8")

            # Simulate an installed, currently active app0 entry in sector 0.
            flash[FLASH.OTADATA_OFFSET:FLASH.OTADATA_OFFSET + FLASH.OTA_SECTOR_SIZE] = ota_sector(3, state=2)
            before_restore = bytes(flash)
            writes: list[tuple[int, int]] = []

            def fake_run(_port: str, _baud: int, *arguments: str, capture: bool = False) -> str:
                self.assertEqual(arguments[0], "write-flash")
                address = int(arguments[-2], 0)
                payload = Path(arguments[-1]).read_bytes()
                flash[address:address + len(payload)] = payload
                writes.append((address, len(payload)))
                return ""

            def fake_read(_port: str, _baud: int, offset: int, size: int, output: Path) -> None:
                output.write_bytes(flash[offset:offset + size])

            args = argparse.Namespace(confirm="RESTORE-FACTORY-BOOT", backup=str(backup_dir), port="FAKE", baud=460800)
            with mock.patch.object(FLASH, "require_supported_tools"), mock.patch.object(
                FLASH, "check_live_security", return_value=("MAC: 02:00:00:00:00:01\n", "", "")
            ), mock.patch.object(FLASH, "run_esptool", side_effect=fake_run), mock.patch.object(
                FLASH, "read_flash", side_effect=fake_read):
                FLASH.command_restore(args)

            self.assertEqual(writes, [(FLASH.OTADATA_OFFSET + FLASH.OTA_SECTOR_SIZE, FLASH.OTA_SECTOR_SIZE)])
            self.assertEqual(flash[:FLASH.OTADATA_OFFSET], before_restore[:FLASH.OTADATA_OFFSET])
            self.assertEqual(flash[FLASH.APP0_OFFSET:], before_restore[FLASH.APP0_OFFSET:])
            self.assertEqual(FLASH.active_ota_entry(
                flash[FLASH.OTADATA_OFFSET:FLASH.OTADATA_OFFSET + FLASH.OTADATA_SIZE]
            )[2], 1)


if __name__ == "__main__":
    unittest.main()
