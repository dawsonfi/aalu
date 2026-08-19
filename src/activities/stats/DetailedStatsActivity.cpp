#include "DetailedStatsActivity.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "activities/ActivityManager.h"
#include "components/HomeRenderer.h"  // kThumbnailCoverHeight — shared with stats list, home, groups
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"  // for GUI macro and drawButtonHints
#include "fontIds.h"
#include "stats/ReadingStatsManager.h"

namespace {
void fmtDuration(uint32_t ms, char* buf, size_t n) {
  const uint32_t h = ms / 3600000UL;
  const uint32_t m = (ms % 3600000UL) / 60000UL;
  if (h > 0) {
    std::snprintf(buf, n, "%uh %um", h, m);
  } else {
    std::snprintf(buf, n, "%um", m);
  }
}
}  // namespace

DetailedStatsActivity::DetailedStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint16_t bookIndex)
    : Activity("DetailedStats", renderer, mappedInput), _bookIndex(bookIndex) {}

void DetailedStatsActivity::onEnter() {
  Activity::onEnter();
  activityManager.requestUpdateAndWait();
}

void DetailedStatsActivity::onExit() { Activity::onExit(); }

void DetailedStatsActivity::loop() {
  // We can only go back to the list from this view
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.popActivity();
    return;
  }
}

void DetailedStatsActivity::render(RenderLock&& lock) {
  renderer.clearScreen();
  renderDetailedGrid();

  // Bottom button bar: only Back is functional, so draw just that glyph in its fixed slot.
  // fixed: using a GUI macro
  GUI.drawButtonHintsGlyphs(renderer, BaseTheme::ButtonHintGlyphSet::Navigation, 0b0001);

  renderer.displayBuffer();
}

void DetailedStatsActivity::renderDetailedGrid() const {
  const auto& book = StatsManager.getBook(_bookIndex);
  const auto& global = StatsManager.getGlobal();
  char buf[48];

  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();
  int mt, mr, mb, ml;
  renderer.getOrientedViewableTRBL(&mt, &mr, &mb, &ml);
  const int cx = ml;
  const int cw = W - ml - mr;
  const int top = mt;
  const int bottom = H - mb - BaseMetrics::values.buttonHintsHeight;
  const int ch = bottom - top;

  const int lh12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int lh10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int pad = 12;

  const int coverH = ch * 22 / 100;
  const int coverW = coverH * 3 / 4;
  const int coverX = cx;
  const int coverY = top;
  drawCover(book, coverX, coverY, coverW, coverH);

  const int infoX = coverX + coverW + pad;
  const int infoW = cx + cw - infoX;
  int yy = top + 2;
  const std::vector<std::string> titleLines =
      renderer.wrappedText(UI_12_FONT_ID, book.title, infoW, 3, EpdFontFamily::BOLD);
  for (const std::string& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, infoX, yy, line.c_str(), true, EpdFontFamily::BOLD);
    yy += lh12;
  }
  if (book.author[0] != '\0') {
    const std::string author = renderer.truncatedText(UI_10_FONT_ID, book.author, infoW);
    renderer.drawText(UI_10_FONT_ID, infoX, yy, author.c_str(), true);
    yy += lh10;
  }
  yy += 6;
  const int barH = 10;
  renderer.drawRect(infoX, yy, infoW, barH, true);
  const int fillW = (infoW - 2) * book.progressPercent / 100;
  if (fillW > 0) {
    renderer.fillRect(infoX + 1, yy + 1, fillW, barH - 2, true);
  }
  yy += barH + 4;
  std::snprintf(buf, sizeof(buf), "%u%% complete", book.progressPercent);
  renderer.drawText(UI_10_FONT_ID, infoX, yy, buf, true);
  yy += lh10;

  const int headerBottom = ((coverY + coverH) > yy ? (coverY + coverH) : yy) + pad;
  renderer.drawLine(cx, headerBottom - pad / 2, cx + cw, headerBottom - pad / 2, true);

  const float totMin = static_cast<float>(book.totalReadingMs) / 60000.0f;
  const int cols = 2;
  const int rows = 3;
  const int cellW = cw / cols;
  const int cellH = lh12 + lh10 + pad;
  const int gridTop = headerBottom;

  for (int i = 0; i < 6; ++i) {
    char value[24];
    char label[24];
    switch (i) {
      case 0:
        fmtDuration(book.totalReadingMs, value, sizeof(value));
        std::snprintf(label, sizeof(label), "time spent");
        break;
      case 1:
        std::snprintf(value, sizeof(value), "%u", book.sessionCount);
        std::snprintf(label, sizeof(label), "sessions");
        break;
      case 2: {
        const float avg = (book.sessionCount > 0) ? totMin / static_cast<float>(book.sessionCount) : 0.0f;
        std::snprintf(value, sizeof(value), "%.1f", avg);
        std::snprintf(label, sizeof(label), "avg min/session");
        break;
      }
      case 3: {
        const float ppm = (totMin > 0.05f) ? static_cast<float>(book.totalPagesRead) / totMin : 0.0f;
        std::snprintf(value, sizeof(value), "%.2f", ppm);
        std::snprintf(label, sizeof(label), "avg pages/min");
        break;
      }
      case 4:
        if (book.progressPercent == 0 || book.totalReadingMs == 0) {
          std::snprintf(value, sizeof(value), "--");
        } else if (book.progressPercent >= 100) {
          std::snprintf(value, sizeof(value), "Done");
        } else {
          const uint64_t msPerPct = static_cast<uint64_t>(book.totalReadingMs) / book.progressPercent;
          const uint64_t remMs = msPerPct * (100u - book.progressPercent);
          const uint32_t rh = static_cast<uint32_t>(remMs / 3600000ULL);
          const uint32_t rm = static_cast<uint32_t>((remMs % 3600000ULL) / 60000ULL);
          if (rh > 0) {
            std::snprintf(value, sizeof(value), "~%uh %um", rh, rm);
          } else {
            std::snprintf(value, sizeof(value), "~%um", rm);
          }
        }
        std::snprintf(label, sizeof(label), "time to finish");
        break;
      default:
        std::snprintf(value, sizeof(value), "%u min", static_cast<unsigned>(book.lastSessionMs / 60000UL));
        std::snprintf(label, sizeof(label), "last session");
        break;
    }
    const int gcol = i % cols;
    const int grow = i / cols;
    const int gxx = cx + gcol * cellW;
    const int gyy = gridTop + grow * cellH;
    const int vw = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, gxx + (cellW - vw) / 2, gyy, value, true, EpdFontFamily::BOLD);
    const int lw = renderer.getTextWidth(UI_10_FONT_ID, label);
    renderer.drawText(UI_10_FONT_ID, gxx + (cellW - lw) / 2, gyy + lh12, label, true);
  }
  const int gridBottom = gridTop + rows * cellH;

  renderer.drawLine(cx, gridBottom + pad / 2, cx + cw, gridBottom + pad / 2, true);
  const int sy = gridBottom + pad;
  renderer.drawText(UI_10_FONT_ID, cx, sy, "Reading speed (pages/hr)", true);

  uint16_t pph[4];
  const uint8_t n = (book.speedCount > 4) ? 4 : book.speedCount;
  for (uint8_t i = 0; i < n; ++i) {
    const uint8_t idx = static_cast<uint8_t>((book.speedHead + 4 - n + i) % 4);
    pph[i] = book.speedSamples[idx].pagesPerHour;
  }
  if (n >= 2) {
    const uint16_t tol = static_cast<uint16_t>(pph[0] / 12 + 1);
    const char* trend = (pph[n - 1] > pph[0] + tol) ? "faster" : (pph[n - 1] + tol < pph[0]) ? "slower" : "steady";
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, trend);
    renderer.drawText(UI_10_FONT_ID, cx + cw - tw, sy, trend, true);
    uint16_t mx = 1;
    for (uint8_t i = 0; i < n; ++i) {
      if (pph[i] > mx) mx = pph[i];
    }
    const int barsY = sy + lh10 + 4;
    const int barsH = 30;
    const int slot = cw / n;
    for (uint8_t i = 0; i < n; ++i) {
      const int bh = static_cast<int>(static_cast<uint32_t>(pph[i]) * barsH / mx);
      const int bw = slot * 6 / 10;
      const int bxx = cx + i * slot + (slot - bw) / 2;
      renderer.fillRect(bxx, barsY + (barsH - bh), bw, bh, true);
    }
  } else {
    renderer.drawText(UI_10_FONT_ID, cx, sy + lh10 + 4, "Not enough data yet", true);
  }

  char allTime[24];
  fmtDuration(global.totalReadingMs, allTime, sizeof(allTime));
  std::snprintf(buf, sizeof(buf), "All time: %s", allTime);
  renderer.drawText(UI_10_FONT_ID, cx, bottom - lh10, buf, true);
}

void DetailedStatsActivity::drawCover(const BookStatEntry& book, int x, int y, int w, int h) const {
  const std::string thumbPath =
      UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), HomeRenderer::kThumbnailCoverHeight);
  FsFile f;
  if (Storage.openFileForRead("STATS", thumbPath.c_str(), f)) {
    Bitmap bmp(f, false);
    if (bmp.parseHeaders() == BmpReaderError::Ok) {
      if (bmp.is1Bit()) {
        renderer.drawBitmapStretched1Bit(bmp, x, y, w, h);
      } else {
        renderer.drawBitmap(bmp, x, y, w, h);
      }
      renderer.roundCoverCorners(x, y, w, h, HomeRenderer::kCoverCornerRadius);
      f.close();
      return;
    }
    f.close();
  }
  drawCoverPlaceholder(x, y, w, h, book.title);
}

void DetailedStatsActivity::drawCoverPlaceholder(int x, int y, int w, int h, const char* /*title*/) const {
  // Optional system asset takes precedence; otherwise an empty bordered rect
  // (matches the home screen's no-cover behaviour). Title is rendered next to
  // the cover by the caller, so we don't overlay it inside the box.
  static constexpr const char* PLACEHOLDER_PATH = "/.crosspoint/system/BasicCover.bmp";
  if (Storage.exists(PLACEHOLDER_PATH)) {
    FsFile f;
    if (Storage.openFileForRead("STATS", PLACEHOLDER_PATH, f)) {
      Bitmap bmp(f, false);
      if (bmp.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(bmp, x + 2, y + 2, w - 4, h - 4);
        renderer.roundCoverCorners(x, y, w, h, HomeRenderer::kCoverCornerRadius);
        f.close();
        return;
      }
      f.close();
    }
  }
  renderer.drawRoundedRect(x, y, w, h, 1, HomeRenderer::kCoverCornerRadius, true);
}
