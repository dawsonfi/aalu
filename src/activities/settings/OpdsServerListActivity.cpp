#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerEditActivity.h"
#include "OpdsServerStore.h"
#include "activities/ActivityManager.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void OpdsServerListActivity::onEnter() {
  Activity::onEnter();
  OPDS_STORE.loadFromFile();

  // One-time migration: seed the multi-server store from the legacy single-server settings so an
  // existing OPDS setup keeps working after the upgrade.
  if (!OPDS_STORE.hasServers() && strlen(SETTINGS.opdsServerUrl) > 0) {
    OpdsServer s;
    s.name = SETTINGS.opdsServerUrl;
    s.url = SETTINGS.opdsServerUrl;
    s.username = SETTINGS.opdsUsername;
    s.password = SETTINGS.opdsPassword;
    OPDS_STORE.addServer(s);
  }

  selectedIndex = 0;
  requestUpdate();
}

void OpdsServerListActivity::onExit() { Activity::onExit(); }

// Real servers plus a trailing "Add Server" item (both modes, so an empty list is never a dead end).
int OpdsServerListActivity::getItemCount() const { return static_cast<int>(OPDS_STORE.getCount()) + 1; }

void OpdsServerListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }
  const int itemCount = getItemCount();
  buttonNavigator.onNext([this, itemCount] {
    selectedIndex = (selectedIndex + 1) % itemCount;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, itemCount] {
    selectedIndex = (selectedIndex + itemCount - 1) % itemCount;
    requestUpdate();
  });
}

void OpdsServerListActivity::handleSelection() {
  const int serverCount = static_cast<int>(OPDS_STORE.getCount());
  const int sel = static_cast<int>(selectedIndex);

  // Trailing "Add Server" item: open the editor for a new server in either mode.
  if (sel >= serverCount) {
    startActivityForResult(std::make_unique<OpdsServerEditActivity>(renderer, mappedInput, -1),
                           [this](const ActivityResult&) {
                             OPDS_STORE.loadFromFile();
                             selectedIndex = 0;
                             requestUpdate();
                           });
    return;
  }

  if (pickerMode) {
    const auto* server = OPDS_STORE.getServer(static_cast<size_t>(sel));
    if (server) {
      activityManager.replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, *server));
    }
    return;
  }

  startActivityForResult(std::make_unique<OpdsServerEditActivity>(renderer, mappedInput, sel),
                         [this](const ActivityResult&) {
                           OPDS_STORE.loadFromFile();
                           selectedIndex = 0;
                           requestUpdate();
                         });
}

void OpdsServerListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OPDS_SERVERS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int serverCount = static_cast<int>(OPDS_STORE.getCount());

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, getItemCount(), static_cast<int>(selectedIndex),
      [serverCount](int index) {
        if (index < serverCount) {
          const auto* s = OPDS_STORE.getServer(static_cast<size_t>(index));
          if (s) {
            return s->name.empty() ? s->url : s->name;
          }
        }
        return std::string(I18N.get(StrId::STR_ADD_SERVER));
      },
      nullptr, nullptr,
      [serverCount](int index) {
        if (index < serverCount) {
          const auto* s = OPDS_STORE.getServer(static_cast<size_t>(index));
          if (s && !s->name.empty()) {
            return s->url;
          }
        }
        return std::string("");
      },
      true);

  GUI.drawButtonHintsGlyphs(renderer, BaseTheme::ButtonHintGlyphSet::Navigation);
  renderer.displayBuffer();
}
