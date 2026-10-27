#include "sshkey.hpp"

#include "base64.hpp"

#include <openssl/evp.h>

#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace sne {
namespace {

class Reader {
 public:
  explicit Reader(std::string_view in) : in_(in) {}

  uint32_t u32() {
    if (i_ + 4 > in_.size()) {
      fail();
    }
    const auto b0 = static_cast<unsigned char>(in_[i_]);
    const auto b1 = static_cast<unsigned char>(in_[i_ + 1]);
    const auto b2 = static_cast<unsigned char>(in_[i_ + 2]);
    const auto b3 = static_cast<unsigned char>(in_[i_ + 3]);
    i_ += 4;
    return (static_cast<uint32_t>(b0) << 24) | (static_cast<uint32_t>(b1) << 16) |
           (static_cast<uint32_t>(b2) << 8) | static_cast<uint32_t>(b3);
  }

  std::string_view str() {
    const uint32_t n = u32();
    if (i_ + n > in_.size()) {
      fail();
    }
    const std::string_view out = in_.substr(i_, n);
    i_ += n;
    return out;
  }

 private:
  [[noreturn]] void fail() const { throw std::runtime_error("invalid OpenSSH key"); }

  std::string_view in_;
  size_t i_ = 0;
};

std::string read_file(const std::string& path) {
  std::ifstream input{path};
  if (!input) {
    throw std::runtime_error("cannot read key file: " + path);
  }
  return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string unarmor(const std::string& text) {
  std::string b64;
  bool body = false;
  std::string line;
  for (char c : text) {
    if (c == '\n' || c == '\r') {
      if (line.rfind("-----BEGIN", 0) == 0) {
        body = true;
      } else if (line.rfind("-----END", 0) == 0) {
        break;
      } else if (body) {
        b64 += line;
      }
      line.clear();
      continue;
    }
    line.push_back(c);
  }
  if (!body || b64.empty()) {
    throw std::runtime_error("invalid OpenSSH key");
  }
  try {
    return base64_decode(b64);
  } catch (const std::runtime_error&) {
    throw std::runtime_error("invalid OpenSSH key");
  }
}

Ed25519Public public_from_blob(std::string_view blob) {
  Reader reader{blob};
  const std::string_view type = reader.str();
  const std::string_view raw = reader.str();
  if (type != "ssh-ed25519" || raw.size() != 32) {
    throw std::runtime_error("unsupported key type");
  }
  Ed25519Public key;
  for (size_t i = 0; i < 32; ++i) {
    key.bytes[i] = static_cast<unsigned char>(raw[i]);
  }
  key.openssh = "ssh-ed25519 " + base64_encode(blob);
  return key;
}

Ed25519Public public_from_line(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.remove_suffix(1);
  }
  if (text.rfind("ssh-ed25519 ", 0) != 0) {
    throw std::runtime_error("unsupported key type");
  }
  text.remove_prefix(std::string_view{"ssh-ed25519 "}.size());
  const size_t space = text.find(' ');
  if (space != std::string_view::npos) {
    text = text.substr(0, space);
  }
  if (text.empty()) {
    throw std::runtime_error("invalid OpenSSH key");
  }
  try {
    return public_from_blob(base64_decode(text));
  } catch (const std::runtime_error&) {
    throw std::runtime_error("invalid OpenSSH key");
  }
}

}  // namespace

Ed25519Public load_public_key(const std::string& path) { return public_from_line(read_file(path)); }

Ed25519Public parse_public_key(std::string_view line) { return public_from_line(line); }

Ed25519Private load_private_key(const std::string& path) {
  const std::string blob = unarmor(read_file(path));
  constexpr std::string_view kMagic = "openssh-key-v1";
  if (blob.size() < kMagic.size() + 1 || blob.compare(0, kMagic.size(), kMagic) != 0 ||
      blob[kMagic.size()] != '\0') {
    throw std::runtime_error("invalid OpenSSH key");
  }
  Reader reader{std::string_view{blob}.substr(kMagic.size() + 1)};
  const std::string_view cipher = reader.str();
  const std::string_view kdf = reader.str();
  (void)reader.str();
  if (cipher != "none" || kdf != "none") {
    throw std::runtime_error("private key is encrypted");
  }
  if (reader.u32() != 1) {
    throw std::runtime_error("invalid OpenSSH key");
  }
  const Ed25519Public pub = public_from_blob(reader.str());
  Reader secret{reader.str()};
  const uint32_t check1 = secret.u32();
  const uint32_t check2 = secret.u32();
  if (check1 != check2 || secret.str() != "ssh-ed25519") {
    throw std::runtime_error("invalid OpenSSH key");
  }
  const std::string_view embedded_pub = secret.str();
  const std::string_view embedded_priv = secret.str();
  if (embedded_pub.size() != 32 || embedded_priv.size() != 64) {
    throw std::runtime_error("invalid OpenSSH key");
  }
  Ed25519Private key;
  key.pub = pub;
  for (size_t i = 0; i < 32; ++i) {
    key.seed[i] = static_cast<unsigned char>(embedded_priv[i]);
  }
  return key;
}

std::string sign_ed25519(const Ed25519Private& key, std::string_view message) {
  EVP_PKEY* pkey =
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, key.seed, sizeof(key.seed));
  if (pkey == nullptr) {
    throw std::runtime_error("invalid OpenSSH key");
  }
  struct PKey {
    EVP_PKEY* handle;
    ~PKey() { EVP_PKEY_free(handle); }
  } owned{pkey};
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    throw std::runtime_error("invalid OpenSSH key");
  }
  struct Ctx {
    EVP_MD_CTX* handle;
    ~Ctx() { EVP_MD_CTX_free(handle); }
  } md{ctx};
  if (EVP_DigestSignInit(md.handle, nullptr, nullptr, nullptr, owned.handle) != 1) {
    throw std::runtime_error("invalid OpenSSH key");
  }
  size_t len = 64;
  std::string signature(64, '\0');
  if (EVP_DigestSign(md.handle, reinterpret_cast<unsigned char*>(signature.data()), &len,
                     reinterpret_cast<const unsigned char*>(message.data()), message.size()) != 1 ||
      len != 64) {
    throw std::runtime_error("invalid OpenSSH key");
  }
  return signature;
}

bool verify_ed25519(const Ed25519Public& key, std::string_view message, std::string_view signature) {
  if (signature.size() != 64) {
    return false;
  }
  EVP_PKEY* pkey =
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.bytes, sizeof(key.bytes));
  if (pkey == nullptr) {
    return false;
  }
  struct PKey {
    EVP_PKEY* handle;
    ~PKey() { EVP_PKEY_free(handle); }
  } owned{pkey};
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    return false;
  }
  struct Ctx {
    EVP_MD_CTX* handle;
    ~Ctx() { EVP_MD_CTX_free(handle); }
  } md{ctx};
  if (EVP_DigestVerifyInit(md.handle, nullptr, nullptr, nullptr, owned.handle) != 1) {
    return false;
  }
  const int ok = EVP_DigestVerify(md.handle, reinterpret_cast<const unsigned char*>(signature.data()),
                                  signature.size(),
                                  reinterpret_cast<const unsigned char*>(message.data()),
                                  message.size());
  return ok == 1;
}

}  // namespace sne
