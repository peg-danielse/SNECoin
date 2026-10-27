#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sne {
namespace {

namespace fs = std::filesystem;

std::string trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return std::string{text};
}

[[noreturn]] void config_error(const std::string& path, int line, const std::string& message) {
  throw std::invalid_argument(path + ":" + std::to_string(line) + ": " + message);
}

std::string parse_quoted(const std::string& path, int line, std::string_view text) {
  if (text.empty() || text.front() != '"') {
    config_error(path, line, "expected a quoted value");
  }
  std::string value;
  size_t i = 1;
  while (i < text.size()) {
    const char c = text[i++];
    if (c == '"') {
      if (trim(text.substr(i)).empty()) {
        return value;
      }
      config_error(path, line, "trailing characters after quoted value");
    }
    if (c != '\\') {
      value.push_back(c);
      continue;
    }
    if (i >= text.size()) {
      config_error(path, line, "unterminated escape");
    }
    const char escaped = text[i++];
    switch (escaped) {
      case '"':
      case '\\':
        value.push_back(escaped);
        break;
      case 'n':
        value.push_back('\n');
        break;
      case 't':
        value.push_back('\t');
        break;
      case 'r':
        value.push_back('\r');
        break;
      default:
        config_error(path, line, "unknown escape");
    }
  }
  config_error(path, line, "unterminated string");
}

void assign(ClientConfig& config, const std::string& path, int line, bool& seen_endpoints,
            bool& seen_username, bool& seen_password, bool& seen_ca, bool& seen_cert,
            bool& seen_key, const std::string& key, const std::string& value) {
  auto take = [&](bool& seen, std::string& slot) {
    if (seen) {
      config_error(path, line, "duplicate setting '" + key + "'");
    }
    seen = true;
    slot = value;
  };
  if (key == "endpoints") {
    if (value.empty()) {
      config_error(path, line, "endpoints is empty");
    }
    take(seen_endpoints, config.endpoints);
  } else if (key == "username") {
    take(seen_username, config.username);
  } else if (key == "password") {
    take(seen_password, config.password);
  } else if (key == "ca_cert") {
    take(seen_ca, config.ca_cert);
  } else if (key == "cert") {
    take(seen_cert, config.cert);
  } else if (key == "key") {
    take(seen_key, config.key);
  } else {
    config_error(path, line, "unknown setting '" + key + "'");
  }
}

}  // namespace

std::optional<std::string> find_config_file() {
  std::error_code error;
  if (fs::is_regular_file("sne.conf", error) && !error) {
    return std::string{"sne.conf"};
  }
  fs::path dir;
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
    dir = xdg;
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    dir = fs::path{home} / ".config";
  } else {
    return std::nullopt;
  }
  const fs::path file = dir / "sne" / "config";
  error.clear();
  if (fs::is_regular_file(file, error) && !error) {
    return file.string();
  }
  return std::nullopt;
}

ClientConfig load_config_file(const std::string& path) {
  std::ifstream input{path};
  if (!input) {
    throw std::invalid_argument("cannot read config file: " + path);
  }
  ClientConfig config;
  bool seen_endpoints = false;
  bool seen_username = false;
  bool seen_password = false;
  bool seen_ca = false;
  bool seen_cert = false;
  bool seen_key = false;
  std::string line;
  int number = 0;
  while (std::getline(input, line)) {
    ++number;
    const std::string text = trim(line);
    if (text.empty() || text.front() == '#') {
      continue;
    }
    const size_t eq = text.find('=');
    if (eq == std::string::npos) {
      config_error(path, number, "expected key = value");
    }
    const std::string key = trim(std::string_view{text}.substr(0, eq));
    const std::string raw = trim(std::string_view{text}.substr(eq + 1));
    if (key.empty()) {
      config_error(path, number, "missing key");
    }
    const std::string value = !raw.empty() && raw.front() == '"' ? parse_quoted(path, number, raw) : raw;
    assign(config, path, number, seen_endpoints, seen_username, seen_password, seen_ca, seen_cert,
           seen_key, key, value);
  }
  if (seen_password && !seen_username) {
    throw std::invalid_argument(path + ": password requires username");
  }
  if (seen_cert != seen_key) {
    throw std::invalid_argument(path + ": cert and key must be set together");
  }
  return config;
}

}  // namespace sne
