#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/HomeRenderer.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
#include "components/icons/cover.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int batteryPercentSpacing = 4;
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int popupMarginX = 16;
constexpr int popupMarginY = 12;
constexpr int maxSubtitleWidth = 100;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 24;
constexpr int mainMenuColumns = 2;
constexpr int sectionLabelHeight = 28;
constexpr const char* listChevronLeft = "‹";
constexpr const char* listChevronRight = "›";
int coverWidth = 0;

void drawLyraBatteryIcon(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight,
                         uint16_t percentage) {
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
    // Plugged in: solid black body + centred bolt. Matches BaseTheme so the
    // charging state looks identical across skins.
    renderer.fillRect(x + 2, y + 2, maxFillWidth, fillHeight);
    const int boltX = x + (battWidth - 8) / 2;
    const int boltY = y + 2 + (rectHeight - 12) / 2;
    renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
    renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
    renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
    renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
    renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
    renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
    renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
    renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
    return;
  }

  // macOS-style proportional fill: a single solid bar whose width tracks the
  // remaining charge. The +1 rounds up so even 1% shows a pixel.
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }
  renderer.fillRect(x + 2, y + 2, filledWidth, fillHeight);
}

const uint8_t* iconForName(UIIcon icon, int size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return Folder24Icon;
      case UIIcon::Text:
        return Text24Icon;
      case UIIcon::Image:
        return Image24Icon;
      case UIIcon::Book:
        return Book24Icon;
      case UIIcon::File:
        return File24Icon;
      default:
        return nullptr;
    }
  } else if (size == 32) {
    switch (icon) {
      case UIIcon::Folder:
        return FolderIcon;
      case UIIcon::Book:
        return BookIcon;
      case UIIcon::Recent:
        return RecentIcon;
      case UIIcon::Settings:
        return Settings2Icon;
      case UIIcon::Transfer:
        return TransferIcon;
      case UIIcon::Library:
        return LibraryIcon;
      case UIIcon::Wifi:
        return WifiIcon;
      case UIIcon::Hotspot:
        return HotspotIcon;
      case UIIcon::Bookmark:
        return BookmarkIcon;
      default:
        return nullptr;
    }
  }
  return nullptr;
}
}  // namespace

void LyraTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + LyraMetrics::values.batteryWidth, rect.y,
                      percentageText.c_str());
  }

  drawLyraBatteryIcon(renderer, rect.x, rect.y + 6, LyraMetrics::values.batteryWidth, rect.height, percentage);
}

void LyraTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  const uint16_t percentage = powerManager.getBatteryPercentage();

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    // Clear the area where we're going to draw the text to prevent ghosting
    const auto textHeight = renderer.getTextHeight(SMALL_FONT_ID);
    renderer.fillRect(rect.x - textWidth - batteryPercentSpacing, rect.y, textWidth, textHeight, false);
    // Draw text to the left of the icon
    renderer.drawText(SMALL_FONT_ID, rect.x - textWidth - batteryPercentSpacing, rect.y, percentageText.c_str());
  }

  drawLyraBatteryIcon(renderer, rect.x, rect.y + 6, LyraMetrics::values.batteryWidth, rect.height, percentage);
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - LyraMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight},
                   showBatteryPercentage);

  int maxTitleWidth =
      rect.width - LyraMetrics::values.contentSidePadding * 2 - (subtitle != nullptr ? maxSubtitleWidth : 0);

  if (title) {
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding,
                      rect.y + LyraMetrics::values.batteryBarHeight + 3, truncatedTitle.c_str(), true,
                      EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(SMALL_FONT_ID, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - LyraMetrics::values.contentSidePadding - truncatedSubtitleWidth,
                      rect.y + 50, truncatedSubtitle.c_str(), true);
  }
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;
  int rightSpace = LyraMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - LyraMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_10_FONT_ID, label, rect.width - LyraMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, bool selected,
                           bool solidSelection) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);
    const int pillWidth = textWidth + 2 * hPaddingInSelection;

    bool invertLabel = false;
    if (tab.selected) {
      if (solidSelection) {
        if (selected) {
          renderer.fillRoundedRect(currentX, rect.y + 1, pillWidth, rect.height - 4, cornerRadius, Color::Black);
          invertLabel = true;
        } else {
          renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + pillWidth, rect.y + rect.height - 3, 2,
                            true);
        }
      } else {
        renderer.drawRoundedRect(currentX, rect.y + 1, pillWidth, rect.height - 4, selected ? 2 : 1, cornerRadius,
                                 true);
      }
    }

    renderer.drawText(UI_10_FONT_ID, currentX + hPaddingInSelection, rect.y + 6, tab.label, !invertLabel,
                      EpdFontFamily::REGULAR);

    currentX += textWidth + LyraMetrics::values.tabSpacing + 2 * hPaddingInSelection;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<std::string(int index)>& rowSection,
                         const std::function<ListToggleState(int index)>& rowToggle,
                         const std::function<bool(int index)>& rowAction, bool solidSelection,
                         const std::function<int(int index)>& rowSignal) const {
  int rowHeight =
      (rowSubtitle != nullptr) ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;

  const auto startsSection = [&](int index) {
    if (rowSection == nullptr || index < 0) {
      return false;
    }
    const std::string section = rowSection(index);
    if (section.empty()) {
      return false;
    }
    return index == 0 || section != rowSection(index - 1);
  };

  const auto rowFullHeight = [&](int index) { return rowHeight + (startsSection(index) ? sectionLabelHeight : 0); };

  const int focusIndex = (selectedIndex < 0) ? 0 : selectedIndex;
  int totalPages = 0;
  int currentPage = 0;
  int pageStartIndex = 0;
  int pageEndIndex = itemCount;
  {
    int scanStart = 0;
    int scanHeight = 0;
    for (int i = 0; i < itemCount; i++) {
      const int h = rowFullHeight(i);
      if (i > scanStart && scanHeight + h > rect.height) {
        if (focusIndex >= scanStart && focusIndex < i) {
          currentPage = totalPages;
          pageStartIndex = scanStart;
          pageEndIndex = i;
        }
        totalPages++;
        scanStart = i;
        scanHeight = 0;
      }
      scanHeight += h;
    }
    if (focusIndex >= scanStart) {
      currentPage = totalPages;
      pageStartIndex = scanStart;
      pageEndIndex = itemCount;
    }
    totalPages++;
  }

  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;
    const int thumbItems = std::max(1, rect.height / rowHeight);
    const int scrollBarHeight = (scrollAreaHeight * thumbItems) / itemCount;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - LyraMetrics::values.scrollBarWidth, scrollBarY, LyraMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  int contentWidth =
      rect.width -
      (totalPages > 1 ? (LyraMetrics::values.scrollBarWidth + LyraMetrics::values.scrollBarRightOffset) : 1);

  int textX = rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - LyraMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2;
  int iconSize;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  if (selectedIndex >= 0 && selectedIndex >= pageStartIndex && selectedIndex < pageEndIndex) {
    int selTop = rect.y;
    for (int i = pageStartIndex; i < selectedIndex; i++) {
      selTop += rowFullHeight(i);
    }
    const int selOffset = startsSection(selectedIndex) ? sectionLabelHeight : 0;
    const int selX = rect.x + LyraMetrics::values.contentSidePadding;
    const int selW = contentWidth - LyraMetrics::values.contentSidePadding * 2;
    if (solidSelection) {
      renderer.fillRoundedRect(selX, selTop + selOffset, selW, rowHeight, cornerRadius, Color::Black);
    } else {
      renderer.drawRoundedRect(selX, selTop + selOffset, selW, rowHeight, 2, cornerRadius, true);
    }
  }

  // Draw all items
  int iconY = (rowSubtitle != nullptr) ? 16 : 10;
  int rowTop = rect.y;
  for (int i = pageStartIndex; i < itemCount && i < pageEndIndex; i++) {
    int sectionOffset = 0;
    if (startsSection(i)) {
      sectionOffset = sectionLabelHeight;
      const std::string section = rowSection(i);
      const int sectionLabelTop = rowTop + 4;
      renderer.drawText(SMALL_FONT_ID, textX, sectionLabelTop, section.c_str(), true, EpdFontFamily::REGULAR);
      const int sectionRuleY = sectionLabelTop + renderer.getLineHeight(SMALL_FONT_ID) + 3;
      renderer.drawLine(rect.x + LyraMetrics::values.contentSidePadding, sectionRuleY,
                        rect.x + contentWidth - LyraMetrics::values.contentSidePadding, sectionRuleY, true);
    }
    const int itemY = rowTop + sectionOffset;
    const int rowContentHeight = rowHeight;
    const int rightEdge = rect.x + contentWidth - LyraMetrics::values.contentSidePadding;

    const ListToggleState toggleState = (rowToggle != nullptr) ? rowToggle(i) : ListToggleState::NotToggle;
    const bool isToggle = toggleState != ListToggleState::NotToggle;
    const bool isAction = (rowAction != nullptr) && rowAction(i);
    const bool inverted = solidSelection && (i == selectedIndex);

    int rowTextX = textX;
    int rowTextWidth = textWidth;

    const int sigLevel = (!isToggle && !isAction && rowSignal != nullptr) ? rowSignal(i) : -1;
    constexpr int signalBarsWidth = 22;
    constexpr int signalBarsHeight = 16;
    const int signalReserve = (sigLevel >= 0) ? (signalBarsWidth + hPaddingInSelection) : 0;

    int valueWidth = 0;
    std::string valueText = "";
    if (!isToggle && !isAction && rowValue != nullptr) {
      valueText = rowValue(i);
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxListValueWidth);
      valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + hPaddingInSelection;
    }
    rowTextWidth -= valueWidth + signalReserve;

    // Draw name
    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    renderer.drawText(UI_10_FONT_ID, rowTextX, itemY + 7, item.c_str(), !inverted);

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection,
                          itemY + iconY, iconSize, iconSize);
      }
    }

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, rowTextX, itemY + 30, subtitle.c_str(), true);
    }

    int valueRightEdge = rightEdge;
    if (sigLevel >= 0) {
      drawSignalBars(renderer, rightEdge - signalBarsWidth, itemY + 6, signalBarsWidth, signalBarsHeight, sigLevel,
                     !inverted);
      valueRightEdge = rightEdge - signalBarsWidth - hPaddingInSelection;
    }

    if (isToggle) {
      drawToggleSwitch(renderer, Rect{rect.x, itemY, rightEdge - hPaddingInSelection - rect.x, rowContentHeight},
                       toggleState == ListToggleState::On, inverted);
    } else if (isAction) {
      const int chevronWidth = renderer.getTextWidth(UI_12_FONT_ID, listChevronRight);
      const int chevronY = itemY + (rowContentHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
      renderer.drawText(UI_12_FONT_ID, rightEdge - hPaddingInSelection - chevronWidth, chevronY, listChevronRight,
                        !inverted);
    } else if (!valueText.empty()) {
      const bool focusedValue = (i == selectedIndex && highlightValue);
      if (focusedValue) {
        char steppedValue[80];
        snprintf(steppedValue, sizeof(steppedValue), "%s %s %s", listChevronLeft, valueText.c_str(), listChevronRight);
        const int steppedWidth = renderer.getTextWidth(UI_10_FONT_ID, steppedValue);
        renderer.drawText(UI_10_FONT_ID, valueRightEdge - hPaddingInSelection - steppedWidth, itemY + 6, steppedValue,
                          !inverted);
      } else {
        renderer.drawText(UI_10_FONT_ID, valueRightEdge - valueWidth, itemY + 6, valueText.c_str(), !inverted);
      }
    }

    rowTop += rowHeight + sectionOffset;
  }
}

void LyraTheme::drawToggleSwitch(const GfxRenderer& renderer, Rect rect, bool on, bool inverted) const {
  constexpr int trackHeight = 24;
  constexpr int trackWidth = 42;
  constexpr int knobInset = 3;

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
    renderer.drawRoundedRect(trackX, trackY, trackWidth, trackHeight, 2, radius, !inverted);
    const int knobX = trackX + knobInset;
    renderer.drawRoundedRect(knobX, trackY + knobInset, knobSize, knobSize, 2, knobSize / 2, !inverted);
  }
}

void LyraTheme::drawBottomSheetFrame(const GfxRenderer& renderer, Rect rect) const {
  constexpr int sheetCornerRadius = 16;
  constexpr int grabberWidth = 44;
  constexpr int grabberHeight = 4;
  constexpr int grabberTopMargin = 10;

  renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, sheetCornerRadius, true, true, false, false,
                           Color::White);
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, sheetCornerRadius, true, true, false, false,
                           true);

  const int grabberX = rect.x + (rect.width - grabberWidth) / 2;
  const int grabberY = rect.y + grabberTopMargin;
  renderer.fillRoundedRect(grabberX, grabberY, grabberWidth, grabberHeight, grabberHeight / 2, Color::Black);
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = LyraMetrics::values.buttonHintsHeight;
  constexpr int buttonY = LyraMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  constexpr int buttonPositions[] = {58, 146, 254, 342};
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Draw the filled background and border for a FULL-sized button
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true, false,
                               false, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
    } else {
      // Draw the filled background and border for a SMALL-sized button
      renderer.fillRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, cornerRadius,
                               Color::White);
      renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, cornerRadius, true,
                               true, false, false, true);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  // Position for the button group - buttons share a border so they're adjacent

  const char* labels[] = {topBtn, bottomBtn};

  // Draw the shared border for both buttons as one unit
  const int x = screenWidth - buttonWidth;

  // Draw top button outline
  if (topBtn != nullptr && topBtn[0] != '\0') {
    renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                             true);
  }

  // Draw bottom button outline
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                             false, true, false, true);
  }

  // Draw text for each button
  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int y = topHintButtonY + (i * buttonHeight + 5);

      // Draw rotated text centered in the button
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);

      renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
    }
  }
}

Rect LyraTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  constexpr int y = 132;
  constexpr int outline = 2;
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::REGULAR);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + popupMarginX * 2;
  const int h = textHeight + popupMarginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  renderer.fillRoundedRect(x - outline, y - outline, w + outline * 2, h + outline * 2, cornerRadius + outline,
                           Color::White);
  renderer.fillRoundedRect(x, y, w, h, cornerRadius, Color::Black);

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + popupMarginY - 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, false, EpdFontFamily::REGULAR);
  renderer.displayBuffer();

  return Rect{x, y, w, h};
}

void LyraTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  constexpr int barHeight = 4;

  // Twice the margin in drawPopup to match text width
  const int barWidth = layout.width - popupMarginX * 2;
  const int barX = layout.x + (layout.width - barWidth) / 2;
  // Center inside the margin of drawPopup. The - 1 is added to account for the - 2 in drawPopup.
  const int barY = layout.y + layout.height - popupMarginY / 2 - barHeight / 2 - 1;

  int fillWidth = barWidth * progress / 100;

  renderer.fillRect(barX, barY, fillWidth, barHeight, false);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void LyraTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth) const {
  int lineY = rect.y + rect.height + renderer.getLineHeight(UI_12_FONT_ID) + LyraMetrics::values.verticalSpacing;
  int lineW = textWidth + hPaddingInSelection * 2;
  renderer.drawLine(rect.x + (rect.width - lineW) / 2, lineY, rect.x + (rect.width + lineW) / 2, lineY, 3);
}

void LyraTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label,
                                const bool isSelected) const {
  if (isSelected) {
    renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cornerRadius, Color::Black);
  }

  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label);
  const int textX = rect.x + (rect.width - textWidth) / 2;
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, label, !isSelected);
}
