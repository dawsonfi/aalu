#pragma once

#include <cstdint>

#include "activities/Activity.h"

struct BookStatEntry;

class DetailedStatsActivity final : public Activity {
 public:
  DetailedStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint16_t bookIndex);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void renderDetailedGrid() const;
  void drawCover(const BookStatEntry& book, int x, int y, int w, int h) const;
  void drawCoverPlaceholder(int x, int y, int w, int h, const char* title) const;

  uint16_t _bookIndex;
};