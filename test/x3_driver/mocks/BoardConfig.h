#pragma once
#include <cstdint>
namespace BoardConfig {
struct Profile {
#ifdef TEST_DEVICE_X4
  uint16_t displayWidth = 800;
  uint16_t displayHeight = 480;
#else
  uint16_t displayWidth = 792;
  uint16_t displayHeight = 528;
#endif
  uint32_t displaySpiHz = 20000000;
};
inline Profile ACTIVE;
}  // namespace BoardConfig
