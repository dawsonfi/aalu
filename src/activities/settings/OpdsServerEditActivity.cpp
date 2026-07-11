#include "OpdsServerEditActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int FIELD_NAME = 0;
constexpr int FIELD_URL = 1;
constexpr int FIELD_USERNAME = 2;
constexpr int FIELD_PASSWORD = 3;
constexpr int FIELD_DELETE = 4;
}  // namespace

void OpdsServerEditActivity::onEnter() {
  Activity::onEnter();
  isNewServer = serverIndex < 0;
  if (!isNewServer) {
    const auto* s = OPDS_STORE.getServer(static_cast<size_t>(serverIndex));
    if (s) {
      editServer = *s;
    } else {
      isNewServer = true;
    }
  }
  selectedIndex = 0;
  requestUpdate();
}

void OpdsServerEditActivity::onExit() { Activity::onExit(); }

int OpdsServerEditActivity::getItemCount() const { return isNewServer ? 4 : 5; }

void OpdsServerEditActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    commit();
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }
  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % getItemCount();
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + getItemCount() - 1) % getItemCount();
    requestUpdate();
  });
}

void OpdsServerEditActivity::handleSelection() {
  const int sel = static_cast<int>(selectedIndex);
  if (sel == FIELD_DELETE && !isNewServer) {
    OPDS_STORE.removeServer(static_cast<size_t>(serverIndex));
    serverIndex = -1;
    isNewServer = true;  // stop commit() from re-adding on the way out
    finish();
    return;
  }

  const char* title = tr(STR_SERVER_NAME);
  std::string initial = editServer.name;
  int maxLen = 63;
  if (sel == FIELD_URL) {
    title = tr(STR_CALIBRE_WEB_URL);
    initial = editServer.url;
    maxLen = 127;
  } else if (sel == FIELD_USERNAME) {
    title = tr(STR_USERNAME);
    initial = editServer.username;
  } else if (sel == FIELD_PASSWORD) {
    title = tr(STR_PASSWORD);
    initial = editServer.password;
  }

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, title, initial.c_str(), maxLen, false),
      [this, sel](const ActivityResult& result) {
        if (result.isCancelled) {
          return;
        }
        const auto& kb = std::get<KeyboardResult>(result.data);
        if (sel == FIELD_NAME) {
          editServer.name = kb.text;
        } else if (sel == FIELD_URL) {
          editServer.url = kb.text;
        } else if (sel == FIELD_USERNAME) {
          editServer.username = kb.text;
        } else if (sel == FIELD_PASSWORD) {
          editServer.password = kb.text;
        }
      });
}

void OpdsServerEditActivity::commit() {
  // An abandoned "Add Server" (no URL entered) is not persisted.
  if (editServer.url.empty()) {
    return;
  }
  if (isNewServer) {
    OPDS_STORE.addServer(editServer);
  } else {
    OPDS_STORE.updateServer(static_cast<size_t>(serverIndex), editServer);
  }
}

void OpdsServerEditActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 isNewServer ? tr(STR_ADD_SERVER) : tr(STR_OPDS_SERVERS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, getItemCount(), static_cast<int>(selectedIndex),
      [](int index) {
        switch (index) {
          case FIELD_URL:
            return std::string(I18N.get(StrId::STR_CALIBRE_WEB_URL));
          case FIELD_USERNAME:
            return std::string(I18N.get(StrId::STR_USERNAME));
          case FIELD_PASSWORD:
            return std::string(I18N.get(StrId::STR_PASSWORD));
          case FIELD_DELETE:
            return std::string(I18N.get(StrId::STR_DELETE));
          default:
            return std::string(I18N.get(StrId::STR_SERVER_NAME));
        }
      },
      nullptr, nullptr,
      [this](int index) {
        switch (index) {
          case FIELD_URL:
            return editServer.url.empty() ? std::string(tr(STR_NOT_SET)) : editServer.url;
          case FIELD_USERNAME:
            return editServer.username.empty() ? std::string(tr(STR_NOT_SET)) : editServer.username;
          case FIELD_PASSWORD:
            return editServer.password.empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
          case FIELD_DELETE:
            return std::string("");
          default:
            return editServer.name.empty() ? std::string(tr(STR_NOT_SET)) : editServer.name;
        }
      },
      true);

  GUI.drawButtonHintsGlyphs(renderer, BaseTheme::ButtonHintGlyphSet::Navigation);
  renderer.displayBuffer();
}
