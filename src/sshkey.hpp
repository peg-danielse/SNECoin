#pragma once

#include <string>
#include <string_view>

namespace sne {

struct Ed25519Public {
  unsigned char bytes[32];
  std::string openssh;
};

struct Ed25519Private {
  Ed25519Public pub;
  unsigned char seed[32];
};

Ed25519Public load_public_key(const std::string& path);
Ed25519Public parse_public_key(std::string_view line);
Ed25519Private load_private_key(const std::string& path);
std::string sign_ed25519(const Ed25519Private& key, std::string_view message);
bool verify_ed25519(const Ed25519Public& key, std::string_view message, std::string_view signature);

}  // namespace sne
