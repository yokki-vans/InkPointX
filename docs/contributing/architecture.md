# X4 Pro firmware architecture

InkPointX in this branch is a single-board ESP32-S3 application. `BoardConfig`
is compiled with only `FREEINK_DEVICE_X4PRO`; there is no runtime X3/X4 board
selection.

```text
XTEINK X4 Pro hardware
  -> FreeInk X4 Pro board profile and drivers
  -> InkPointX HAL (display, input, storage, power, clock, frontlight)
  -> reader/application activities
```

The X4 Pro HAL owns these safety boundaries:

- GPIO1 power rail is asserted before serial or peripheral initialization;
- display controller probing happens before `display.begin()`;
- only the validated SSD1677 path is enabled in the first field build;
- SD uses native 1-bit SDMMC and an active-low GPIO5 rail;
- GT911, CW2017 and BM8563 share the X4 Pro I2C bus;
- first installation writes only inactive OEM app0;
- the external installer accepts only a verified ESP32-S3 app image and keeps
  factory app1 available for ROM-mode recovery. Automatic rollback is used only
  if the preserved OEM bootloader was built with that option.

The reader, parsers and UI remain layered above the HAL and operate on the
X4 Pro portrait framebuffer geometry (480 × 800 logical orientation).
