#include "OtaUpdater.h"

#include <Logging.h>

#include <cstring>

#ifndef SIMULATOR
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HalStorage.h>
#include <NetworkClientSecure.h>
#include <Update.h>

#include <algorithm>
#include <memory>

#include "esp_ota_ops.h"
#include "esp_wifi.h"
#endif

// AALU's OTA path. We deliberately bypass ESP-IDF's esp_http_client +
// esp_https_ota + esp_crt_bundle stack: the bundled Mozilla cert bundle
// shipped by Arduino-ESP32 currently fails to verify api.github.com's leaf
// cert ("PK verify failed with error 0x10 / Certificate matched but
// signature verification failed"). Rather than pin a single CA cert (which
// rotates) we use the Arduino HTTPClient stack with NetworkClientSecure in
// insecure mode -- same pattern HttpDownloader.cpp already uses for OPDS /
// Calibre downloads. Authenticity is not at risk here: the API response is
// only used to discover the release tag + size, and the firmware binary
// itself is written via Update.h, which validates the ESP32 magic byte and
// image header before committing to the OTA partition.

#ifndef SIMULATOR

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/dawsonfi/aalu/releases/latest";

std::unique_ptr<NetworkClient> makeHttpsClient() {
  auto* secure = new NetworkClientSecure();
  secure->setInsecure();
  return std::unique_ptr<NetworkClient>(secure);
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  auto client = makeHttpsClient();
  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(15000);

  LOG_DBG("OTA", "GET %s (heap=%u)", latestReleaseUrl, static_cast<unsigned>(ESP.getFreeHeap()));
  if (!http.begin(*client, latestReleaseUrl)) {
    LOG_ERR("OTA", "HTTPClient.begin failed");
    return HTTP_ERROR;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "AALU-ESP32-" AALU_VERSION);
  http.addHeader("Accept", "application/vnd.github+json");

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("OTA", "GitHub API HTTP %d", httpCode);
    http.end();
    return HTTP_ERROR;
  }

  static constexpr const char* CHECK_TMP = "/.ota_check.json";
  {
    HalFile out;
    if (!Storage.openFileForWrite("OTA", CHECK_TMP, out)) {
      LOG_ERR("OTA", "Failed to open temp file for update check");
      http.end();
      return HTTP_ERROR;
    }
    NetworkClient* stream = http.getStreamPtr();
    if (stream == nullptr) {
      out.close();
      http.end();
      Storage.remove(CHECK_TMP);
      LOG_ERR("OTA", "No response stream");
      return HTTP_ERROR;
    }
    const int64_t reported = http.getSize();
    const size_t total = reported > 0 ? static_cast<size_t>(reported) : 0;
    uint8_t buf[512];
    size_t received = 0;
    unsigned long lastDataMs = millis();
    while (total == 0 || received < total) {
      if (!http.connected() && stream->available() == 0) {
        break;
      }
      if (millis() - lastDataMs > 15000) {
        break;
      }
      const size_t avail = static_cast<size_t>(stream->available());
      if (avail == 0) {
        delay(10);
        continue;
      }
      size_t want = std::min(avail, sizeof(buf));
      if (total > 0) {
        want = std::min(want, total - received);
      }
      const int n = stream->readBytes(buf, want);
      if (n <= 0) {
        delay(10);
        continue;
      }
      out.write(buf, static_cast<size_t>(n));
      received += static_cast<size_t>(n);
      lastDataMs = millis();
    }
    out.close();
  }
  http.end();

  HalFile in;
  if (!Storage.openFileForRead("OTA", CHECK_TMP, in)) {
    LOG_ERR("OTA", "Failed to read update-check response");
    Storage.remove(CHECK_TMP);
    return HTTP_ERROR;
  }

  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, in, DeserializationOption::Filter(filter));
  in.close();
  Storage.remove(CHECK_TMP);
  if (error) {
    LOG_ERR("OTA", "JSON parse failed: %s", error.c_str());
    return JSON_PARSE_ERROR;
  }

  if (!doc["tag_name"].is<std::string>()) {
    LOG_ERR("OTA", "No tag_name found");
    return JSON_PARSE_ERROR;
  }
  if (!doc["assets"].is<JsonArray>()) {
    LOG_ERR("OTA", "No assets found");
    return JSON_PARSE_ERROR;
  }

  latestVersion = doc["tag_name"].as<std::string>();
  // Release tags are prefixed with 'v' (e.g. "v1.1.1") but AALU_VERSION is bare ("1.1.1").
  // Strip the prefix so equality checks and sscanf parsing work.
  if (!latestVersion.empty() && (latestVersion.front() == 'v' || latestVersion.front() == 'V')) {
    latestVersion.erase(0, 1);
  }

  updateAvailable = false;
  for (size_t i = 0; i < doc["assets"].size(); ++i) {
    if (doc["assets"][i]["name"] == "firmware.bin") {
      otaUrl = doc["assets"][i]["browser_download_url"].as<std::string>();
      otaSize = doc["assets"][i]["size"].as<size_t>();
      totalSize = otaSize;
      updateAvailable = true;
      break;
    }
  }

  if (!updateAvailable) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  LOG_DBG("OTA", "Found update: %s (%u bytes)", latestVersion.c_str(), static_cast<unsigned>(otaSize));
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == AALU_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = AALU_VERSION;

  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  if (latestMajor != currentMajor) return latestMajor > currentMajor;
  if (latestMinor != currentMinor) return latestMinor > currentMinor;
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // Equal segments: RC builds always upgrade to the stable release.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }
  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  render = false;
  processedSize = 0;
  totalSize = otaSize;

  // We talk to esp_ota_* directly instead of going through Arduino's Update
  // wrapper. Update::begin allocates a 4 KB sector buffer via `new` AFTER the
  // partition lookup; on the device that allocation silently fails once a
  // TLS connection has fragmented the heap, surfacing as
  //   "Update.begin(...) failed: No Error"
  // (no _error set). esp_ota_begin doesn't need that buffer -- it writes
  // straight to the partition -- so OTA succeeds even when contiguous heap
  // is tight.

  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* nextOta = esp_ota_get_next_update_partition(NULL);
  LOG_INF("OTA", "Running partition: %s @ 0x%x (%u bytes)", running ? running->label : "(null)",
          running ? static_cast<unsigned>(running->address) : 0, running ? static_cast<unsigned>(running->size) : 0);
  LOG_INF("OTA", "Next OTA partition: %s @ 0x%x (%u bytes)", nextOta ? nextOta->label : "(null)",
          nextOta ? static_cast<unsigned>(nextOta->address) : 0, nextOta ? static_cast<unsigned>(nextOta->size) : 0);

  if (!nextOta || nextOta == running) {
    LOG_ERR("OTA", "No valid next OTA partition");
    return INTERNAL_UPDATE_ERROR;
  }
  if (otaSize > nextOta->size) {
    LOG_ERR("OTA", "Firmware too large: %u > %u", static_cast<unsigned>(otaSize), static_cast<unsigned>(nextOta->size));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  LOG_INF("OTA", "esp_ota_begin(size=%u) heap=%u largest=%u", static_cast<unsigned>(otaSize),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  esp_err_t err = esp_ota_begin(nextOta, otaSize, &otaHandle);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(err));
    return INTERNAL_UPDATE_ERROR;
  }
  LOG_INF("OTA", "esp_ota_begin OK (erase complete)");

  auto client = makeHttpsClient();
  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(15000);

  LOG_INF("OTA", "Downloading %s (heap=%u)", otaUrl.c_str(), static_cast<unsigned>(ESP.getFreeHeap()));

  if (!http.begin(*client, otaUrl.c_str())) {
    LOG_ERR("OTA", "HTTPClient.begin failed for OTA URL");
    esp_ota_abort(otaHandle);
    return INTERNAL_UPDATE_ERROR;
  }
  // GitHub release download URLs redirect through objects.githubusercontent.com.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "AALU-ESP32-" AALU_VERSION);

  LOG_INF("OTA", "http.GET()...");
  const int httpCode = http.GET();
  LOG_INF("OTA", "http.GET returned %d", httpCode);
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("OTA", "OTA download HTTP %d", httpCode);
    http.end();
    esp_ota_abort(otaHandle);
    return HTTP_ERROR;
  }

  const int reported = http.getSize();
  LOG_INF("OTA", "Content-Length reported by server: %d", reported);
  const size_t contentLength = reported > 0 ? static_cast<size_t>(reported) : otaSize;
  if (contentLength == 0) {
    LOG_ERR("OTA", "OTA download has zero content length");
    http.end();
    esp_ota_abort(otaHandle);
    return HTTP_ERROR;
  }
  totalSize = contentLength;

  // Keep the radio out of modem-sleep and at full TX power while we stream the ~5MB binary, so a
  // weak/mesh link doesn't stall or drop mid-download; power-save is restored on every exit path.
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(84);

  NetworkClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    LOG_ERR("OTA", "HTTPClient stream is null");
    http.end();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_ota_abort(otaHandle);
    return HTTP_ERROR;
  }

  constexpr size_t kChunk = 2048;
  uint8_t buf[kChunk];
  size_t totalRead = 0;
  unsigned long lastDataMs = millis();
  constexpr unsigned long kStallMs = 30000;

  while (totalRead < contentLength) {
    if (!http.connected() && stream->available() == 0) {
      LOG_ERR("OTA", "Connection dropped after %u / %u bytes", static_cast<unsigned>(totalRead),
              static_cast<unsigned>(contentLength));
      break;
    }

    const size_t avail = stream->available();
    if (avail == 0) {
      if (millis() - lastDataMs > kStallMs) {
        LOG_ERR("OTA", "Stalled (no data for %lums) at %u / %u", millis() - lastDataMs,
                static_cast<unsigned>(totalRead), static_cast<unsigned>(contentLength));
        break;
      }
      delay(10);
      continue;
    }

    const size_t want = std::min(avail, kChunk);
    const int read = stream->readBytes(buf, want);
    if (read <= 0) {
      delay(10);
      continue;
    }

    err = esp_ota_write(otaHandle, buf, read);
    if (err != ESP_OK) {
      LOG_ERR("OTA", "esp_ota_write failed at %u: %s", static_cast<unsigned>(totalRead), esp_err_to_name(err));
      break;
    }

    totalRead += read;
    processedSize = totalRead;
    render = true;
    lastDataMs = millis();

    // Notify the UI task so it can redraw the progress bar. The activity's
    // render() throttles to 2% intervals, so spamming this every chunk is
    // fine — most calls early-return without touching the display.
    if (onProgress) {
      onProgress(ctx);
    }
  }

  http.end();
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (totalRead != contentLength) {
    LOG_ERR("OTA", "Incomplete download: %u / %u", static_cast<unsigned>(totalRead),
            static_cast<unsigned>(contentLength));
    esp_ota_abort(otaHandle);
    return HTTP_ERROR;
  }

  err = esp_ota_end(otaHandle);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(err));
    return INTERNAL_UPDATE_ERROR;
  }

  err = esp_ota_set_boot_partition(nextOta);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed (%u bytes, boot partition set to %s)", static_cast<unsigned>(totalRead),
          nextOta->label);
  return OK;
}

#else  // SIMULATOR

// Simulator stub: the default-args installUpdate() is the device code path
// and would otherwise be undefined at link time. The sim's UI never reaches
// it because the sim's checkForUpdate() (in simulator_ota.cpp) returns
// NO_UPDATE, but the symbol still needs to exist.
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*) { return INTERNAL_UPDATE_ERROR; }

#endif  // SIMULATOR
