#pragma once

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "stats/ReadingStatsManager.h"

class StatsActivity final : public Activity {
 public:
  explicit StatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Stats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void renderTopPanel(int panelY, int panelH, int screenW) const;
  void renderBookPanel(int panelY, int panelH, int screenW) const;
  void renderBookRow(int rowX, int rowY, int rowW, int rowH, const BookStatEntry& book, bool selected) const;
  void renderBadges(int panelY, int panelH, int screenW) const;
  void renderPet(int panelY, int panelH, int screenW) const;
  void renderCalendar(int panelY, int panelH, int screenW) const;
  void renderWrapped(int panelY, int panelH, int screenW) const;
  void drawCoverPlaceholder(int x, int y, int w, int h, const char* title) const;
  bool loadAndDrawCover(int x, int y, int w, int h, const BookStatEntry& book) const;
  // Generates thumbnails on disk for any stats book that is missing one. Run
  // once on activity entry; shows a popup while it works.
  void prepareMissingCovers();

  struct CoverRegenCtx {
    StatsActivity* self;
    uint16_t total;
    bool popupShown;
    Rect popupRect;
  };
  static void regenCoverForEntry(void* rawCtx, uint16_t index, const BookStatEntry& book);
  bool showingFinished = false;  // NEW: Toggle between Reading and Finished views
  uint8_t viewMode = 0;          // 0=Reading,1=Finished,2=Badges,3=Pet,4=Calendar,5=Wrapped

  static void formatDuration(char* buf, size_t bufLen, uint32_t ms);
  uint16_t getVisibleBookCount() const;
  // Resolves the currently focused row to the underlying StatsManager index,
  // or 0xFF when nothing is selected. Shared by Open/More and the long-press
  // remove flow so the same predicate hides hidden entries from all three.
  uint16_t resolveSelectedMemoryIndex() const;
  void confirmRemoveFocusedBook();
  int calendarMinMonthOffset() const;

  int selectedBookIndex = 0;
  int calendarMonthOffset = 0;
};