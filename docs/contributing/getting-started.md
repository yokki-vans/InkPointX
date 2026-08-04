# Getting started

This branch builds only for the XTEINK X4 Pro ESP32-S3 target.

Requirements:

- PlatformIO Core;
- Python dependencies from `lib/EpdFont/scripts/requirements.txt`;
- recursive `freeink-sdk` submodule checkout;
- a project path without whitespace.

```bash
git submodule update --init --recursive
python3 -m pip install -r lib/EpdFont/scripts/requirements.txt
pio run -e x4pro
cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/test
ctest --test-dir build/test --output-on-failure
python3 test/x4pro_safe_flash_test.py
```

Hardware testing must follow `README.md`: full 16-MB backup, a second ROM-mode
verification, app0-only installation, readback verification, and preservation of
factory app1/NVS. Do not test by flashing bootloader, partition table or a merged
factory image.
