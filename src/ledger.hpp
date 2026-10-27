#pragma once

#include "etcd_client.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sne {

struct ReplicaBalance {
  std::string endpoint;
  bool ok = false;
  int64_t balance = 0;
  std::string error;
};

struct AccountBalance {
  std::string name;
  int64_t balance = 0;
};

class Ledger {
 public:
  explicit Ledger(EtcdClient& client) : client_(client) {}

  void open(const std::string& name, int64_t amount, const std::string& pubkey_path);
  void credit(const std::string& name, int64_t amount);
  void debit(const std::string& name, int64_t amount, const std::string& key_path);
  void transfer(const std::string& from, const std::string& to, int64_t amount,
                const std::string& key_path);

  // Drop spend records whose signature does not verify. An empty account
  // checks every record. Returns one notice line per removed transaction.
  std::vector<std::string> purge_illegal(const std::string& account);

  int64_t balance(const std::string& name) const;
  std::vector<ReplicaBalance> balance_from_all(const std::string& name) const;
  std::vector<AccountBalance> list() const;
  std::vector<std::string> history(int64_t limit) const;

 private:
  enum class Op { Open, Credit, Debit, Transfer };

  void mutate(Op op, const std::string& from, const std::string& to, int64_t amount,
              const std::string& openssh_pub, const std::string& key_path);

  EtcdClient& client_;
};

}  // namespace sne
