#include "CatalogScreens.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <numeric>

#include "UITheme.h"

namespace fui = freeink::ui;

void catalogScreenHeader(UiAppHost::UiScreen& screen, const GfxRenderer& renderer, const char* title,
                         const fui::BitmapRef& trailingIcon, const fui::ActionId trailingAction,
                         const fui::ActionId backAction) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto frameRect = screen.frame().screen();
  GUI.drawHeader(renderer, Rect{frameRect.x, metrics.topPadding, frameRect.width, metrics.headerHeight}, title);
  if (backAction != fui::NO_ACTION && screen.device().hasTouch) {
    fui::ButtonProps back;
    back.label = tr(STR_BACK);
    back.action = backAction;
    const int16_t size = screen.theme().minTouchSize;
    screen.button(back, fui::Rect{static_cast<int16_t>(frameRect.x + 4), static_cast<int16_t>(metrics.topPadding),
                                  static_cast<int16_t>(size * 2), size});
  }
  if (trailingIcon && trailingAction != fui::NO_ACTION) {
    fui::ButtonProps search;
    search.icon = trailingIcon;
    search.action = trailingAction;
    const int16_t size = screen.theme().minTouchSize;
    screen.button(search, fui::Rect{static_cast<int16_t>(frameRect.x + frameRect.width - size - 4),
                                    static_cast<int16_t>(metrics.topPadding), size, size});
  }
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
}

void catalogCenteredBlock(UiAppHost::UiScreen& screen, const std::initializer_list<CatalogLine> lines) {
  const int count = static_cast<int>(lines.size());
  if (count == 0) return;
  // Long lines (sign-in hints, server errors) wrap instead of clipping.
  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  centered.maxLines = 4;
  const int16_t gap = screen.theme().spaceMd;
  const int16_t pad = static_cast<int16_t>(UITheme::getInstance().getMetrics().contentSidePadding);
  const int16_t width = static_cast<int16_t>(screen.body().width - 2 * pad);
  const auto styleOf = [&](const CatalogLine& line) {
    fui::TextStyle style = centered;
    style.bold = line.bold;
    return style;
  };
  const auto heightOf = [&](const CatalogLine& line) {
    const int16_t h = fui::measureWrappedText(screen.target(), line.text ? line.text : "", styleOf(line), width).height;
    return std::max(h, screen.target().lineHeight(centered.font));
  };
  const int blockH = std::accumulate(lines.begin(), lines.end(), gap * (count - 1),
                                     [&](const int sum, const CatalogLine& line) { return sum + heightOf(line); });
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
  int i = 0;
  for (const CatalogLine& line : lines) {
    const fui::Rect rect = screen.takeTop(heightOf(line), ++i < count ? gap : 0).inset(fui::Insets{0, pad, 0, pad});
    screen.target().text(rect, line.text ? line.text : "", styleOf(line));
  }
}

void catalogDownloadScreen(UiAppHost::UiScreen& screen, const char* status, const size_t progress, const size_t total,
                           const fui::ActionId cancelAction) {
  // Centered block: status line, item title, progress, optional cancel.
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const bool showBytes = total == 0;
  const int16_t progressH = showBytes ? lh : 16;
  const bool withCancel = cancelAction != fui::NO_ACTION;
  const int16_t btnH = withCancel ? theme.rowHeight : 0;
  // The item title gets two padded lines: long book titles wrap once and then
  // truncate, instead of running edge-to-edge in a single clipped line.
  fui::TextStyle title = centered;
  title.maxLines = 2;
  const int16_t titleH = static_cast<int16_t>(lh * 2);
  const int16_t pad = static_cast<int16_t>(UITheme::getInstance().getMetrics().contentSidePadding);
  const int16_t blockH = static_cast<int16_t>(lh + titleH + progressH + btnH + gap * (withCancel ? 3 : 2));
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_DOWNLOADING), centered);
  screen.target().text(screen.takeTop(titleH, gap).inset(fui::Insets{0, pad, 0, pad}), status, title);

  const fui::Rect progressArea = screen.takeTop(progressH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (!showBytes) {
    fui::ProgressBarProps progressProps;
    progressProps.value = static_cast<int32_t>(progress);
    progressProps.max = static_cast<int32_t>(total);
    progressProps.border = fui::Paint::solid(fui::Color::Black);
    progressProps.borderWidth = 1;
    fui::progressBar(screen.frame(), progressArea, progressProps);
  } else {
    char bytes[24];
    if (progress >= 1024 * 1024) {
      snprintf(bytes, sizeof(bytes), "%u.%u MB", static_cast<unsigned>(progress >> 20),
               static_cast<unsigned>((progress >> 10) % 1024 * 10 / 1024));
    } else {
      snprintf(bytes, sizeof(bytes), "%u KB", static_cast<unsigned>(progress >> 10));
    }
    screen.target().text(progressArea, bytes, centered);
  }

  if (withCancel) {
    const fui::Rect btnArea = screen.takeTop(btnH);
    const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
    fui::ButtonProps cancel;
    cancel.label = tr(STR_CANCEL);
    cancel.action = cancelAction;
    screen.button(cancel,
                  fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
  }
}
