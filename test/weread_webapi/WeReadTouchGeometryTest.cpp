#include <gtest/gtest.h>

#include "activities/apps/weread/WeReadTouchGeometry.h"

TEST(WeReadTouchGeometry, MapsSingleBookPageAndRejectsGaps) {
  const Rect content{0, 0, 480, 700};
  const WeReadShelfGridLayout layout{
      .columns = 1,
      .rows = 1,
      .itemsPerPage = 1,
      .coverWidth = 100,
      .coverHeight = 140,
      .itemHeight = 160,
      .columnGap = 0,
      .rowGap = 0,
      .availableX = 20,
      .availableWidth = 440,
  };

  // The migrated WeRead shelf intentionally remains the firmware's 1×1
  // layout; touch support must not silently adopt crossmux's multi-cover grid.
  EXPECT_EQ(layout.columns, 1);
  EXPECT_EQ(layout.rows, 1);
  EXPECT_EQ(layout.itemsPerPage, 1);

  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 0, 3, 190, 350), 0);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 1, 3, 190, 350), 1);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 0, 3, 100, 350), -1);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 0, 3, 190, 250), -1);
}

TEST(WeReadTouchGeometry, MapsSingleBookPageOffset) {
  const Rect content{0, 0, 800, 420};
  const WeReadShelfGridLayout layout{
      .columns = 1,
      .rows = 1,
      .itemsPerPage = 1,
      .coverWidth = 100,
      .coverHeight = 140,
      .itemHeight = 160,
      .columnGap = 0,
      .rowGap = 0,
      .availableX = 20,
      .availableWidth = 760,
  };

  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 5, 12, 350, 130), 5);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 6, 12, 350, 130), 6);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 5, 12, 300, 130), -1);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 0, 0, 350, 130), -1);
}
