#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include "CrossPointSettings.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

struct SettingInfo;

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  uint32_t sessionPagesTurned = 0;   // NEW: tracks pages turned in the current session
  bool automaticPageTurnActive = false;

  // Transient "Bookmark added/removed" confirmation, shown for a moment after a bookmark toggle.
  bool showBookmarkMessage = false;
  bool bookmarkRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  static constexpr unsigned long BOOKMARK_MESSAGE_MS = 1500UL;

  // Finished-book automation (System settings). recentsEntryRemoved tracks that we dropped this
  // book from Recents at the End-of-Book screen (re-added if paged back in); pendingReadFolderMove
  // arms an onExit relocation of the finished book into /read/.
  bool recentsEntryRemoved = false;
  bool pendingReadFolderMove = false;

  EndOfBookOptions endOfBookOptions;

  // --- QUICK SETTINGS OVERLAY ---
  enum class QuickSettingsState { CLOSED, TAB_FOCUSED, ITEM_FOCUSED };

  QuickSettingsState qsState = QuickSettingsState::CLOSED;
  int qsSelectedTab = 0;   // 0 = Reader, 1 = Controls
  int qsSelectedItem = 0;  // Currently highlighted setting index

  bool qsNeedsBackgroundRender = false;
  bool qsSuppressConfirmRelease = false;

  uint8_t qsPendingOrientation = 0;
  uint8_t autoPageTurnOption = 0;

  const SettingInfo* qsItemAt(int tab, int index) const;
  int getQsItemCount(int tab) const;
  const char* getQsItemName(int tab, int index) const;
  const char* getQsItemValue(int tab, int index, char* tempBuf, size_t tempBufSize) const;
  BaseTheme::ListToggleState getQsItemToggle(int tab, int index) const;
  void adjustQsItemValue(int tab, int index, bool increment);
  void renderQuickSettingsOverlay();
  void openQuickSettings();
  void handleQuickSettingsInput();

  // Renamed to reflect new behavior: saves to SD and reflows EPUB
  void closeAndApplyQuickSettings();
  // ------------------------------

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next
  // render (recovers a transiently corrupt cache); capped so a persistently bad page can't spin
  // forever on a blank screen.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;

  // Incremental section build (render page 1 while the rest lays out behind it). Pages laid out
  // per pump: on the render path catching up to the page being shown, and per loop() tick for the
  // background build of a large chapter. Kept small so a background chunk never noticeably delays
  // input or a pending render. Show the indexing popup only for a deep resume/jump that must lay
  // out more than this many pages up front, so an ordinary landing stays popup-free.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the indexing popup when first building a spine whose uncompressed HTML exceeds this:
  // the whole spine must inflate before page 1 can lay out (the giant single-spine case), a
  // multi-second wait even though its page count is small. Ordinary chapters stay under this.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;

  // Skip a background build tick below these floors. The parse path grows word vectors of heap
  // strings -- an allocation that aborts() on OOM under -fno-exceptions. The tick is deferrable
  // work: page-turn transients free up between turns and the gate is re-checked every tick. Free
  // heap alone isn't enough -- fragmentation can leave plenty of free bytes with no single block
  // big enough for a parse allocation -- so both floors must pass.
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // True while a background build tick is gated on the heap floors above. Lets skipLoopDelay()
  // fall back to the normal delay/power-saving loop cadence during the pause instead of pinning
  // the CPU at full speed while no build work is actually happening.
  bool buildHeapPaused = false;
  // Gate for a background build tick: true when the heap can safely take parse allocations.
  // Updates buildHeapPaused as a side effect.
  bool buildTickHeapGate();
  // Shared by buildTickHeapGate() and the idle-prewarm check below: true when both a free-heap
  // and a largest-allocatable-block floor are satisfied. Free heap alone isn't enough --
  // fragmentation can leave plenty of free bytes with no single block big enough for the
  // allocation that actually needs to happen.
  static bool heapAboveFloors(size_t minFreeHeap, size_t minMaxAlloc);

  // Idle glyph prewarm: after a page settles, load the likely-next page's missing glyphs from SD
  // during idle so the next turn's in-render prewarm is a cache hit instead of paying SD-read cost
  // on the page-turn critical path. Debounced, one attempt per (spine, page) position, and gated
  // on a lower free-heap floor than the background build: a render-time prewarm is a single-page
  // scan (lighter than the parse path's word-vector growth during a chapter build), and this exact
  // pair of floors mirrors upstream CrossPoint's own calibration for the two cases.
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  static constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  int readerClockBandHeight() const;
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void toggleBookmark();
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  bool skipLoopDelay() override { return section && section->isBuilding() && !buildHeapPaused; }
};
