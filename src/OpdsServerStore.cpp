#include "OpdsServerStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

namespace {
constexpr char OPDS_FILE_JSON[] = "/.crosspoint/opds.json";
}  // namespace

OpdsServerStore OpdsServerStore::instance;

bool OpdsServerStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveOpds(*this, OPDS_FILE_JSON);
}

bool OpdsServerStore::loadFromFile() {
  if (Storage.exists(OPDS_FILE_JSON)) {
    const String json = Storage.readFile(OPDS_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      const bool result = JsonSettingsIO::loadOpds(*this, json.c_str(), &resave);
      if (result && resave) {
        saveToFile();
      }
      return result;
    }
  }
  return false;
}

bool OpdsServerStore::addServer(const OpdsServer& server) {
  if (servers.size() >= MAX_SERVERS) {
    LOG_DBG("OPS", "Cannot add more servers, limit of %zu reached", MAX_SERVERS);
    return false;
  }
  servers.push_back(server);
  LOG_DBG("OPS", "Added OPDS server: %s", server.name.c_str());
  return saveToFile();
}

bool OpdsServerStore::updateServer(size_t index, const OpdsServer& server) {
  if (index >= servers.size()) {
    return false;
  }
  servers[index] = server;
  LOG_DBG("OPS", "Updated OPDS server: %s", server.name.c_str());
  return saveToFile();
}

bool OpdsServerStore::removeServer(size_t index) {
  if (index >= servers.size()) {
    return false;
  }
  LOG_DBG("OPS", "Removed OPDS server: %s", servers[index].name.c_str());
  servers.erase(servers.begin() + static_cast<ptrdiff_t>(index));
  return saveToFile();
}

const OpdsServer* OpdsServerStore::getServer(size_t index) const {
  if (index >= servers.size()) {
    return nullptr;
  }
  return &servers[index];
}
