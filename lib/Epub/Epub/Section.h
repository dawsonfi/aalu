#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/ReaderRenderSpec.h"

class Page;
class GfxRenderer;
class ChapterHtmlSlimParser;
class CssParser;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;

  // Held only while an incremental build is in progress. ChapterHtmlSlimParser keeps its file path
  // by reference, so tmpHtmlPath must outlive it and parser is declared last (destroyed first). The
  // page-complete callback appends each finished page's file offset to lut.
  struct BuildContext {
    std::vector<uint32_t> lut;
    // Parallel to lut: the visible-text offset each page starts at.
    std::vector<uint32_t> visibleOffsetLut;
    std::string tmpHtmlPath;
    CssParser* cssParser = nullptr;
    uint32_t bytesConsumed = 0;
    uint32_t totalBytes = 0;
    std::unique_ptr<ChapterHtmlSlimParser> parser;
  };
  std::unique_ptr<BuildContext> build_;
  bool buildComplete_ = false;

  void writeSectionFileHeader(const ReaderRenderSpec& spec);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  bool finalizeBuild();
  std::unique_ptr<Page> loadPageDuringBuild(int page);
  std::unique_ptr<Page> loadPageAt(int page) const;

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(const ReaderRenderSpec& spec);
  bool clearCache() const;
  bool createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn = nullptr);

  // Incremental build: lay out the section a few pages at a time so a large chapter shows its
  // first page immediately and keeps the UI responsive while the rest builds.
  //   if (!startBuild(...)) fail;  each tick: buildSomeMore(N); render up to pageCount; stop when isBuildComplete().
  bool startBuild(const ReaderRenderSpec& spec, const std::function<void()>& popupFn = nullptr);
  bool buildSomeMore(int maxPages);
  bool isBuilding() const { return static_cast<bool>(build_); }
  bool isBuildComplete() const { return buildComplete_; }
  void abandonBuild();
  // Best-known total page count: exact once finalized, or a byte-ratio estimate while building.
  uint16_t estimatedTotalPages() const;

  // Unified page read: from the active build if it has reached the page, otherwise from the file.
  std::unique_ptr<Page> loadPage(int page);
  std::unique_ptr<Page> loadPageFromSectionFile();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Content-based position lookups, for resuming/repositioning on a visible-text offset instead of
  // a page index that shifts across a reflow. The in-memory (still-building) case delegates to
  // VisibleTextOffsetUtils::findPageForOffset; the on-disk case streams the same comparison
  // inline (see the .cpp) rather than materializing the LUT into a vector first.
  std::optional<uint32_t> getVisibleTextOffsetForPage(uint16_t page) const;
  std::optional<uint16_t> getPageForVisibleTextOffset(uint32_t offset, bool preferFirstAtOffset = false) const;
  // True once an in-progress build has laid out far enough to resolve offset. render() uses this to
  // extend a shallow/partial build's target so it doesn't stop short of a pending offset jump and
  // leave the lookup above resolving against a truncated LUT.
  bool buildReachedVisibleTextOffset(const uint32_t offset) const {
    return build_ && !build_->visibleOffsetLut.empty() && offset <= build_->visibleOffsetLut.back();
  }
};
