#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * List of configured OPDS servers. In settings mode a trailing "Add Server" item opens the editor
 * and selecting a server edits it; in picker mode selecting a server opens the OPDS browser on it.
 */
class OpdsServerListActivity final : public Activity {
 public:
  explicit OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pickerMode = false)
      : Activity("OpdsServerList", renderer, mappedInput), pickerMode(pickerMode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;
  bool pickerMode = false;

  int getItemCount() const;
  void handleSelection();
};
