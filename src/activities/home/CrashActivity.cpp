#include "CrashActivity.h"

#include <HalDisplay.h>
#include <HalSystem.h>
#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

void CrashActivity::onEnter() {
  Activity::onEnter();

  panicMessage = HalSystem::getPanicInfo(false);
  if (panicMessage.empty()) {
    panicMessage = tr(STR_CRASH_NO_REASON);
  }
  HalSystem::clearPanic();

  requestUpdateAndWait();
}

void CrashActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
  }
}

void CrashActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  const int margin = 24;
  const int maxWidth = renderer.getScreenWidth() - margin * 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  int y = 64;
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_CRASH_TITLE), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 24;

  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, tr(STR_CRASH_DESCRIPTION), maxWidth, 6)) {
    renderer.drawText(UI_10_FONT_ID, margin, y, line.c_str());
    y += lineHeight;
  }

  y += 24;
  renderer.drawText(UI_10_FONT_ID, margin, y, tr(STR_CRASH_REASON), true, EpdFontFamily::BOLD);
  y += lineHeight + 8;

  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, panicMessage.c_str(), maxWidth, 10)) {
    renderer.drawText(UI_10_FONT_ID, margin, y, line.c_str());
    y += lineHeight;
  }

  GUI.drawButtonHintsGlyphs(renderer, BaseTheme::ButtonHintGlyphSet::Navigation, 0x01);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}
