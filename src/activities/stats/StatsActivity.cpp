#include "activities/stats/StatsActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "CatSprites.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DetailedStatsActivity.h"  // detailedStatistics
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/reader/ReaderUtils.h"  // GO_HOME_MS — shared long-press threshold
#include "activities/util/ConfirmationActivity.h"
#include "components/HomeRenderer.h"  // kThumbnailCoverHeight — shared with home/groups so all three pages read the same on-disk thumb
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "stats/ReadingStatsManager.h"

static constexpr int COVER_PAD = 6;

// A book qualifies for the stats list only if the user has actually engaged
// with it. Books at 0% AND with 0 reading time are typically stale entries
// (added to the cache but never opened past the cover, or imported from a
// path that pre-populated metadata). They clutter the list with no useful
// signal.
static bool isStatsVisible(const BookStatIndexEntry& book) {
  return book.progressPercent > 0 && book.totalReadingMs > 0;
}

// -----------------------------------------------------------------------
// Static helpers
// -----------------------------------------------------------------------

/**
 * @brief Formats a duration in milliseconds into a human-readable string (e.g., "Xh Ym" or "Ym").
 */
void StatsActivity::formatDuration(char* buf, size_t bufLen, uint32_t ms) {
  const uint32_t totalMin = ms / 60000UL;
  const uint32_t hours = totalMin / 60UL;
  const uint32_t mins = totalMin % 60UL;
  if (hours > 0) {
    snprintf(buf, bufLen, "%uh %02um", static_cast<unsigned>(hours), static_cast<unsigned>(mins));
  } else {
    snprintf(buf, bufLen, "%um", static_cast<unsigned>(mins));
  }
}

/**
 * @brief Formats a progress percentage into a string (e.g., "100%").
 */
// void StatsActivity::formatPercent(char* buf, size_t bufLen, uint8_t percent) {
//   snprintf(buf, bufLen, "%u%%", static_cast<unsigned>(percent));
// }

/**
 * @brief Returns the number of books that have not yet reached 100% completion.
 * Completed books are filtered out from the active statistics list.
 */
// Counts books in the current view mode (Reading vs Finished). Hidden books
// (0% progress or 0 reading time) are excluded — see isStatsVisible.
uint16_t StatsActivity::getVisibleBookCount() const {
  uint16_t count = 0;
  for (uint16_t i = 0; i < StatsManager.getBookCount(); ++i) {
    const BookStatIndexEntry& book = StatsManager.getBookSummary(i);
    if (!isStatsVisible(book)) continue;
    const bool isDone = (book.progressPercent >= 95);
    if (isDone == showingFinished) count++;
  }
  return count;
}

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

void StatsActivity::onEnter() {
  Activity::onEnter();
  selectedBookIndex = 0;
  // Stats can outlive thumb files (Clear Cache, file moves, books opened on a
  // firmware version that didn't pre-generate thumbs). Regenerate any missing
  // covers up front so every row has artwork to render.
  prepareMissingCovers();
  activityManager.requestUpdateAndWait();
}

void StatsActivity::prepareMissingCovers() {
  // Stats reads thumbnails at the same resolution the home and group pages
  // use (kThumbnailCoverHeight), so the on-disk cache file is shared across
  // pages — no duplicate regen, and any cover that renders on home will
  // render here. Targeting one canonical size also means the renderer can
  // call drawBitmapStretched1Bit with a known source aspect, fixing the old
  // white-margin bug that came from drawBitmap's aspect-fit fallback.
  static constexpr int kRegenHeight = HomeRenderer::kThumbnailCoverHeight;

  const uint16_t total = StatsManager.getBookCount();
  if (total == 0) return;

  CoverRegenCtx ctx{this, total, false, Rect{}};
  StatsManager.forEachEntry(&ctx, &StatsActivity::regenCoverForEntry);
}

// Runs once per book from forEachEntry's single file handle. Needs the paths and
// so cannot work off the resident summary alone.
void StatsActivity::regenCoverForEntry(void* rawCtx, uint16_t i, const BookStatEntry& book) {
  auto& ctx = *static_cast<CoverRegenCtx*>(rawCtx);
  auto& renderer = ctx.self->renderer;
  static constexpr int kRegenHeight = HomeRenderer::kThumbnailCoverHeight;
  {
    if (!(book.progressPercent > 0 && book.totalReadingMs > 0)) return;
    if (book.thumbBmpPath[0] == '\0') return;
    if (book.bookPath[0] == '\0') return;

    const std::string thumbPath = UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), kRegenHeight);
    if (Storage.exists(thumbPath.c_str())) return;

    // The book file itself may have moved/been deleted - skip silently.
    if (!Storage.exists(book.bookPath)) return;

    // Only EPUB regen is wired up right now. TXT/XTC entries fall through to
    // the placeholder in the row renderer (matches home/group behaviour).
    if (!FsHelpers::hasEpubExtension(std::string(book.bookPath))) return;

    if (!ctx.popupShown) {
      ctx.popupShown = true;
      ctx.popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    }
    GUI.fillPopupProgress(renderer, ctx.popupRect, 10 + (i * 90) / std::max<uint16_t>(ctx.total, 1));

    Epub epub(std::string(book.bookPath), "/.crosspoint");
    if (!epub.load(false, true)) {
      LOG_DBG("STATS", "Could not load EPUB for cover regen: %s", book.bookPath);
      return;
    }
    if (!epub.generateThumbBmp(kRegenHeight)) {
      LOG_DBG("STATS", "Cover regen failed (no embedded image?) for %s", book.bookPath);
    }
  }
}

void StatsActivity::onExit() { Activity::onExit(); }

// -----------------------------------------------------------------------
// Input Handling
// -----------------------------------------------------------------------

void StatsActivity::loop() {
  // Return to the Home screen
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.popActivity();
    return;
  }

  // Toggle between Reading and Finished lists. Must be handled BEFORE the
  // empty-list early return — otherwise an empty current list traps the user
  // (e.g. when Finished has no entries, Right would no-op and the only way
  // back to Reading is Back -> re-enter Stats).
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    viewMode = static_cast<uint8_t>((viewMode + 1) % 6);
    showingFinished = (viewMode == 1);
    selectedBookIndex = 0;
    calendarMonthOffset = 0;
    requestUpdate();
    return;
  }

  // Calendar pages through months with Up/Down; the other non-list views
  // (Badges, Pet, Wrapped) take no further input.
  if (viewMode >= 2) {
    if (viewMode == 4) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        if (calendarMonthOffset > calendarMinMonthOffset()) {
          calendarMonthOffset--;
          requestUpdate();
        }
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        if (calendarMonthOffset < 0) {
          calendarMonthOffset++;
          requestUpdate();
        }
      }
    }
    return;
  }

  const uint8_t bookCount = getVisibleBookCount();
  if (bookCount == 0) return;

  bool changed = false;

  // Navigate up in the book list
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (selectedBookIndex > 0) {
      selectedBookIndex--;
      changed = true;
    }
  }

  // Navigate down in the book list
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (selectedBookIndex < static_cast<int>(bookCount) - 1) {
      selectedBookIndex++;
      changed = true;
    }
  }

  const uint8_t actualMemoryIndex = resolveSelectedMemoryIndex();

  // Open detailed stats (More...)
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    activityManager.pushActivity(std::make_unique<DetailedStatsActivity>(renderer, mappedInput, actualMemoryIndex));
    return;
  }

  // Confirm: short press opens the book, long press prompts to remove it from
  // the stats list. Mirrors the home-screen recents long-press gesture.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
      confirmRemoveFocusedBook();
      return;
    }
    const BookStatEntry& book = StatsManager.getBook(actualMemoryIndex);
    if (book.bookPath[0] != '\0') {
      activityManager.goToReader(std::string(book.bookPath));
    }
    return;
  }

  if (changed) {
    requestUpdate();
  }
}

uint16_t StatsActivity::resolveSelectedMemoryIndex() const {
  int currentMatch = 0;
  for (uint16_t j = 0; j < StatsManager.getBookCount(); ++j) {
    const BookStatIndexEntry& book = StatsManager.getBookSummary(j);
    if (!isStatsVisible(book)) continue;
    const bool isDone = (book.progressPercent >= 95);
    if (isDone != showingFinished) continue;
    if (currentMatch == selectedBookIndex) return j;
    currentMatch++;
  }
  return STATS_INVALID_BOOK;
}

void StatsActivity::confirmRemoveFocusedBook() {
  const uint16_t memoryIndex = resolveSelectedMemoryIndex();
  if (memoryIndex == STATS_INVALID_BOOK) return;

  // Snapshot the cacheKey now -- the dialog runs as a sub-activity, so the
  // underlying array could in theory be mutated before the handler fires
  // (e.g. a background end-of-session save). Re-resolving via cacheKey on
  // confirm keeps us pointed at the right row even if indices have shifted.
  char cacheKey[sizeof(BookStatEntry::cacheKey)];
  strncpy(cacheKey, StatsManager.getBook(memoryIndex).cacheKey, sizeof(cacheKey));
  cacheKey[sizeof(cacheKey) - 1] = '\0';

  const std::string title = StatsManager.getBook(memoryIndex).title;

  auto handler = [this, cacheKey = std::string(cacheKey)](const ActivityResult& result) {
    if (result.isCancelled) return;

    uint16_t target = STATS_INVALID_BOOK;
    const uint32_t wantedHash = stats::hashCacheKey(cacheKey.c_str());
    for (uint16_t i = 0; i < StatsManager.getBookCount(); ++i) {
      if (StatsManager.getBookSummary(i).cacheKeyHash != wantedHash) continue;
      if (strncmp(StatsManager.getBook(i).cacheKey, cacheKey.c_str(), sizeof(BookStatEntry::cacheKey)) == 0) {
        target = i;
        break;
      }
    }
    if (target == STATS_INVALID_BOOK) return;
    if (!StatsManager.removeBook(target)) return;

    const uint16_t remaining = getVisibleBookCount();
    if (remaining == 0) {
      selectedBookIndex = 0;
    } else if (selectedBookIndex >= static_cast<int>(remaining)) {
      selectedBookIndex = static_cast<int>(remaining) - 1;
    }
    requestUpdate();
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_STATS), title), handler);
}

int StatsActivity::calendarMinMonthOffset() const {
  // Furthest back the calendar can page. Deliberately independent of reading
  // history so the user can browse months with no data (they render as an empty
  // grid). Gated on the same "today" renderCalendar derives -- valid live clock,
  // else the persisted lastSyncedDay -- so paging works whenever a grid is
  // actually shown, including when the RTC-less clock isn't currently synced.
  // Only "never synced" (today == 0, the "sync time" prompt) has nothing to page.
  constexpr int kMonthsBack = 24;
  const auto& global = StatsManager.getGlobal();
  const time_t nowEpoch = time(nullptr);
  const bool haveToday = stats::epochValid(static_cast<int64_t>(nowEpoch)) || global.lastSyncedDay != 0;
  return haveToday ? -kMonthsBack : 0;
}

// -----------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------

void StatsActivity::render(RenderLock&& lock) {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();

  const char* viewName = (viewMode == 0)   ? tr(STR_STATS_VIEW_READING)
                         : (viewMode == 1) ? tr(STR_STATS_VIEW_FINISHED)
                         : (viewMode == 2) ? tr(STR_STATS_BADGES)
                         : (viewMode == 3) ? tr(STR_STATS_PET)
                         : (viewMode == 4) ? tr(STR_STATS_CALENDAR)
                                           : tr(STR_STATS_WRAPPED);
  GUI.drawHeader(renderer, Rect(0, metrics.topPadding, screenW, metrics.headerHeight), tr(STR_STATS_TITLE), viewName);

  const int contentTop = metrics.topPadding + metrics.headerHeight;
  const int contentH = screenH - contentTop - metrics.buttonHintsHeight;

  if (viewMode <= 1) {
    const int topH = contentH / 4;
    const int bottomH = contentH - topH;
    renderTopPanel(contentTop, topH, screenW);
    renderBookPanel(contentTop + topH, bottomH, screenW);
  } else if (viewMode == 2) {
    renderBadges(contentTop, contentH, screenW);
  } else if (viewMode == 3) {
    renderPet(contentTop, contentH, screenW);
  } else if (viewMode == 4) {
    renderCalendar(contentTop, contentH, screenW);
  } else {
    renderWrapped(contentTop, contentH, screenW);
  }

  GUI.drawButtonHintsGlyphs(renderer, viewMode == 4 ? BaseTheme::ButtonHintGlyphSet::Navigation
                                                    : BaseTheme::ButtonHintGlyphSet::StatsActions);

  renderer.displayBuffer();
}

// -----------------------------------------------------------------------
// Top Panel — Global Statistics (All Time & Last 7 Sessions)
// -----------------------------------------------------------------------
void StatsActivity::renderTopPanel(int panelY, int panelH, int screenW) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& global = StatsManager.getGlobal();
  const int pad = metrics.contentSidePadding;

  // Bottom divider and vertical column separator
  renderer.drawLine(0, panelY + panelH, screenW, panelY + panelH, 1, true);
  renderer.drawLine(screenW / 2, panelY + 8, screenW / 2, panelY + panelH - 8, 1, true);

  const int col1X = pad;
  const int col2X = screenW / 2 + pad;
  const int rowStep = panelH / 4;
  const int row1Y = panelY + rowStep / 2;
  const int row2Y = row1Y + rowStep;
  const int row3Y = row2Y + rowStep;

  const time_t nowEpoch = time(nullptr);
  const bool clockOk = stats::epochValid(static_cast<int64_t>(nowEpoch));
  const uint16_t today =
      clockOk ? stats::dayNumber(static_cast<int64_t>(nowEpoch), stats::utcOffsetSeconds(SETTINGS.clockUtcOffsetQ)) : 0;

  char buf[24];

  renderer.drawText(UI_12_FONT_ID, col1X, row1Y, tr(STR_STATS_STREAK), true, EpdFontFamily::BOLD);
  if (clockOk) {
    const uint16_t live = stats::streakAlive(global, today) ? global.currentStreakDays : 0;
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(live));
    renderer.drawText(UI_12_FONT_ID, col1X, row2Y, buf, true);
    snprintf(buf, sizeof(buf), "%s %u", tr(STR_STATS_LONGEST), static_cast<unsigned>(global.longestStreakDays));
    renderer.drawText(UI_10_FONT_ID, col1X, row3Y, buf, true);
  } else {
    renderer.drawText(UI_10_FONT_ID, col1X, row2Y, tr(STR_STATS_SYNC_TIME), true);
  }

  if (clockOk) {
    renderer.drawText(UI_12_FONT_ID, col2X, row1Y, tr(STR_STATS_TODAY), true, EpdFontFamily::BOLD);
    const uint32_t todayMin = stats::sumLastNDays(global, today, 1);
    snprintf(buf, sizeof(buf), "%u / %u min", static_cast<unsigned>(todayMin),
             static_cast<unsigned>(global.goalTarget));
    renderer.drawText(UI_12_FONT_ID, col2X, row2Y, buf, true);
    const int barW = screenW / 2 - 2 * pad;
    const int barY = row3Y - 2;
    const int barH = 8;
    renderer.drawRect(col2X, barY, barW, barH, true);
    unsigned pct = (global.goalTarget > 0) ? (todayMin * 100u / global.goalTarget) : 0u;
    if (pct > 100u) pct = 100u;
    const int fw = (barW - 2) * static_cast<int>(pct) / 100;
    if (fw > 0) renderer.fillRect(col2X + 1, barY + 1, fw, barH - 2, true);
  } else {
    renderer.drawText(UI_12_FONT_ID, col2X, row1Y, tr(STR_STATS_ALL_TIME), true, EpdFontFamily::BOLD);
    char bufAllTime[16];
    formatDuration(bufAllTime, sizeof(bufAllTime), global.totalReadingMs);
    renderer.drawText(UI_12_FONT_ID, col2X, row2Y, bufAllTime, true);
    snprintf(buf, sizeof(buf), "%s %u", tr(STR_FINISHED_BOOKS), static_cast<unsigned>(global.totalBooksFinished));
    renderer.drawText(UI_10_FONT_ID, col2X, row3Y, buf, true);
  }
}

static void fillTriUp(GfxRenderer& r, int cx, int baseY, int halfW, int height, bool state) {
  if (height <= 0) return;
  for (int i = 0; i <= height; ++i) {
    const int w = halfW * (height - i) / height;
    r.drawLine(cx - w, baseY - i, cx + w, baseY - i, state);
  }
}

static void drawDisc(GfxRenderer& r, int cx, int cy, int radius, Color color) {
  if (radius <= 0) return;
  r.fillRoundedRect(cx - radius, cy - radius, radius * 2, radius * 2, radius, color);
}

static void badgeRing(GfxRenderer& r, int cx, int cy, int radius, int lineWidth, bool state) {
  if (radius <= 0) return;
  r.drawRoundedRect(cx - radius, cy - radius, radius * 2, radius * 2, lineWidth, radius, state);
}

static void icFlame(GfxRenderer& r, int cx, int cy, int s, bool earned) {
  const bool fg = !earned, bg = earned;
  const Color fgC = earned ? White : Black, bgC = earned ? Black : White;
  fillTriUp(r, cx, cy + s * 40 / 100, s * 62 / 100, s * 150 / 100, fg);
  drawDisc(r, cx, cy + s * 38 / 100, s * 62 / 100, fgC);
  fillTriUp(r, cx, cy + s * 46 / 100, s * 30 / 100, s * 85 / 100, bg);
  drawDisc(r, cx, cy + s * 42 / 100, s * 30 / 100, bgC);
}

static void icBooks(GfxRenderer& r, int cx, int cy, int s, bool earned) {
  const bool bg = earned;
  const Color fgC = earned ? White : Black;
  int hh = s * 32 / 100;
  if (hh < 4) hh = 4;
  const int ws[3] = {s * 160 / 100, s * 190 / 100, s * 150 / 100};
  const int offs[3] = {-s * 18 / 100, s * 12 / 100, -s * 6 / 100};
  int y = cy - s * 78 / 100;
  for (int i = 0; i < 3; ++i) {
    const int x = cx - ws[i] / 2 + offs[i];
    r.fillRoundedRect(x, y, ws[i], hh, 2, fgC);
    r.fillRect(x + 3, y + 1, 2, hh - 2, bg);
    y += hh + 3;
  }
}

static void icPage(GfxRenderer& r, int cx, int cy, int s, bool earned) {
  const bool bg = earned;
  const Color fgC = earned ? White : Black;
  const int w = s * 125 / 100, h = s * 170 / 100, x = cx - w / 2, y = cy - h / 2;
  r.fillRoundedRect(x, y, w, h, 3, fgC);
  fillTriUp(r, x + w - 1, y + s * 45 / 100, s * 45 / 100, s * 45 / 100, bg);
  for (int k = 0; k < 3; ++k) r.drawLine(x + 4, y + s * 70 / 100 + k * 5, x + w - 6, y + s * 70 / 100 + k * 5, bg);
}

static void icClock(GfxRenderer& r, int cx, int cy, int s, bool earned) {
  const bool fg = !earned;
  const Color fgC = earned ? White : Black;
  const int rad = s * 75 / 100;
  badgeRing(r, cx, cy, rad, 2, fg);
  r.drawLine(cx, cy, cx, cy - rad * 60 / 100, 2, fg);
  r.drawLine(cx, cy, cx + rad * 50 / 100, cy, 2, fg);
  drawDisc(r, cx, cy, 2, fgC);
}

static void icSun(GfxRenderer& r, int cx, int cy, int s, bool earned) {
  const bool fg = !earned;
  const Color fgC = earned ? White : Black;
  const int rad = s * 50 / 100;
  drawDisc(r, cx, cy, rad, fgC);
  const int dirs[8][2] = {{0, -10}, {7, -7}, {10, 0}, {7, 7}, {0, 10}, {-7, 7}, {-10, 0}, {-7, -7}};
  const int ri = rad + 3, ro = rad + s * 45 / 100;
  for (int a = 0; a < 8; ++a) {
    const int dx = dirs[a][0], dy = dirs[a][1];
    r.drawLine(cx + ri * dx / 10, cy + ri * dy / 10, cx + ro * dx / 10, cy + ro * dy / 10, 2, fg);
  }
}

static void icMoon(GfxRenderer& r, int cx, int cy, int s, bool earned) {
  const Color fgC = earned ? White : Black, bgC = earned ? Black : White;
  const int rad = s * 80 / 100;
  drawDisc(r, cx, cy, rad, fgC);
  drawDisc(r, cx + rad * 55 / 100, cy - rad * 25 / 100, rad, bgC);
}

void StatsActivity::renderBadges(int panelY, int panelH, int screenW) const {
  const auto& global = StatsManager.getGlobal();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pad = metrics.contentSidePadding;
  const int lh10 = renderer.getLineHeight(UI_10_FONT_ID);

  enum { K_FLAME, K_BOOKS, K_PAGE, K_CLOCK, K_SUN, K_MOON };
  struct Badge {
    int kind;
    uint32_t bit;
    const char* tier;
    const char* cap;
  };
  const Badge badges[16] = {
      {K_FLAME, ACH_STREAK_3, "3", tr(STR_STATS_STREAK)},    {K_FLAME, ACH_STREAK_7, "7", tr(STR_STATS_STREAK)},
      {K_FLAME, ACH_STREAK_30, "30", tr(STR_STATS_STREAK)},  {K_FLAME, ACH_STREAK_100, "100", tr(STR_STATS_STREAK)},
      {K_BOOKS, ACH_BOOKS_1, "1", tr(STR_STATS_BOOKS)},      {K_BOOKS, ACH_BOOKS_10, "10", tr(STR_STATS_BOOKS)},
      {K_BOOKS, ACH_BOOKS_25, "25", tr(STR_STATS_BOOKS)},    {K_BOOKS, ACH_BOOKS_50, "50", tr(STR_STATS_BOOKS)},
      {K_PAGE, ACH_PAGES_1K, "1k", tr(STR_STATS_PAGES)},     {K_PAGE, ACH_PAGES_10K, "10k", tr(STR_STATS_PAGES)},
      {K_PAGE, ACH_PAGES_100K, "100k", tr(STR_STATS_PAGES)}, {K_CLOCK, ACH_HOURS_10, "10", tr(STR_STATS_HOURS)},
      {K_CLOCK, ACH_HOURS_50, "50", tr(STR_STATS_HOURS)},    {K_CLOCK, ACH_HOURS_100, "100", tr(STR_STATS_HOURS)},
      {K_SUN, ACH_EARLY_BIRD, "", tr(STR_STATS_EARLY)},      {K_MOON, ACH_NIGHT_OWL, "", tr(STR_STATS_NIGHT)},
  };

  int cols = (screenW - 2 * pad) / 100;
  if (cols < 4) cols = 4;
  if (cols > 8) cols = 8;
  const int rows = (16 + cols - 1) / cols;
  const int colW = (screenW - 2 * pad) / cols;
  int D = colW - 14;
  const int byH = (panelH - 12) / rows - 40;
  if (D > byH) D = byH;
  if (D > 96) D = 96;
  if (D < 40) D = 40;
  const int rowStep = D + 40;

  for (int i = 0; i < 16; ++i) {
    const int c = i % cols, rr = i / cols;
    const int cx = pad + c * colW + colW / 2;
    const int cy = panelY + lh10 + 12 + rr * rowStep + D / 2;
    const int rad = D / 2;
    const bool earned = (global.achievementBits & badges[i].bit) != 0;
    if (earned) {
      drawDisc(renderer, cx, cy, rad, Black);
      badgeRing(renderer, cx, cy, rad - 3, 2, false);
    } else {
      drawDisc(renderer, cx, cy, rad, White);
      badgeRing(renderer, cx, cy, rad, 2, true);
      badgeRing(renderer, cx, cy, rad - 4, 1, true);
    }
    const int s = D / 4;
    const int iconCy = cy - D / 8;
    switch (badges[i].kind) {
      case K_FLAME:
        icFlame(renderer, cx, iconCy, s, earned);
        break;
      case K_BOOKS:
        icBooks(renderer, cx, iconCy, s, earned);
        break;
      case K_PAGE:
        icPage(renderer, cx, iconCy, s, earned);
        break;
      case K_CLOCK:
        icClock(renderer, cx, iconCy, s, earned);
        break;
      case K_SUN:
        icSun(renderer, cx, iconCy, s, earned);
        break;
      case K_MOON:
        icMoon(renderer, cx, iconCy, s, earned);
        break;
    }
    if (badges[i].tier[0] != '\0') {
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, badges[i].tier, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, cx - tw / 2, cy + D / 4 - lh10 / 2, badges[i].tier, !earned,
                        EpdFontFamily::BOLD);
    }
    const int cw = renderer.getTextWidth(UI_10_FONT_ID, badges[i].cap);
    renderer.drawText(UI_10_FONT_ID, cx - cw / 2, cy - rad - lh10 - 6, badges[i].cap, true);
  }
}

static void drawCatSprite(GfxRenderer& r, const CatSprite& s, int x, int y) {
  const int total = static_cast<int>(s.w) * static_cast<int>(s.h);
  for (int i = 0; i < total; ++i) {
    const uint8_t v = (s.data[i >> 2] >> ((i & 3) * 2)) & 0x3;
    if (v == 0) continue;
    const int px = x + (i % s.w);
    const int py = y + (i / s.w);
    if (v == 1) {
      r.drawPixel(px, py, true);
    } else {
      r.fillRectDither(px, py, 1, 1, LightGray);
    }
  }
}

static void drawPetBar(GfxRenderer& r, int x, int y, int w, int h, const char* label, const char* rightText,
                       uint8_t pct) {
  const int lh = r.getLineHeight(UI_10_FONT_ID);
  r.drawText(UI_10_FONT_ID, x, y, label, true);
  const int pw = r.getTextWidth(UI_10_FONT_ID, rightText);
  r.drawText(UI_10_FONT_ID, x + w - pw, y, rightText, true);
  const int by = y + lh + 1;
  r.drawRect(x, by, w, h, true);
  int fillW = (w - 2) * pct / 100;
  if (fillW < 0) fillW = 0;
  if (fillW > w - 2) fillW = w - 2;
  if (fillW > 0) r.fillRectDither(x + 1, by + 1, fillW, h - 2, Black);
}

struct PetVisual {
  StrId nameKey;
  const CatSprite* sprite;
};

static constexpr PetVisual kPetVisuals[stats::kPetStageCount] = {
    {StrId::STR_PET_CAT_KITTEN, &kCatKitten},       {StrId::STR_PET_CAT_ADOLESCENT, &kCatTeen},
    {StrId::STR_PET_CAT_ADULT, &kCatAdult},         {StrId::STR_PET_TIGER_CUB, &kTigerCub},
    {StrId::STR_PET_TIGER_ADOLESCENT, &kTigerTeen}, {StrId::STR_PET_TIGER_ADULT, &kTigerAdult},
    {StrId::STR_PET_DRAGON_EGG, &kDragonEgg},       {StrId::STR_PET_DRAGON_HATCHLING, &kDragonHatch},
    {StrId::STR_PET_DRAGON_JUVENILE, &kDragonJuv},  {StrId::STR_PET_DRAGON_ADULT, &kDragonAdult},
    {StrId::STR_PET_DRAGON_ELDER, &kDragonElder}};

void StatsActivity::renderPet(int panelY, int panelH, int screenW) const {
  const auto& global = StatsManager.getGlobal();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pad = metrics.contentSidePadding;
  const int lh10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lh12 = renderer.getLineHeight(UI_12_FONT_ID);
  char buf[48];

  const int stage = stats::petStageForXp(global.petXp);
  const uint8_t curStage = static_cast<uint8_t>(stage);
  const bool justEvolved = APP_STATE.lastShownPetStage != UINT8_MAX && curStage > APP_STATE.lastShownPetStage;
  if (curStage != APP_STATE.lastShownPetStage) {
    APP_STATE.lastShownPetStage = curStage;
    if (justEvolved) APP_STATE.saveToFile();
  }
  const int cx = screenW / 2;
  const CatSprite& cat = *kPetVisuals[stage].sprite;
  const int barBlock = lh10 + 1 + 10;
  const int blockH = cat.h + 8 + lh12 + 8 + barBlock + 8 + barBlock + 8 + barBlock + 10 + lh12;
  int catTop = panelY + (panelH - blockH) / 2;
  if (catTop < panelY + 8) catTop = panelY + 8;
  drawCatSprite(renderer, cat, cx - cat.w / 2, catTop);
  int ty = catTop + cat.h + 8;

  snprintf(buf, sizeof(buf), "%s  %s %u", I18n::getInstance().get(kPetVisuals[stage].nameKey), tr(STR_STATS_PET_LEVEL),
           static_cast<unsigned>(stats::petLevelForXp(global.petXp)));
  renderer.drawCenteredText(UI_12_FONT_ID, ty, buf, true);
  ty += lh12 + 8;

  int barW = screenW - 2 * pad;
  if (barW > 240) barW = 240;
  const int barX = cx - barW / 2;

  const time_t nowEpoch = time(nullptr);
  uint8_t hunger, happiness;
  stats::petEffectiveStats(global, static_cast<int64_t>(nowEpoch), hunger, happiness);

  char rbuf[12];
  snprintf(rbuf, sizeof(rbuf), "%u%%", static_cast<unsigned>(hunger));
  drawPetBar(renderer, barX, ty, barW, 10, tr(STR_STATS_HUNGER), rbuf, hunger);
  ty += barBlock + 8;
  snprintf(rbuf, sizeof(rbuf), "%u%%", static_cast<unsigned>(happiness));
  drawPetBar(renderer, barX, ty, barW, 10, tr(STR_STATS_HAPPINESS), rbuf, happiness);
  ty += barBlock + 8;

  const uint16_t nextXp = stats::petXpNextForStage(static_cast<uint8_t>(stage));
  char xpRight[20];
  uint8_t xpPct;
  if (nextXp == 0) {
    xpPct = 100;
    snprintf(xpRight, sizeof(xpRight), "%u %s", static_cast<unsigned>(global.petXp), tr(STR_STATS_PET_MAX));
  } else {
    const uint16_t floorXp = stats::petXpFloorForStage(static_cast<uint8_t>(stage));
    const uint16_t span = static_cast<uint16_t>(nextXp - floorXp);
    const uint16_t into = global.petXp > floorXp ? static_cast<uint16_t>(global.petXp - floorXp) : 0;
    xpPct = span ? static_cast<uint8_t>(into * 100u / span) : 0;
    snprintf(xpRight, sizeof(xpRight), "%u/%u", static_cast<unsigned>(global.petXp), static_cast<unsigned>(nextXp));
  }
  drawPetBar(renderer, barX, ty, barW, 10, tr(STR_STATS_XP), xpRight, xpPct);
  ty += barBlock + 10;

  bool thriving = false;
  if (stats::epochValid(static_cast<int64_t>(nowEpoch))) {
    const uint16_t today =
        stats::dayNumber(static_cast<int64_t>(nowEpoch), stats::utcOffsetSeconds(SETTINGS.clockUtcOffsetQ));
    thriving = stats::streakAlive(global, today) && global.currentStreakDays > 0;
  }
  const char* status =
      justEvolved ? tr(STR_PET_EVOLVED) : (thriving ? tr(STR_STATS_PET_THRIVING) : tr(STR_STATS_PET_RESTING));
  renderer.drawCenteredText(UI_12_FONT_ID, ty, status, true);
}

void StatsActivity::renderCalendar(int panelY, int panelH, int screenW) const {
  static const char* const kMonths[12] = {"January", "February", "March",     "April",   "May",      "June",
                                          "July",    "August",   "September", "October", "November", "December"};
  static const char* const kDow[7] = {"M", "T", "W", "T", "F", "S", "S"};

  const auto& global = StatsManager.getGlobal();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pad = metrics.contentSidePadding;
  const int lh10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lh12 = renderer.getLineHeight(UI_12_FONT_ID);

  const time_t nowEpoch = time(nullptr);
  const uint16_t today =
      stats::epochValid(static_cast<int64_t>(nowEpoch))
          ? stats::dayNumber(static_cast<int64_t>(nowEpoch), stats::utcOffsetSeconds(SETTINGS.clockUtcOffsetQ))
          : global.lastSyncedDay;
  if (today == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, panelY + panelH / 2, tr(STR_STATS_SYNC_TIME), true);
    return;
  }

  const uint16_t goal = global.goalTarget > 0 ? global.goalTarget : STATS_DEFAULT_GOAL_MINUTES;
  const stats::CivilDate tcd = stats::civilFromDays(static_cast<int>(today));
  int dispYear = tcd.year;
  int dispMonth = tcd.month + calendarMonthOffset;
  while (dispMonth < 1) {
    dispMonth += 12;
    dispYear--;
  }
  while (dispMonth > 12) {
    dispMonth -= 12;
    dispYear++;
  }
  const stats::CivilDate cd = {dispYear, dispMonth, 1};
  const int firstWd = stats::weekdayMon(stats::daysFromCivil(cd.year, cd.month, 1));
  const int nDays = stats::daysInMonth(cd.year, cd.month);
  const int rows = (firstWd + nDays + 6) / 7;

  char title[24];
  snprintf(title, sizeof(title), "%s %d", kMonths[(cd.month - 1) % 12], cd.year);
  renderer.drawCenteredText(UI_12_FONT_ID, panelY + 4, title, true);

  const int gridX = pad;
  const int gridY = panelY + 8 + lh12 + 4;
  const int cw = (screenW - 2 * pad) / 7;
  const int hdrH = lh10 + 4;
  int ch = (panelH - (gridY - panelY) - hdrH - lh10 - 14) / (rows > 0 ? rows : 1);
  if (ch > 72) ch = 72;
  if (ch < 22) ch = 22;

  for (int c = 0; c < 7; ++c) {
    const int hx = gridX + c * cw + (cw - renderer.getTextWidth(UI_10_FONT_ID, kDow[c])) / 2;
    renderer.drawText(UI_10_FONT_ID, hx, gridY, kDow[c], true);
  }

  const int gy = gridY + hdrH;
  for (int n = 1; n <= nDays; ++n) {
    const int posn = firstWd + (n - 1);
    const int x = gridX + (posn % 7) * cw;
    const int y = gy + (posn / 7) * ch;
    const uint16_t dn = static_cast<uint16_t>(stats::daysFromCivil(cd.year, cd.month, n));
    const uint16_t m = stats::minutesOnDay(global, dn);

    bool numWhite = false;
    if (m > 0) {
      const Color shade = (m >= goal) ? Black : (m >= goal / 2) ? DarkGray : LightGray;
      numWhite = (shade == Black || shade == DarkGray);
      renderer.fillRoundedRect(x + 2, y + 2, cw - 4, ch - 4, 4, shade);
    }
    if (dn == today) {
      renderer.drawRoundedRect(x + 5, y + 5, cw - 10, ch - 10, 3, 3, !numWhite);
    }
    char dnum[4];
    snprintf(dnum, sizeof(dnum), "%d", n);
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, dnum);
    renderer.drawText(UI_10_FONT_ID, x + (cw - tw) / 2, y + (ch - lh10) / 2, dnum, !numWhite);
  }

  const int ly = gy + rows * ch + 4;
  const char* lo = "Light reader";
  const char* hi = "Avid reader";
  const Color sh[3] = {LightGray, DarkGray, Black};
  const int swW = 16, swH = lh10 - 2, swGap = 3;
  const int swatchesW = 3 * swW + 2 * swGap;
  const int loW = renderer.getTextWidth(UI_10_FONT_ID, lo);
  const int hiW = renderer.getTextWidth(UI_10_FONT_ID, hi);
  const int totalW = loW + 10 + swatchesW + 10 + hiW;
  int lx = (screenW - totalW) / 2;
  if (lx < pad) lx = pad;
  renderer.drawText(UI_10_FONT_ID, lx, ly, lo, true);
  lx += loW + 10;
  const int swY = ly + (lh10 - swH) / 2;
  for (int i = 0; i < 3; ++i) {
    renderer.fillRoundedRect(lx, swY, swW, swH, 2, sh[i]);
    lx += swW + swGap;
  }
  lx += 10 - swGap;
  renderer.drawText(UI_10_FONT_ID, lx, ly, hi, true);
}

void StatsActivity::renderWrapped(int panelY, int panelH, int screenW) const {
  (void)screenW;
  const auto& global = StatsManager.getGlobal();
  const int lh12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int lh10 = renderer.getLineHeight(UI_10_FONT_ID);

  struct WrapStat {
    char value[24];
    const char* label;
  };
  WrapStat items[5];
  StatsActivity::formatDuration(items[0].value, sizeof(items[0].value), global.totalReadingMs);
  items[0].label = tr(STR_STATS_HOURS_READ);
  snprintf(items[1].value, sizeof(items[1].value), "%u", static_cast<unsigned>(global.totalPagesLifetime));
  items[1].label = tr(STR_STATS_PAGES_TURNED);
  snprintf(items[2].value, sizeof(items[2].value), "%u", static_cast<unsigned>(global.totalBooksFinished));
  items[2].label = tr(STR_FINISHED_BOOKS);
  snprintf(items[3].value, sizeof(items[3].value), "%u", static_cast<unsigned>(global.longestStreakDays));
  items[3].label = tr(STR_STATS_LONGEST_STREAK);
  snprintf(items[4].value, sizeof(items[4].value), "%u", static_cast<unsigned>(global.lifetimeActiveDays));
  items[4].label = tr(STR_STATS_DAYS_READ);

  const int blockH = lh12 + lh10 + 14;
  int y = panelY + (panelH - 5 * blockH) / 2;
  if (y < panelY + 6) y = panelY + 6;
  for (int i = 0; i < 5; ++i) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, items[i].value, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, y + lh12, items[i].label, true);
    y += blockH;
  }
}

// -----------------------------------------------------------------------
// Bottom Panel — Scrollable Book List
// -----------------------------------------------------------------------
void StatsActivity::renderBookPanel(int panelY, int panelH, int screenW) const {
  const uint8_t count = getVisibleBookCount();

  if (count == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, panelY + panelH / 2, tr(STR_STATS_NO_DATA), true);
    return;
  }

  static constexpr int VISIBLE_ROWS = 3;
  const int rowH = panelH / VISIBLE_ROWS;
  const int scrollOffset = (selectedBookIndex / VISIBLE_ROWS) * VISIBLE_ROWS;
  const int visibleCount = std::min(static_cast<int>(count) - scrollOffset, VISIBLE_ROWS);

  for (int i = 0; i < visibleCount; ++i) {
    const int visibleIdx = scrollOffset + i;
    const int rowY = panelY + i * rowH;

    uint16_t targetIndex = STATS_INVALID_BOOK;
    int currentMatch = 0;

    for (uint16_t j = 0; j < StatsManager.getBookCount(); ++j) {
      const BookStatIndexEntry& book = StatsManager.getBookSummary(j);
      if (!isStatsVisible(book)) continue;
      const bool isDone = (book.progressPercent >= 95);
      if (isDone != showingFinished) continue;

      if (currentMatch == visibleIdx) {
        targetIndex = j;
        break;
      }
      currentMatch++;
    }

    if (targetIndex != STATS_INVALID_BOOK) {
      renderBookRow(0, rowY, screenW, rowH, StatsManager.getBook(targetIndex), visibleIdx == selectedBookIndex);
    }
  }
}

// -----------------------------------------------------------------------
// Single Book Row Rendering
// -----------------------------------------------------------------------
// src/activities/stats/StatsActivity.cpp

void StatsActivity::renderBookRow(int rowX, int rowY, int rowW, int rowH, const BookStatEntry& book,
                                  bool selected) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pad = metrics.contentSidePadding;

  // Top divider for the row
  renderer.drawLine(rowX, rowY, rowX + rowW, rowY, 1, true);

  // Visual highlight for the currently selected book (dithered background)
  if (selected) {
    renderer.fillRectDither(rowX, rowY + 1, rowW, rowH - 1, LightGray);
    renderer.drawRect(rowX, rowY + 1, rowW, rowH - 1, 2, true);
  }

  // Cover dimensions: Portrait rectangle (3:4 aspect ratio to match physical books)
  const int coverH = rowH - COVER_PAD * 2;
  const int coverW = (coverH * 3) / 4;
  const int coverX = rowX + COVER_PAD;
  const int coverY = rowY + COVER_PAD;

  // Attempt to draw the generated cover; fallback to a title placeholder if
  // the book has no thumbnail at all (no embedded cover image, generation
  // failed, or the cache was cleared).
  if (!loadAndDrawCover(coverX, coverY, coverW, coverH, book)) {
    drawCoverPlaceholder(coverX, coverY, coverW, coverH, book.title);
  }

  const int textX = coverX + coverW + pad + 10;
  const int textW = rowX + rowW - textX - pad - 10;
  const int line1Y = coverY + 10;

  // --- FIX: Proper Title Truncation Logic ---
  char truncatedTitle[64];
  size_t titleLen = strlen(book.title);
  // Using a strict 16 character limit to prevent overlap with the percentage text
  static constexpr size_t MAX_LIST_TITLE_LEN = 26;

  if (titleLen > MAX_LIST_TITLE_LEN) {
    strncpy(truncatedTitle, book.title, MAX_LIST_TITLE_LEN);
    truncatedTitle[MAX_LIST_TITLE_LEN] = '\0';
    strcat(truncatedTitle, "...");
  } else {
    strncpy(truncatedTitle, book.title, sizeof(truncatedTitle) - 1);
  }

  // Line 1 — Book Title (Bold, UI_12) - Using the truncated buffer!
  renderer.drawText(UI_12_FONT_ID, textX, line1Y, truncatedTitle, true, EpdFontFamily::BOLD);

  // Distribute text lines evenly across the usable row height
  const int line2Y = coverY + 40;  // Progress bar
  const int line3Y = coverY + 70;  // Time spent
  const int line4Y = coverY + 95;  // Sessions

  // Line 2 — Custom Progress Bar + Percentage Text (UI_10)
  const int barH = 8;
  const int barY = line2Y + 4;
  const int pctLabelW = 40;                // Reserved width for "100%"
  const int barW = textW - pctLabelW - 5;  // Prevents overlap with text
  const int pctX = textX + barW + 10;

  if (barW > 10) {
    HomeRenderer::drawRoundedProgressBar(renderer, textX, barY, barW, barH, static_cast<int8_t>(book.progressPercent));
    char bufPct[6];
    snprintf(bufPct, sizeof(bufPct), "%u%%", static_cast<unsigned>(book.progressPercent));
    // Vertically centre the label on the bar (same trick as the home hero) so
    // the percent text doesn't float above the rounded ends.
    const int pctY = barY + (barH - renderer.getLineHeight(UI_10_FONT_ID)) / 2 - 1;
    renderer.drawText(UI_10_FONT_ID, pctX, pctY, bufPct, true);
  }

  // Line 3 — Time Spent (UI_10)
  char bufDur[16];
  char bufLine3[40];
  formatDuration(bufDur, sizeof(bufDur), book.totalReadingMs);
  snprintf(bufLine3, sizeof(bufLine3), "%s: %s", tr(STR_STATS_TIME_SPENT), bufDur);
  renderer.drawText(UI_10_FONT_ID, textX, line3Y, bufLine3, true);

  // Line 4 — Session Count (UI_10)
  char bufLine4[32];
  snprintf(bufLine4, sizeof(bufLine4), "%s: %u", tr(STR_STATS_SESSIONS_COUNT),
           static_cast<unsigned>(book.sessionCount));
  renderer.drawText(UI_10_FONT_ID, textX, line4Y, bufLine4, true);
}

// -----------------------------------------------------------------------
// Cover Drawing and Placeholders
// -----------------------------------------------------------------------

/**
 * @brief Draws a fallback placeholder when a book cover is missing.
 */
void StatsActivity::drawCoverPlaceholder(int x, int y, int w, int h, const char* /*title*/) const {
  // Empty bordered rect - matches the home screen's behaviour for books with
  // no embedded cover image. The book title is already visible to the right
  // of the cover in the row layout, so we don't paint it inside the box.
  renderer.drawRoundedRect(x, y, w, h, 1, HomeRenderer::kCoverCornerRadius, false);
}

/**
 * @brief Loads and renders a book cover, mirroring HomeRenderer::drawCover.
 *
 * Uses the same single canonical resolution (kThumbnailCoverHeight) and the
 * same is1Bit() dispatch as the home page and series-group page, so all three
 * surfaces read the same on-disk thumb and render it identically. Falls back
 * to drawCoverPlaceholder on any miss; never renders a degraded cover.
 *
 * @return true if a valid cover was found and successfully rendered.
 */
bool StatsActivity::loadAndDrawCover(int x, int y, int w, int h, const BookStatEntry& book) const {
  if (book.thumbBmpPath[0] == '\0') {
    return false;
  }

  const std::string thumbPath =
      UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), HomeRenderer::kThumbnailCoverHeight);

  FsFile f;
  if (!Storage.openFileForRead("STATS", thumbPath.c_str(), f)) return false;

  Bitmap bmp(f, false);
  if (bmp.parseHeaders() != BmpReaderError::Ok) {
    f.close();
    return false;
  }

  // 1-bit thumbs are stretched non-uniformly to fill the slot exactly, so the
  // cover never leaves a white margin on the right when its source aspect
  // ratio doesn't match the row's 3:4 cover slot. Identical dispatch to
  // HomeRenderer::drawCover.
  if (bmp.is1Bit()) {
    renderer.drawBitmapStretched1Bit(bmp, x, y, w, h);
  } else {
    renderer.drawBitmap(bmp, x, y, w, h);
  }
  renderer.roundCoverCorners(x, y, w, h, HomeRenderer::kCoverCornerRadius);

  f.close();
  return true;
}