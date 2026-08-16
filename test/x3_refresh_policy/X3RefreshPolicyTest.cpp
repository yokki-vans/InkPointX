#include <gtest/gtest.h>

#include "freeink-sdk/libs/display/FreeInkDisplay/src/driver/X3RefreshPolicy.h"

TEST(X3RefreshPolicy, AllowsSlowBusyAssertionWithoutPrematureRecovery) {
  EXPECT_GE(freeink::x3_refresh::BUSY_START_TIMEOUT_MS, 1000U);
}

TEST(X3RefreshPolicy, UsesOtpPartialOnlyWithAValidOldPlane) {
  using freeink::x3_refresh::useOtpPartial;

  EXPECT_TRUE(useOtpPartial(/*fastRequested=*/true, /*oldPlaneSynced=*/true, /*fullSyncForced=*/false));
  EXPECT_FALSE(useOtpPartial(/*fastRequested=*/true, /*oldPlaneSynced=*/false, /*fullSyncForced=*/false));
  EXPECT_FALSE(useOtpPartial(/*fastRequested=*/false, /*oldPlaneSynced=*/true, /*fullSyncForced=*/false));
  EXPECT_FALSE(useOtpPartial(/*fastRequested=*/true, /*oldPlaneSynced=*/true, /*fullSyncForced=*/true));
}
