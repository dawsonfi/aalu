#include "EndOfBookOptions.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/NextBookFinder.h"

namespace {
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}
}  // namespace

void EndOfBookOptions::loadOnce(const std::string& currentBookPath) {
  if (isLoaded.load(std::memory_order_acquire)) {
    return;
  }
  folder = FsHelpers::extractFolderPath(currentBookPath);
  names = NextBookFinder::findNextBooks(currentBookPath, MAX_SUGGESTIONS);
  selector = 0;
  isLoaded.store(true, std::memory_order_release);
}

bool EndOfBookOptions::menuActive() const { return isLoaded.load(std::memory_order_acquire) && !names.empty(); }

std::string EndOfBookOptions::fullPath(const size_t index) const {
  if (index >= names.size()) {
    return {};
  }
  return folder == "/" ? "/" + names[index] : folder + "/" + names[index];
}

EndOfBookOptions::Action EndOfBookOptions::handleMenuInput(const MappedInputManager& input, std::string* openPath) {
  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selector < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(selector);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;
  }

  if (input.wasReleased(MappedInputManager::Button::Back) && input.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    return Action::LastPage;
  }

  const auto turn = ReaderUtils::detectPageTurn(input);
  const int itemCount = static_cast<int>(names.size()) + 1;
  if (turn.prev) {
    selector = ButtonNavigator::previousIndex(selector, itemCount);
    return Action::Redraw;
  }
  if (turn.next) {
    selector = ButtonNavigator::nextIndex(selector, itemCount);
    return Action::Redraw;
  }
  return Action::None;
}

void EndOfBookOptions::render(GfxRenderer& renderer) const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (!menuActive()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() * 3 / 8, tr(STR_END_OF_BOOK), true,
                              EpdFontFamily::BOLD);
    return;
  }

  int vTop, vRight, vBottom, vLeft;
  renderer.getOrientedViewableTRBL(&vTop, &vRight, &vBottom, &vLeft);
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int bandThickness = metrics.buttonHintsHeight + metrics.verticalSpacing;
  const auto gutter = ReaderUtils::bandGutterForBottomHints(renderer, bandThickness);

  const int areaTop = vTop + gutter.top;
  const int areaLeft = vLeft + gutter.left;
  const int areaRight = vRight + gutter.right;
  const int areaBottom = vBottom + gutter.bottom;
  const int areaHeight = screenH - areaTop - areaBottom;
  const int areaWidth = screenW - areaLeft - areaRight;

  const int titleY = areaTop + areaHeight / 8;
  const int subtitleY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  const int listTop = subtitleY + renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing * 2;
  const int listHeight = areaTop + areaHeight - listTop;

  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, subtitleY, tr(STR_EOB_CONTINUE_WITH));

  GUI.drawList(renderer, Rect{areaLeft, listTop, areaWidth, listHeight}, static_cast<int>(names.size()) + 1, selector,
               [this](const int index) {
                 return index < static_cast<int>(names.size()) ? displayName(names[index])
                                                               : std::string(tr(STR_EOB_HOME));
               });
  GUI.drawButtonHintsGlyphs(renderer);
}
