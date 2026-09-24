#pragma once

#include <stddef.h>

// Per-frame e-ink decision trace for field diagnosis.
//
// One short line per displayBuffer() attempt, emitted by the FreeInkDisplay
// facade: controller identity (probed at boot), the waveform bank the driver
// actually chose (fast/half/full or DU/GC), the requested mode, attempt/soft
// retry bookkeeping, bus health with the failing wait's tag, elapsed time and
// the driver's refresh-state flags.
//
// In INKPOINTX_DEVICE_QA builds every line goes to the USB serial console and
// is mirrored into a ring file /.crosspoint/epd.log on the SD card (rewritten
// atomically on each line, so the card can be pulled at any moment). In
// production builds this is a no-op.
namespace EpdTrace {

void log(const char* line);

// Copy the current in-RAM trace ring into buf (NUL-terminated).
// Returns bytes written; 0 in production builds or when the ring is empty.
size_t snapshot(char* buf, size_t cap);

}  // namespace EpdTrace
