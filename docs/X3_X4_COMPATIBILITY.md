# X3/X4 display compatibility and acceptance

## Supported controller paths

| Device | Controller | Logical landscape buffer | Baseline / waveform | Verification in v2.3.5 |
|---|---|---|---|---|
| X3 original | UC8253 | 792 × 528, 52,272 bytes | OLD/NEW planes, external LUTs | Driver and shared-bus host regressions; physical test pending |
| X3 newer | UC8279D | 792 × 528, 52,272 bytes | Windowed OLD/NEW, external LUTs including blank-MTP init | Driver, initialization, calibration and shared-bus host regressions; physical test pending |
| X4 original | SSD1677 | 800 × 480, 48,000 bytes | Existing OTP sequence, active-high BUSY | Production build and shared-bus polarity regression; physical test pending |
| X4 newer | UC8179 | 800 × 480, 48,000 bytes; padded controller transfer | OTP, active-low BUSY, OLD-plane synchronization | Driver and shared-bus host regressions; physical test pending |

X4 Pro is a different ESP32-S3 target and is **not** supported by this ESP32-C3
release. USB-locked/unlocked status is independent of the panel controller.
There is no complete manufacturer-provided revision inventory here, and the
firmware cannot infer optical success from a BUSY signal alone.

The existing X3 I2C fingerprint, controller VER/FLG/RMTP probe and family-matched
OEM `screenType` fallback are retained. Ambiguous live probes remain ambiguous;
unknown future boards and undocumented factory calibration cannot be certified.
X3 GPIO13 controls the SD rail; X4 GPIO13 is the power latch. Neither is changed
by this patch. No ADC thresholds or button mappings were changed without input
measurements from the affected device.

## Reproduced software faults

The regression suite links the production UC8253, UC8279 and UC8179 driver
implementations to a recording bus, and separately compiles the real EpdBus
against GPIO/RTOS stubs. Against SDK `cc2db24922aedf79dd0da97bbeb0e40a8d3dca95`,
14 checks fail; the fixed SDK passes them:

- UC8279 retains a narrow PTL window across PTOUT and reuses it for a full frame.
- Both X3 drivers can treat cleared/unknown OLD RAM as valid after seamless wake.
- Both X3 drivers can use abandoned grayscale planes as a B/W baseline.
- UC8253 downgrades an explicit FULL to HALF on power-on.
- UC8253 and UC8179 reinitialization leaves pending/previous-frame state alive.
- UC8179 rejects a delayed (250 ms simulated) start and treats CLEAN as FAST.
- UC8253 grayscale failure still commits the OLD plane.
- Polling completion mistakes an already-finished split refresh for one that
  never started, or retains a start token for another operation. This includes
  the no-semaphore fallback.

The UC8179 async facade is also gated by driver capability, not the X3/X4 model
name: a driver that rereads submitted pixels in displayFinish cannot allow the
caller to overwrite those pixels before completion. This avoids an extra
48 KB shadow allocation in that fallback; no new framebuffer is allocated.

These are code-level causes of stale/corrupt refreshes, not proof that every
fault occurred on the reporting user's unit.

## Physical acceptance required per available revision

Record device model, detected controller/profile, stock firmware/calibration,
installation route and lock status. Preserve an existing recovery route.

1. Install over a known-working application without replacing the bootloader
   or partition table. Check the displayed version and OTA discovery.
2. From cold boot and wake, make 30 separate front-button navigation presses.
   Every press must visibly move the selection once; manual refresh must not
   be necessary. Repeat with the side buttons and after an OTA restart.
3. Turn 30 text pages, then alternate an illustrated/grayscale page and menu.
   Verify text is erased correctly and the complete screen updates, including
   areas outside the last image strip. Repeat in light and dark mode.
4. Exercise manual FULL refresh, periodic CLEAN, sleep cover, quick resume,
   Wi-Fi/OTA screens and 10 sleep/wake cycles. Confirm SD remains accessible.
5. Capture BUSY start/completion and input diagnostics on an instrumentable
   build when a failure occurs. A framebuffer screenshot alone cannot verify
   the physical panel. Locked units can be checked with video and their
   existing SD updater; USB access is not required for the visual checks.

Mark each row hardware-verified only after these checks run on that controller
revision. No blanket “100% all revisions” claim is supported by host tests.
