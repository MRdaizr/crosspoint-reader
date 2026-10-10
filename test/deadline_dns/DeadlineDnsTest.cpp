#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <type_traits>

#include "DeadlineDns.h"
#include "FakePlatform.h"

namespace {
class DeadlineDnsTest : public testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
  void TearDown() override {
    fake::onDelay = {};
    fake::drain();
  }
  NetworkManager manager;
  IPAddress result{123};

  int resolve(const char* host = "book.example") { return manager.hostByName(host, result); }
  void immediate() { fake::dnsError = ERR_OK; }
  void timeoutOne() {
    DeadlineDns::Guard guard(static_cast<uint32_t>(fake::now + 6));
    EXPECT_EQ(resolve(), 0);
    EXPECT_EQ(result.address, IPAddress(0).address);
  }
};

static_assert(!std::is_copy_constructible_v<DeadlineDns::Guard>);
static_assert(!std::is_move_constructible_v<DeadlineDns::Guard>);
static_assert(sizeof(DeadlineDns::Guard) < 256);

TEST_F(DeadlineDnsTest, OrdinaryTaskForwardsExactThisArgumentsReturnAndResult) {
  const char host[] = "ordinary.example";
  EXPECT_EQ(resolve(host), 17);
  EXPECT_EQ(fake::realManager, &manager);
  EXPECT_EQ(fake::realHost, host);
  EXPECT_EQ(result.address, fake::answer);
  fake::realReturn = -54;
  EXPECT_EQ(resolve(nullptr), -54);
  EXPECT_EQ(fake::realCalls, 2u);
  EXPECT_TRUE(fake::posts.empty());
  EXPECT_TRUE(fake::dnsHosts.empty());
}

TEST_F(DeadlineDnsTest, DestructionRestoresOrdinaryForwarding) {
  {
    DeadlineDns::Guard guard(0);
    EXPECT_EQ(resolve(), 0);
  }
  EXPECT_EQ(resolve(), 17);
  EXPECT_EQ(fake::realCalls, 1u);
}

TEST_F(DeadlineDnsTest, GuardIsBoundToItsTaskNotForeignTasks) {
  DeadlineDns::Guard guard(0);
  fake::task = fake::taskId(2);
  EXPECT_EQ(resolve(), 17);
  fake::task = fake::taskId(1);
  EXPECT_EQ(resolve(), 0);
  EXPECT_EQ(fake::realCalls, 1u);
}

TEST_F(DeadlineDnsTest, NestedGuardCannotExtendOuterDeadline) {
  DeadlineDns::Guard outer(7);
  DeadlineDns::Guard inner(100);
  EXPECT_EQ(resolve(), 0);
  EXPECT_EQ(fake::now, 7u);
  EXPECT_EQ(fake::waits, (std::vector<uint32_t>{5, 2}));
}

TEST_F(DeadlineDnsTest, NestedEarlierDeadlineWinsAndOuterIsRestored) {
  DeadlineDns::Guard outer(100);
  {
    DeadlineDns::Guard inner(6);
    EXPECT_EQ(resolve(), 0);
  }
  immediate();
  EXPECT_EQ(resolve(), 1);
  EXPECT_EQ(fake::realCalls, 0u);
}

TEST_F(DeadlineDnsTest, CancelledAncestorFailsClosedEvenWithLaterInnerDeadline) {
  DeadlineDns::Guard outer(100);
  DeadlineDns::Guard inner(200);
  outer.cancel();
  EXPECT_EQ(resolve(), 0);
  EXPECT_TRUE(fake::posts.empty());
}

TEST_F(DeadlineDnsTest, CancellationFromAnotherTaskDuringWaitRetainsCallbackSlot) {
  DeadlineDns::Guard guard(100);
  fake::onDelay = [&] {
    fake::task = fake::taskId(2);
    guard.cancel();
    fake::task = fake::taskId(1);
  };
  EXPECT_EQ(resolve(), 0);
  EXPECT_EQ(fake::now, 5u);
  ASSERT_EQ(fake::pending.size(), 1u);
  fake::onDelay = {};
  fake::deliverOne(&fake::answer);
  EXPECT_EQ(result.address, IPAddress(0).address);
}

TEST_F(DeadlineDnsTest, CancellationCallbackRunsOnGuardedTaskOutsideCriticalSection) {
  bool cancelled = false;
  DeadlineDns::Guard guard(
      100,
      [](void* ctx) {
        EXPECT_EQ(fake::task, fake::taskId(1));
        EXPECT_EQ(fake::criticalDepth, 0u);
        EXPECT_FALSE(fake::inTcpip);
        return *static_cast<bool*>(ctx);
      },
      &cancelled);
  fake::onDelay = [&] { cancelled = true; };
  EXPECT_EQ(resolve(), 0);
  EXPECT_EQ(fake::now, 5u);
  EXPECT_EQ(fake::realCalls, 0u);
}

TEST_F(DeadlineDnsTest, ParentCancellationCallbackCannotBeHiddenByNestedGuard) {
  bool stop = true;
  DeadlineDns::Guard outer(100, [](void* ctx) { return *static_cast<bool*>(ctx); }, &stop);
  DeadlineDns::Guard inner(200);
  EXPECT_EQ(resolve(), 0);
  EXPECT_TRUE(fake::posts.empty());
}

TEST_F(DeadlineDnsTest, ExpiredDeadlineRejectsEvenLiteralAndDoesNotPost) {
  fake::now = 20;
  DeadlineDns::Guard guard(20);
  EXPECT_EQ(resolve(), 0);
  EXPECT_EQ(resolve("192.0.2.42"), 0);
  EXPECT_TRUE(fake::posts.empty());
  EXPECT_EQ(result.address, IPAddress(0).address);
}

TEST_F(DeadlineDnsTest, PastOrHalfRangeAmbiguousDeadlineFailsClosed) {
  {
    DeadlineDns::Guard guard(0x80000000u);
    EXPECT_EQ(resolve(), 0);
  }
  fake::now = 100;
  {
    DeadlineDns::Guard guard(99);
    EXPECT_EQ(resolve(), 0);
  }
  EXPECT_TRUE(fake::posts.empty());
}

TEST_F(DeadlineDnsTest, AbsoluteDeadlineWorksAcrossUint32MillisWrap) {
  fake::now = 0xfffffffcu;
  DeadlineDns::Guard guard(3);
  EXPECT_EQ(resolve(), 0);
  EXPECT_EQ(static_cast<uint32_t>(fake::now), 3u);
  EXPECT_EQ(fake::waits, (std::vector<uint32_t>{5, 2}));
  ASSERT_EQ(fake::pending.size(), 1u);
  fake::deliverOne(&fake::answer);
}

TEST_F(DeadlineDnsTest, ZeroIsAnAbsoluteWrappedDeadlineNotUnlimitedSentinel) {
  fake::now = 0xfffffffcu;
  DeadlineDns::Guard guard(0);
  EXPECT_EQ(resolve(), 0);
  EXPECT_EQ(static_cast<uint32_t>(fake::now), 0u);
  EXPECT_EQ(fake::waits, (std::vector<uint32_t>{4}));
  EXPECT_TRUE(fake::dnsHosts.empty());
}

TEST_F(DeadlineDnsTest, PollsInFiveMsQuantaAndCapsFinalWait) {
  DeadlineDns::Guard guard(13);
  EXPECT_EQ(resolve(), 0);
  EXPECT_EQ(fake::waits, (std::vector<uint32_t>{5, 5, 3}));
  EXPECT_EQ(fake::now, 13u);
}

TEST_F(DeadlineDnsTest, Ipv4LiteralSucceedsWithoutTcpipPostOrDns) {
  DeadlineDns::Guard guard(100);
  EXPECT_EQ(resolve("192.0.2.42"), 1);
  EXPECT_EQ(result.address, fake::ipv4());
  EXPECT_TRUE(fake::posts.empty());
  EXPECT_TRUE(fake::waits.empty());
  EXPECT_EQ(fake::realCalls, 0u);
}

TEST_F(DeadlineDnsTest, Ipv6LiteralPreservesFamily) {
  DeadlineDns::Guard guard(100);
  EXPECT_EQ(resolve("fe80::1"), 1);
  EXPECT_EQ(result.address, fake::ipv6(0));
  EXPECT_TRUE(fake::posts.empty());
}

TEST_F(DeadlineDnsTest, NullEmptyAndOversizedHostAreRejectedWithoutDns) {
  DeadlineDns::Guard guard(100);
  const std::string longHost(254, 'a');
  EXPECT_EQ(resolve(nullptr), 0);
  EXPECT_EQ(resolve(""), 0);
  EXPECT_EQ(resolve(longHost.c_str()), 0);
  EXPECT_TRUE(fake::posts.empty());
  EXPECT_TRUE(fake::dnsHosts.empty());
  EXPECT_EQ(fake::realCalls, 0u);
}

TEST_F(DeadlineDnsTest, MaxLengthHostnameCopiedIntoStableQueuedSlot) {
  fake::autoStart = false;
  std::array<char, 254> host;
  host.fill('a');
  host.back() = '\0';
  {
    DeadlineDns::Guard guard(100);
    fake::onDelay = [&] {
      host.fill('b');
      host.back() = '\0';
      fake::startOne();
      fake::deliverOne(&fake::answer);
    };
    EXPECT_EQ(resolve(host.data()), 1);
  }
  ASSERT_EQ(fake::dnsHosts.size(), 1u);
  EXPECT_EQ(fake::dnsHosts[0], std::string(253, 'a'));
}

TEST_F(DeadlineDnsTest, CachedIpv4CompletesAndReleasesSlotWithoutFutureCallback) {
  immediate();
  DeadlineDns::Guard guard(100);
  for (int i = 0; i < 8; ++i) EXPECT_EQ(resolve(), 1);
  EXPECT_EQ(result.address, fake::answer);
  EXPECT_EQ(fake::dnsHosts.size(), 8u);
  EXPECT_TRUE(fake::pending.empty());
}

TEST_F(DeadlineDnsTest, CachedIpv6PreservesAddressFamilyAndZone) {
  immediate();
  fake::answer = fake::ipv6();
  DeadlineDns::Guard guard(100);
  EXPECT_EQ(resolve(), 1);
  EXPECT_EQ(result.address, fake::answer);
}

TEST_F(DeadlineDnsTest, AsyncCallbackCompletesAndReleasesSlot) {
  DeadlineDns::Guard guard(100);
  fake::onDelay = [] { fake::deliverOne(&fake::answer); };
  for (int i = 0; i < 8; ++i) EXPECT_EQ(resolve(), 1);
  EXPECT_EQ(result.address, fake::answer);
  EXPECT_EQ(fake::dnsHosts.size(), 8u);
  EXPECT_TRUE(fake::pending.empty());
}

TEST_F(DeadlineDnsTest, AsyncIpv6ResultPreservesScopeZone) {
  fake::answer = fake::ipv6(9);
  DeadlineDns::Guard guard(100);
  fake::onDelay = [] { fake::deliverOne(&fake::answer); };
  EXPECT_EQ(resolve(), 1);
  EXPECT_EQ(result.address, fake::answer);
}

TEST_F(DeadlineDnsTest, DnsImmediateErrorFailsClosedAndReleasesSlot) {
  fake::dnsError = ERR_ARG;
  DeadlineDns::Guard guard(100);
  for (int i = 0; i < 8; ++i) EXPECT_EQ(resolve(), 0);
  EXPECT_EQ(fake::dnsHosts.size(), 8u);
  EXPECT_TRUE(fake::pending.empty());
  EXPECT_EQ(fake::realCalls, 0u);
}

TEST_F(DeadlineDnsTest, NullDnsCallbackMeansFailureNotFakeSuccess) {
  DeadlineDns::Guard guard(100);
  fake::onDelay = [] { fake::deliverOne(nullptr); };
  EXPECT_EQ(resolve(), 0);
  EXPECT_EQ(result.address, IPAddress(0).address);
  EXPECT_EQ(fake::realCalls, 0u);
}

TEST_F(DeadlineDnsTest, TcpipQueueFailureNeverFallsBackAndDoesNotLeakPoolSlots) {
  fake::enqueueError = ERR_MEM;
  DeadlineDns::Guard guard(100);
  for (int i = 0; i < 8; ++i) EXPECT_EQ(resolve(), 0);
  EXPECT_TRUE(fake::posts.empty());
  EXPECT_TRUE(fake::waits.empty());
  EXPECT_EQ(fake::realCalls, 0u);
  fake::enqueueError = ERR_OK;
  immediate();
  EXPECT_EQ(resolve(), 1);
}

TEST_F(DeadlineDnsTest, QueuedStartAtDeadlineDoesNotIssueAnExpiredDnsRequest) {
  {
    DeadlineDns::Guard guard(5);
    EXPECT_EQ(resolve(), 0);
  }
  EXPECT_TRUE(fake::dnsHosts.empty());
  EXPECT_TRUE(fake::pending.empty());
  immediate();
  {
    DeadlineDns::Guard guard(100);
    EXPECT_EQ(resolve(), 1);
  }
}

TEST_F(DeadlineDnsTest, CallbackAtDeadlineIsRejectedAndCompletedSlotReleased) {
  {
    DeadlineDns::Guard guard(10);
    fake::onDelay = [] {
      if (fake::now == 10) fake::deliverOne(&fake::answer);
    };
    EXPECT_EQ(resolve(), 0);
    EXPECT_EQ(result.address, IPAddress(0).address);
  }
  fake::onDelay = {};
  immediate();
  {
    DeadlineDns::Guard guard(100);
    EXPECT_EQ(resolve(), 1);
  }
}

TEST_F(DeadlineDnsTest, FourTimedOutResolvingSlotsStayReservedUntilLateCallbacks) {
  for (int i = 0; i < 4; ++i) timeoutOne();
  ASSERT_EQ(fake::pending.size(), 4u);
  const auto waitCount = fake::waits.size();
  {
    DeadlineDns::Guard guard(100);
    EXPECT_EQ(resolve(), 0);
  }
  EXPECT_EQ(fake::waits.size(), waitCount);
  EXPECT_EQ(fake::dnsHosts.size(), 4u);
  EXPECT_EQ(fake::realCalls, 0u);
  fake::deliverOne(&fake::answer);
  immediate();
  {
    DeadlineDns::Guard guard(100);
    EXPECT_EQ(resolve(), 1);
  }
  EXPECT_EQ(fake::pending.size(), 3u);
}

TEST_F(DeadlineDnsTest, TimedOutQueuedSlotsStayReservedUntilPostedStartRuns) {
  fake::autoStart = false;
  for (int i = 0; i < 4; ++i) timeoutOne();
  ASSERT_EQ(fake::posts.size(), 4u);
  {
    DeadlineDns::Guard guard(100);
    EXPECT_EQ(resolve(), 0);
  }
  EXPECT_TRUE(fake::dnsHosts.empty());
  fake::startOne();
  EXPECT_TRUE(fake::dnsHosts.empty());
  fake::autoStart = true;
  immediate();
  {
    DeadlineDns::Guard guard(100);
    EXPECT_EQ(resolve(), 1);
  }
  EXPECT_EQ(fake::dnsHosts.size(), 1u);
}

TEST_F(DeadlineDnsTest, LateCallbackCannotWriteCallerResultOrAReusedSlot) {
  timeoutOne();
  ASSERT_EQ(fake::pending.size(), 1u);
  void* abandoned = fake::pending.front().argument;
  fake::onDelay = [&] {
    ASSERT_EQ(fake::pending.size(), 2u);
    EXPECT_NE(fake::pending.back().argument, abandoned);
    const auto staleAnswer = fake::ipv4(99);
    fake::deliverOne(&staleAnswer);
    fake::deliverOne(&fake::answer);
  };
  {
    DeadlineDns::Guard guard(100);
    EXPECT_EQ(resolve(), 1);
  }
  EXPECT_EQ(result.address, fake::answer);
}

TEST_F(DeadlineDnsTest, LateCallbackAfterGuardAndResultDestructionHasNoDanglingAccess) {
  {
    IPAddress temporary;
    DeadlineDns::Guard guard(6);
    EXPECT_EQ(manager.hostByName("temporary.example", temporary), 0);
  }
  ASSERT_EQ(fake::pending.size(), 1u);
  fake::deliverOne(&fake::answer);
  EXPECT_EQ(resolve(), 17);
}

TEST_F(DeadlineDnsTest, LiteralStillWorksWhenAllAsyncSlotsAreQuarantined) {
  for (int i = 0; i < 4; ++i) timeoutOne();
  DeadlineDns::Guard guard(100);
  EXPECT_EQ(resolve("192.0.2.42"), 1);
  EXPECT_EQ(fake::pending.size(), 4u);
  EXPECT_EQ(result.address, fake::answer);
}

TEST_F(DeadlineDnsTest, RepeatedTimeoutCallbackReuseKeepsPoolUsableAcrossWrap) {
  fake::now = 0xfffffff0u;
  for (int i = 0; i < 40; ++i) {
    timeoutOne();
    ASSERT_EQ(fake::pending.size(), 1u);
    fake::deliverOne(i % 2 ? &fake::answer : nullptr);
  }
  EXPECT_EQ(fake::dnsHosts.size(), 40u);
  immediate();
  {
    DeadlineDns::Guard guard(static_cast<uint32_t>(fake::now + 20));
    EXPECT_EQ(resolve(), 1);
  }
}
}  // namespace
