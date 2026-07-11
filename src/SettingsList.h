#pragma once

#include <I18n.h>

#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
inline const std::vector<SettingInfo>& getSettingsList() {
  // Built with reserve + push_back rather than a braced initializer list: each
  // SettingInfo holds four std::function members, so a ~39-element init list
  // would materialise the whole array (~6 KB) as one stack temporary. On the
  // web request's deep call chain (8 KB loopTask) that overflows the stack and
  // reboots the device. push_back keeps only one temporary live at a time.
  static const std::vector<SettingInfo> list = [] {
    std::vector<SettingInfo> v;
    v.reserve(40);

    // --- Display ---
    v.push_back(SettingInfo::Enum(StrId::STR_HOME_STYLE, &CrossPointSettings::homeStyle,
                                  {StrId::STR_HOME_STYLE_FLAT, StrId::STR_HOME_STYLE_CARROUSEL}, "homeStyle",
                                  StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(StrId::STR_BROWSE_MODE, &CrossPointSettings::browseMode,
                                  {StrId::STR_LIBRARY_VIEW_BOOKSHELF, StrId::STR_LIBRARY_VIEW_FILES}, "browseMode",
                                  StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                                  {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER,
                                   StrId::STR_COVER_CUSTOM, StrId::STR_NONE_OPT, StrId::STR_CUSTOM_INSIGHTS},
                                  "sleepScreen", StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                                  {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                                  {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                                  "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                                  {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                                  StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Enum(
        StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
        {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
        "refreshFrequency", StrId::STR_CAT_DISPLAY));
    v.push_back(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                                    StrId::STR_CAT_DISPLAY));

    // --- Reader ---
    v.push_back(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                                  {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS}, "fontFamily", StrId::STR_CAT_READER)
                    .inSection(StrId::STR_SECT_TEXT));
    v.push_back(SettingInfo::Enum(
                    StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                    {StrId::STR_X_SMALL, StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE},
                    "fontSize", StrId::STR_CAT_READER)
                    .inSection(StrId::STR_SECT_TEXT));
    v.push_back(SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                                  {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing",
                                  StrId::STR_CAT_READER)
                    .inSection(StrId::STR_SECT_TEXT));
    v.push_back(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                                  {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                   StrId::STR_BOOK_S_STYLE},
                                  "paragraphAlignment", StrId::STR_CAT_READER)
                    .inSection(StrId::STR_SECT_TEXT));
    v.push_back(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5},
                                   "screenMargin", StrId::STR_CAT_READER)
                    .inSection(StrId::STR_SECT_LAYOUT));
    v.push_back(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                                    "extraParagraphSpacing", StrId::STR_CAT_READER)
                    .inSection(StrId::STR_SECT_LAYOUT));
    v.push_back(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                                    StrId::STR_CAT_READER)
                    .inSection(StrId::STR_SECT_LAYOUT));
    v.push_back(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled,
                                    "hyphenationEnabled", StrId::STR_CAT_READER)
                    .inSection(StrId::STR_SECT_LAYOUT));
    v.push_back(SettingInfo::Toggle(StrId::STR_BIONIC_READING, &CrossPointSettings::bionicReading, "bionicReading",
                                    StrId::STR_CAT_READER)
                    .inSection(StrId::STR_SECT_LAYOUT));
    v.push_back(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                                    StrId::STR_CAT_READER)
                    .inSection(StrId::STR_SECT_LAYOUT));
    v.push_back(
        SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER)
            .inSection(StrId::STR_SECT_LAYOUT));
    v.push_back(
        SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                          {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                          "orientation", StrId::STR_CAT_READER)
            .inSection(StrId::STR_SECT_SCREEN));

    // --- Controls ---
    v.push_back(SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                                  {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV}, "sideButtonLayout",
                                  StrId::STR_CAT_CONTROLS));
    v.push_back(SettingInfo::Toggle(StrId::STR_LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip,
                                    "longPressChapterSkip", StrId::STR_CAT_CONTROLS));
    v.push_back(SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                                  {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH,
                                   StrId::STR_CYCLE_WALLPAPER},
                                  "shortPwrBtn", StrId::STR_CAT_CONTROLS));

    // --- System ---
    v.push_back(SettingInfo::Enum(
        StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
        {StrId::STR_MIN_1, StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15, StrId::STR_MIN_30, StrId::STR_NEVER},
        "sleepTimeout", StrId::STR_CAT_SYSTEM));
    v.push_back(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles,
                                    "showHiddenFiles", StrId::STR_CAT_SYSTEM));
    v.push_back(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS,
                                    &CrossPointSettings::removeReadBooksFromRecents, "removeReadBooksFromRecents",
                                    StrId::STR_CAT_SYSTEM));
    v.push_back(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                                    "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM));

    // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
    v.push_back(SettingInfo::DynamicString(
        StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
        [](const std::string& s) {
          KOREADER_STORE.setCredentials(s, KOREADER_STORE.getPassword());
          KOREADER_STORE.saveToFile();
        },
        "koUsername", StrId::STR_KOREADER_SYNC));
    v.push_back(SettingInfo::DynamicString(
        StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
        [](const std::string& s) {
          KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), s);
          KOREADER_STORE.saveToFile();
        },
        "koPassword", StrId::STR_KOREADER_SYNC));
    v.push_back(SettingInfo::DynamicString(
        StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
        [](const std::string& s) {
          KOREADER_STORE.setServerUrl(s);
          KOREADER_STORE.saveToFile();
        },
        "koServerUrl", StrId::STR_KOREADER_SYNC));
    v.push_back(SettingInfo::DynamicEnum(
        StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
        [](uint8_t mm) {
          KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(mm));
          KOREADER_STORE.saveToFile();
        },
        "koMatchMethod", StrId::STR_KOREADER_SYNC));

    // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
    v.push_back(SettingInfo::String(StrId::STR_OPDS_SERVER_URL, SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl),
                                    "opdsServerUrl", StrId::STR_OPDS_BROWSER));
    v.push_back(SettingInfo::String(StrId::STR_USERNAME, SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername),
                                    "opdsUsername", StrId::STR_OPDS_BROWSER));
    v.push_back(SettingInfo::String(StrId::STR_PASSWORD, SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword),
                                    "opdsPassword", StrId::STR_OPDS_BROWSER)
                    .withObfuscated());

    // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
    v.push_back(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                                    "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE,
                                    &CrossPointSettings::statusBarBookProgressPercentage,
                                    "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                                  {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(
        SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                                  {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                                    StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock,
                                  {StrId::STR_HIDE, StrId::STR_TOP}, "statusBarClock",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                                  {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
    v.push_back(SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                                   "clockUtcOffsetQ", StrId::STR_CUSTOMISE_STATUS_BAR));
    return v;
  }();
  return list;
}

inline std::vector<const SettingInfo*> projectSettings(const std::vector<StrId>& order) {
  std::vector<const SettingInfo*> result;
  result.reserve(order.size());
  const auto& list = getSettingsList();
  for (const StrId id : order) {
    for (const auto& setting : list) {
      if (setting.nameId == id) {
        result.push_back(&setting);
        break;
      }
    }
  }
  return result;
}

inline const std::vector<const SettingInfo*>& getQuickSettingsReaderItems() {
  static const std::vector<const SettingInfo*> items = projectSettings({
      StrId::STR_FONT_FAMILY,
      StrId::STR_FONT_SIZE,
      StrId::STR_LINE_SPACING,
      StrId::STR_SCREEN_MARGIN,
      StrId::STR_PARA_ALIGNMENT,
      StrId::STR_EMBEDDED_STYLE,
      StrId::STR_HYPHENATION,
      StrId::STR_EXTRA_SPACING,
      StrId::STR_TEXT_AA,
      StrId::STR_ORIENTATION,
  });
  return items;
}

inline const std::vector<const SettingInfo*>& getQuickSettingsControlsItems() {
  static const std::vector<const SettingInfo*> items = projectSettings({
      StrId::STR_SIDE_BTN_LAYOUT,
      StrId::STR_LONG_PRESS_SKIP,
      StrId::STR_SHORT_PWR_BTN,
  });
  return items;
}
