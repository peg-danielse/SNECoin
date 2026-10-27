#include "ledger.hpp"

#include "base64.hpp"
#include "json.hpp"
#include "sshkey.hpp"

#include <cstdlib>
#include <ctime>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unistd.h>

namespace sne {
namespace {

constexpr const char* kAccountPrefix = "/sne/acct/";
constexpr const char* kPubPrefix = "/sne/pub/";
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

std::string pub_key(const std::string& name) { return std::string{kPubPrefix} + name; }

std::string spend_message(const char* op, const std::string& from, const std::string& to,
                          int64_t amount, int64_t revision) {
  return std::string{"snecoin/v1\n"} + op + "\n" + from + "\n" + to + "\n" + std::to_string(amount) +
         "\n" + std::to_string(revision) + "\n";
}

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
                        int64_t amount, int64_t signed_revision, const std::string& pubkey,
                        const std::string& signature) {
  std::string json = "{\"seq\":" + std::to_string(seq) + ",\"op\":\"" + op + "\"";
  if (!from.empty()) {
    json += ",\"from\":\"" + from + "\"";
  }
  if (!to.empty()) {
    json += ",\"to\":\"" + to + "\"";
  }
  json += ",\"amount\":" + std::to_string(amount);
  if (signed_revision >= 0) {
    json += ",\"revision\":" + std::to_string(signed_revision);
  }
  if (!pubkey.empty()) {
    json += ",\"pubkey\":\"" + pubkey + "\"";
  }
  if (!signature.empty()) {
    json += ",\"sig\":\"" + signature + "\"";
  }
  json += "}";
  return json;
}

struct IllegalSpend {
  std::string key;
  int64_t seq = 0;
  int64_t amount = 0;
  std::string op;
  std::string from;
  std::string to;
};

const Json* json_string(const Json& doc, const char* key) {
  const Json* field = doc.find(key);
  if (field == nullptr || field->type != Json::Type::String) {
    return nullptr;
  }
  return field;
}

bool signature_ok(const EtcdClient& client, const Json& doc, const std::string& op,
                  const std::string& from, const std::string& to, int64_t amount,
                  int64_t revision) {
  const Json* sig = json_string(doc, "sig");
  if (sig == nullptr) {
    return false;
  }
  std::string raw;
  try {
    raw = base64_decode(sig->text);
  } catch (const std::runtime_error&) {
    return false;
  }
  const KeyState pub = read_key(client, pub_key(from));
  if (!pub.present) {
    return false;
  }
  try {
    const Ed25519Public stored = parse_public_key(pub.value);
    const std::string message = spend_message(op.c_str(), from, to, amount, revision);
    return verify_ed25519(stored, message, raw);
  } catch (const std::runtime_error&) {
    return false;
  }
}

std::optional<IllegalSpend> illegal_spend(const EtcdClient& client, const Kv& kv) {
  Json doc;
  try {
    doc = parse_json(kv.value);
  } catch (const std::runtime_error&) {
    return std::nullopt;
  }
  const Json* op = json_string(doc, "op");
  if (op == nullptr || (op->text != "debit" && op->text != "transfer")) {
    return std::nullopt;
  }
  if (doc.find("revision") == nullptr) {
    return std::nullopt;
  }
  const Json* from = json_string(doc, "from");
  const Json* amount = doc.find("amount");
  const Json* seq = doc.find("seq");
  const Json* revision = doc.find("revision");
  if (from == nullptr || from->text.empty() || amount == nullptr || seq == nullptr ||
      revision == nullptr) {
    return std::nullopt;
  }
  int64_t amount_n = 0;
  int64_t seq_n = 0;
  int64_t revision_n = 0;
  try {
    amount_n = amount->i64();
    seq_n = seq->i64();
    revision_n = revision->i64();
  } catch (const std::runtime_error&) {
    return std::nullopt;
  }
  std::string to;
  if (const Json* to_field = doc.find("to")) {
    if (to_field->type != Json::Type::String) {
      return std::nullopt;
    }
    to = to_field->text;
  }
  if (signature_ok(client, doc, op->text, from->text, to, amount_n, revision_n)) {
    return std::nullopt;
  }
  IllegalSpend spend;
  spend.key = kv.key;
  spend.seq = seq_n;
  spend.amount = amount_n;
  spend.op = op->text;
  spend.from = from->text;
  spend.to = to;
  return spend;
}

std::optional<std::string> undo_illegal(EtcdClient& client, const IllegalSpend& spend) {
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const KeyState log = read_key(client, spend.key);
    if (!log.present) {
      return std::nullopt;
    }
    const bool self = spend.from == spend.to;
    const bool touch_dest = spend.op == "transfer" && !spend.to.empty() && !self;
    const KeyState src =
        self || spend.from.empty() ? KeyState{} : read_key(client, account_key(spend.from));
    const KeyState dst = touch_dest ? read_key(client, account_key(spend.to)) : KeyState{};

    int64_t returned = 0;
    int64_t new_src = 0;
    int64_t new_dst = 0;
    bool write_src = false;
    bool write_dst = false;
    if (!self && spend.amount > 0 && src.present) {
      const int64_t bal = parse_stored_int(src.value, "balance");
      int64_t give = spend.amount;
      if (touch_dest && dst.present) {
        const int64_t dest_bal = parse_stored_int(dst.value, "balance");
        if (dest_bal < give) {
          give = dest_bal;
        }
      }
      if (give > std::numeric_limits<int64_t>::max() - bal) {
        throw std::runtime_error("balance overflow");
      }
      if (give > 0) {
        returned = give;
        new_src = bal + give;
        write_src = true;
        if (touch_dest && dst.present) {
          new_dst = parse_stored_int(dst.value, "balance") - give;
          write_dst = true;
        }
      }
    } else if (!self && spend.amount > 0 && !src.present && touch_dest && dst.present) {
      const int64_t dest_bal = parse_stored_int(dst.value, "balance");
      const int64_t take = dest_bal < spend.amount ? dest_bal : spend.amount;
      if (take > 0) {
        new_dst = dest_bal - take;
        write_dst = true;
      }
    }

    TxnRequest txn;
    add_compare(txn, spend.key, log);
    if (write_src) {
      add_compare(txn, account_key(spend.from), src);
      txn.success.push_back(PutOp{account_key(spend.from), std::to_string(new_src)});
    }
    if (write_dst) {
      add_compare(txn, account_key(spend.to), dst);
      txn.success.push_back(PutOp{account_key(spend.to), std::to_string(new_dst)});
    }
    txn.deletes.push_back(DeleteOp{spend.key});
    if (client.txn(txn)) {
      std::string note = "illegal " + spend.op + " seq " + std::to_string(spend.seq);
      if (!spend.from.empty()) {
        note += " from " + spend.from;
      }
      if (!spend.to.empty()) {
        note += " to " + spend.to;
      }
      note += " amount " + std::to_string(spend.amount) + " removed";
      if (!self && !spend.from.empty() && !src.present) {
        note += "; " + spend.from + " is missing";
      } else if (src.present && returned != spend.amount) {
        note += "; returned " + std::to_string(returned) + " to " + spend.from;
      }
      return note;
    }
    if (attempt + 1 < kMaxAttempts) {
      backoff(attempt);
    }
  }
  throw std::runtime_error("commit conflicted too many times");
}

}  // namespace

std::vector<std::string> Ledger::purge_illegal(const std::string& account) {
  std::vector<std::string> notes;
  for (;;) {
    const RangeResult logs = client_.range_prefix(kLogPrefix);
    bool removed = false;
    for (const Kv& kv : logs.kvs) {
      const std::optional<IllegalSpend> spend = illegal_spend(client_, kv);
      if (!spend) {
        continue;
      }
      if (!account.empty() && spend->from != account && spend->to != account) {
        continue;
      }
      const std::optional<std::string> note = undo_illegal(client_, *spend);
      if (!note) {
        continue;
      }
      notes.push_back(*note);
      removed = true;
      break;
    }
    if (!removed) {
      return notes;
    }
  }
}

void Ledger::open(const std::string& name, int64_t amount, const std::string& pubkey_path) {
  const Ed25519Public pubkey = load_public_key(pubkey_path);
  mutate(Op::Open, "", name, amount, pubkey.openssh, "");
}

void Ledger::credit(const std::string& name, int64_t amount) {
  mutate(Op::Credit, "", name, amount, "", "");
}

void Ledger::debit(const std::string& name, int64_t amount, const std::string& key_path) {
  mutate(Op::Debit, name, "", amount, "", key_path);
}

void Ledger::transfer(const std::string& from, const std::string& to, int64_t amount,
                      const std::string& key_path) {
  mutate(Op::Transfer, from, to, amount, "", key_path);
}

void Ledger::mutate(Op op, const std::string& from, const std::string& to, int64_t amount,
                    const std::string& openssh_pub, const std::string& key_path) {
  const bool spend = op == Op::Debit || op == Op::Transfer;
  Ed25519Private signer{};
  if (spend) {
    signer = load_private_key(key_path);
  }
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const KeyState seq = read_key(client_, kSeqKey);
    const KeyState src = from.empty() ? KeyState{} : read_key(client_, account_key(from));
    const KeyState dst = to.empty() ? KeyState{} : read_key(client_, account_key(to));

    const std::string owner = op == Op::Open ? to : from;
    const KeyState pub =
        (op == Op::Open || spend) ? read_key(client_, pub_key(owner)) : KeyState{};
    if (op == Op::Open) {
      if (dst.present || pub.present) {
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

    std::string signature;
    if (spend) {
      if (!pub.present) {
        throw std::runtime_error("account has no key: " + from);
      }
      const Ed25519Public stored = parse_public_key(pub.value);
      const std::string message = spend_message(op_name, from, to, amount, src.mod_revision);
      signature = sign_ed25519(signer, message);
      if (!verify_ed25519(stored, message, signature)) {
        throw std::runtime_error("signature rejected");
      }
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
    if (op == Op::Open) {
      add_compare(txn, pub_key(to), pub);
      txn.success.push_back(PutOp{pub_key(to), openssh_pub});
    }
    if (spend) {
      add_compare(txn, pub_key(from), pub);
    }
    add_compare(txn, kSeqKey, seq);
    txn.success.push_back(PutOp{kSeqKey, std::to_string(next_seq)});
    txn.success.push_back(PutOp{
        log_key(next_seq),
        record_json(next_seq, op_name, from, to, amount, spend ? src.mod_revision : -1,
                    op == Op::Open ? openssh_pub : "",
                    signature.empty() ? "" : base64_encode(signature))});

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
