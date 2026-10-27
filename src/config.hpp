#pragma once

#include <optional>
#include <string>

namespace sne {

struct ClientConfig {
  std::string endpoints;
  std::string username;
  std::string password;
  std::string ca_cert;
  std::string cert;
  std::string key;
};

// Reads ./sne.conf, then $XDG_CONFIG_HOME/sne/config or ~/.config/sne/config.
std::optional<std::string> find_config_file();

ClientConfig load_config_file(const std::string& path);

}  // namespace sne
