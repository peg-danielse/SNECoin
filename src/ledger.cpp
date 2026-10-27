#include "ledger.hpp"

#include <cstdlib>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace sne {
namespace {

constexpr const char* kAccountPrefix = "/sne/acct/";
constexpr const char* kLogPrefix = "/sne/log/";
constexpr const char* kSeqKey = "/sne/meta/seq";
constexpr int kMaxAttempts = 64;

void backoff(int attempt) {
  static bool seeded = false;
  if (!seeded) {
    std::srand(static_cast<unsigned>(std::time(nullptr)) ^ static_cast<unsigned>(::getpid()));
    seeded = true;
  }
  const int millis = 2 * (attempt + 1) + (std::rand() % 5);
  usleep(static_cast<useconds_t>(millis) * 1000);
}

struct KeyState {
  bool present = false;
  int64_t mod_revision = 0;
  std::string value;
};

std::string account_key(const std::string& name) { return std::string{kAccountPrefix} + name; }

std::string log_key(int64_t seq) {
  const std::string digits = std::to_string(seq);
  if (digits.size() > 20) {
    throw std::runtime_error("sequence overflow");
  }
  return std::string{kLogPrefix} + std::string(20 - digits.size(), '0') + digits;
}

int64_t parse_stored_int(const std::string& text, const char* what) {
  if (text.empty()) {
    throw std::runtime_error(std::string{"corrupt "} + what);
  }
  int64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      throw std::runtime_error(std::string{"corrupt "} + what);
    }
    const int digit = c - '0';
    if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
      throw std::runtime_error(std::string{"corrupt "} + what);
    }
    value = value * 10 + digit;
  }
  return value;
}

KeyState read_key(const EtcdClient& client, const std::string& key) {
  const RangeResult result = client.range(key);
  KeyState state;
  if (result.kvs.empty()) {
    return state;
  }
  state.present = true;
  state.mod_revision = result.kvs[0].mod_revision;
  state.value = result.kvs[0].value;
  return state;
}

void expect_present(const KeyState& state, const std::string& name) {
  if (!state.present) {
    throw std::runtime_error("account not found: " + name);
  }
}

void add_compare(TxnRequest& txn, const std::string& key, const KeyState& state) {
  Compare compare;
  compare.key = key;
  if (state.present) {
    compare.target = CompareTarget::Mod;
    compare.revision = state.mod_revision;
  } else {
    compare.target = CompareTarget::Create;
    compare.revision = 0;
  }
  txn.compare.push_back(std::move(compare));
}

std::string record_json(int64_t seq, const char* op, const std::string& from, const std::string& to,
                        int64_t amount) {
  std::string json = "{\"seq\":" + std::to_string(seq) + ",\"op\":\"" + op + "\"";
  if (!from.empty()) {
    json += ",\"from\":\"" + from + "\"";
  }
  if (!to.empty()) {
    json += ",\"to\":\"" + to + "\"";
  }
  json += ",\"amount\":" + std::to_string(amount) + "}";
  return json;
}

}  // namespace

void Ledger::open(const std::string& name, int64_t amount) { mutate(Op::Open, "", name, amount); }

void Ledger::credit(const std::string& name, int64_t amount) {
  mutate(Op::Credit, "", name, amount);
}

void Ledger::debit(const std::string& name, int64_t amount) { mutate(Op::Debit, name, "", amount); }

void Ledger::transfer(const std::string& from, const std::string& to, int64_t amount) {
  mutate(Op::Transfer, from, to, amount);
}

void Ledger::mutate(Op op, const std::string& from, const std::string& to, int64_t amount) {
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const KeyState seq = read_key(client_, kSeqKey);
    const KeyState src = from.empty() ? KeyState{} : read_key(client_, account_key(from));
    const KeyState dst = to.empty() ? KeyState{} : read_key(client_, account_key(to));

    if (op == Op::Open) {
      if (dst.present) {
        throw std::runtime_error("account exists: " + to);
      }
    } else {
      if (!from.empty()) {
        expect_present(src, from);
      }
      if (!to.empty()) {
        expect_present(dst, to);
      }
    }

    int64_t next_seq = 1;
    if (seq.present) {
      const int64_t current = parse_stored_int(seq.value, "sequence");
      if (current == std::numeric_limits<int64_t>::max()) {
        throw std::runtime_error("sequence overflow");
      }
      next_seq = current + 1;
    }

    int64_t new_src = 0;
    int64_t new_dst = 0;
    if (op == Op::Open) {
      new_dst = amount;
    } else if (op == Op::Credit) {
      const int64_t balance = parse_stored_int(dst.value, "balance");
      if (amount > std::numeric_limits<int64_t>::max() - balance) {
        throw std::runtime_error("balance overflow");
      }
      new_dst = balance + amount;
    } else if (op == Op::Debit) {
      const int64_t balance = parse_stored_int(src.value, "balance");
      if (balance < amount) {
        throw std::runtime_error("insufficient balance");
      }
      new_src = balance - amount;
    } else {
      const int64_t source = parse_stored_int(src.value, "balance");
      const int64_t dest = parse_stored_int(dst.value, "balance");
      if (source < amount) {
        throw std::runtime_error("insufficient balance");
      }
      if (amount > std::numeric_limits<int64_t>::max() - dest) {
        throw std::runtime_error("balance overflow");
      }
      new_src = source - amount;
      new_dst = dest + amount;
    }

    const char* op_name = "transfer";
    switch (op) {
      case Op::Open:
        op_name = "open";
        break;
      case Op::Credit:
        op_name = "credit";
        break;
      case Op::Debit:
        op_name = "debit";
        break;
      case Op::Transfer:
        op_name = "transfer";
        break;
    }

    TxnRequest txn;
    if (!from.empty()) {
      add_compare(txn, account_key(from), src);
      txn.success.push_back(PutOp{account_key(from), std::to_string(new_src)});
    }
    if (!to.empty()) {
      add_compare(txn, account_key(to), dst);
      txn.success.push_back(PutOp{account_key(to), std::to_string(new_dst)});
    }
    add_compare(txn, kSeqKey, seq);
    txn.success.push_back(PutOp{kSeqKey, std::to_string(next_seq)});
    txn.success.push_back(
        PutOp{log_key(next_seq), record_json(next_seq, op_name, from, to, amount)});

    if (client_.txn(txn)) {
      return;
    }
    if (attempt + 1 < kMaxAttempts) {
      backoff(attempt);
    }
  }
  throw std::runtime_error("commit conflicted too many times");
}

int64_t Ledger::balance(const std::string& name) const {
  const KeyState state = read_key(client_, account_key(name));
  if (!state.present) {
    throw std::runtime_error("account not found: " + name);
  }
  return parse_stored_int(state.value, "balance");
}

std::vector<ReplicaBalance> Ledger::balance_from_all(const std::string& name) const {
  std::vector<ReplicaBalance> rows;
  const std::string key = account_key(name);
  for (const std::string& endpoint : client_.endpoints()) {
    ReplicaBalance row;
    row.endpoint = endpoint;
    try {
      const RangeResult result = client_.range_on(endpoint, key);
      if (result.kvs.empty()) {
        throw std::runtime_error("account not found: " + name);
      }
      row.balance = parse_stored_int(result.kvs[0].value, "balance");
      row.ok = true;
    } catch (const std::exception& error) {
      row.error = error.what();
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<AccountBalance> Ledger::list() const {
  const RangeResult result = client_.range_prefix(kAccountPrefix);
  std::vector<AccountBalance> accounts;
  const std::string prefix = kAccountPrefix;
  for (const Kv& kv : result.kvs) {
    if (kv.key.compare(0, prefix.size(), prefix) != 0 || kv.key.size() == prefix.size()) {
      throw std::runtime_error("unexpected account key");
    }
    AccountBalance account;
    account.name = kv.key.substr(prefix.size());
    account.balance = parse_stored_int(kv.value, "balance");
    accounts.push_back(std::move(account));
  }
  return accounts;
}

std::vector<std::string> Ledger::history(int64_t limit) const {
  const RangeResult result = client_.range_prefix_desc(kLogPrefix, limit);
  std::vector<std::string> lines;
  lines.reserve(result.kvs.size());
  for (auto it = result.kvs.rbegin(); it != result.kvs.rend(); ++it) {
    lines.push_back(it->value);
  }
  return lines;
}

}  // namespace sne
