#include "EpdTrace.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <cstring>

#if defined(INKPOINTX_DEVICE_QA)

namespace {

constexpr char kLogPath[] = "/.crosspoint/epd.log";
// Small ring: a menu session's worth of frames, oldest dropped first.
constexpr size_t kRingBytes = 8192;

char ring[kRingBytes + 1];
size_t ringLen = 0;
bool bannerDone = false;

void appendLine(const char* line) {
  const size_t n = strlen(line);
  if (n >= kRingBytes) {
    // Pathological line: keep only its tail.
    memcpy(ring, line + (n - kRingBytes), kRingBytes);
    ringLen = kRingBytes;
  } else {
    if (ringLen + n + 1 > kRingBytes) {
      // Drop whole lines from the front until it fits.
      size_t drop = ringLen + n + 1 - kRingBytes;
      size_t i = 0;
      while (i < ringLen && i < drop) ++i;
      while (i < ringLen && ring[i] != '\n') ++i;
      if (i < ringLen) ++i;
      memmove(ring, ring + i, ringLen - i);
      ringLen -= i;
    }
    memcpy(ring + ringLen, line, n);
    ringLen += n;
    ring[ringLen++] = '\n';
  }
  ring[ringLen] = '\0';
}

}  // namespace

namespace EpdTrace {

void log(const char* line) {
  if (!line || !*line) return;
  if (!bannerDone) {
    bannerDone = true;
    char banner[128];
    snprintf(banner, sizeof(banner), "epd-trace start version=%s qa=1",
#ifdef CROSSPOINT_VERSION
             CROSSPOINT_VERSION
#else
             "?"
#endif
    );
    appendLine(banner);
  }
  appendLine(line);
  if (Serial) Serial.println(line);
  if (Storage.ready()) Storage.writeFile(kLogPath, String(ring));
}

size_t snapshot(char* buf, size_t cap) {
  if (!buf || cap == 0) return 0;
  const size_t n = (ringLen < cap - 1) ? ringLen : cap - 1;
  if (n > 0) memcpy(buf, ring + (ringLen - n), n);
  buf[n] = '\0';
  return n;
}

}  // namespace EpdTrace

#else  // production: no-op

namespace EpdTrace {
void log(const char*) {}
size_t snapshot(char*, size_t) { return 0; }

}  // namespace EpdTrace

#endif  // INKPOINTX_DEVICE_QA
