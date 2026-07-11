#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"

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
    std::string tmpHtmlPath;
    CssParser* cssParser = nullptr;
    uint32_t bytesConsumed = 0;
    uint32_t totalBytes = 0;
    std::unique_ptr<ChapterHtmlSlimParser> parser;
  };
  std::unique_ptr<BuildContext> build_;
  bool buildComplete_ = false;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  bool finalizeBuild();
  std::unique_ptr<Page> loadPageDuringBuild(int page);
  std::unique_ptr<Page> loadPageAt(int page) const;

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, const std::function<void()>& popupFn = nullptr);

  // Incremental build: lay out the section a few pages at a time so a large chapter shows its
  // first page immediately and keeps the UI responsive while the rest builds.
  //   if (!startBuild(...)) fail;  each tick: buildSomeMore(N); render up to pageCount; stop when isBuildComplete().
  bool startBuild(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                  uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                  uint8_t imageRendering, const std::function<void()>& popupFn = nullptr);
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
};
