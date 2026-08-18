#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include "Epub/VisibleTextOffsetUtils.h"
#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 22;
// Written into the version byte while a build is in progress; finalizeBuild() patches it to
// SECTION_FILE_VERSION once the section is complete. An abandoned / crash-interrupted .bin keeps
// version 0, which loadSectionFile rejects, so it is transparently rebuilt on the next open.
constexpr uint8_t SECTION_FILE_INCOMPLETE_VERSION = 0;
// Trailing header fields, in file order: pageCount, then these three uint32 offsets.
// trailingOffsetFieldPosition(index) below is the single place that turns a field's index into a
// seek position, so adding a 4th field only means bumping TRAILING_OFFSET_COUNT and adding an
// index constant -- every existing seek stays correct instead of needing its own manual update.
constexpr int TRAILING_OFFSET_COUNT = 3;
constexpr int PAGE_LUT_OFFSET_FIELD = 0;
constexpr int ANCHOR_MAP_OFFSET_FIELD = 1;
constexpr int VISIBLE_LUT_OFFSET_FIELD = 2;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(uint32_t) * TRAILING_OFFSET_COUNT;

constexpr uint32_t trailingOffsetFieldPosition(const int fieldIndex) {
  return HEADER_SIZE - sizeof(uint32_t) * (TRAILING_OFFSET_COUNT - fieldIndex);
}
}  // namespace

Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}

Section::~Section() {
  if (build_) {
    abandonBuild();
  }
}

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const ReaderRenderSpec& spec) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(spec.fontId) + sizeof(spec.lineCompression) +
                                   sizeof(spec.extraParagraphSpacing) + sizeof(spec.paragraphAlignment) +
                                   sizeof(spec.viewportWidth) + sizeof(spec.viewportHeight) + sizeof(pageCount) +
                                   sizeof(spec.hyphenationEnabled) + sizeof(spec.embeddedStyle) +
                                   sizeof(spec.imageRendering) + sizeof(uint32_t) * TRAILING_OFFSET_COUNT,
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_INCOMPLETE_VERSION);
  serialization::writePod(file, spec.fontId);
  serialization::writePod(file, spec.lineCompression);
  serialization::writePod(file, spec.extraParagraphSpacing);
  serialization::writePod(file, spec.paragraphAlignment);
  serialization::writePod(file, spec.viewportWidth);
  serialization::writePod(file, spec.viewportHeight);
  serialization::writePod(file, spec.hyphenationEnabled);
  serialization::writePod(file, spec.embeddedStyle);
  serialization::writePod(file, spec.imageRendering);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for visible-offset LUT offset (patched later)
}

bool Section::loadSectionFile(const ReaderRenderSpec& spec) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);

    if (spec.fontId != fileFontId || spec.lineCompression != fileLineCompression ||
        spec.extraParagraphSpacing != fileExtraParagraphSpacing || spec.paragraphAlignment != fileParagraphAlignment ||
        spec.viewportWidth != fileViewportWidth || spec.viewportHeight != fileViewportHeight ||
        spec.hyphenationEnabled != fileHyphenationEnabled || spec.embeddedStyle != fileEmbeddedStyle ||
        spec.imageRendering != fileImageRendering) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::startBuild(const ReaderRenderSpec& spec, const std::function<void()>& popupFn) {
  if (build_) {
    LOG_ERR("SCT", "Build already in progress");
    return false;
  }
  buildComplete_ = false;
  pageCount = 0;

  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Stream the spine item to a temp file (retry for SD timing hiccups)
  bool streamed = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);
    }
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    streamed = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    tmpHtml.close();
    if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }
  if (!streamed) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }
  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  writeSectionFileHeader(spec);

  auto ctx = makeUniqueNoThrow<BuildContext>();
  if (!ctx) {
    LOG_ERR("SCT", "Failed to allocate build context");
    file.close();
    Storage.remove(filePath.c_str());
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  ctx->tmpHtmlPath = tmpHtmlPath;

  const size_t lastSlash = localPath.find_last_of('/');
  const std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  const std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  if (spec.embeddedStyle) {
    ctx->cssParser = epub->getCssParser();
    if (ctx->cssParser && !ctx->cssParser->loadFromCache()) {
      LOG_ERR("SCT", "Failed to load CSS from cache");
    }
  }

  BuildContext* const ctxPtr = ctx.get();
  ctx->parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      epub, ctxPtr->tmpHtmlPath, renderer, spec.fontId, spec.lineCompression, spec.extraParagraphSpacing,
      spec.paragraphAlignment, spec.viewportWidth, spec.viewportHeight, spec.hyphenationEnabled,
      [this, ctxPtr](std::unique_ptr<Page> page) {
        const uint32_t visibleOffset = page->visibleTextOffset;
        ctxPtr->lut.push_back(this->onPageComplete(std::move(page)));
        ctxPtr->visibleOffsetLut.push_back(visibleOffset);
      },
      spec.embeddedStyle, contentBase, imageBasePath, spec.imageRendering, popupFn, ctxPtr->cssParser);
  if (!ctx->parser) {
    LOG_ERR("SCT", "Failed to allocate parser");
    if (ctx->cssParser) {
      ctx->cssParser->clear();
    }
    file.close();
    Storage.remove(filePath.c_str());
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  Hyphenator::setPreferredLanguage(epub->getLanguage());
  build_ = std::move(ctx);
  if (!build_->parser->beginParse()) {
    LOG_ERR("SCT", "Failed to begin parse");
    abandonBuild();
    return false;
  }
  build_->totalBytes = static_cast<uint32_t>(build_->parser->parseTotalBytes());
  return true;
}

bool Section::buildSomeMore(const int maxPages) {
  if (!build_ || !build_->parser) {
    LOG_ERR("SCT", "buildSomeMore called with no active build");
    return false;
  }
  const size_t startCount = build_->lut.size();
  int step = 0;
  for (;;) {
    const auto status = build_->parser->parseStep();
    if (status == ChapterHtmlSlimParser::ParseStatus::Error) {
      LOG_ERR("SCT", "Parse error during incremental build");
      abandonBuild();
      return false;
    }
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) {
      return finalizeBuild();
    }
    if (maxPages > 0 && (build_->lut.size() - startCount) >= static_cast<size_t>(maxPages)) {
      build_->bytesConsumed = static_cast<uint32_t>(build_->parser->parseBytesConsumed());
      return true;
    }
    if ((++step & 0x7) == 0) {
      delay(1);
    }
  }
}

bool Section::finalizeBuild() {
  if (!build_ || !build_->parser) {
    return false;
  }
  const bool parsedOk = build_->parser->finishParse();
  if (!parsedOk) {
    LOG_ERR("SCT", "Failed to finish parse");
    abandonBuild();
    return false;
  }

  bool hasFailedLutRecords = false;
  for (const uint32_t pos : build_->lut) {
    if (pos == 0) {
      hasFailedLutRecords = true;
      break;
    }
  }
  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    abandonBuild();
    return false;
  }

  const uint32_t lutOffset = file.position();
  for (const uint32_t pos : build_->lut) {
    serialization::writePod(file, pos);
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = build_->parser->getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  // Write the per-page visible-text-offset LUT, parallel to the page LUT above.
  const uint32_t visibleLutOffset = file.position();
  for (const uint32_t offset : build_->visibleOffsetLut) {
    serialization::writePod(file, offset);
  }

  pageCount = static_cast<uint16_t>(build_->lut.size());

  // Patch the version byte from the in-progress sentinel to the real version, then the page count
  // and offsets. The version is written last so a crash mid-build leaves an unusable (rebuildable)
  // file rather than a truncated one that reads as valid.
  file.seek(trailingOffsetFieldPosition(PAGE_LUT_OFFSET_FIELD) - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, visibleLutOffset);
  file.seek(0);
  serialization::writePod(file, SECTION_FILE_VERSION);
  file.close();

  if (build_->cssParser) {
    build_->cssParser->clear();
  }
  Storage.remove(build_->tmpHtmlPath.c_str());
  build_.reset();
  buildComplete_ = true;
  return true;
}

void Section::abandonBuild() {
  if (!build_) {
    return;
  }
  if (build_->parser) {
    build_->parser->abortParse();
  }
  if (file) {
    file.close();
  }
  Storage.remove(filePath.c_str());
  if (!build_->tmpHtmlPath.empty() && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  if (build_->cssParser) {
    build_->cssParser->clear();
  }
  build_.reset();
  buildComplete_ = false;
  pageCount = 0;
}

bool Section::createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn) {
  if (!startBuild(spec, popupFn)) {
    return false;
  }
  if (!buildSomeMore(0)) {
    return false;
  }
  return buildComplete_;
}

std::unique_ptr<Page> Section::loadPageAt(const int page) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return nullptr;
  }
  f.seek(trailingOffsetFieldPosition(PAGE_LUT_OFFSET_FIELD));
  uint32_t lutOffset;
  serialization::readPod(f, lutOffset);
  f.seek(lutOffset + sizeof(uint32_t) * page);
  uint32_t pagePos;
  serialization::readPod(f, pagePos);
  f.seek(pagePos);
  auto result = Page::deserialize(f);
  f.close();
  return result;
}

std::unique_ptr<Page> Section::loadPageDuringBuild(const int page) {
  if (!build_ || page < 0 || page >= static_cast<int>(build_->lut.size()) || !file) {
    return nullptr;
  }
  const uint32_t pagePos = build_->lut[page];
  if (pagePos == 0) {
    return nullptr;
  }
  // Read a finished page from the still-open write handle, then restore the append cursor so the
  // parser keeps writing where it left off.
  const uint32_t writeCursor = file.position();
  file.seek(pagePos);
  auto result = Page::deserialize(file);
  file.seek(writeCursor);
  return result;
}

std::unique_ptr<Page> Section::loadPage(const int page) {
  if (page < 0) {
    return nullptr;
  }
  if (build_) {
    return page < static_cast<int>(build_->lut.size()) ? loadPageDuringBuild(page) : nullptr;
  }
  return page < pageCount ? loadPageAt(page) : nullptr;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() { return loadPage(currentPage); }

uint16_t Section::estimatedTotalPages() const {
  if (!build_) {
    return pageCount;
  }
  const uint16_t built = static_cast<uint16_t>(build_->lut.size());
  const uint32_t consumed = build_->bytesConsumed;
  const uint32_t total = build_->totalBytes;
  if (consumed == 0 || total <= consumed) {
    return built;
  }
  const uint64_t est = static_cast<uint64_t>(built) * total / consumed;
  if (est <= built) {
    return built;
  }
  return est > 60000 ? 60000 : static_cast<uint16_t>(est);
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(trailingOffsetFieldPosition(ANCHOR_MAP_OFFSET_FIELD));
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      f.close();
      return page;
    }
  }

  f.close();
  return std::nullopt;
}

std::optional<uint32_t> Section::getVisibleTextOffsetForPage(const uint16_t page) const {
  if (build_) {
    if (page >= build_->visibleOffsetLut.size()) {
      return std::nullopt;
    }
    return build_->visibleOffsetLut[page];
  }
  if (page >= pageCount) {
    return std::nullopt;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }
  f.seek(trailingOffsetFieldPosition(VISIBLE_LUT_OFFSET_FIELD));
  uint32_t visibleLutOffset;
  serialization::readPod(f, visibleLutOffset);
  std::optional<uint32_t> result;
  if (visibleLutOffset != 0) {
    f.seek(visibleLutOffset + sizeof(uint32_t) * page);
    uint32_t offset;
    serialization::readPod(f, offset);
    result = offset;
  }
  f.close();
  return result;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset,
                                                             const bool preferFirstAtOffset) const {
  if (build_) {
    return VisibleTextOffsetUtils::findPageForOffset(build_->visibleOffsetLut, offset, preferFirstAtOffset);
  }
  if (pageCount == 0) {
    return std::nullopt;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }
  f.seek(trailingOffsetFieldPosition(VISIBLE_LUT_OFFSET_FIELD));
  uint32_t visibleLutOffset;
  serialization::readPod(f, visibleLutOffset);
  if (visibleLutOffset == 0) {
    f.close();
    return std::nullopt;
  }

  // Stream the on-disk LUT sequentially rather than materializing it, since pageCount is
  // caller-controlled (untrusted EPUB content) and could otherwise force an unbounded allocation.
  f.seek(visibleLutOffset);
  std::optional<uint16_t> best;
  for (uint16_t page = 0; page < pageCount; page++) {
    uint32_t pageOffset;
    serialization::readPod(f, pageOffset);
    if (pageOffset > offset) break;
    best = page;
    if (preferFirstAtOffset && pageOffset == offset) break;
  }
  f.close();
  return best;
}
