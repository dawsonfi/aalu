#pragma once
#include <WString.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace FsHelpers {

std::string decodeUriEscapes(const std::string& path);

std::string normalisePath(const std::string& path);

// Stable, process-independent hash for on-disk cache directory names
// (e.g. `epub_<hash>`, `xtc_<hash>`, `recent_series_count_<hash>`).
//
// On the device (libstdc++) `std::hash<std::string>` is deterministic
// across runs, and existing user caches were named with its output — so
// we keep using it there.
//
// On the host simulator (libc++ on macOS) `std::hash<std::string>` is
// per-process randomised as a hash-flooding mitigation. That meant every
// `make emulator` invocation generated a new cache key for the same book
// path, orphaning the previous run's directory and triggering full
// re-parses (or worse, "covers disappear" when a stored coverBmpPath had
// last run's hash baked in). FNV-1a is fixed-seed and gives us stable
// keys across sim restarts.
inline uint64_t cachePathHash(const std::string& s) {
#ifdef SIMULATOR
  uint64_t h = 14695981039346656037ULL;  // FNV-1a 64-bit offset basis
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;  // FNV-1a 64-bit prime
  }
  return h;
#else
  return static_cast<uint64_t>(std::hash<std::string>{}(s));
#endif
}

bool naturalLess(const std::string& str1, const std::string& str2);

void sortFileList(std::vector<std::string>& strs);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(std::string_view fileName, const char* extension);
inline bool checkFileExtension(const String& fileName, const char* extension) {
  return checkFileExtension(std::string_view{fileName.c_str(), fileName.length()}, extension);
}

// Check for either .jpg or .jpeg extension (case-insensitive)
bool hasJpgExtension(std::string_view fileName);
inline bool hasJpgExtension(const String& fileName) {
  return hasJpgExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .png extension (case-insensitive)
bool hasPngExtension(std::string_view fileName);
inline bool hasPngExtension(const String& fileName) {
  return hasPngExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .bmp extension (case-insensitive)
bool hasBmpExtension(std::string_view fileName);

// Check for .gif extension (case-insensitive)
bool hasGifExtension(std::string_view fileName);
inline bool hasGifExtension(const String& fileName) {
  return hasGifExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .epub extension (case-insensitive)
bool hasEpubExtension(std::string_view fileName);
inline bool hasEpubExtension(const String& fileName) {
  return hasEpubExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for either .xtc or .xtch extension (case-insensitive)
bool hasXtcExtension(std::string_view fileName);

// Check for .txt extension (case-insensitive)
bool hasTxtExtension(std::string_view fileName);
inline bool hasTxtExtension(const String& fileName) {
  return hasTxtExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .md extension (case-insensitive)
bool hasMarkdownExtension(std::string_view fileName);

// Check for .css extension (case-insensitive)
bool hasCssExtension(std::string_view fileName);
inline bool hasCssExtension(const String& fileName) {
  return hasCssExtension(std::string_view{fileName.c_str(), fileName.length()});
}
std::string extractFolderPath(const std::string& filePath);

/**
 * Sanitize a filename/path component for FAT32 in a caller-provided buffer.
 * Replaces invalid path characters, spaces, and control characters with '-'.
 */
void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen);

}  // namespace FsHelpers
