#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <cstdint>
#include <cstring>

#include "../BionicReading.h"

size_t TextBlock::arenaSize(const uint16_t wordCount, const uint16_t textBytes) {
  return static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t)) + textBytes;
}

void TextBlock::bindArenaPointers() {
  uint8_t* base = arena.get();
  const size_t wc = numWords;
  textOffArr = reinterpret_cast<const uint16_t*>(base);
  xposArr = reinterpret_cast<const int16_t*>(base + wc * 2);
  stylesArr = base + wc * 4;
  textArr = reinterpret_cast<const char*>(base + wc * 5);
}

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const BlockStyle& blockStyle)
    : blockStyle(blockStyle) {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() || words.size() > 10000) {
    LOG_ERR("TXB", "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  if (numWords == 0) {
    return;
  }

  size_t totalText = 0;
  for (const auto& w : words) totalText += w.size() + 1;
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, textBytes);
  arena = makeUniqueNoThrow<uint8_t[]>(size);
  if (!arena) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    isValid = false;
    return;
  }
  bindArenaPointers();

  auto* textOff = const_cast<uint16_t*>(textOffArr);
  auto* xpos = const_cast<int16_t*>(xposArr);
  auto* styles = const_cast<uint8_t*>(stylesArr);
  auto* text = const_cast<char*>(textArr);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = wordXpos[i];
    styles[i] = static_cast<uint8_t>(wordStyles[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid block");
    return;
  }

  for (uint16_t i = 0; i < numWords; i++) {
    const int wordX = xposArr[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyle(i);
    const char* const wordStr = wordText(i);
    const size_t wordLen = wordTextLen(i);

    // Bionic Reading: bold the prefix, draw the suffix in the original style. Falls through to
    // a single drawText when the helper returns 0 (word too short / disabled).
    const size_t split = BionicReading::enabled ? BionicReading::prefixByteLength(wordStr, wordLen) : 0;
    if (split > 0 && split < wordLen && split < 24) {
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      char prefixBuf[24];
      memcpy(prefixBuf, wordStr, split);
      prefixBuf[split] = '\0';
      renderer.drawText(fontId, wordX, y, prefixBuf, true, boldStyle);
      const int prefixWidth = renderer.getTextAdvanceX(fontId, prefixBuf, boldStyle);
      renderer.drawText(fontId, wordX + prefixWidth, y, wordStr + split, true, currentStyle);
    } else {
      renderer.drawText(fontId, wordX, y, wordStr, true, currentStyle);
    }

    if (EpdFontFamily::hasTextDecoration(currentStyle)) {
      const char* const w = wordStr;
      int startX = wordX;
      int lineWidth = renderer.getTextWidth(fontId, w, currentStyle);

      // if word starts with em-space ("\xe2\x80\x83"), account for the additional indent before drawing the line
      if (wordLen >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
          static_cast<uint8_t>(w[2]) == 0x83) {
        const char* visiblePtr = w + 3;
        startX = wordX + renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        lineWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle);
      }

      // y is the top of the text line; add ascender to reach baseline.
      const int ascender = renderer.getFontAscenderSize(fontId);
      if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
        const int underlineY = y + ascender + 2;
        renderer.drawLine(startX, underlineY, startX + lineWidth, underlineY, true);
      }
      if ((currentStyle & EpdFontFamily::STRIKETHROUGH) != 0) {
        const int strikeY = y + ascender * 4 / 5;
        renderer.drawLine(startX, strikeY, startX + lineWidth, strikeY, true);
      }
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (!isValid) {
    LOG_ERR("TXB", "Serialization failed: invalid block");
    return false;
  }

  serialization::writePod(file, numWords);
  serialization::writePod(file, textBytes);
  if (numWords > 0) {
    const size_t size = arenaSize(numWords, textBytes);
    if (file.write(arena.get(), size) != size) {
      LOG_ERR("TXB", "Serialization failed: arena write (%u bytes)", static_cast<uint32_t>(size));
      return false;
    }
  }

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  uint16_t textBytes;
  serialization::readPod(file, wc);
  serialization::readPod(file, textBytes);

  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }
  if ((wc == 0 && textBytes != 0) || (wc > 0 && textBytes < wc)) {
    LOG_ERR("TXB", "Deserialization failed: bad text size %u for %u words", textBytes, wc);
    return nullptr;
  }

  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    LOG_ERR("TXB", "OOM: TextBlock");
    return nullptr;
  }
  block->numWords = wc;
  block->textBytes = textBytes;

  if (wc > 0) {
    const size_t size = arenaSize(wc, textBytes);
    block->arena = makeUniqueNoThrow<uint8_t[]>(size);
    if (!block->arena) {
      LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
      return nullptr;
    }
    if (file.read(block->arena.get(), size) != static_cast<int>(size)) {
      LOG_ERR("TXB", "Deserialization failed: arena read (%u bytes)", static_cast<uint32_t>(size));
      return nullptr;
    }
    block->bindArenaPointers();

    const uint16_t* textOff = block->textOffArr;
    const char* text = block->textArr;
    if (textOff[0] != 0 || text[textBytes - 1] != '\0') {
      LOG_ERR("TXB", "Deserialization failed: corrupt text layout");
      return nullptr;
    }
    for (uint16_t i = 1; i < wc; i++) {
      if (textOff[i] <= textOff[i - 1] || textOff[i] >= textBytes || text[textOff[i] - 1] != '\0') {
        LOG_ERR("TXB", "Deserialization failed: corrupt word offset %u", i);
        return nullptr;
      }
    }
  }

  // Style (alignment + margins/padding/indent)
  BlockStyle& blockStyle = block->blockStyle;
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);

  return block;
}
