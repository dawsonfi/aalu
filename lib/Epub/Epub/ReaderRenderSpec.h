#pragma once
#include <cstdint>

// The resolved text-rendering configuration the reader hands to Section's layout/cache
// pipeline. Section's on-disk header validates every field: a cached section built with a
// different spec is discarded and rebuilt (see Section::loadSectionFile).
//
// Build one via CrossPointSettings::readerRenderSpec(width, height), which fills every
// field: the settings-derived ones from the store, the viewport from the caller.
//
// Note: upstream CrossPoint's equivalent struct also carries a `focusReadingEnabled` field.
// AALU has no such setting, so it is intentionally omitted here rather than added as a dead
// always-false field that never reaches the section cache header.
struct ReaderRenderSpec {
  int fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
};
