#include "EpubReaderActivity.h"

#include <Epub/BionicReading.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "JsonSettingsIO.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "activities/settings/ClockSyncActivity.h"
#include "components/HomeProgressCache.h"
#include "components/HomeRenderer.h"  // for kHeroCoverHeight / kThumbnailCoverHeight
#include "components/UITheme.h"
#include "fontIds.h"
#include "stats/ReadingStatsManager.h"  // added when developing Statistics menu
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

// Dictionary development
#include "DictionaryWordSelectActivity.h"
#include "LookedUpWordsActivity.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
// pages per minute, first item is 1 to prevent division by zero if accessed
const std::vector<int> PAGE_TURN_LABELS = {1, 1, 3, 6, 12};
const char* const PAGE_TURN_DISPLAY[] = {"Off", "1", "3", "6", "12"};
constexpr int PAGE_TURN_OPTION_COUNT = 5;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD folder that finished books are relocated into when moveFinishedToReadFolder is on.
constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

// Pick a non-colliding destination inside /read/ for a finished book ("name.epub" -> "name (2).epub").
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;
  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }
  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, repointing the recents entry (only if it
// was still listed) and the resume pointer. On book-rename failure everything is left in place; a
// failed cache-dir rename is non-fatal (the book just re-indexes at its new path).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath, const std::string& title, const std::string& author,
                                  const std::string& oldThumb, const std::string& seriesName,
                                  const std::string& seriesIndex) {
  LOG_INF("ERS", "Moving finished book: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to Read folder");
    return;
  }

  std::string newThumb = oldThumb;
  const size_t epubPos = oldCachePath.rfind("/epub_");
  if (epubPos != std::string::npos && Storage.exists(oldCachePath.c_str())) {
    const std::string cacheDir = oldCachePath.substr(0, epubPos);
    const std::string newCachePath = cacheDir + "/epub_" + std::to_string(FsHelpers::cachePathHash(dstPath));
    if (Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      if (newThumb.compare(0, oldCachePath.size(), oldCachePath) == 0) {
        newThumb = newCachePath + newThumb.substr(oldCachePath.size());
      }
    } else {
      LOG_ERR("ERS", "Failed to rename cache dir (non-fatal): %s", oldCachePath.c_str());
    }
  }

  if (RECENT_BOOKS.removeBook(srcPath)) {
    RECENT_BOOKS.addBook(dstPath, title, author, newThumb, seriesName, seriesIndex);
  }
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  sessionPagesTurned = 0;  // RESET: Ensure counter starts at zero for every session

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath(),
                       epub->getSeriesName(), epub->getSeriesIndex());

  // Trigger first update
  requestUpdate();
  // Pre-generate the thumbnails the home screen will need so closing the
  // book and returning home doesn't trigger a multi-second cover-rendering
  // pass. Hero size is for the freshly-promoted book; thumb size is for
  // when the user later opens a different book and this one slides into
  // the thumbnail row.
  if (!Storage.exists(epub->getThumbBmpPath(HomeRenderer::kHeroCoverHeight).c_str())) {
    epub->generateThumbBmp(HomeRenderer::kHeroCoverHeight);
  }
  if (!Storage.exists(epub->getThumbBmpPath(HomeRenderer::kThumbnailCoverHeight).c_str())) {
    epub->generateThumbBmp(HomeRenderer::kThumbnailCoverHeight);
  }

  StatsManager.beginSession(
      epub->getCachePath().c_str(), epub->getTitle().c_str(),
      epub->getAuthor().c_str(),  // new
      epub->getPath().c_str(), epub->getThumbBmpPath().c_str(),
      static_cast<uint8_t>(epub->progressPercent(currentSpineIndex, nextPageNumber, cachedChapterTotalPageCount)));
}

void EpubReaderActivity::onExit() {
  // End reading stats session — compute current progress
  {
    const int currentPage = section ? section->currentPage : 0;
    const int pageCount = section ? section->estimatedTotalPages() : 0;
    const uint8_t prog = static_cast<uint8_t>(epub->progressPercent(currentSpineIndex, currentPage, pageCount));
    StatsManager.endSession(prog, sessionPagesTurned);  // new
  }
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string title = epub->getTitle();
    const std::string author = epub->getAuthor();
    const std::string oldThumb = epub->getThumbBmpPath();
    const std::string seriesName = epub->getSeriesName();
    const std::string seriesIndex = epub->getSeriesIndex();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and its open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath, title, author, oldThumb, seriesName, seriesIndex);
  } else {
    epub.reset();
  }
}

bool EpubReaderActivity::heapAboveFloors(const size_t minFreeHeap, const size_t minMaxAlloc) {
  return ESP.getFreeHeap() >= minFreeHeap && ESP.getMaxAllocHeap() >= minMaxAlloc;
}

bool EpubReaderActivity::buildTickHeapGate() {
  buildHeapPaused = !heapAboveFloors(BACKGROUND_BUILD_MIN_FREE_HEAP, BACKGROUND_BUILD_MIN_MAX_ALLOC);
  return !buildHeapPaused;
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // Drive a still-building section forward a little each tick so a large chapter finishes laying
  // out in the background while the reader shows the pages already built. Under the render lock so
  // it never races render()/pageTurn() mutating the section; skipped when the lock is already held,
  // and re-checked under the lock since another path may have finished the build in between.
  if (section && section->isBuilding() && !RenderLock::peek() && buildTickHeapGate()) {
    RenderLock lock(*this);
    // Re-check under the lock: render() (also RenderLock-guarded) may have finalized the build
    // between the outer check and acquiring the lock, and it may have grown retained glyph
    // buffers in the process, invalidating the pre-lock heap reading.
    if (section && section->isBuilding() && buildTickHeapGate() &&
        !section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
      section.reset();
      requestUpdate();
    }
  }

  // Idle glyph prewarm for the likely next page (currentPage + 1). The scan pass draws nothing
  // (FontCacheManager scan mode suppresses pixels), so the displayed framebuffer is untouched;
  // endScanAndPrewarm loads only glyphs not already cached. Debounced past rapid page-flipping,
  // one attempt per position, and deferred while a render/build owns the CPU or the heap is at the
  // render floor. Cross-chapter prewarm is deliberately out of scope (next spine's section isn't
  // loaded).
  if (section && !section->isBuilding() && !RenderLock::peek() && renderer.getFrameBuffer() != nullptr &&
      lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
      heapAboveFloors(RENDER_MIN_FREE_HEAP, BACKGROUND_BUILD_MIN_MAX_ALLOC) &&
      (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
    RenderLock lock(*this);  // the page table must not change under the scan
    // Re-check under the lock: peek() and acquisition are not atomic, so the render path may have
    // reset/replaced the section or moved the page in between.
    if (section && !section->isBuilding() &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      idlePrewarmSpine = currentSpineIndex;
      idlePrewarmPage = section->currentPage;
      const int nextPage = section->currentPage + 1;
      if (nextPage < static_cast<int>(section->pageCount)) {
        if (const auto p = section->loadPage(nextPage)) {
          if (auto* fcm = renderer.getFontCacheManager()) {
            const auto t0 = millis();
            auto scope = fcm->createPrewarmScope();
            p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);  // scan only, no pixels
            scope.endScanAndPrewarm();
            LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
          }
        }
      }
    }
  }

  // Auto-dismiss the transient bookmark confirmation after a moment.
  if (showBookmarkMessage && millis() - bookmarkMessageTime > BOOKMARK_MESSAGE_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // --- QUICK SETTINGS INTERCEPT ---
  // Trap all inputs if the Quick Settings overlay is open.
  if (qsState != QuickSettingsState::CLOSED) {
    handleQuickSettingsInput();
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= ReaderUtils::QUICK_SETTINGS_LONG_PRESS_MS) {
    openQuickSettings();
    qsSuppressConfirmRelease = true;
    return;
  }

  if (currentSpineIndex >= epub->getSpineItemsCount() && endOfBookOptions.menuActive()) {
    std::string openPath;
    switch (endOfBookOptions.handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage:
        currentSpineIndex = epub->getSpineItemsCount() - 1;
        nextPageNumber = UINT16_MAX;
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() < ReaderUtils::QUICK_SETTINGS_LONG_PRESS_MS) {
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->estimatedTotalPages() : 0;
    const int bookProgressPercent = (epub->getBookSize() > 0 && section)
                                        ? clampPercent(epub->progressPercent(currentSpineIndex, section->currentPage,
                                                                             section->estimatedTotalPages()))
                                        : 0;

    const bool hasDictionary = Dictionary::exists();
    const bool hasLookupHistory = hasDictionary && LookupHistory::hasHistory(epub->getCachePath());

    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                               !currentPageFootnotes.empty(), hasDictionary, hasLookupHistory),

                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& menu = std::get<MenuResult>(result.data);
                               onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                             }
                           });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToLibrary(true);
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    // Instant feedback: the reader -> home transition takes ~700ms+ (reader onExit, home
    // onEnter cover/progress loading, full e-ink refresh). Without this popup the screen
    // stays on the last reader page for that full duration and the device feels frozen.
    // FAST_REFRESH paints the popup in ~50ms with no black flash; the subsequent home
    // render does its own FULL_REFRESH so ghosting clears.
    GUI.drawPopup(renderer, tr(STR_GOING_HOME));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    onGoHome();
    return;
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // any botton press when at end of the book goes back to the last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (endOfBookOptions.menuActive()) {
      return;
    }
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = UINT16_MAX;
    requestUpdate();
    return;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    lastPageTurnTime = millis();
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              RenderLock lock(*this);
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              nextPageNumber = 0;
              section.reset();
            }
          });
      break;
    }

    case EpubReaderMenuActivity::MenuAction::ADD_BOOKMARK: {
      toggleBookmark();
      requestUpdate();
      break;
    }

    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      const std::string path = epub->getPath();
      startActivityForResult(std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, path),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled && std::holds_alternative<ProgressChangeResult>(result.data)) {
                                 const auto& jump = std::get<ProgressChangeResult>(result.data);
                                 RenderLock lock(*this);
                                 currentSpineIndex = jump.spineIndex;
                                 nextPageNumber = jump.page;
                                 section.reset();
                               } else {
                                 requestUpdate();
                               }
                             });
      break;
    }

    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP: {
      std::unique_ptr<Page> pageForLookup;
      std::string nextPageFirstWord;
      int orientedMarginTop = 0;
      int orientedMarginLeft = 0;

      {
        RenderLock lock(*this);
        if (!section) {
          requestUpdate();
          break;
        }

        int orientedMarginRight;
        int orientedMarginBottom;
        renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                         &orientedMarginLeft);

        orientedMarginTop += SETTINGS.screenMargin;
        orientedMarginLeft += SETTINGS.screenMargin;
        orientedMarginRight += SETTINGS.screenMargin;
        orientedMarginTop += readerClockBandHeight();

        const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
        if (automaticPageTurnActive &&
            (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
          orientedMarginBottom += std::max(
              SETTINGS.screenMargin,
              static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
        } else {
          orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
        }

        pageForLookup = section->loadPageFromSectionFile();

        if (section->currentPage < section->pageCount - 1) {
          const int savedPage = section->currentPage;
          section->currentPage = savedPage + 1;
          auto nextPage = section->loadPageFromSectionFile();
          section->currentPage = savedPage;

          if (nextPage) {
            for (const auto& element : nextPage->elements) {
              if (!element || element->getTag() != TAG_PageLine) continue;

              const auto& line = static_cast<const PageLine&>(*element);
              auto block = line.getBlock();
              if (!block) continue;

              if (block->wordCount() > 0) {
                nextPageFirstWord = block->wordText(0);
                break;
              }
            }
          }
        }
      }

      if (pageForLookup) {
        startActivityForResult(
            std::make_unique<DictionaryWordSelectActivity>(
                renderer, mappedInput, std::move(pageForLookup), SETTINGS.getReaderFontId(), orientedMarginLeft,
                orientedMarginTop, epub->getCachePath(), SETTINGS.orientation, nextPageFirstWord),
            [this](const ActivityResult& result) { requestUpdate(); });
      }
      break;
    }

    case EpubReaderMenuActivity::MenuAction::LOOKED_UP_WORDS: {
      startActivityForResult(std::make_unique<LookedUpWordsActivity>(renderer, mappedInput, epub->getCachePath()),
                             [this](const ActivityResult& result) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READING_SETTINGS: {
      openQuickSettings();
      break;
    }

    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const int initialPercent = (epub && epub->getBookSize() > 0 && section)
                                     ? clampPercent(epub->progressPercent(currentSpineIndex, section->currentPage,
                                                                          section->estimatedTotalPages()))
                                     : 0;
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;

          fullText.reserve(1024);  // reserve 1KB to prevent memory fragmentation

          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& b = *line.getBlock();
                for (uint16_t i = 0; i < b.wordCount(); i++) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += b.wordText(i);
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->estimatedTotalPages();
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          saveProgress(backupSpine, backupPage, backupPageCount);
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : 0;
        const int totalPages = section ? section->estimatedTotalPages() : 0;

        // --- ADDED: Read exact DOM path from the SD Card cache ---
        std::string exactXPath = "";
        if (section && currentPage >= 0 && currentPage < totalPages) {
          auto p = section->loadPageFromSectionFile();
          if (p) exactXPath = p->syncXPath;
        }

        startActivityForResult(
            std::make_unique<KOReaderSyncActivity>(renderer, mappedInput, epub, epub->getPath(), currentSpineIndex,
                                                   currentPage, totalPages, exactXPath),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& sync = std::get<SyncResult>(result.data);
                if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
                  RenderLock lock(*this);
                  currentSpineIndex = sync.spineIndex;
                  nextPageNumber = sync.page;
                  section.reset();
                }
              }
            });
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC_CLOCK: {
      const int clockBandBefore = readerClockBandHeight();
      startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput),
                             [this, clockBandBefore](const ActivityResult&) {
                               if (readerClockBandHeight() == clockBandBefore) return;
                               RenderLock lock(*this);
                               if (section) {
                                 cachedSpineIndex = currentSpineIndex;
                                 cachedChapterTotalPageCount = section->estimatedTotalPages();
                                 nextPageNumber = section->currentPage;
                               }
                               section.reset();
                             });
      break;
    }
  }
}

void EpubReaderActivity::toggleBookmark() {
  if (!epub) {
    return;
  }

  int currentPage = 0;
  int pageCount = 0;
  std::string pageText;
  {
    RenderLock lock(*this);
    if (!section) {
      return;
    }
    pageCount = section->pageCount;
    currentPage = section->currentPage;
    if (currentPage >= 0 && currentPage < pageCount) {
      if (auto page = section->loadPageFromSectionFile()) {
        pageText.reserve(256);
        for (const auto& el : page->elements) {
          if (el->getTag() == TAG_PageLine) {
            const auto& line = static_cast<const PageLine&>(*el);
            if (line.getBlock()) {
              const auto& b = *line.getBlock();
              for (uint16_t i = 0; i < b.wordCount(); i++) {
                if (!pageText.empty()) {
                  pageText += " ";
                }
                pageText += b.wordText(i);
              }
            }
          }
          if (pageText.size() >= 128) {
            break;
          }
        }
      }
    }
  }

  std::vector<BookmarkEntry> bookmarks;
  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  if (Storage.exists(path.c_str())) {
    const String json = Storage.readFile(path.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(bookmarks, json.c_str());
    }
  }

  const size_t countBefore = bookmarks.size();
  bookmarks.erase(std::remove_if(bookmarks.begin(), bookmarks.end(),
                                 [&](const BookmarkEntry& b) {
                                   return b.computedSpineIndex == currentSpineIndex &&
                                          b.computedChapterProgress == currentPage;
                                 }),
                  bookmarks.end());

  const bool added = (bookmarks.size() == countBefore);
  if (added) {
    const KOReaderPosition koPos =
        ProgressMapper::toKOReader(epub, CrossPointPosition{currentSpineIndex, currentPage, pageCount});
    BookmarkEntry entry;
    entry.xpath = koPos.xpath;
    entry.percentage = koPos.percentage;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = static_cast<uint16_t>(currentSpineIndex);
    entry.computedChapterPageCount = static_cast<uint16_t>(pageCount);
    entry.computedChapterProgress = static_cast<uint16_t>(currentPage);
    bookmarks.insert(bookmarks.begin(), entry);
  }

  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  if (!JsonSettingsIO::saveBookmarks(bookmarks, path.c_str())) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
  }

  showBookmarkMessage = true;
  bookmarkRemoved = !added;
  bookmarkMessageTime = millis();
  requestUpdate();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->estimatedTotalPages();
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= PAGE_TURN_LABELS.size()) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_LABELS[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->estimatedTotalPages();
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    sessionPagesTurned++;  // new
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // Publish the bionic-reading flag for TextBlock::render — set once per top-level render so
  // every subsequent page->render call in this method (BW scan, BW emit, grayscale LSB/MSB)
  // sees the current setting without paying for parameter plumbing.
  BionicReading::enabled = SETTINGS.bionicReading;

  if (qsState != QuickSettingsState::CLOSED && !qsNeedsBackgroundRender) {
    renderQuickSettingsOverlay();
    renderer.displayBuffer();
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  // Finished-book automation: drop this book from Recents on the End-of-Book transition (re-add if
  // paged back in), and arm the /read/ relocation for onExit(). Transition-guarded on
  // recentsEntryRemoved so there are no per-frame writes while the End-of-Book screen shows.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeBook(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath(),
                           epub->getSeriesName(), epub->getSeriesIndex());
      recentsEntryRemoved = false;
    }
  }
  pendingReadFolderMove = atEndOfBook && SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    endOfBookOptions.loadOnce(epub->getPath());
    renderer.clearScreen();
    endOfBookOptions.render(renderer);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  // Margók és viewport számolás
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginTop += readerClockBandHeight();

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer);
    if (!section) {
      LOG_ERR("ERS", "OOM allocating Section");
      return;
    }
    const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

    if (!section->loadSectionFile(renderSpec)) {
      // Percent jumps, anchor jumps (footnotes/ToC) and settings-change repagination all need the
      // whole chapter up front (final page count / anchor map), so they build fully behind the
      // indexing popup. A plain resume lays out just enough to reach the landing page and lets
      // loop() build the rest behind it, so the first page appears without waiting on the chapter.
      const bool needsFullBuild = pendingPercentJump || !pendingAnchor.empty() ||
                                  (cachedChapterTotalPageCount > 0 && currentSpineIndex == cachedSpineIndex);
      bool built;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };
        built = section->createSectionFile(renderSpec, popupFn);
      } else {
        // UINT16_MAX (resume to last page) makes target huge, so the loop below builds to
        // completion; an ordinary page target builds just past it. Show the popup only for a deep
        // resume so a shallow landing stays popup-free.
        const int target = (nextPageNumber < 0) ? 0 : nextPageNumber;
        const size_t spineBytes = epub->getCumulativeSpineItemSize(currentSpineIndex) -
                                  (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
        if (target > BUILD_POPUP_PAGE_THRESHOLD || spineBytes > BUILD_POPUP_BYTE_THRESHOLD) {
          GUI.drawPopup(renderer, tr(STR_INDEXING));
          pagesUntilFullRefresh = 1;
        }
        built = section->startBuild(renderSpec);
        while (built && !section->isBuildComplete() && static_cast<int>(section->pageCount) <= target) {
          built = section->buildSomeMore(BUILD_PAGES_PER_CHUNK);
        }
      }
      if (!built) {
        // A build failure here means an invalid/corrupt EPUB that failed to parse. Surface an
        // explicit error instead of leaving the "Indexing" popup on screen with no way forward.
        LOG_ERR("ERS", "Failed to index section %d (invalid book)", currentSpineIndex);
        section.reset();
        automaticPageTurnActive = false;
        renderer.clearScreen();
        GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        return;
      }
    }

    if (nextPageNumber == UINT16_MAX)
      section->currentPage = section->pageCount - 1;
    else
      section->currentPage = nextPageNumber;

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) section->currentPage = *page;
      pendingAnchor.clear();
    }

    if (cachedChapterTotalPageCount > 0) {
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        section->currentPage = static_cast<int>(progress * section->pageCount);
      }
      cachedChapterTotalPageCount = 0;
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // Extend an in-progress incremental build until the page we're about to show is laid out. Runs
  // every render, so it also covers a forward turn that got ahead of the background builder;
  // already-built pages do no work here.
  if (section->isBuilding()) {
    bool ok = true;
    while (ok && !section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      ok = section->buildSomeMore(BUILD_PAGES_PER_CHUNK);
    }
    if (!ok) {
      LOG_ERR("ERS", "Failed during incremental section build");
      section.reset();
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }
  }

  // Once the real page count is known, clamp a target that lands past the end (the UINT16_MAX
  // "last page" sentinel from backward chapter navigation, or a stale saved position).
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  renderer.clearScreen();

  if (section->pageCount == 0 || section->currentPage < 0 || section->currentPage >= section->pageCount) {
    renderer.drawCenteredText(UI_12_FONT_ID, 300,
                              section->pageCount == 0 ? tr(STR_EMPTY_CHAPTER) : tr(STR_OUT_OF_BOUNDS), true,
                              EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from section file");
      automaticPageTurnActive = false;
      // Retrying rebuilds a transiently corrupt section and usually recovers, but a page that
      // keeps failing would loop forever on a blank screen, so bound the retries before giving up.
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;  // reset so a later user-initiated navigation can try afresh
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        return;
      }
      requestUpdate();  // try again after clearing cache
      return;
    }
    pageLoadRetryCount = 0;  // reset once a page loads cleanly

    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();

    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }

  // Only persist when the position actually changed. render() also runs on menu,
  // bookmark and screenshot re-renders, and writeAtomic is several FAT ops for 6 bytes.
  // Every real page turn changes currentPage, so progress durability is unaffected.
  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->estimatedTotalPages() != lastSavedPageCount) {
    // Only mark this position as saved on success -- a failed write (e.g. a transient SD error)
    // must stay eligible for retry on the next render, or the failure would be silently permanent.
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  if (showBookmarkMessage && qsState == QuickSettingsState::CLOSED) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }

  if (qsState != QuickSettingsState::CLOSED) {
    renderQuickSettingsOverlay();

    if (qsNeedsBackgroundRender) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    } else {
      renderer.displayBuffer();
    }
    qsNeedsBackgroundRender = false;
  }

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  lastRenderCompleteMs = millis();
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount)) {
    LOG_ERR("ERS", "Could not save progress!");
    return false;
  }

  // Push the percent into the home cache so the next home entry paints the
  // progress ring instantly without re-parsing book.bin. Stays chapter-grain
  // (chapter-start fraction) so recordProgress no-ops on every page turn
  // within a chapter — page-grain would write home_progress.json on every
  // forward, hammering flash. The one exception is the last page of the
  // last chapter: chapter-start there is ~99%, but the user has finished
  // the book and home should show 100%, so we override.
  const int spineCount = epub->getSpineItemsCount();
  const bool atEndOfBook =
      (spineCount > 0 && currentSpineIndex == spineCount - 1 && pageCount > 0 && currentPage >= pageCount - 1);
  const int percent = atEndOfBook ? 100 : static_cast<int>(epub->calculateProgress(currentSpineIndex, 0.0f) * 100.0f);
  HomeProgressCache::getInstance().recordProgress(epub->getPath(), currentSpineIndex, static_cast<int8_t>(percent));
  return true;
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  const uint32_t heapBefore = esp_get_free_heap_size();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap: before=%lu after=%lu delta=%ld", heapBefore, heapAfter,
          (int32_t)heapAfter - (int32_t)heapBefore);

  // --- QUICK SETTINGS OPTIMIZATION FLAG ---
  // If the overlay is open, we suppress display updates and heavy grayscale logic
  // to ensure instantaneous, flicker-free menu navigation.
  const bool isQsOpen = (qsState != QuickSettingsState::CLOSED);

  // Force special handling for pages with images when anti-aliasing is on
  bool imagePageWithAA = page->hasImages() && SETTINGS.textAntiAliasing;

  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  fcm->logStats("bw_render");
  const auto tBwRender = millis();

  // ONLY push to display here if Quick Settings is CLOSED
  if (imagePageWithAA) {
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      if (!isQsOpen) renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      if (!isQsOpen) renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      if (!isQsOpen) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  } else {
    if (!isQsOpen) ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Save bw buffer to reset buffer state after grayscale data sync
  renderer.storeBwBuffer();
  const auto tBwStore = millis();

  // grayscale rendering
  // SKIP grayscale completely if QuickSettings is open to ensure instant menu navigation!
  if (SETTINGS.textAntiAliasing && !isQsOpen) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    fcm->logStats("gray");

    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
            tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    // restore the bw data (Menu will be drawn on top of this later in render() )
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tBwRestore - tBwStore,
            tEnd - t0);
  }
}

int EpubReaderActivity::readerClockBandHeight() const {
  if (SETTINGS.statusBarClock == CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE ||
      !halClock.isAvailable()) {
    return 0;
  }
  constexpr int kReaderClockGap = 6;
  return renderer.getLineHeight(SMALL_FONT_ID) + kReaderClockGap;
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->estimatedTotalPages();
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true);

  if (readerClockBandHeight() > 0) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      int orientedTop, orientedRight, orientedBottom, orientedLeft;
      renderer.getOrientedViewableTRBL(&orientedTop, &orientedRight, &orientedBottom, &orientedLeft);
      const int clockWidth = renderer.getTextWidth(SMALL_FONT_ID, timeBuf);
      const int viewableWidth = renderer.getScreenWidth() - orientedLeft - orientedRight;
      const int clockX = orientedLeft + (viewableWidth - clockWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, clockX, orientedTop + 4, timeBuf);
    }
  }
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

// ============================================================================
// QUICK SETTINGS OVERLAY IMPLEMENTATION
// ============================================================================

const SettingInfo* EpubReaderActivity::qsItemAt(int tab, int index) const {
  if (tab == 0) {
    const auto& reader = getQuickSettingsReaderItems();
    if (index >= 0 && index < static_cast<int>(reader.size())) {
      return reader[index];
    }
    return nullptr;
  }
  const auto& controls = getQuickSettingsControlsItems();
  if (index >= 0 && index < static_cast<int>(controls.size())) {
    return controls[index];
  }
  return nullptr;
}

int EpubReaderActivity::getQsItemCount(int tab) const {
  if (tab == 0) {
    return static_cast<int>(getQuickSettingsReaderItems().size()) + 1;
  }
  return static_cast<int>(getQuickSettingsControlsItems().size());
}

const char* EpubReaderActivity::getQsItemName(int tab, int index) const {
  const SettingInfo* setting = qsItemAt(tab, index);
  if (setting == nullptr) {
    return tr(STR_AUTO_TURN_PAGES_PER_MIN);
  }
  return I18N.get(setting->nameId);
}

const char* EpubReaderActivity::getQsItemValue(int tab, int index, char* tempBuf, size_t tempBufSize) const {
  const SettingInfo* setting = qsItemAt(tab, index);
  if (setting == nullptr) {
    const uint8_t option = (autoPageTurnOption < PAGE_TURN_OPTION_COUNT) ? autoPageTurnOption : 0;
    return PAGE_TURN_DISPLAY[option];
  }

  if (setting->nameId == StrId::STR_FONT_FAMILY) {
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      return SETTINGS.sdFontFamilyName;
    }
    return (SETTINGS.fontFamily == 0) ? tr(STR_BOOKERLY) : tr(STR_NOTO_SANS);
  }

  if (setting->nameId == StrId::STR_ORIENTATION) {
    const uint8_t value = (qsPendingOrientation < setting->enumValues.size()) ? qsPendingOrientation : 0;
    return I18N.get(setting->enumValues[value]);
  }

  if (setting->valuePtr == nullptr) {
    return "";
  }

  if (setting->type == SettingType::ENUM) {
    const uint8_t value = SETTINGS.*(setting->valuePtr);
    if (value < setting->enumValues.size()) {
      return I18N.get(setting->enumValues[value]);
    }
    return "";
  }

  if (setting->type == SettingType::VALUE) {
    if (setting->nameId == StrId::STR_SCREEN_MARGIN) {
      snprintf(tempBuf, tempBufSize, "%d %s", SETTINGS.*(setting->valuePtr), tr(STR_PX));
    } else {
      snprintf(tempBuf, tempBufSize, "%d", SETTINGS.*(setting->valuePtr));
    }
    return tempBuf;
  }

  return "";
}

BaseTheme::ListToggleState EpubReaderActivity::getQsItemToggle(int tab, int index) const {
  const SettingInfo* setting = qsItemAt(tab, index);
  if (setting == nullptr || setting->type != SettingType::TOGGLE || setting->valuePtr == nullptr) {
    return BaseTheme::ListToggleState::NotToggle;
  }
  return (SETTINGS.*(setting->valuePtr)) ? BaseTheme::ListToggleState::On : BaseTheme::ListToggleState::Off;
}

void EpubReaderActivity::adjustQsItemValue(int tab, int index, bool increment) {
  const SettingInfo* setting = qsItemAt(tab, index);

  auto cycle = [increment](uint8_t value, int count) -> uint8_t {
    if (count <= 0) {
      return value;
    }
    if (increment) {
      return static_cast<uint8_t>((value + 1) % count);
    }
    return static_cast<uint8_t>((value == 0) ? count - 1 : value - 1);
  };

  if (setting == nullptr) {
    autoPageTurnOption = cycle(autoPageTurnOption, PAGE_TURN_OPTION_COUNT);
    return;
  }

  if (setting->nameId == StrId::STR_FONT_FAMILY) {
    const auto& families = sdFontSystem.registry().getFamilies();
    const int sdCount = static_cast<int>(families.size());
    const int total = CrossPointSettings::BUILTIN_FONT_COUNT + sdCount;
    int current;
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      current = CrossPointSettings::BUILTIN_FONT_COUNT;
      for (int i = 0; i < sdCount; i++) {
        if (families[i].name == SETTINGS.sdFontFamilyName) {
          current = CrossPointSettings::BUILTIN_FONT_COUNT + i;
          break;
        }
      }
    } else {
      current = SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
    }
    const int next = increment ? (current + 1) % total : (current == 0 ? total - 1 : current - 1);
    if (next < CrossPointSettings::BUILTIN_FONT_COUNT) {
      SETTINGS.fontFamily = static_cast<uint8_t>(next);
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s",
               families[next - CrossPointSettings::BUILTIN_FONT_COUNT].name.c_str());
    }
    return;
  }

  if (setting->nameId == StrId::STR_ORIENTATION) {
    qsPendingOrientation = cycle(qsPendingOrientation, static_cast<int>(setting->enumValues.size()));
    return;
  }

  if (setting->valuePtr == nullptr) {
    return;
  }

  switch (setting->type) {
    case SettingType::TOGGLE:
      SETTINGS.*(setting->valuePtr) = (SETTINGS.*(setting->valuePtr)) ? 0 : 1;
      break;
    case SettingType::ENUM:
      SETTINGS.*(setting->valuePtr) =
          cycle(SETTINGS.*(setting->valuePtr), static_cast<int>(setting->enumValues.size()));
      break;
    case SettingType::VALUE: {
      const int step = setting->valueRange.step;
      const int minValue = setting->valueRange.min;
      const int maxValue = setting->valueRange.max;
      const int currentValue = SETTINGS.*(setting->valuePtr);
      int newValue;
      if (increment) {
        newValue = (currentValue + step > maxValue) ? minValue : currentValue + step;
      } else {
        newValue = (currentValue - step < minValue) ? maxValue : currentValue - step;
      }
      SETTINGS.*(setting->valuePtr) = static_cast<uint8_t>(newValue);
      break;
    }
    default:
      break;
  }
}

void EpubReaderActivity::openQuickSettings() {
  qsState = QuickSettingsState::TAB_FOCUSED;
  qsSelectedTab = 0;
  qsSelectedItem = 0;
  qsPendingOrientation = SETTINGS.orientation;
  qsNeedsBackgroundRender = true;
  requestUpdate();
}

void EpubReaderActivity::handleQuickSettingsInput() {
  if (qsSuppressConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      qsSuppressConfirmRelease = false;
    }
    return;
  }

  const int itemCount = getQsItemCount(qsSelectedTab);

  if (qsState == QuickSettingsState::TAB_FOCUSED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      qsSelectedTab = (qsSelectedTab == 0) ? 1 : 0;
      qsSelectedItem = 0;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      qsState = QuickSettingsState::ITEM_FOCUSED;
      qsSelectedItem = 0;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      qsState = QuickSettingsState::ITEM_FOCUSED;
      qsSelectedItem = itemCount - 1;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
               mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Close overlay and commit changes
      closeAndApplyQuickSettings();
    }
  } else if (qsState == QuickSettingsState::ITEM_FOCUSED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      qsSelectedItem--;
      if (qsSelectedItem < 0) {
        qsState = QuickSettingsState::TAB_FOCUSED;
        qsSelectedItem = 0;
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      qsSelectedItem++;
      if (qsSelectedItem >= itemCount) {
        qsSelectedItem = 0;
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      adjustQsItemValue(qsSelectedTab, qsSelectedItem, false);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      adjustQsItemValue(qsSelectedTab, qsSelectedItem, true);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Confirm acts as a quick apply & close from anywhere
      closeAndApplyQuickSettings();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Back steps up to the Tab focus
      qsState = QuickSettingsState::TAB_FOCUSED;
      requestUpdate();
    }
  }
}

void EpubReaderActivity::closeAndApplyQuickSettings() {
  // Close the overlay BEFORE the blocking reflow operation
  qsState = QuickSettingsState::CLOSED;
  qsSuppressConfirmRelease = false;

  applyOrientation(qsPendingOrientation);
  toggleAutoPageTurn(autoPageTurnOption);

  // Commit all direct global modifications to SD card
  SETTINGS.saveToFile();

  sdFontSystem.ensureLoaded(renderer);

  {
    RenderLock lock(*this);
    cachedSpineIndex = currentSpineIndex;
    if (section) {
      cachedChapterTotalPageCount = section->estimatedTotalPages();
    }
    // Force EPUB engine to recalculate pages with the new global settings
    section.reset();
    pagesUntilFullRefresh = 0;  // Force crisp AA text on next draw
  }

  // Trigger a full screen render to apply the heavy layout changes
  requestUpdate();
}

void EpubReaderActivity::renderQuickSettingsOverlay() {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // The glyph hint band always paints the fixed physical bottom edge, which maps
  // to a different logical edge per orientation. Inset the sheet and its content
  // by the matching gutter so the band never overlaps the tabs or rows.
  const ReaderUtils::BandGutter gutter = ReaderUtils::bandGutterForBottomHints(renderer, metrics.buttonHintsHeight);
  const int sheetX = gutter.left;
  const int sheetW = w - gutter.left - gutter.right;

  constexpr int grabberZone = 28;
  const int rowsHeight = 5 * metrics.listRowHeight;
  int sheetHeight = grabberZone + metrics.tabBarHeight + metrics.verticalSpacing + rowsHeight + gutter.bottom;
  const int maxSheetHeight = h - 60;
  if (sheetHeight > maxSheetHeight) {
    sheetHeight = maxSheetHeight;
  }
  const int sheetTop = h - sheetHeight;

  GUI.drawBottomSheetFrame(renderer, Rect{sheetX, sheetTop, sheetW, sheetHeight});

  const int tabTop = sheetTop + grabberZone;
  std::vector<TabInfo> tabs = {{tr(STR_CAT_READER), qsSelectedTab == 0}, {tr(STR_CAT_CONTROLS), qsSelectedTab == 1}};
  GUI.drawTabBar(renderer, Rect{sheetX, tabTop, sheetW, metrics.tabBarHeight}, tabs,
                 qsState == QuickSettingsState::TAB_FOCUSED, false);

  const int listTop = tabTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int listHeight = h - gutter.bottom - listTop;
  const int itemCount = getQsItemCount(qsSelectedTab);
  const int selectedIndex = (qsState == QuickSettingsState::ITEM_FOCUSED) ? qsSelectedItem : -1;
  const int tab = qsSelectedTab;

  GUI.drawList(
      renderer, Rect{sheetX, listTop, sheetW, listHeight}, itemCount, selectedIndex,
      [this, tab](int index) { return std::string(getQsItemName(tab, index)); }, nullptr, nullptr,
      [this, tab](int index) {
        char buf[40];
        return std::string(getQsItemValue(tab, index, buf, sizeof(buf)));
      },
      true, nullptr, [this, tab](int index) { return getQsItemToggle(tab, index); }, nullptr, false);

  GUI.drawButtonHintsGlyphs(renderer);
}