#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sne {

struct Kv {
  std::string key;
  std::string value;
  int64_t create_revision = 0;
  int64_t mod_revision = 0;
};

struct RangeResult {
  std::vector<Kv> kvs;
};

enum class CompareTarget { Mod, Create };

struct Compare {
  std::string key;
  CompareTarget target = CompareTarget::Mod;
  int64_t revision = 0;
};

struct PutOp {
  std::string key;
  std::string value;
};

struct TxnRequest {
  std::vector<Compare> compare;
  std::vector<PutOp> success;
};

struct ClientOptions {
  std::vector<std::string> endpoints;
  std::string username;
  std::string password;
  std::string ca_cert;
  std::string cert;
  std::string key;
};

// HTTP client for etcd's v3 gateway. Each call tries endpoints in order
// until a member accepts the request. A username is exchanged for a token
// before the first ledger call.
class EtcdClient {
 public:
  explicit EtcdClient(ClientOptions options);

  const std::vector<std::string>& endpoints() const { return endpoints_; }

  RangeResult range(const std::string& key) const;
  RangeResult range_prefix(const std::string& prefix) const;
  RangeResult range_prefix_desc(const std::string& prefix, int64_t limit) const;
  RangeResult range_on(const std::string& endpoint, const std::string& key) const;
  bool txn(const TxnRequest& request) const;

 private:
  struct RangeQuery {
    std::string key;
    std::string range_end;
    int64_t limit = 0;
    bool descend = false;
  };

  static std::string build_range(const RangeQuery& query);
  RangeResult range_query(const RangeQuery& query) const;
  RangeResult range_query_on(const std::string& endpoint, const RangeQuery& query) const;
  void authenticate();
  std::string post(const std::string& path, const std::string& body) const;
  std::string post_on(const std::string& endpoint, const std::string& path, const std::string& body,
                      bool failover) const;

  std::vector<std::string> endpoints_;
  std::string username_;
  std::string password_;
  std::string ca_cert_;
  std::string cert_;
  std::string key_;
  std::string token_;
};

std::vector<std::string> split_endpoints(std::string_view text);

}  // namespace sne
