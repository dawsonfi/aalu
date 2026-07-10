#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "components/HomeProgressCache.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"

void SleepActivity::onEnter() {
  Activity::onEnter();
  GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM_INSIGHTS):
      // Same image discovery + selection as CUSTOM; the overlay is drawn
      // inside renderBitmapSleepScreen by inspecting SETTINGS.sleepScreen.
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      return renderCoverSleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::cycleWallpaper(GfxRenderer& renderer) {
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");
  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    if (dir) dir.close();
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }
  if (!sleepDir) {
    if (dir) dir.close();
    LOG_DBG("SLP", "Cycle wallpaper: no /.sleep directory");
    return;
  }

  std::vector<std::string> files;
  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    file.getName(name, sizeof(name));
    std::string filename(name);
    if (filename[0] == '.' || !FsHelpers::hasBmpExtension(filename)) {
      file.close();
      continue;
    }
    files.push_back(std::move(filename));
    file.close();
  }
  dir.close();

  if (files.empty()) {
    LOG_DBG("SLP", "Cycle wallpaper: no wallpapers found");
    return;
  }
  FsHelpers::sortFileList(files);

  const size_t count = files.size();
  const size_t start =
      (APP_STATE.lastSleepImage == UINT8_MAX) ? 0 : (static_cast<size_t>(APP_STATE.lastSleepImage) + 1) % count;

  for (size_t attempt = 0; attempt < count; attempt++) {
    const size_t idx = (start + attempt) % count;
    const std::string path = std::string(sleepDir) + "/" + files[idx];
    FsFile file;
    if (!Storage.openFileForRead("SLP", path, file)) {
      LOG_ERR("SLP", "Cycle wallpaper: cannot open %s", path.c_str());
      continue;
    }
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) {
      file.close();
      LOG_ERR("SLP", "Cycle wallpaper: invalid BMP %s", path.c_str());
      continue;
    }

    const int pageWidth = renderer.getScreenWidth();
    const int pageHeight = renderer.getScreenHeight();
    int x, y;
    if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
      const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
      if (ratio > screenRatio) {
        x = 0;
        y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      } else {
        x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
        y = 0;
      }
    } else {
      x = (pageWidth - bitmap.getWidth()) / 2;
      y = (pageHeight - bitmap.getHeight()) / 2;
    }

    renderer.clearScreen();
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    file.close();

    APP_STATE.lastSleepImage = static_cast<uint8_t>(idx);
    LOG_DBG("SLP", "Cycled wallpaper to %s", files[idx].c_str());
    return;
  }

  LOG_ERR("SLP", "Cycle wallpaper: no valid wallpaper found");
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");
  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    if (dir) dir.close();
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid BMP files
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        file.close();
        continue;
      }

      if (!FsHelpers::hasBmpExtension(filename)) {
        LOG_DBG("SLP", "Skipping non-.bmp file name: %s", name);
        file.close();
        continue;
      }
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
        file.close();
        continue;
      }
      files.emplace_back(filename);
      file.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Generate a random number between 1 and numFiles
      auto randomFileIndex = random(numFiles);
      // If we picked the same image as last time, reroll
      while (numFiles > 1 && APP_STATE.lastSleepImage != UINT8_MAX && randomFileIndex == APP_STATE.lastSleepImage) {
        randomFileIndex = random(numFiles);
      }
      APP_STATE.lastSleepImage = randomFileIndex;
      APP_STATE.saveToFile();
      const auto filename = std::string(sleepDir) + "/" + files[randomFileIndex];
      FsFile file;
      if (Storage.openFileForRead("SLP", filename, file)) {
        LOG_DBG("SLP", "Randomly loading: %s/%s", sleepDir, files[randomFileIndex].c_str());
        delay(100);
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          file.close();
          dir.close();
          return;
        }
        file.close();
      }
    }
  }
  if (dir) dir.close();

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  FsFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_AALU), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // CUSTOM_INSIGHTS overlays the book title + read percentage on top of the
  // bitmap. Drawn here before displayBuffer so it makes it onto the same
  // frame as the background. Grayscale is skipped below since the grayscale
  // path repaints via displayGrayBuffer and would clobber the overlay.
  const bool drawInsights = (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM_INSIGHTS);
  if (drawInsights) {
    drawBookInsightsOverlay();
  }

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  if (hasGreyscale && !drawInsights) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::drawBookInsightsOverlay() const {
  if (APP_STATE.openEpubPath.empty()) return;

  // Title: prefer the cached title from RecentBooksStore (already loaded at
  // boot, no SD round-trip). Fall back to the filename so we never render an
  // empty overlay — worse than no overlay at all.
  std::string title;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path == APP_STATE.openEpubPath) {
      title = book.title;
      break;
    }
  }
  if (title.empty()) {
    const size_t slash = APP_STATE.openEpubPath.find_last_of('/');
    title = (slash != std::string::npos) ? APP_STATE.openEpubPath.substr(slash + 1) : APP_STATE.openEpubPath;
  }
  if (title.empty()) return;

  // Progress: actively hydrate the cache before the lookup. HomeActivity's
  // onExit calls HomeProgressCache::clear(), so if the user went
  // home → reader → sleep, the in-memory cache is empty when we get here.
  // loadProgressFor reads home_progress.json (and falls back to recomputing
  // from book.bin if the cache is stale), then getProgress returns the
  // freshly-loaded percent. Without this call, getProgress always returns
  // Unknown in that flow and the percentage stays hidden.
  HomeProgressCache::getInstance().loadProgressFor(APP_STATE.openEpubPath);
  const int8_t percent = HomeProgressCache::getInstance().getProgress(APP_STATE.openEpubPath);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int kSidePadding = 16;
  constexpr int kBottomPadding = 16;
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int percentLineHeight = renderer.getLineHeight(SMALL_FONT_ID);

  // Adaptive contrast: count ink pixels in the bounding box the text is about
  // to occupy. If the majority of the underlying wallpaper is dark, draw the
  // text in white; otherwise draw black. Stride-3 sampling is sufficient for
  // a majority estimate and keeps this cheap (one short call per overlay,
  // bounded by text area on a 1-bit framebuffer).
  auto isRegionDark = [&](int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    constexpr int kStride = 3;
    int dark = 0;
    int total = 0;
    for (int dy = 0; dy < h; dy += kStride) {
      for (int dx = 0; dx < w; dx += kStride) {
        if (renderer.readPixel(x + dx, y + dy)) ++dark;
        ++total;
      }
    }
    return total > 0 && dark * 2 > total;
  };

  // Title hugs the bottom-center; percent (if any) sits in the bottom-right.
  // Reserve space on the right for the percent so the centered title doesn't
  // collide with it; titles longer than that budget get an ellipsis.
  constexpr int kPercentReserveWidth = 56;
  const int titleMaxWidth = pageWidth - 2 * kSidePadding - ((percent >= 0) ? kPercentReserveWidth : 0);
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), titleMaxWidth, EpdFontFamily::BOLD);
  const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD);
  const int titleX = (pageWidth - titleWidth) / 2;
  const int titleY = pageHeight - kBottomPadding - titleLineHeight;
  const bool titleOnDark = isRegionDark(titleX, titleY, titleWidth, titleLineHeight);
  renderer.drawText(UI_12_FONT_ID, titleX, titleY, truncTitle.c_str(), /*black=*/!titleOnDark, EpdFontFamily::BOLD);

  if (percent >= 0) {
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%d%%", static_cast<int>(percent));
    const int pctWidth = renderer.getTextWidth(SMALL_FONT_ID, pctBuf);
    const int pctX = pageWidth - kSidePadding - pctWidth;
    const int pctY = pageHeight - kBottomPadding - percentLineHeight;
    const bool pctOnDark = isRegionDark(pctX, pctY, pctWidth, percentLineHeight);
    renderer.drawText(SMALL_FONT_ID, pctX, pctY, pctBuf, /*black=*/!pctOnDark);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
