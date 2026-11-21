#include <gtest/gtest.h>

#include "util/PowerDoublePress.h"

namespace {

using Event = PowerDoublePress::Event;

PowerDoublePress::Input idle(const uint32_t now) {
  PowerDoublePress::Input input;
  input.now = now;
  return input;
}

PowerDoublePress::Input press(const uint32_t now) {
  PowerDoublePress::Input input = idle(now);
  input.pressed = true;
  return input;
}

PowerDoublePress::Input shortRelease(const uint32_t now, const uint32_t heldMs = 80, const bool canStart = true) {
  PowerDoublePress::Input input = idle(now);
  input.shortRelease = true;
  input.heldMs = heldMs;
  input.canStart = canStart;
  return input;
}

TEST(PowerDoublePressTest, IgnoresInputUntilAShortClickCanStartAGesture) {
  PowerDoublePress detector;
  EXPECT_EQ(detector.update(idle(0)), Event::None);
  // A press on its own is not a click yet. Long presses never report a short
  // release, so they never reach the detector.
  EXPECT_EQ(detector.update(press(10)), Event::None);
  EXPECT_FALSE(detector.pending());
}

TEST(PowerDoublePressTest, ScreensThatCannotStartAGestureKeepTheClick) {
  PowerDoublePress detector;
  EXPECT_EQ(detector.update(shortRelease(100, 80, false)), Event::None);
  EXPECT_FALSE(detector.pending());
}

TEST(PowerDoublePressTest, SingleClickIsReplayedAfterTheWindowWithItsHeldTime) {
  PowerDoublePress detector;
  EXPECT_EQ(detector.update(shortRelease(1000, 120)), Event::HoldClick);
  EXPECT_TRUE(detector.pending());

  // Still inside the window: keep waiting for a second press.
  EXPECT_EQ(detector.update(idle(1000 + PowerDoublePress::WINDOW_MS)), Event::None);

  EXPECT_EQ(detector.update(idle(1000 + PowerDoublePress::WINDOW_MS + 1)), Event::ReplayClick);
  EXPECT_EQ(detector.heldMs(), 120u);
  EXPECT_FALSE(detector.pending());
  EXPECT_EQ(detector.update(idle(2000)), Event::None);
}

TEST(PowerDoublePressTest, SecondPressInsideTheWindowIsADoublePress) {
  PowerDoublePress detector;
  ASSERT_EQ(detector.update(shortRelease(500)), Event::HoldClick);
  EXPECT_EQ(detector.update(idle(600)), Event::None);
  EXPECT_EQ(detector.update(press(500 + PowerDoublePress::WINDOW_MS)), Event::DoublePress);
  EXPECT_FALSE(detector.pending());
  // The held click is never replayed after a double press.
  EXPECT_EQ(detector.update(idle(2000)), Event::None);
}

TEST(PowerDoublePressTest, PressAfterTheWindowReplaysTheFirstClickInstead) {
  PowerDoublePress detector;
  ASSERT_EQ(detector.update(shortRelease(0)), Event::HoldClick);
  // A queued press that arrives late, by its own sample time, is not part of
  // the double press even if the loop only sees it now.
  EXPECT_EQ(detector.update(press(PowerDoublePress::WINDOW_MS + 5)), Event::ReplayClick);
  EXPECT_FALSE(detector.pending());
}

TEST(PowerDoublePressTest, WholeClickInOneFrameStillCountsAsTheSecondPress) {
  // Shared Confirm/Power hardware can report press and release together.
  PowerDoublePress detector;
  ASSERT_EQ(detector.update(shortRelease(0)), Event::HoldClick);
  PowerDoublePress::Input pulse = shortRelease(150);
  pulse.pressed = true;
  EXPECT_EQ(detector.update(pulse), Event::DoublePress);
  EXPECT_FALSE(detector.pending());
}

TEST(PowerDoublePressTest, ThirdClickStartsANewGesture) {
  PowerDoublePress detector;
  ASSERT_EQ(detector.update(shortRelease(0)), Event::HoldClick);
  ASSERT_EQ(detector.update(press(100)), Event::DoublePress);
  EXPECT_EQ(detector.update(shortRelease(400)), Event::HoldClick);
  EXPECT_EQ(detector.update(idle(400 + PowerDoublePress::WINDOW_MS + 1)), Event::ReplayClick);
}

TEST(PowerDoublePressTest, ClickTooOldToReplayIsDropped) {
  // The loop can skip the detector while another handler owns input. A click
  // from long ago must not fire when the detector runs again.
  PowerDoublePress detector;
  ASSERT_EQ(detector.update(shortRelease(0)), Event::HoldClick);
  EXPECT_EQ(detector.update(idle(PowerDoublePress::STALE_MS + 1)), Event::None);
  EXPECT_FALSE(detector.pending());
}

TEST(PowerDoublePressTest, ResetDropsAPendingClick) {
  PowerDoublePress detector;
  ASSERT_EQ(detector.update(shortRelease(0)), Event::HoldClick);
  detector.reset();
  EXPECT_EQ(detector.update(idle(PowerDoublePress::WINDOW_MS + 1)), Event::None);
}

TEST(PowerDoublePressTest, HandlesMillisecondCounterWraparound) {
  PowerDoublePress detector;
  const uint32_t nearWrap = UINT32_MAX - 50;
  ASSERT_EQ(detector.update(shortRelease(nearWrap)), Event::HoldClick);
  EXPECT_EQ(detector.update(press(nearWrap + 200)), Event::DoublePress);
}

}  // namespace
