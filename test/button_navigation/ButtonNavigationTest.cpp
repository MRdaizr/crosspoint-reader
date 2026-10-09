#include <ButtonNavigator.h>
#include <gtest/gtest.h>
class ButtonNavigationTest : public testing::Test {
 protected:
  MappedInputManager input;
  ButtonNavigator nav;
  void SetUp() override {
    navigatorTestMillis = 0;
    ButtonNavigator::setMappedInputManager(input);
  }
};
TEST_F(ButtonNavigationTest, PressMovesExactlyOnceEvenWithOldHeldDuration) {
  int moves = 0;
  navigatorTestMillis = 1000;
  input.heldMs = 1000;
  input.down[0] = input.pressed[0] = true;
  nav.onNext([&] { ++moves; });
  EXPECT_EQ(1, moves);
}
TEST_F(ButtonNavigationTest, ShortReleaseDoesNotMovePressBasedListAgain) {
  int moves = 0;
  input.down[0] = input.pressed[0] = true;
  nav.onNext([&] { ++moves; });
  input.down[0] = input.pressed[0] = false;
  input.released[0] = true;
  nav.onNext([&] { ++moves; });
  EXPECT_EQ(1, moves);
}
TEST_F(ButtonNavigationTest, HoldRepeatsAtIntervalsAndReleaseDoesNotRepeat) {
  int moves = 0;
  input.down[0] = input.pressed[0] = true;
  nav.onNext([&] { ++moves; });
  input.pressed[0] = false;
  input.heldMs = navigatorTestMillis = 600;
  nav.onNext([&] { ++moves; });
  EXPECT_EQ(2, moves);
  input.heldMs = navigatorTestMillis = 900;
  nav.onNext([&] { ++moves; });
  EXPECT_EQ(2, moves);
  input.down[0] = false;
  input.released[0] = true;
  input.heldMs = navigatorTestMillis = 1500;
  nav.onNext([&] { ++moves; });
  EXPECT_EQ(2, moves);
}
TEST_F(ButtonNavigationTest, ExistingReleaseBasedScreensStillSuppressAfterHold) {
  int moves = 0;
  input.down[0] = true;
  input.heldMs = navigatorTestMillis = 600;
  nav.onNextRelease([&] { ++moves; });
  nav.onNextContinuous([&] { ++moves; });
  EXPECT_EQ(1, moves);
  input.down[0] = false;
  input.released[0] = true;
  nav.onNextRelease([&] { ++moves; });
  nav.onNextContinuous([&] { ++moves; });
  EXPECT_EQ(1, moves);
}
TEST_F(ButtonNavigationTest, PreviousAndMultipleButtonsUseLogicalMapping) {
  int moves = 0;
  input.pressed[1] = input.down[1] = true;
  nav.onPreviousPress([&] { --moves; });
  EXPECT_EQ(-1, moves);
  input.pressed[1] = false;
  input.pressed[2] = true;
  nav.onPressAndContinuous({MappedInputManager::Button::Left, MappedInputManager::Button::Right}, [&] { ++moves; });
  EXPECT_EQ(0, moves);
}
