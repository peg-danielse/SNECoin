#include "config.hpp"
#include "etcd_client.hpp"
#include "ledger.hpp"

#include <curl/curl.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kDefaultEndpoints =
    "http://127.0.0.1:2379,http://127.0.0.1:2381,http://127.0.0.1:2383";

struct CurlGlobal {
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
      throw std::runtime_error("curl_global_init failed");
    }
  }
  ~CurlGlobal() { curl_global_cleanup(); }
  CurlGlobal(const CurlGlobal&) = delete;
  CurlGlobal& operator=(const CurlGlobal&) = delete;
};

void report_illegal(sne::Ledger& ledger, const std::string& account) {
  for (const std::string& line : ledger.purge_illegal(account)) {
    std::cerr << "snecoin: " << line << '\n';
  }
}

void print_usage(std::ostream& out) {
  out << "snecoin " << SNE_VERSION << " — Security & Network Engineering ledger\n"
      << "\n"
      << "Usage:\n"
      << "  snecoin [--config FILE] [--endpoints URL[,URL...]] <command> [args]\n"
      << "\n"
      << "Commands:\n"
      << "  open <name> <amount> --pubkey FILE\n"
      << "  balance <name> [--from-all]   Print a balance, or read every member\n"
      << "  credit <name> <amount>        Add to an account\n"
      << "  debit <name> <amount> --key FILE\n"
      << "  transfer <from> <to> <amount> --key FILE\n"
      << "  list                          Print every account\n"
      << "  history [n]                   Print the latest n records (default 20)\n"
      << "  version                       Print the client version\n"
      << "  help                          Show this text\n"
      << "\n"
      << "Default endpoints: " << kDefaultEndpoints << "\n"
      << "A config file overrides those endpoints and can set username, password,\n"
      << "ca_cert, cert, and key. Search order: --config, ./sne.conf,\n"
      << "~/.config/sne/config. The repo file is sne.conf.\n"
      << "open stores an ssh-ed25519 public key. debit and transfer are rejected\n"
      << "unless --key signs them with the matching private key.\n";
}

sne::ClientOptions resolve_options(bool endpoints_set, const std::string& endpoints_value,
                                   bool config_set, const std::string& config_path) {
  sne::ClientConfig file;
  if (config_set) {
    file = sne::load_config_file(config_path);
  } else if (const std::optional<std::string> found = sne::find_config_file()) {
    file = sne::load_config_file(*found);
  }
  sne::ClientOptions options;
  const std::string endpoints =
      endpoints_set ? endpoints_value
                    : (file.endpoints.empty() ? std::string{kDefaultEndpoints} : file.endpoints);
  options.endpoints = sne::split_endpoints(endpoints);
  options.username = std::move(file.username);
  options.password = std::move(file.password);
  options.ca_cert = std::move(file.ca_cert);
  options.cert = std::move(file.cert);
  options.key = std::move(file.key);
  return options;
}

int fail_args(const std::string& message) {
  std::cerr << "snecoin: " << message << '\n';
  return 2;
}

bool valid_name(std::string_view name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  for (char c : name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::optional<int64_t> parse_amount(std::string_view text, bool allow_zero) {
  if (text.empty()) {
    return std::nullopt;
  }
  int64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const int digit = c - '0';
    if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  if (!allow_zero && value == 0) {
    return std::nullopt;
  }
  return value;
}

std::string require_name(std::string_view name) {
  if (!valid_name(name)) {
    throw std::invalid_argument("invalid account name");
  }
  return std::string{name};
}

}  // namespace

int main(int argc, char** argv) {
  std::string endpoints{kDefaultEndpoints};
  bool endpoints_set = false;
  std::string config_path;
  bool config_set = false;
  std::string command;
  std::vector<std::string> args;
  bool from_all = false;
  std::string pubkey_path;
  bool pubkey_set = false;
  std::string key_path;
  bool key_set = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--help" || arg == "-h") {
      print_usage(std::cout);
      return 0;
    }
    if (arg == "--version" || arg == "-V") {
      std::cout << "snecoin " << SNE_VERSION << '\n';
      return 0;
    }
    if (arg == "--from-all") {
      from_all = true;
      continue;
    }
    if (arg == "--pubkey") {
      if (i + 1 >= argc) {
        return fail_args("--pubkey requires a value");
      }
      pubkey_path = argv[++i];
      pubkey_set = true;
      continue;
    }
    if (arg.rfind("--pubkey=", 0) == 0) {
      pubkey_path = std::string{arg.substr(std::string_view{"--pubkey="}.size())};
      pubkey_set = true;
      continue;
    }
    if (arg == "--key") {
      if (i + 1 >= argc) {
        return fail_args("--key requires a value");
      }
      key_path = argv[++i];
      key_set = true;
      continue;
    }
    if (arg.rfind("--key=", 0) == 0) {
      key_path = std::string{arg.substr(std::string_view{"--key="}.size())};
      key_set = true;
      continue;
    }
    if (arg == "--config") {
      if (i + 1 >= argc) {
        return fail_args("--config requires a value");
      }
      config_path = argv[++i];
      config_set = true;
      continue;
    }
    if (arg.rfind("--config=", 0) == 0) {
      config_path = std::string{arg.substr(std::string_view{"--config="}.size())};
      config_set = true;
      continue;
    }
    if (arg == "--endpoints") {
      if (i + 1 >= argc) {
        return fail_args("--endpoints requires a value");
      }
      endpoints = argv[++i];
      endpoints_set = true;
      continue;
    }
    if (arg.rfind("--endpoints=", 0) == 0) {
      endpoints = std::string{arg.substr(std::string_view{"--endpoints="}.size())};
      endpoints_set = true;
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      return fail_args("unknown option '" + std::string{arg} + "'");
    }
    if (command.empty()) {
      command = std::string{arg};
    } else {
      args.emplace_back(arg);
    }
  }

  if (command.empty()) {
    print_usage(std::cerr);
    return 2;
  }
  if (command == "help") {
    if (!args.empty() || from_all || key_set || pubkey_set) {
      return fail_args("unexpected argument");
    }
    print_usage(std::cout);
    return 0;
  }
  if (command == "version") {
    if (!args.empty() || from_all || key_set || pubkey_set) {
      return fail_args("unexpected argument");
    }
    std::cout << "snecoin " << SNE_VERSION << '\n';
    return 0;
  }
  if (from_all && command != "balance") {
    return fail_args("--from-all applies to balance");
  }

  try {
    const sne::ClientOptions options =
        resolve_options(endpoints_set, endpoints, config_set, config_path);
    if (command == "open") {
      if (args.size() != 2) {
        return fail_args("open requires <name> <amount>");
      }
      const std::string name = require_name(args[0]);
      const std::optional<int64_t> amount = parse_amount(args[1], true);
      if (!amount) {
        return fail_args("invalid amount");
      }
      if (!pubkey_set) {
        return fail_args("open requires --pubkey");
      }
      if (key_set) {
        return fail_args("--key does not apply to open");
      }
      const CurlGlobal curl;
      sne::EtcdClient client{options};
      sne::Ledger ledger{client};
      ledger.open(name, *amount, pubkey_path);
      return 0;
    }
    if (command == "credit" || command == "debit") {
      if (args.size() != 2) {
        return fail_args(command + " requires <name> <amount>");
      }
      const std::string name = require_name(args[0]);
      const std::optional<int64_t> amount = parse_amount(args[1], false);
      if (!amount) {
        return fail_args("invalid amount");
      }
      if (command == "credit") {
        if (key_set || pubkey_set) {
          return fail_args("credit does not take a key");
        }
      } else {
        if (!key_set) {
          return fail_args("debit requires --key");
        }
        if (pubkey_set) {
          return fail_args("--pubkey does not apply to debit");
        }
      }
      const CurlGlobal curl;
      sne::EtcdClient client{options};
      sne::Ledger ledger{client};
      report_illegal(ledger, name);
      if (command == "credit") {
        ledger.credit(name, *amount);
      } else {
        ledger.debit(name, *amount, key_path);
      }
      return 0;
    }
    if (command == "transfer") {
      if (args.size() != 3) {
        return fail_args("transfer requires <from> <to> <amount>");
      }
      const std::string from = require_name(args[0]);
      const std::string to = require_name(args[1]);
      if (from == to) {
        return fail_args("transfer requires two different accounts");
      }
      const std::optional<int64_t> amount = parse_amount(args[2], false);
      if (!amount) {
        return fail_args("invalid amount");
      }
      if (!key_set) {
        return fail_args("transfer requires --key");
      }
      if (pubkey_set) {
        return fail_args("--pubkey does not apply to transfer");
      }
      const CurlGlobal curl;
      sne::EtcdClient client{options};
      sne::Ledger ledger{client};
      report_illegal(ledger, from);
      report_illegal(ledger, to);
      ledger.transfer(from, to, *amount, key_path);
      return 0;
    }
    if (command == "balance") {
      if (key_set || pubkey_set) {
        return fail_args("balance does not take a key");
      }
      if (args.size() != 1) {
        return fail_args("balance requires <name>");
      }
      const std::string name = require_name(args[0]);
      const CurlGlobal curl;
      sne::EtcdClient client{options};
      sne::Ledger ledger{client};
      report_illegal(ledger, name);
      if (from_all) {
        bool failed = false;
        for (const sne::ReplicaBalance& row : ledger.balance_from_all(name)) {
          if (row.ok) {
            std::cout << row.endpoint << ' ' << row.balance << '\n';
          } else {
            std::cerr << "snecoin: " << row.endpoint << ": " << row.error << '\n';
            failed = true;
          }
        }
        return failed ? 1 : 0;
      }
      std::cout << ledger.balance(name) << '\n';
      return 0;
    }
    if (command == "list") {
      if (!args.empty() || key_set || pubkey_set) {
        return fail_args("unexpected argument");
      }
      const CurlGlobal curl;
      sne::EtcdClient client{options};
      sne::Ledger ledger{client};
      report_illegal(ledger, "");
      for (const sne::AccountBalance& account : ledger.list()) {
        std::cout << account.name << ' ' << account.balance << '\n';
      }
      return 0;
    }
    if (command == "history") {
      if (key_set || pubkey_set) {
        return fail_args("history does not take a key");
      }
      int64_t limit = 20;
      if (args.size() > 1) {
        return fail_args("history takes at most one limit");
      }
      if (args.size() == 1) {
        const std::optional<int64_t> parsed = parse_amount(args[0], false);
        if (!parsed) {
          return fail_args("invalid history limit");
        }
        limit = *parsed;
      }
      const CurlGlobal curl;
      sne::EtcdClient client{options};
      sne::Ledger ledger{client};
      report_illegal(ledger, "");
      for (const std::string& line : ledger.history(limit)) {
        std::cout << line << '\n';
      }
      return 0;
    }
  } catch (const std::invalid_argument& error) {
    return fail_args(error.what());
  } catch (const std::exception& error) {
    std::cerr << "snecoin: " << error.what() << '\n';
    return 1;
  }

  std::cerr << "snecoin: unknown command '" << command << "'\n";
  print_usage(std::cerr);
  return 2;
}
