# InkPointX for XTEINK X4 Pro

This branch contains firmware **only for the XTEINK X4 Pro**. It targets the
ESP32-S3/16 MB flash/8 MB PSRAM board and must not be installed on the ESP32-C3
XTEINK X3 or X4.

> Field-test status: the code and binaries are checked without a local device.
> The first tester must use the backup-first procedure below. Do not use an
> all-flash/factory image and never run `erase-flash`.

## Hardware target

The profile is based on the X4 Pro OEM firmware analysis and FreeInk bring-up:

- ESP32-S3, 16 MB flash, 8 MB octal PSRAM;
- 800 × 480 SSD1677 e-paper over SPI;
- GT911 touch on SDA39/SCL38, INT10, RST4, active-low rail GPIO2;
- navigation GPIO0/GPIO7 and power GPIO3, active-low;
- native 1-bit SDMMC: CLK41, CMD42, DAT0=40, active-low rail GPIO5;
- CW2017 fuel gauge and BM8563 RTC on the shared I2C bus;
- warm/cold frontlight PWM on GPIO8/GPIO9;
- master peripheral rail GPIO1, asserted before any peripheral access.

The initial field image accepts only the hardware-validated SSD1677 controller.
If the live two-pass probe reports UC8179/UC8279 or an inconclusive result, the
firmware fails closed before display high-voltage refresh and remains available
for serial diagnostics. This prevents an unvalidated waveform from being sent
to a different panel batch.

Detailed evidence and remaining hardware checks are in
[`docs/X4_PRO_SUPPORT_RU.md`](docs/X4_PRO_SUPPORT_RU.md).

## Build

Clone with submodules into a path without spaces, then build the only target:

```bash
git clone --branch dev-x4pro-test --recurse-submodules \
  https://github.com/yokki-vans/InkPointX.git InkPointX-X4Pro
cd InkPointX-X4Pro
python3 -m pip install -r lib/EpdFont/scripts/requirements.txt
pio run -e x4pro
```

Application image:

```text
.pio/build/x4pro/firmware.bin
```

The project deliberately does not publish a merged `factory.bin`. The checked-in
partition CSV mirrors the OEM layout so runtime OTA APIs understand the original
slots, but first installation preserves the device's existing bootloader,
partition table, NVS and factory `app1` byte-for-byte.

## Safe first installation

Requirements: Python 3 and `esptool` 4.x/5.x.

1. Enter ESP32-S3 ROM download mode using the GPIO0 navigation key during reset.
   Prove this works before any write.
2. Create a complete backup:

```bash
python3 scripts/x4pro_safe_flash.py backup \
  --port /dev/ttyACM0 \
  --output x4pro-backup
```

3. Disconnect the reader, enter ROM download mode a second time, and verify that
   the backup belongs to the same unchanged device:

```bash
python3 scripts/x4pro_safe_flash.py verify \
  --port /dev/ttyACM0 \
  --backup x4pro-backup
```

4. Only after both stages pass, install the application into inactive OEM
   `app0`:

```bash
python3 scripts/x4pro_safe_flash.py install \
  --port /dev/ttyACM0 \
  --backup x4pro-backup \
  --firmware .pio/build/x4pro/firmware.bin \
  --confirm PRESERVE-FACTORY-APP1
```

The installer validates ESP32-S3 chip ID, security eFuses, the exact OEM
partition map, active factory `app1`, firmware size and target chip. It then:

- verifies that protected flash still matches the backup;
- writes only inactive `app0` at `0x10000`;
- reads `app0` back and compares SHA-256;
- writes one previously inactive 4 KiB `otadata` sector;
- leaves bootloader, partition table, NVS and factory `app1` untouched.

If the test image does not boot, re-enter ROM mode and restore the original boot
selection:

```bash
python3 scripts/x4pro_safe_flash.py restore-stock \
  --port /dev/ttyACM0 \
  --backup x4pro-backup \
  --confirm RESTORE-FACTORY-BOOT
```

Keep the entire backup directory. `nvs.bin` is device-specific and must never be
copied to another reader.

## Test-build safeguards

- X3/X4 build environments and runtime board detection are removed.
- The ESP32-C3 eFuse-validation bypass is excluded from the build.
- Firmware files with a non-ESP32-S3 image header are rejected by the installer.
- OTA and SD self-update code is excluded from this first hardware-test branch.
- Cold boot stays awake because the X4 Pro VBUS-detect pin is not yet validated.
- A pending first-install image is marked valid only after SD, display and main-loop health
  milestones complete.

## License

InkPointX is distributed under the repository's existing license. XTEINK is a
third-party trademark; this project is independent and unofficial.
