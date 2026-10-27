#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sne {

inline std::string base64_encode(std::string_view in) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= in.size()) {
    const unsigned n = (static_cast<unsigned>(static_cast<uint8_t>(in[i])) << 16) |
                       (static_cast<unsigned>(static_cast<uint8_t>(in[i + 1])) << 8) |
                       static_cast<unsigned>(static_cast<uint8_t>(in[i + 2]));
    out.push_back(kTable[(n >> 18) & 63]);
    out.push_back(kTable[(n >> 12) & 63]);
    out.push_back(kTable[(n >> 6) & 63]);
    out.push_back(kTable[n & 63]);
    i += 3;
  }
  if (i < in.size()) {
    unsigned n = static_cast<unsigned>(static_cast<uint8_t>(in[i])) << 16;
    if (i + 1 < in.size()) {
      n |= static_cast<unsigned>(static_cast<uint8_t>(in[i + 1])) << 8;
    }
    out.push_back(kTable[(n >> 18) & 63]);
    out.push_back(kTable[(n >> 12) & 63]);
    if (i + 1 < in.size()) {
      out.push_back(kTable[(n >> 6) & 63]);
      out.push_back('=');
    } else {
      out.push_back('=');
      out.push_back('=');
    }
  }
  return out;
}

inline int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

inline std::string base64_decode(std::string_view in) {
  std::string filtered;
  filtered.reserve(in.size());
  for (char c : in) {
    if (c == '\n' || c == '\r' || c == ' ') {
      continue;
    }
    filtered.push_back(c);
  }
  if (filtered.size() % 4 != 0) {
    throw std::runtime_error("bad etcd response");
  }
  std::string out;
  out.reserve(filtered.size() / 4 * 3);
  for (size_t i = 0; i < filtered.size(); i += 4) {
    const int v[4] = {
        filtered[i] == '=' ? 0 : base64_value(filtered[i]),
        filtered[i + 1] == '=' ? 0 : base64_value(filtered[i + 1]),
        filtered[i + 2] == '=' ? 0 : base64_value(filtered[i + 2]),
        filtered[i + 3] == '=' ? 0 : base64_value(filtered[i + 3]),
    };
    if (v[0] < 0 || v[1] < 0 || v[2] < 0 || v[3] < 0) {
      throw std::runtime_error("bad etcd response");
    }
    out.push_back(static_cast<char>((v[0] << 2) | (v[1] >> 4)));
    if (filtered[i + 2] != '=') {
      out.push_back(static_cast<char>(((v[1] & 15) << 4) | (v[2] >> 2)));
    }
    if (filtered[i + 3] != '=') {
      out.push_back(static_cast<char>(((v[2] & 3) << 6) | v[3]));
    }
  }
  return out;
}

}  // namespace sne
