#include <gtest/gtest.h>

#include "activities/reader/ReaderResumeGate.h"

TEST(ReaderResumeGate, LoadOrBuildFailureDoesNotRememberBook) {
  ReaderResumeGate gate;
  EXPECT_FALSE(gate.takeRememberRequest());
  EXPECT_FALSE(gate.takeRememberRequest());
}

TEST(ReaderResumeGate, FirstSuccessfulPageIsRememberedExactlyOnce) {
  ReaderResumeGate gate;
  gate.markPageRendered();
  EXPECT_TRUE(gate.takeRememberRequest());
  EXPECT_FALSE(gate.takeRememberRequest());
  gate.markPageRendered();
  EXPECT_FALSE(gate.takeRememberRequest());
}

TEST(ReaderResumeGate, ExitCanConsumePendingFirstPage) {
  ReaderResumeGate gate;
  EXPECT_FALSE(gate.takeRememberRequest());
  gate.markPageRendered();
  EXPECT_TRUE(gate.takeRememberRequest());
}
