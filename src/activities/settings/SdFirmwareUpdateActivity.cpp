#include "SdFirmwareUpdateActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_ota_ops.h>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/FirmwareFlasher.h"

namespace {
constexpr const char* kUpdatePath = "/update.bin";
}

void SdFirmwareUpdateActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("FW", "SdFirmwareUpdateActivity build=%s %s recovery=%d", __DATE__, __TIME__, recoveryMode ? 1 : 0);
  beginFlow();
}

void SdFirmwareUpdateActivity::beginFlow() {
  HalFile file;
  if (!Storage.openFileForRead("FW", kUpdatePath, file) || !file) {
    LOG_INF("FW", "no update.bin at SD root");
    RenderLock lock(*this);
    state = State::NO_FILE;
    requestUpdate();
    return;
  }
  file.close();

  {
    RenderLock lock(*this);
    state = State::VALIDATING;
  }
  requestUpdateAndWait();

  if (!validateFirmware()) {
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  promptConfirmation();
}

bool SdFirmwareUpdateActivity::validateFirmware() {
  HalFile file;
  if (!Storage.openFileForRead("FW", kUpdatePath, file) || !file) {
    errorMessage = tr(STR_FIRMWARE_FILE_OPEN_FAILED);
    return false;
  }
  firmwareSize = file.fileSize();
  file.close();

  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FW", "no next-update partition available");
    errorMessage = tr(STR_INVALID_FIRMWARE);
    return false;
  }
  const size_t partitionLimit = dest->size;
  if (firmwareSize > partitionLimit) {
    LOG_ERR("FW", "firmware (%u bytes) exceeds partition (%u bytes)", static_cast<unsigned>(firmwareSize),
            static_cast<unsigned>(partitionLimit));
    errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    return false;
  }

  const auto vr = firmware_flash::validateImageFile(kUpdatePath, partitionLimit);
  if (vr != firmware_flash::Result::OK) {
    LOG_ERR("FW", "image validation failed: %s", firmware_flash::resultName(vr));
    if (vr == firmware_flash::Result::TOO_LARGE) {
      errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    } else if (vr == firmware_flash::Result::TOO_SMALL) {
      errorMessage = tr(STR_FIRMWARE_TOO_SMALL);
    } else {
      errorMessage = std::string(tr(STR_INVALID_FIRMWARE)) + " (" + firmware_flash::resultName(vr) + ")";
    }
    return false;
  }
  return true;
}

void SdFirmwareUpdateActivity::promptConfirmation() {
  {
    RenderLock lock(*this);
    state = State::CONFIRMING;
  }
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_FIRMWARE_UPDATE_PROMPT), kUpdatePath),
      [this](const ActivityResult& result) { onConfirmationResult(result); });
}

void SdFirmwareUpdateActivity::onConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    if (recoveryMode) {
      RenderLock lock(*this);
      state = State::FAILED;
      errorMessage = tr(STR_NO_UPDATE_BIN);
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state = State::UPDATING;
    writtenBytes = 0;
    lastRenderedPercent = 101;
  }
  requestUpdateAndWait();
  performUpdate();
}

void SdFirmwareUpdateActivity::performUpdate() {
  LOG_INF("FW", "SD update: %s (%u bytes)", kUpdatePath, static_cast<unsigned>(firmwareSize));

  auto progressCb = +[](size_t written, size_t total, void* ctx) {
    auto* self = static_cast<SdFirmwareUpdateActivity*>(ctx);
    self->writtenBytes = written;
    self->firmwareSize = total;
    self->requestUpdate(true);
  };

  // Re-validate at flash time (TOCTOU): SD is removable, so don't trust the
  // pre-confirmation pass. Passing alreadyValidated=false makes the flasher
  // re-run the full integrity check against the freshly-opened stream.
  const auto result = firmware_flash::flashFromSdPath(kUpdatePath, progressCb, this, /*alreadyValidated=*/false);
  if (result != firmware_flash::Result::OK) {
    LOG_ERR("FW", "flash failed: %s", firmware_flash::resultName(result));
    errorMessage = tr(STR_FIRMWARE_WRITE_FAILED);
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  LOG_INF("FW", "SD firmware update complete, restarting");
  {
    RenderLock lock(*this);
    state = State::SUCCESS;
  }
  requestUpdateAndWait();
  delay(1500);
  ESP.restart();
}

void SdFirmwareUpdateActivity::loop() {
  if (state == State::NO_FILE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (recoveryMode) {
        state = State::VALIDATING;
        beginFlow();
        return;
      }
      finish();
    }
  } else if (state == State::FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = State::VALIDATING;
      beginFlow();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (recoveryMode) {
        state = State::VALIDATING;
        beginFlow();
        return;
      }
      finish();
    }
  }
}

void SdFirmwareUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const char* headerText = recoveryMode ? tr(STR_RECOVERY_MODE) : tr(STR_SD_FIRMWARE_UPDATE);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerText);

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;

  if (state == State::NO_FILE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_UPDATE_BIN));
    GUI.drawButtonHintsGlyphs(renderer, BaseTheme::ButtonHintGlyphSet::Navigation, 0x03);
  } else if (state == State::VALIDATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_VALIDATING_FIRMWARE));
  } else if (state == State::UPDATING) {
    const unsigned int pct = firmwareSize > 0 ? static_cast<unsigned int>((writtenBytes * 100) / firmwareSize) : 0;
    if (pct == lastRenderedPercent) {
      return;
    }
    lastRenderedPercent = pct;

    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING), true, EpdFontFamily::BOLD);

    int y = top + lineHeight + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(pct), 100);
    y += metrics.progressBarHeight + metrics.verticalSpacing;
    y += lineHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
  } else if (state == State::SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, tr(STR_RESTARTING_HINT));
  } else if (state == State::FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, errorMessage.c_str());
    }
    GUI.drawButtonHintsGlyphs(renderer, BaseTheme::ButtonHintGlyphSet::Navigation, 0x03);
  } else {
    if (recoveryMode) {
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_RECOVERY_MODE));
    }
  }

  renderer.displayBuffer();
}
