#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sne {

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool b = false;
  std::string text;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json* find(std::string_view key) const {
    if (type != Type::Object) {
      return nullptr;
    }
    for (const auto& field : obj) {
      if (field.first == key) {
        return &field.second;
      }
    }
    return nullptr;
  }

  const Json& at(std::string_view key) const {
    const Json* value = find(key);
    if (value == nullptr) {
      throw std::runtime_error("bad etcd response");
    }
    return *value;
  }

  const std::string& str() const {
    if (type != Type::String) {
      throw std::runtime_error("bad etcd response");
    }
    return text;
  }

  int64_t i64() const {
    if (type != Type::String && type != Type::Number) {
      throw std::runtime_error("bad etcd response");
    }
    int64_t value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
      throw std::runtime_error("bad etcd response");
    }
    return value;
  }
};

inline Json parse_json(std::string_view input) {
  struct Parser {
    std::string_view in;
    size_t i = 0;

    [[noreturn]] void fail() const { throw std::runtime_error("bad etcd response"); }

    void skip() {
      while (i < in.size()) {
        const char c = in[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
          break;
        }
        ++i;
      }
    }

    char peek() {
      skip();
      if (i >= in.size()) {
        fail();
      }
      return in[i];
    }

    char get() {
      const char c = peek();
      ++i;
      return c;
    }

    Json parse() {
      Json value = parse_value();
      skip();
      if (i != in.size()) {
        fail();
      }
      return value;
    }

    Json parse_value() {
      const char c = peek();
      if (c == '{') {
        return parse_object();
      }
      if (c == '[') {
        return parse_array();
      }
      if (c == '"') {
        return parse_string();
      }
      if (c == 't' || c == 'f') {
        return parse_bool();
      }
      if (c == 'n') {
        return parse_null();
      }
      if (c == '-' || (c >= '0' && c <= '9')) {
        return parse_number();
      }
      fail();
    }

    Json parse_object() {
      Json value;
      value.type = Json::Type::Object;
      get();  // '{'
      if (peek() == '}') {
        get();
        return value;
      }
      while (true) {
        if (peek() != '"') {
          fail();
        }
        Json key = parse_string();
        if (get() != ':') {
          fail();
        }
        value.obj.emplace_back(std::move(key.text), parse_value());
        const char sep = get();
        if (sep == '}') {
          break;
        }
        if (sep != ',') {
          fail();
        }
      }
      return value;
    }

    Json parse_array() {
      Json value;
      value.type = Json::Type::Array;
      get();  // '['
      if (peek() == ']') {
        get();
        return value;
      }
      while (true) {
        value.arr.push_back(parse_value());
        const char sep = get();
        if (sep == ']') {
          break;
        }
        if (sep != ',') {
          fail();
        }
      }
      return value;
    }

    Json parse_string() {
      Json value;
      value.type = Json::Type::String;
      get();  // '"'
      while (i < in.size()) {
        const char c = in[i++];
        if (c == '"') {
          return value;
        }
        if (c != '\\') {
          value.text.push_back(c);
          continue;
        }
        if (i >= in.size()) {
          fail();
        }
        const char esc = in[i++];
        switch (esc) {
          case '"':
          case '\\':
          case '/':
            value.text.push_back(esc);
            break;
          case 'b':
            value.text.push_back('\b');
            break;
          case 'f':
            value.text.push_back('\f');
            break;
          case 'n':
            value.text.push_back('\n');
            break;
          case 'r':
            value.text.push_back('\r');
            break;
          case 't':
            value.text.push_back('\t');
            break;
          case 'u':
            append_hex(value.text);
            break;
          default:
            fail();
        }
      }
      fail();
    }

    void append_hex(std::string& out) {
      if (i + 4 > in.size()) {
        fail();
      }
      int code = 0;
      for (int n = 0; n < 4; ++n) {
        const char c = in[i++];
        code <<= 4;
        if (c >= '0' && c <= '9') {
          code += c - '0';
        } else if (c >= 'a' && c <= 'f') {
          code += c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
          code += c - 'A' + 10;
        } else {
          fail();
        }
      }
      if (code <= 0x7F) {
        out.push_back(static_cast<char>(code));
      } else if (code <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      }
    }

    Json parse_bool() {
      Json value;
      value.type = Json::Type::Bool;
      if (in.substr(i, 4) == "true") {
        value.b = true;
        i += 4;
        return value;
      }
      if (in.substr(i, 5) == "false") {
        value.b = false;
        i += 5;
        return value;
      }
      fail();
    }

    Json parse_null() {
      if (in.substr(i, 4) != "null") {
        fail();
      }
      i += 4;
      Json value;
      value.type = Json::Type::Null;
      return value;
    }

    Json parse_number() {
      const size_t start = i;
      if (in[i] == '-') {
        ++i;
      }
      if (i >= in.size() || in[i] < '0' || in[i] > '9') {
        fail();
      }
      if (in[i] == '0') {
        ++i;
      } else {
        while (i < in.size() && in[i] >= '0' && in[i] <= '9') {
          ++i;
        }
      }
      if (i < in.size() && in[i] == '.') {
        ++i;
        if (i >= in.size() || in[i] < '0' || in[i] > '9') {
          fail();
        }
        while (i < in.size() && in[i] >= '0' && in[i] <= '9') {
          ++i;
        }
      }
      if (i < in.size() && (in[i] == 'e' || in[i] == 'E')) {
        ++i;
        if (i < in.size() && (in[i] == '+' || in[i] == '-')) {
          ++i;
        }
        if (i >= in.size() || in[i] < '0' || in[i] > '9') {
          fail();
        }
        while (i < in.size() && in[i] >= '0' && in[i] <= '9') {
          ++i;
        }
      }
      Json value;
      value.type = Json::Type::Number;
      value.text = std::string{in.substr(start, i - start)};
      return value;
    }
  };

  return Parser{input, 0}.parse();
}

}  // namespace sne
