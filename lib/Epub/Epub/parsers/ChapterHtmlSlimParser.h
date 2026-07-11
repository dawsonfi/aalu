#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../FootnoteEntry.h"
#include "../ParsedText.h"
#include "../blocks/ImageBlock.h"
#include "../blocks/TextBlock.h"
#include "../css/CssParser.h"
#include "../css/CssStyle.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  int strikethroughUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
    bool hasStrikethrough = false, strikethrough = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  bool effectiveStrikethrough = false;
  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;

  std::string pendingAnchorId;
  std::vector<std::pair<std::string, uint16_t>> anchorData;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  // --- ADDED: Exact DOM Path Tracking for KOReader Sync ---
  struct TagCount {
    std::string tag;
    int count;
  };
  std::vector<std::vector<TagCount>> childTracker;
  std::vector<std::string> pathElements;
  std::string currentXPath;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  char currentFootnoteLinkText[24] = {};
  int currentFootnoteLinkTextLen = 0;
  char currentFootnoteLinkHref[64] = {};
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  // Resumable-parse state. The one-shot parseAndBuildPages() drives these internally; the
  // incremental section builder drives them across render ticks so a large chapter can yield
  // between buffer chunks instead of blocking the UI until the whole chapter is laid out.
  XML_Parser xmlParser_ = nullptr;
  FsFile parseFile_;
  uint32_t parseStartTime_ = 0;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPartWordBuffer();
  void makePages();
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer,
                                 const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight, const bool hyphenationEnabled,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                                 const bool embeddedStyle, const std::string& contentBase,
                                 const std::string& imageBasePath, const uint8_t imageRendering = 0,
                                 const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath) {}

  ~ChapterHtmlSlimParser();

  // One-shot parse: builds every page before returning (beginParse + parseStep* + finishParse).
  bool parseAndBuildPages();

  // Resumable parse for the incremental section builder. Drive as:
  //   if (!beginParse()) fail;
  //   loop: switch (parseStep()) { More: keep going / yield; Done: finishParse(); Error: abortParse(); }
  // Pages are emitted via completePageFn as they complete inside parseStep().
  enum class ParseStatus { More, Done, Error };
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();
  void abortParse();

  // Byte progress of the in-flight parse, for estimating a still-building section's page count.
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }

  void addLineToPage(std::shared_ptr<TextBlock> line);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
};
