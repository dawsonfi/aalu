#include "BaseTheme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "I18n.h"
#include "RecentBooksStore.h"
#include "components/HomeRenderer.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int batteryPercentSpacing = 4;
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;

// Helper: draw battery icon at given position
void drawBatteryIcon(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight, uint16_t percentage) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);

  const bool charging = gpio.isUsbConnected();

  const int maxFillWidth = battWidth - 5;
  const int fillHeight = rectHeight - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }

  if (charging) {
    // Plugged in: fill the whole body solid black and put a centred white bolt
    // on top so the charging state reads as a clean icon, not a partial bar.
    renderer.fillRect(x + 2, y + 2, maxFillWidth, fillHeight);
    // Horizontal lightning bolt: 18 px wide × 7 px tall. Spans almost the full battery body
    // (~80%) so the orientation reads unambiguously. Two 2-px-thick horizontal strokes meet
    // at a 3-step stepped diagonal kink — the universal "Z-bolt" shape used on power adapters
    // and EV charging plugs.
    // Layout (X = white pixel on the black battery body):
    //   X X X X X X X X . . . .  . . . . . .       ← upper stroke (top, 2 px thick)
    //   X X X X X X X X . . . .  . . . . . .
    //   . . . . . . X X X X . .  . . . . . .       ← stepped diagonal kink
    //   . . . . . . . X X X X .  . . . . . .
    //   . . . . . . . . X X X X  . . . . . .
    //   . . . . . . . . . . X X  X X X X X X       ← lower stroke (bottom, 2 px thick)
    //   . . . . . . . . . . X X  X X X X X X
    constexpr int kBoltW = 18;
    constexpr int kBoltH = 7;
    const int boltX = x + (battWidth - kBoltW) / 2;
    const int boltY = y + (rectHeight - kBoltH) / 2;
    // Upper stroke (rows 0-1)
    renderer.drawLine(boltX + 0, boltY + 0, boltX + 7, boltY + 0, false);
    renderer.drawLine(boltX + 0, boltY + 1, boltX + 7, boltY + 1, false);
    // Stepped diagonal kink (rows 2-4)
    renderer.drawLine(boltX + 6, boltY + 2, boltX + 9, boltY + 2, false);
    renderer.drawLine(boltX + 7, boltY + 3, boltX + 10, boltY + 3, false);
    renderer.drawLine(boltX + 8, boltY + 4, boltX + 11, boltY + 4, false);
    // Lower stroke (rows 5-6)
    renderer.drawLine(boltX + 10, boltY + 5, boltX + 17, boltY + 5, false);
    renderer.drawLine(boltX + 10, boltY + 6, boltX + 17, boltY + 6, false);
    return;
  }

  // The +1 is to round up, so that we always fill at least one pixel.
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }
  renderer.fillRect(x + 2, y + 2, filledWidth, fillHeight);
}
}  // namespace

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + BaseMetrics::values.batteryWidth, rect.y,
                      percentageText.c_str());
  }

  drawBatteryIcon(renderer, rect.x, y, BaseMetrics::values.batteryWidth, rect.height, percentage);
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  // rect.x is already positioned for the icon (drawHeader calculated it)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    // Clear the area where we're going to draw the text to prevent ghosting
    const auto textHeight = renderer.getTextHeight(SMALL_FONT_ID);
    renderer.fillRect(rect.x - textWidth - batteryPercentSpacing, rect.y, textWidth, textHeight, false);
    // Draw text to the left of the icon
    renderer.drawText(SMALL_FONT_ID, rect.x - textWidth - batteryPercentSpacing, rect.y, percentageText.c_str());
  }

  // Icon is already at correct position from rect.x
  drawBatteryIcon(renderer, rect.x, y, BaseMetrics::values.batteryWidth, rect.height, percentage);
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = BaseMetrics::values.buttonHintsHeight;
  constexpr int buttonY = BaseMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  constexpr int buttonPositions[] = {25, 130, 245, 350};
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
    }
  }

  renderer.setOrientation(orig_orientation);
}

namespace {
void drawSolidDisc(const GfxRenderer& renderer, int cx, int cy, int radius) {
  const int rsq = radius * radius;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      if (dx * dx + dy * dy <= rsq) {
        renderer.drawPixel(cx + dx, cy + dy, true);
      }
    }
  }
}
}  // namespace

void BaseTheme::drawButtonHintsGlyphs(GfxRenderer& renderer, ButtonHintGlyphSet variant, uint8_t slotMask) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int bandHeight = BaseMetrics::values.buttonHintsHeight;
  const int bandTop = pageHeight - bandHeight;

  constexpr int kSlotCount = 4;
  constexpr int kGlyphHalf = 6;
  const int slotWidth = pageWidth / kSlotCount;
  const int glyphCy = bandTop + bandHeight / 2;

  for (int i = 0; i < kSlotCount; ++i) {
    if (!(slotMask & (1 << i))) continue;
    const int cx = i * slotWidth + slotWidth / 2;

    if (i == 0) {
      const int xPts[3] = {cx - kGlyphHalf, cx + kGlyphHalf, cx + kGlyphHalf};
      const int yPts[3] = {glyphCy, glyphCy - kGlyphHalf, glyphCy + kGlyphHalf};
      renderer.fillPolygon(xPts, yPts, 3, true);
      continue;
    }
    if (i == 1) {
      if (variant == ButtonHintGlyphSet::FontDownload) {
        // Down-arrow into a base line -- "download to device".
        renderer.drawLine(cx, glyphCy - 5, cx, glyphCy + 1, 2, true);
        const int headX[3] = {cx - 4, cx + 4, cx};
        const int headY[3] = {glyphCy - 1, glyphCy - 1, glyphCy + 4};
        renderer.fillPolygon(headX, headY, 3, true);
        renderer.drawLine(cx - 5, glyphCy + 5, cx + 5, glyphCy + 5, 2, true);
      } else if (variant == ButtonHintGlyphSet::FontDelete) {
        // Trash can -- destructive delete, deliberately not a download glyph.
        renderer.drawLine(cx - 5, glyphCy - 3, cx + 5, glyphCy - 3, 2, true);  // lid
        renderer.fillRect(cx - 2, glyphCy - 5, 4, 2, true);                    // handle
        renderer.drawRect(cx - 4, glyphCy - 2, 8, 9, true);                    // body
      } else {
        drawSolidDisc(renderer, cx, glyphCy, kGlyphHalf);
      }
      continue;
    }

    if (variant == ButtonHintGlyphSet::StatsActions) {
      if (i == 2) {
        // Three horizontal dots ("more").
        constexpr int kDotRadius = 1;
        constexpr int kDotSpacing = 5;
        for (int d = -1; d <= 1; ++d) {
          drawSolidDisc(renderer, cx + d * kDotSpacing, glyphCy, kDotRadius);
        }
      } else {
        // Two stacked horizontal arrows pointing opposite ways (swap/toggle).
        constexpr int kArrowHalfW = 5;
        constexpr int kArrowHalfH = 2;
        const int topY = glyphCy - 3;
        const int botY = glyphCy + 3;
        // Top arrow: shaft + right-pointing head.
        renderer.drawLine(cx - kArrowHalfW, topY, cx + kArrowHalfW - kArrowHalfH, topY);
        {
          const int xPts[3] = {cx + kArrowHalfW, cx + kArrowHalfW - kArrowHalfH, cx + kArrowHalfW - kArrowHalfH};
          const int yPts[3] = {topY, topY - kArrowHalfH, topY + kArrowHalfH};
          renderer.fillPolygon(xPts, yPts, 3, true);
        }
        // Bottom arrow: shaft + left-pointing head.
        renderer.drawLine(cx - kArrowHalfW + kArrowHalfH, botY, cx + kArrowHalfW, botY);
        {
          const int xPts[3] = {cx - kArrowHalfW, cx - kArrowHalfW + kArrowHalfH, cx - kArrowHalfW + kArrowHalfH};
          const int yPts[3] = {botY, botY - kArrowHalfH, botY + kArrowHalfH};
          renderer.fillPolygon(xPts, yPts, 3, true);
        }
      }
      continue;
    }

    // Default: navigation up/down triangles.
    if (i == 2) {
      const int xPts[3] = {cx - kGlyphHalf, cx + kGlyphHalf, cx};
      const int yPts[3] = {glyphCy + kGlyphHalf, glyphCy + kGlyphHalf, glyphCy - kGlyphHalf};
      renderer.fillPolygon(xPts, yPts, 3, true);
    } else {
      const int xPts[3] = {cx - kGlyphHalf, cx + kGlyphHalf, cx};
      const int yPts[3] = {glyphCy - kGlyphHalf, glyphCy - kGlyphHalf, glyphCy + kGlyphHalf};
      renderer.fillPolygon(xPts, yPts, 3, true);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonX = 4;                                             // Distance from right edge
  // Position for the button group - buttons share a border so they're adjacent
  constexpr int topButtonY = 345;  // Top button position

  const char* labels[] = {topBtn, bottomBtn};

  // Draw the shared border for both buttons as one unit
  const int x = screenWidth - buttonX - buttonWidth;

  // Draw top button outline (3 sides, bottom open)
  if (topBtn != nullptr && topBtn[0] != '\0') {
    renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);                                       // Top
    renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);                                      // Left
    renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);  // Right
  }

  // Draw shared middle border
  if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
    renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);  // Shared border
  }

  // Draw bottom button outline (3 sides, top is shared)
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);  // Left
    renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                      topButtonY + 2 * buttonHeight - 1);  // Right
    renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1,
                      topButtonY + 2 * buttonHeight - 1);  // Bottom
  }

  // Draw text for each button
  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int y = topButtonY + i * buttonHeight;

      // Draw rotated text centered in the button
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);

      // Center the rotated text in the button
      const int textX = x + (buttonWidth - textHeight) / 2;
      const int textY = y + (buttonHeight + textWidth) / 2;

      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i]);
    }
  }
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<std::string(int index)>& rowSection,
                         const std::function<ListToggleState(int index)>& rowToggle,
                         const std::function<bool(int index)>& rowAction, bool solidSelection,
                         const std::function<int(int index)>& rowSignal) const {
  (void)highlightValue;
  (void)solidSelection;
  int rowHeight =
      (rowSubtitle != nullptr) ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight;
  int pageItems = rect.height / rowHeight;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;  // Offset from right edge

    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int indicatorTop = rect.y;  // Offset to avoid overlapping side button hints
    const int indicatorBottom = rect.y + rect.height - arrowSize;

    // Draw up arrow at top (^) - narrow point at top, wide base at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + i * 2;
      const int startX = centerX - i;
      renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
    }

    // Draw down arrow at bottom (v) - wide base at top, narrow point at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
      const int startX = centerX - (arrowSize - 1 - i);
      renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                        indicatorBottom - arrowSize + 1 + i);
    }
  }

  // Draw selection
  int contentWidth = rect.width - 5;
  if (selectedIndex >= 0) {
    renderer.fillRect(0, rect.y + selectedIndex % pageItems * rowHeight - 2, rect.width, rowHeight);
  }
  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int textWidth = contentWidth - BaseMetrics::values.contentSidePadding * 2 - (rowValue != nullptr ? 60 : 0);

    if (rowSection != nullptr) {
      const std::string section = rowSection(i);
      const std::string prevSection = (i > 0) ? rowSection(i - 1) : std::string();
      if (!section.empty() && section != prevSection) {
        renderer.drawText(SMALL_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, itemY - rowHeight + 4,
                          section.c_str(), true, EpdFontFamily::REGULAR);
      }
    }

    // Draw name
    auto itemName = rowTitle(i);
    auto font = (rowSubtitle != nullptr) ? UI_12_FONT_ID : UI_10_FONT_ID;
    auto item = renderer.truncatedText(font, itemName.c_str(), textWidth);
    renderer.drawText(font, rect.x + BaseMetrics::values.contentSidePadding, itemY, item.c_str(), i != selectedIndex);

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(UI_10_FONT_ID, subtitleText.c_str(), textWidth);
      renderer.drawText(UI_10_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, itemY + 30, subtitle.c_str(),
                        i != selectedIndex);
    }

    const ListToggleState toggleState = (rowToggle != nullptr) ? rowToggle(i) : ListToggleState::NotToggle;
    const bool isAction = (rowAction != nullptr) && rowAction(i);

    if (toggleState != ListToggleState::NotToggle) {
      drawToggleSwitch(renderer, Rect{rect.x, itemY, contentWidth - BaseMetrics::values.contentSidePadding, rowHeight},
                       toggleState == ListToggleState::On, i == selectedIndex);
    } else if (isAction) {
      constexpr int chevronHalf = 5;
      const int chevronTipX = rect.x + contentWidth - BaseMetrics::values.contentSidePadding;
      const int chevronCy = itemY + rowHeight / 2 - 2;
      renderer.drawLine(chevronTipX - chevronHalf, chevronCy - chevronHalf, chevronTipX, chevronCy, i != selectedIndex);
      renderer.drawLine(chevronTipX - chevronHalf, chevronCy + chevronHalf, chevronTipX, chevronCy, i != selectedIndex);
    } else if (rowValue != nullptr || rowSignal != nullptr) {
      int valueRight = rect.x + contentWidth - BaseMetrics::values.contentSidePadding;
      if (rowSignal != nullptr) {
        const int level = rowSignal(i);
        if (level >= 0) {
          constexpr int barsWidth = 22;
          constexpr int barsHeight = 16;
          drawSignalBars(renderer, valueRight - barsWidth, itemY + 2, barsWidth, barsHeight, level, i != selectedIndex);
          valueRight -= barsWidth + 6;
        }
      }
      if (rowValue != nullptr) {
        std::string valueText = rowValue(i);
        const auto valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
        renderer.drawText(UI_10_FONT_ID, valueRight - valueTextWidth, itemY, valueText.c_str(), i != selectedIndex);
      }
    }
  }
}

void BaseTheme::drawSignalBars(const GfxRenderer& renderer, int x, int y, int width, int height, int level,
                               bool foreground) const {
  constexpr int numBars = 4;
  constexpr int gap = 2;
  const int barWidth = (width - gap * (numBars - 1)) / numBars;
  if (barWidth <= 0) {
    return;
  }
  for (int b = 0; b < numBars; b++) {
    const int barHeight = height * (b + 1) / numBars;
    const int barX = x + b * (barWidth + gap);
    const int barY = y + (height - barHeight);
    if (b < level) {
      renderer.fillRect(barX, barY, barWidth, barHeight, foreground);
    } else {
      renderer.drawRect(barX, barY, barWidth, barHeight, foreground);
    }
  }
}

void BaseTheme::drawToggleSwitch(const GfxRenderer& renderer, Rect rect, bool on, bool inverted) const {
  constexpr int trackHeight = 22;
  constexpr int trackWidth = 40;
  constexpr int knobInset = 2;

  const int trackX = rect.x + rect.width - trackWidth;
  const int trackY = rect.y + (rect.height - trackHeight) / 2;
  const int radius = trackHeight / 2;
  const int knobSize = trackHeight - knobInset * 2;
  const Color fg = inverted ? Color::White : Color::Black;
  const Color bg = inverted ? Color::Black : Color::White;

  if (on) {
    renderer.fillRoundedRect(trackX, trackY, trackWidth, trackHeight, radius, fg);
    const int knobX = trackX + trackWidth - knobInset - knobSize;
    renderer.fillRoundedRect(knobX, trackY + knobInset, knobSize, knobSize, knobSize / 2, bg);
  } else {
    renderer.drawRoundedRect(trackX, trackY, trackWidth, trackHeight, 1, radius, !inverted);
    const int knobX = trackX + knobInset;
    renderer.drawRoundedRect(knobX, trackY + knobInset, knobSize, knobSize, 1, knobSize / 2, !inverted);
  }
}

void BaseTheme::drawBottomSheetFrame(const GfxRenderer& renderer, Rect rect) const {
  constexpr int cornerRadius = 14;
  constexpr int grabberWidth = 44;
  constexpr int grabberHeight = 4;
  constexpr int grabberTopMargin = 8;

  renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cornerRadius, true, true, false, false,
                           Color::White);
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, cornerRadius, true, true, false, false, true);
  renderer.drawLine(rect.x, rect.y, rect.x + rect.width - 1, rect.y, true);

  const int grabberX = rect.x + (rect.width - grabberWidth) / 2;
  const int grabberY = rect.y + grabberTopMargin;
  renderer.fillRoundedRect(grabberX, grabberY, grabberWidth, grabberHeight, grabberHeight / 2, Color::Black);
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  // Hide last battery draw
  constexpr int maxBatteryWidth = 80;
  renderer.fillRect(rect.x + rect.width - maxBatteryWidth, rect.y + 5, maxBatteryWidth,
                    BaseMetrics::values.batteryHeight + 10, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - BaseMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, BaseMetrics::values.batteryWidth, BaseMetrics::values.batteryHeight},
                   showBatteryPercentage);

  if (title) {
    int padding = rect.width - batteryX + BaseMetrics::values.batteryWidth;
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title,
                                                 rect.width - padding * 2 - BaseMetrics::values.contentSidePadding * 2,
                                                 EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, rect.y + 5, truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(
        SMALL_FONT_ID, subtitle, rect.width - BaseMetrics::values.contentSidePadding * 2, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - BaseMetrics::values.contentSidePadding - truncatedSubtitleWidth, subtitleY,
                      truncatedSubtitle.c_str(), true);
  }
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline
  constexpr int maxListValueWidth = 200;

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  int rightSpace = BaseMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - BaseMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + 10;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_12_FONT_ID, label, rect.width - BaseMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, currentX, rect.y, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
}

void BaseTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected, bool solidSelection) const {
  (void)solidSelection;
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;

  for (const auto& tab : tabs) {
    const int textWidth =
        renderer.getTextWidth(UI_12_FONT_ID, tab.label, tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    // Draw underline for selected tab
    if (tab.selected) {
      if (selected) {
        renderer.fillRect(currentX - 3, rect.y, textWidth + 6, lineHeight + underlineGap);
      } else {
        renderer.fillRect(currentX, rect.y + lineHeight + underlineGap, textWidth, underlineHeight);
      }
    }

    // Draw tab label
    renderer.drawText(UI_12_FONT_ID, currentX, rect.y, tab.label, !(tab.selected && selected),
                      tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    currentX += textWidth + BaseMetrics::values.tabSpacing;
  }
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  constexpr int margin = 15;
  constexpr int y = 60;
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + margin * 2;
  const int h = textHeight + margin * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  renderer.fillRect(x - 2, y - 2, w + 4, h + 4, true);  // frame thickness 2
  renderer.fillRect(x, y, w, h, false);

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + margin - 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  constexpr int barHeight = 4;
  const int barWidth = layout.width - 30;  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - 10;

  int fillWidth = barWidth * progress / 100;

  renderer.fillRect(barX, barY, fillWidth, barHeight, true);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom, const int textYOffset,
                              const bool suppressClock) const {
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  // Draw Progress Text
  const auto screenHeight = renderer.getScreenHeight();
  auto textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom - 4;
  int progressTextWidth = 0;

  if (SETTINGS.statusBarBookProgressPercentage || SETTINGS.statusBarChapterPageCount) {
    // Right aligned text for progress counter
    char progressStr[32];

    if (SETTINGS.statusBarBookProgressPercentage && SETTINGS.statusBarChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage, pageCount, bookProgress);
    } else if (SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage, pageCount);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(
        SMALL_FONT_ID,
        renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight - progressTextWidth, textY,
        progressStr);
  }

  // Draw Progress Bar
  if (SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    const int progressBarMaxWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom -
                             ((SETTINGS.statusBarProgressBarThickness + 1) * 2) - paddingBottom;
    size_t progress;
    if (SETTINGS.statusBarProgressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progress = static_cast<size_t>(bookProgress);
    } else {
      // Chapter progress
      progress = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) * 100 : 0;
    }
    const int barWidth = progressBarMaxWidth * progress / 100;
    renderer.fillRect(orientedMarginLeft, progressBarY, barWidth, ((SETTINGS.statusBarProgressBarThickness + 1) * 2),
                      true);
  }

  // Draw Battery
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{metrics.statusBarHorizontalMargin + orientedMarginLeft + 1, textY, metrics.batteryWidth,
                             metrics.batteryHeight},
                        showBatteryPercentage);
  }

  const int batterySize = SETTINGS.statusBarBattery ? (showBatteryPercentage ? 50 : 20) : 0;

  // Draw Clock
  int clockLeftExtra = 0;
  int clockRightExtra = 0;
  if (!suppressClock && SETTINGS.statusBarClock != CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE &&
      halClock.isAvailable()) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      const int clockTextWidth = renderer.getTextWidth(SMALL_FONT_ID, timeBuf);
      const int clockRenderableWidth = renderer.getScreenWidth() - (metrics.statusBarHorizontalMargin * 2) -
                                       orientedMarginLeft - orientedMarginRight;
      const int clockX =
          metrics.statusBarHorizontalMargin + orientedMarginLeft + (clockRenderableWidth - clockTextWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, clockX, textY, timeBuf);
      title.clear();
    }
  }

  // Draw Title
  if (!title.empty()) {
    textY -= textYOffset;
    // Centered chapter title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth =
        renderer.getScreenWidth() - (metrics.statusBarHorizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;

    const int titleMarginLeft = batterySize + 30 + clockLeftExtra;
    const int titleMarginRight = progressTextWidth + 30 + clockRightExtra;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    int titleWidth;
    titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTitleSpace) {
      // Not enough space to center on the screen, center it within the remaining space instead
      availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + metrics.statusBarHorizontalMargin + orientedMarginLeft +
                          (availableTitleSpace - titleWidth) / 2,
                      textY, title.c_str());
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth) const {
  renderer.drawText(UI_12_FONT_ID, rect.x + 10, rect.y, "[");
  renderer.drawText(UI_12_FONT_ID, rect.x + rect.width - 15, rect.y + rect.height, "]");
}

void BaseTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label,
                                const bool isSelected) const {
  const int itemWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
  const int textX = rect.x + (rect.width - itemWidth) / 2;
  if (isSelected) {
    renderer.drawText(UI_10_FONT_ID, textX - 6, rect.y, "[");
    renderer.drawText(UI_10_FONT_ID, textX + itemWidth, rect.y, "]");
  }
  renderer.drawText(UI_10_FONT_ID, textX, rect.y, label);
}
