#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ProgressFile {

// Writes reader progress to `<cachePath>/progress.bin` without ever leaving the canonical file
// half-written. The bytes go to a temporary `progress.bin.tmp` first; only once that is fully
// written and closed is it renamed over progress.bin. An interrupted write (power loss or a crash
// mid-SPI) therefore damages only the throwaway temp file. A truncate-in-place write cut short
// used to leave progress.bin with a broken FAT cluster chain the firmware could neither rewrite
// nor clear, stranding the book on an old page.
//
// Crash-safe, not metadata-atomic: on FAT the replace is remove + rename, so a crash between them
// can leave neither file -- which reads as "no saved progress" on next launch, never a corrupt one.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  const std::string finalPath = cachePath + "/progress.bin";
  const std::string tmpPath = cachePath + "/progress.bin.tmp";

  {
    FsFile f;
    if (!Storage.openFileForWrite("PRG", tmpPath, f)) {
      LOG_ERR("PRG", "Could not open temp progress file for write: %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(data, len);
    if (written != len) {
      LOG_ERR("PRG", "Short write saving progress to %s: %u/%u bytes", tmpPath.c_str(), (unsigned)written,
              (unsigned)len);
      return false;
    }
    f.flush();
    // f is closed at scope exit (DESTRUCTOR_CLOSES_FILE=1) before the rename below -- SdFat must
    // not rename a path that still has an open FsFile.
  }

  // SdFat's rename does not overwrite an existing destination, so drop the old canonical file
  // first. The brief window where neither file exists reads as "no saved progress" next launch.
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("PRG", "Failed to rename temp progress into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

}  // namespace ProgressFile
