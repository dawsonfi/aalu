#pragma once
#include <string>
#include <vector>

struct OpdsServer {
  std::string name;
  std::string url;
  std::string username;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
};

class OpdsServerStore;
namespace JsonSettingsIO {
bool saveOpds(const OpdsServerStore& store, const char* path);
bool loadOpds(OpdsServerStore& store, const char* json, bool* needsResave);
}  // namespace JsonSettingsIO

/**
 * Singleton storing OPDS catalog servers on the SD card. Passwords are XOR-obfuscated with the
 * device hardware key and base64-encoded before writing to JSON, matching WifiCredentialStore.
 */
class OpdsServerStore {
 private:
  static OpdsServerStore instance;
  std::vector<OpdsServer> servers;

  static constexpr size_t MAX_SERVERS = 8;

  OpdsServerStore() = default;

  friend bool JsonSettingsIO::saveOpds(const OpdsServerStore&, const char*);
  friend bool JsonSettingsIO::loadOpds(OpdsServerStore&, const char*, bool*);

 public:
  OpdsServerStore(const OpdsServerStore&) = delete;
  OpdsServerStore& operator=(const OpdsServerStore&) = delete;

  static OpdsServerStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  bool addServer(const OpdsServer& server);
  bool updateServer(size_t index, const OpdsServer& server);
  bool removeServer(size_t index);

  const std::vector<OpdsServer>& getServers() const { return servers; }
  const OpdsServer* getServer(size_t index) const;
  size_t getCount() const { return servers.size(); }
  bool hasServers() const { return !servers.empty(); }
  static constexpr size_t maxServers() { return MAX_SERVERS; }
};

#define OPDS_STORE OpdsServerStore::getInstance()
