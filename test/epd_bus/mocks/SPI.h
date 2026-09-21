#pragma once
#include <cstdint>
constexpr int MSBFIRST = 1, SPI_MODE0 = 0;
struct SPISettings {
  SPISettings() = default;
  SPISettings(uint32_t, int, int) {}
};
struct SpiMock {
  void begin(int, int, int, int) {}
  void beginTransaction(SPISettings) {}
  void endTransaction() {}
  void transfer(uint8_t) {}
  void writeBytes(const uint8_t*, uint16_t) {}
};
inline SpiMock SPI;
