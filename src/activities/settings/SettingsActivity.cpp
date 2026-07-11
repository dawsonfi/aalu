#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "ButtonRemapActivity.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "FontSelectionActivity.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Build per-category vectors from the shared settings list
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  for (const auto& setting : getSettingsList()) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
    // Web-only categories (KOReader Sync, OPDS Browser) are skipped for device UI
  }

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  systemSettings.push_back(
      SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network).inSection(StrId::STR_SECT_TOOLS));
  systemSettings.push_back(
      SettingInfo::Action(StrId::STR_FILE_TRANSFER, SettingAction::FileTransfer).inSection(StrId::STR_SECT_TOOLS));
  systemSettings.push_back(
      SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync).inSection(StrId::STR_SECT_TOOLS));
  systemSettings.push_back(
      SettingInfo::Action(StrId::STR_OPDS_BROWSER, SettingAction::OPDSBrowser).inSection(StrId::STR_SECT_TOOLS));
  systemSettings.push_back(
      SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache).inSection(StrId::STR_SECT_TOOLS));
  systemSettings.push_back(
      SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates).inSection(StrId::STR_SECT_TOOLS));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate)
                               .inSection(StrId::STR_SECT_TOOLS));
  // Language selector removed in 1.2.0 — firmware ships English-only. The LanguageSelectActivity
  // class is left in place for now in case multi-lang support returns; the dead-code-elimination
  // pass removes it from the binary since no reachable code references it.
  readerSettings.push_back(SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::ManageFonts));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;

  // Initialize with first category (Display)
  currentSettings = &displaySettings;
  settingsCount = static_cast<int>(displaySettings.size());

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoHome();
    return;
  }

  const bool onTabBar = (selectedSettingIndex == 0);

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (onTabBar) {
      changeCategory(1);
    } else {
      confirmCurrentSetting();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (onTabBar) {
      changeCategory(1);
    } else {
      adjustSettingValue(1);
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (onTabBar) {
      changeCategory(-1);
    } else {
      adjustSettingValue(-1);
    }
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });
}

void SettingsActivity::changeCategory(const int delta) {
  selectedCategoryIndex = (delta > 0) ? ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount)
                                      : ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);

  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }

  settingsCount = static_cast<int>(currentSettings->size());
  selectedSettingIndex = 0;
  requestUpdate();
}

void SettingsActivity::confirmCurrentSetting() {
  const int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  if (setting.nameId == StrId::STR_FONT_FAMILY || setting.type == SettingType::ACTION) {
    activateCurrentSetting();
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr && setting.enumValues.size() > 2) {
    const auto valuePtr = setting.valuePtr;
    optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()),
                     SETTINGS.*valuePtr, [this, valuePtr](int idx) {
                       SETTINGS.*valuePtr = static_cast<uint8_t>(idx);
                       SETTINGS.saveToFile();
                       requestUpdate();
                     });
    requestUpdate();
  } else {
    adjustSettingValue(1);
  }
}

void SettingsActivity::adjustSettingValue(const int delta) {
  const int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  if (setting.nameId == StrId::STR_FONT_FAMILY || setting.valuePtr == nullptr) {
    return;
  }

  if (setting.type == SettingType::TOGGLE) {
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM) {
    const int count = static_cast<int>(setting.enumValues.size());
    if (count <= 0) {
      return;
    }
    const int currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>(((currentValue + delta) % count + count) % count);
  } else if (setting.type == SettingType::VALUE) {
    const int currentValue = SETTINGS.*(setting.valuePtr);
    const int step = setting.valueRange.step;
    const int minValue = setting.valueRange.min;
    const int maxValue = setting.valueRange.max;
    int newValue;
    if (delta > 0) {
      newValue = (currentValue + step > maxValue) ? minValue : currentValue + step;
    } else {
      newValue = (currentValue - step < minValue) ? maxValue : currentValue - step;
    }
    SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>(newValue);
  } else {
    return;
  }

  SETTINGS.saveToFile();
  requestUpdate();
}

void SettingsActivity::activateCurrentSetting() {
  const int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];

  if (setting.nameId == StrId::STR_FONT_FAMILY) {
    startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                           [this](const ActivityResult&) {
                             SETTINGS.saveToFile();
                             sdFontSystem.ensureLoaded(renderer);
                           });
    return;
  }

  if (setting.type != SettingType::ACTION) {
    return;
  }

  auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

  switch (setting.action) {
    case SettingAction::RemapFrontButtons:
      startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::CustomiseStatusBar:
      startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::KOReaderSync:
      startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::OPDSBrowser:
      startActivityForResult(std::make_unique<CalibreSettingsActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::Network:
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
      break;
    case SettingAction::ClearCache:
      startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::CheckForUpdates:
      startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::SdFirmwareUpdate:
      startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::FileTransfer:
      startActivityForResult(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::ManageFonts:
      startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               sdFontSystem.ensureLoaded(renderer);
                             });
      break;
    case SettingAction::Language:
    case SettingAction::None:
      // Do nothing
      break;
  }
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 AALU_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          valueText = std::to_string(SETTINGS.*(setting.valuePtr));
        }
        return valueText;
      },
      true,
      [&settings](int i) {
        const StrId section = settings[i].section;
        return section == StrId::STR_NONE_OPT ? std::string() : std::string(I18N.get(section));
      },
      [&settings](int i) {
        const auto& setting = settings[i];
        if (setting.type != SettingType::TOGGLE || setting.valuePtr == nullptr) {
          return BaseTheme::ListToggleState::NotToggle;
        }
        return (SETTINGS.*(setting.valuePtr)) ? BaseTheme::ListToggleState::On : BaseTheme::ListToggleState::Off;
      },
      [&settings](int i) { return settings[i].type == SettingType::ACTION; });

  GUI.drawButtonHintsGlyphs(renderer, BaseTheme::ButtonHintGlyphSet::Navigation);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
