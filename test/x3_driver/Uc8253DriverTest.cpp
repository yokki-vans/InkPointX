#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

#include "freeink-sdk/libs/display/FreeInkDisplay/src/driver/Uc8253X3Driver.h"

namespace {
unsigned long clockMs;
const uint8_t* bank;
bool powerOk = true;
bool finishOk = true;
int oldWrites;
std::vector<uint8_t> commands;
std::array<uint8_t, 52272> frame{};
}  // namespace
unsigned long millis() { return clockMs; }
void delay(unsigned long ms) { clockMs += ms; }
int digitalRead(int) { return LOW; }
namespace freeink {
void EpdBus::reset(uint16_t) {}
void EpdBus::cmd(uint8_t c) { commands.push_back(c); }
void EpdBus::data(uint8_t) {}
void EpdBus::data(const uint8_t*, uint16_t) {}
void EpdBus::cmdData(uint8_t c, const uint8_t* p, uint16_t) {
  if (c == 0x20) bank = p;
}
void EpdBus::cmdData2(uint8_t, uint8_t, uint8_t) {}
void EpdBus::beginTxn() {}
void EpdBus::endTxn() {}
void EpdBus::rawWriteBytes(const uint8_t*, uint16_t) {}
void EpdBus::fillPlane(uint8_t c, uint8_t, uint16_t, uint16_t) { cmd(c); }
void EpdBus::sendPlaneFlipped(uint8_t c, const uint8_t*, uint16_t, uint16_t) {
  cmd(c);
  if (c == 0x10) ++oldWrites;
}
bool EpdBus::waitBusy(const char*) { return powerOk; }
bool EpdBus::waitForBusyStart(uint32_t, const char*) { return powerOk; }
bool EpdBus::waitRefreshComplete(const char*) { return finishOk; }
}  // namespace freeink
class Uc8253DriverTest : public ::testing::Test {
 protected:
  freeink::EpdBus bus;
  freeink::Uc8253X3Driver driver;
  void SetUp() override {
    clockMs = 0;
    powerOk = finishOk = true;
    driver.begin(bus);
    driver.skipInitialResync();
    driver.display(bus, frame.data(), nullptr, freeink::RefreshMode::Full, false);
    commands.clear();
    oldWrites = 0;
  }
  bool start(freeink::RefreshMode mode = freeink::RefreshMode::Fast) {
    return driver.displayStart(bus, frame.data(), nullptr, mode, false);
  }
};
TEST_F(Uc8253DriverTest, FullRequestIsNotDowngradedWhenPowerIsOff) {
  driver.deepSleep(bus);
  ASSERT_TRUE(start(freeink::RefreshMode::Full));
  EXPECT_EQ(bank, freeink::uc8253X3DefaultConfig().full.vcom);
}
TEST_F(Uc8253DriverTest, WakeMustReestablishOldRamDespiteSkippingBootClears) {
  driver.begin(bus);
  driver.skipInitialResync();
  ASSERT_TRUE(start());
  EXPECT_EQ(bank, freeink::uc8253X3DefaultConfig().full.vcom);
}
TEST_F(Uc8253DriverTest, ResetCancelsUnfinishedSubmission) {
  ASSERT_TRUE(start());
  driver.begin(bus);
  driver.displayFinish(bus, frame.data());
  EXPECT_EQ(oldWrites, 0);
}
TEST_F(Uc8253DriverTest, AbandonedGrayscalePlanesForceCleanFrame) {
  driver.copyGrayscaleLsb(bus, frame.data());
  ASSERT_TRUE(start());
  EXPECT_EQ(bank, freeink::uc8253X3DefaultConfig().full.vcom);
}
TEST_F(Uc8253DriverTest, CompletionFailureDoesNotCommitOldPlane) {
  ASSERT_TRUE(start());
  finishOk = false;
  driver.displayFinish(bus, frame.data());
  EXPECT_EQ(oldWrites, 0);
  finishOk = true;
  ASSERT_TRUE(start());
  EXPECT_EQ(bank, freeink::uc8253X3DefaultConfig().full.vcom);
}
TEST_F(Uc8253DriverTest, FailedGrayscaleBaseDoesNotCommitOldPlane) {
  driver.deepSleep(bus);
  commands.clear();
  powerOk = false;
  driver.displayGrayscaleBase(bus, frame.data(), freeink::RefreshMode::Fast, false);
  EXPECT_EQ(oldWrites, 0);
  EXPECT_EQ(std::count(commands.begin(), commands.end(), 0x12), 0);
}
