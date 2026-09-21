#include <freertos/semphr.h>
#include <gtest/gtest.h>

#include "freeink-sdk/libs/display/FreeInkDisplay/src/bus/EpdBus.h"

namespace {
unsigned long nowMs, activeAt, doneAt;
int workingLevel;
bool allocateSemaphore = false;
bool token;
void (*interruptHandler)() = nullptr;
int begins, ends, slices;
bool slice(int8_t, uint8_t) {
  ++slices;
  delay(1);
  return true;
}
}  // namespace
unsigned long millis() { return nowMs; }
int digitalRead(int) { return nowMs >= activeAt && nowMs < doneAt ? workingLevel : !workingLevel; }
void delay(unsigned long ms) {
  const bool wasBusy = digitalRead(6) == workingLevel;
  nowMs += ms;
  if (wasBusy && digitalRead(6) != workingLevel && interruptHandler) interruptHandler();
}
void attachInterrupt(int, void (*fn)(), int) { interruptHandler = fn; }
void detachInterrupt(int) { interruptHandler = nullptr; }
SemaphoreHandle_t xSemaphoreCreateBinary() { return allocateSemaphore ? &token : nullptr; }
int xSemaphoreTake(SemaphoreHandle_t, unsigned long timeout) {
  for (unsigned long t = 0; !token && t < timeout; ++t) delay(1);
  if (!token) return pdFALSE;
  token = false;
  return pdTRUE;
}
void xSemaphoreGiveFromISR(SemaphoreHandle_t, BaseType_t*) { token = true; }

class EpdBusTest : public ::testing::Test {
 protected:
  freeink::EpdBus bus;
  void SetUp() override {
    nowMs = 0;
    activeAt = 100;
    doneAt = 500;
    workingLevel = LOW;
    token = false;
    begins = ends = slices = 0;
    bus.begin({8, 10, 21, 4, 5, 6}, 20000000, freeink::BusyPolarity::X3TwoPhase);
  }
  void hooked() {
    bus.setBusyWaitSliceHook(slice);
    bus.setBusyWaitHooks([] { ++begins; }, [] { ++ends; });
  }
};
TEST_F(EpdBusTest, PollCompletionAfterAlreadyFinishedDoesNotWaitForSecondStart) {
  hooked();
  ASSERT_TRUE(bus.waitForBusyStart(1000));
  nowMs = 600;
  EXPECT_TRUE(bus.waitRefreshComplete());
  EXPECT_EQ(nowMs, 600u);
  EXPECT_TRUE(bus.waitHealthy());
}
TEST_F(EpdBusTest, PollCompletionDuringWaveformBalancesPowerHooks) {
  hooked();
  ASSERT_TRUE(bus.waitForBusyStart(1000));
  EXPECT_TRUE(bus.waitRefreshComplete());
  EXPECT_EQ(nowMs, doneAt);
  EXPECT_EQ(begins, 1);
  EXPECT_EQ(ends, 1);
  EXPECT_GT(slices, 0);
}
TEST_F(EpdBusTest, CompletionTokenCannotValidateAnotherUnstartedRefresh) {
  hooked();
  ASSERT_TRUE(bus.waitForBusyStart(1000));
  nowMs = 600;
  ASSERT_TRUE(bus.waitRefreshComplete());
  EXPECT_FALSE(bus.waitRefreshComplete());
  EXPECT_FALSE(bus.waitHealthy());
}
TEST_F(EpdBusTest, NoSemaphoreStillCompletesPreviouslyObservedRefresh) {
  ASSERT_TRUE(bus.waitForBusyStart(1000));
  nowMs = 600;
  EXPECT_TRUE(bus.waitRefreshComplete());
  EXPECT_EQ(nowMs, 600u);
}
TEST_F(EpdBusTest, MissingStartIsFailureAndResetClearsEvidence) {
  ASSERT_TRUE(bus.waitForBusyStart(1000));
  bus.reset();
  nowMs = 600;
  EXPECT_FALSE(bus.waitForBusyStart(50));
  EXPECT_FALSE(bus.waitHealthy());
}
TEST_F(EpdBusTest, X4ActiveHighKeepsItsPolarity) {
  workingLevel = HIGH;
  bus.begin({8, 10, 21, 4, 5, 6}, 20000000, freeink::BusyPolarity::ActiveHigh);
  hooked();
  ASSERT_TRUE(bus.waitForBusyStart(1000));
  EXPECT_TRUE(bus.waitRefreshComplete());
  EXPECT_EQ(nowMs, doneAt);
}
TEST_F(EpdBusTest, CompletionTimeoutPreservesFailure) {
  hooked();
  ASSERT_TRUE(bus.waitForBusyStart(1000));
  doneAt = 40000;
  EXPECT_FALSE(bus.waitRefreshComplete());
  EXPECT_FALSE(bus.waitHealthy());
  EXPECT_EQ(begins, ends);
}

TEST_F(EpdBusTest, InterruptCompletionAfterAlreadyFinishedConsumesStart) {
  allocateSemaphore = true;
  bus.begin({8, 10, 21, 4, 5, 6}, 20000000, freeink::BusyPolarity::X3TwoPhase);
  ASSERT_TRUE(bus.waitForBusyStart(1000));
  nowMs = 600;
  EXPECT_TRUE(bus.waitRefreshComplete());
  EXPECT_EQ(nowMs, 600u);
  EXPECT_FALSE(bus.waitRefreshComplete());
}
TEST_F(EpdBusTest, InterruptCompletionWaitsForTheEndEdge) {
  allocateSemaphore = true;
  bus.begin({8, 10, 21, 4, 5, 6}, 20000000, freeink::BusyPolarity::X3TwoPhase);
  ASSERT_TRUE(bus.waitForBusyStart(1000));
  EXPECT_TRUE(bus.waitRefreshComplete());
  EXPECT_EQ(nowMs, doneAt);
  EXPECT_TRUE(bus.waitHealthy());
}
