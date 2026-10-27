#include "etcd_client.hpp"

#include "base64.hpp"
#include "json.hpp"

#include <curl/curl.h>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace sne {
namespace {

struct TransportError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string json_escape(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0x0f]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

std::string flatten(std::string text) {
  for (char& c : text) {
    if (c == '\n' || c == '\r' || c == '\t') {
      c = ' ';
    }
  }
  if (text.size() > 180) {
    text.resize(180);
  }
  return text;
}

std::string prefix_end(std::string key) {
  for (int i = static_cast<int>(key.size()) - 1; i >= 0; --i) {
    const auto byte = static_cast<unsigned char>(key[static_cast<size_t>(i)]);
    if (byte < 0xff) {
      key[static_cast<size_t>(i)] = static_cast<char>(byte + 1);
      key.resize(static_cast<size_t>(i) + 1);
      return key;
    }
  }
  throw std::runtime_error("prefix has no successor");
}

std::string quote_b64(std::string_view raw) { return base64_encode(raw); }

std::string build_txn(const TxnRequest& request) {
  std::string body = "{\"compare\":[";
  for (size_t i = 0; i < request.compare.size(); ++i) {
    if (i != 0) {
      body += ',';
    }
    const Compare& compare = request.compare[i];
    const bool mod = compare.target == CompareTarget::Mod;
    body += "{\"key\":\"" + quote_b64(compare.key) + "\",\"target\":\"";
    body += mod ? "MOD" : "CREATE";
    body += "\",\"result\":\"EQUAL\",\"";
    body += mod ? "mod_revision" : "create_revision";
    body += "\":\"" + std::to_string(compare.revision) + "\"}";
  }
  body += "],\"success\":[";
  for (size_t i = 0; i < request.success.size(); ++i) {
    if (i != 0) {
      body += ',';
    }
    const PutOp& put = request.success[i];
    body += "{\"request_put\":{\"key\":\"" + quote_b64(put.key) + "\",\"value\":\"" +
            quote_b64(put.value) + "\"}}";
  }
  for (size_t i = 0; i < request.deletes.size(); ++i) {
    if (i != 0 || !request.success.empty()) {
      body += ',';
    }
    body += "{\"request_delete_range\":{\"key\":\"" + quote_b64(request.deletes[i].key) + "\"}}";
  }
  body += "]}";
  return body;
}

RangeResult parse_range(const Json& json) {
  RangeResult result;
  const Json* kvs = json.find("kvs");
  if (kvs == nullptr) {
    return result;
  }
  if (kvs->type != Json::Type::Array) {
    throw std::runtime_error("bad etcd response");
  }
  for (const Json& item : kvs->arr) {
    Kv kv;
    kv.key = base64_decode(item.at("key").str());
    if (const Json* value = item.find("value")) {
      kv.value = base64_decode(value->str());
    }
    kv.create_revision = item.at("create_revision").i64();
    kv.mod_revision = item.at("mod_revision").i64();
    result.kvs.push_back(std::move(kv));
  }
  return result;
}

std::string error_message(const std::string& body, long status) {
  try {
    const Json json = parse_json(body);
    if (const Json* message = json.find("message")) {
      if (message->type == Json::Type::String && !message->text.empty()) {
        return flatten(message->text);
      }
    }
  } catch (const std::exception&) {
  }
  if (body.empty()) {
    return "etcd HTTP " + std::to_string(status);
  }
  return flatten(body);
}

}  // namespace

std::vector<std::string> split_endpoints(std::string_view text) {
  std::vector<std::string> endpoints;
  std::string current;
  auto push = [&]() {
    size_t begin = 0;
    while (begin < current.size() && (current[begin] == ' ' || current[begin] == '\t')) {
      ++begin;
    }
    size_t end = current.size();
    while (end > begin && (current[end - 1] == ' ' || current[end - 1] == '\t')) {
      --end;
    }
    if (begin != end) {
      endpoints.emplace_back(current.substr(begin, end - begin));
    }
    current.clear();
  };
  for (char c : text) {
    if (c == ',') {
      push();
    } else {
      current.push_back(c);
    }
  }
  push();
  return endpoints;
}

std::string EtcdClient::build_range(const RangeQuery& query) {
  std::string body = "{\"key\":\"" + quote_b64(query.key) + "\"";
  if (!query.range_end.empty()) {
    body += ",\"range_end\":\"" + quote_b64(query.range_end) + "\"";
  }
  if (query.limit > 0) {
    body += ",\"limit\":\"" + std::to_string(query.limit) + "\"";
  }
  if (query.descend) {
    body += ",\"sort_order\":\"DESCEND\",\"sort_target\":\"KEY\"";
  }
  body += "}";
  return body;
}

EtcdClient::EtcdClient(ClientOptions options)
    : endpoints_(std::move(options.endpoints)),
      username_(std::move(options.username)),
      password_(std::move(options.password)),
      ca_cert_(std::move(options.ca_cert)),
      cert_(std::move(options.cert)),
      key_(std::move(options.key)) {
  if (endpoints_.empty()) {
    throw std::invalid_argument("no etcd endpoints");
  }
  if (username_.empty() && !password_.empty()) {
    throw std::invalid_argument("password requires username");
  }
  if (cert_.empty() != key_.empty()) {
    throw std::invalid_argument("cert and key must be set together");
  }
  if (!username_.empty()) {
    authenticate();
  }
}

void EtcdClient::authenticate() {
  const std::string body = "{\"name\":\"" + json_escape(username_) + "\",\"password\":\"" +
                           json_escape(password_) + "\"}";
  const std::string response = post("/v3/auth/authenticate", body);
  try {
    const Json json = parse_json(response);
    const Json* token = json.find("token");
    if (token == nullptr || token->type != Json::Type::String || token->text.empty()) {
      throw std::runtime_error("bad etcd response");
    }
    token_ = token->text;
  } catch (const std::runtime_error&) {
    throw std::runtime_error("bad etcd response: " + flatten(response));
  }
}

RangeResult EtcdClient::range(const std::string& key) const {
  RangeQuery query;
  query.key = key;
  return range_query(query);
}

RangeResult EtcdClient::range_prefix(const std::string& prefix) const {
  RangeQuery query;
  query.key = prefix;
  query.range_end = prefix_end(prefix);
  return range_query(query);
}

RangeResult EtcdClient::range_prefix_desc(const std::string& prefix, int64_t limit) const {
  RangeQuery query;
  query.key = prefix;
  query.range_end = prefix_end(prefix);
  query.limit = limit;
  query.descend = true;
  return range_query(query);
}

RangeResult EtcdClient::range_on(const std::string& endpoint, const std::string& key) const {
  RangeQuery query;
  query.key = key;
  return range_query_on(endpoint, query);
}

RangeResult EtcdClient::range_query(const RangeQuery& query) const {
  const std::string body = build_range(query);
  const std::string response = post("/v3/kv/range", body);
  try {
    return parse_range(parse_json(response));
  } catch (const std::runtime_error&) {
    throw std::runtime_error("bad etcd response: " + flatten(response));
  }
}

RangeResult EtcdClient::range_query_on(const std::string& endpoint, const RangeQuery& query) const {
  const std::string response = post_on(endpoint, "/v3/kv/range", build_range(query), false);
  try {
    return parse_range(parse_json(response));
  } catch (const std::runtime_error&) {
    throw std::runtime_error("bad etcd response: " + flatten(response));
  }
}

bool EtcdClient::txn(const TxnRequest& request) const {
  const std::string response = post("/v3/kv/txn", build_txn(request));
  try {
    const Json json = parse_json(response);
    const Json* succeeded = json.find("succeeded");
    // protojson omits false, the zero value, so a lost compare has no field.
    if (succeeded == nullptr) {
      return false;
    }
    if (succeeded->type != Json::Type::Bool) {
      throw std::runtime_error("bad etcd response");
    }
    return succeeded->b;
  } catch (const std::runtime_error&) {
    throw std::runtime_error("bad etcd response: " + flatten(response));
  }
}

std::string EtcdClient::post(const std::string& path, const std::string& body) const {
  std::string last_error = "no etcd member answered";
  for (const std::string& endpoint : endpoints_) {
    try {
      return post_on(endpoint, path, body, true);
    } catch (const TransportError& error) {
      last_error = error.what();
    }
  }
  throw std::runtime_error("no etcd member answered: " + last_error);
}

std::string EtcdClient::post_on(const std::string& endpoint, const std::string& path,
                                const std::string& body, bool failover) const {
  std::string root = endpoint;
  while (!root.empty() && root.back() == '/') {
    root.pop_back();
  }
  const std::string url = root + path;

  CURL* raw = curl_easy_init();
  if (raw == nullptr) {
    throw std::runtime_error("curl_easy_init failed");
  }
  struct CurlEasy {
    CURL* handle;
    ~CurlEasy() { curl_easy_cleanup(handle); }
  } easy{raw};

  std::string response;
  char errbuf[CURL_ERROR_SIZE] = {};
  std::string authorization;
  curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
  if (!token_.empty() && path != "/v3/auth/authenticate") {
    authorization = "Authorization: " + token_;
    headers = curl_slist_append(headers, authorization.c_str());
  }
  struct HeaderList {
    curl_slist* list;
    ~HeaderList() { curl_slist_free_all(list); }
  } header_list{headers};

  curl_easy_setopt(easy.handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(easy.handle, CURLOPT_POST, 1L);
  curl_easy_setopt(easy.handle, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(easy.handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(easy.handle, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(easy.handle, CURLOPT_WRITEFUNCTION, write_body);
  curl_easy_setopt(easy.handle, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(easy.handle, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(easy.handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy.handle, CURLOPT_CONNECTTIMEOUT, 2L);
  curl_easy_setopt(easy.handle, CURLOPT_TIMEOUT, 10L);
  if (!ca_cert_.empty()) {
    curl_easy_setopt(easy.handle, CURLOPT_CAINFO, ca_cert_.c_str());
  }
  if (!cert_.empty()) {
    curl_easy_setopt(easy.handle, CURLOPT_SSLCERT, cert_.c_str());
    curl_easy_setopt(easy.handle, CURLOPT_SSLKEY, key_.c_str());
  }

  const CURLcode rc = curl_easy_perform(easy.handle);
  if (rc != CURLE_OK) {
    const char* message = errbuf[0] != '\0' ? errbuf : curl_easy_strerror(rc);
    if (failover) {
      throw TransportError(message);
    }
    throw std::runtime_error(message);
  }

  long status = 0;
  curl_easy_getinfo(easy.handle, CURLINFO_RESPONSE_CODE, &status);
  if (status >= 500 || status == 0) {
    const std::string message = error_message(response, status);
    if (failover) {
      throw TransportError(message);
    }
    throw std::runtime_error(message);
  }
  if (status < 200 || status >= 300) {
    throw std::runtime_error(error_message(response, status));
  }
  return response;
}

}  // namespace sne
