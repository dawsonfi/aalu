#pragma once

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Edit screen for a single OPDS server (name, URL, username, password) plus a Delete option for
 * existing servers. serverIndex < 0 creates a new server. The edited server is written back to
 * OpdsServerStore when Back is pressed (if it has a URL).
 */
class OpdsServerEditActivity final : public Activity {
 public:
  explicit OpdsServerEditActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int serverIndex = -1)
      : Activity("OpdsServerEdit", renderer, mappedInput), serverIndex(serverIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;
  int serverIndex;
  OpdsServer editServer;
  bool isNewServer = false;

  int getItemCount() const;
  void handleSelection();
  void commit();
};
