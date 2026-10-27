# SNECoin

Small ledger of user account balances. etcd 3.7 is the storage and replication layer. A C++ client reads and writes the keyspace. etcd's Raft log is what copies each balance onto every member.

This document is the contract for the client.

## Roles

| Piece | Responsibility |
| --- | --- |
| etcd member | Stores the keyspace and replicates it with Raft |
| `snecoin` | Submits balance changes and reads them back |

One `snecoin` process is enough. It is a client. Running more clients does not add replicas. Replicas are etcd members. SNE is Security & Network Engineering.

The etcd tree is `~/Documents/etcd` (etcd 3.7.0). The binaries are `~/Documents/etcd/bin/etcd`, `etcdctl`, and `etcdutl`. The client speaks HTTP to member client URLs. It does not spawn etcd.

## Cluster

Development cluster is three members on localhost. `--enable-grpc-gateway` stays at its default (`true`), so the HTTP JSON API is served on the client URL.

| Member | Client URL | Peer URL |
| --- | --- | --- |
| sne-1 | `http://127.0.0.1:2379` | `http://127.0.0.1:2380` |
| sne-2 | `http://127.0.0.1:2381` | `http://127.0.0.1:2382` |
| sne-3 | `http://127.0.0.1:2383` | `http://127.0.0.1:2384` |

Static bootstrap:

- `--initial-cluster-token snecoin`
- `--initial-cluster-state new`
- `--initial-cluster sne-1=http://127.0.0.1:2380,sne-2=http://127.0.0.1:2382,sne-3=http://127.0.0.1:2384`
- A separate `--data-dir` per member, outside the source tree

`scripts/etcd-cluster.sh start` launches these members. `stop` kills them and keeps the data. `reset` deletes `/tmp/snecoin-etcd` and bootstraps again.

A balance write is finished when a majority has committed it. After that, a linearizable read of the same key on any member returns that balance. Checking replication means reading one account through each client URL and comparing the values.

A call uses the first configured member that answers. A committed key is readable from the other members.

## Client configuration

`snecoin` reads a config file when one is present:

1. `--config FILE`
2. `./sne.conf`
3. `$XDG_CONFIG_HOME/sne/config`, or `~/.config/sne/config` when `XDG_CONFIG_HOME` is unset

```
endpoints = http://127.0.0.1:2379,http://127.0.0.1:2381,http://127.0.0.1:2383
username = root
password = secret
ca_cert = /path/to/ca.pem
cert = /path/to/client.pem
key = /path/to/client-key.pem
```

`endpoints` is a comma-separated list of client URLs. `https://` URLs are verified with `ca_cert` when that is set, and with the system trust store otherwise. `cert` and `key` are a client certificate pair. `username` and `password` are an etcd user. `snecoin` exchanges them for a token at `/v3/auth/authenticate` and sends that token in the `Authorization` header. `--endpoints` replaces `endpoints` from the file and leaves the credentials in place.

With no file, `snecoin` uses the three local client URLs above and does not authenticate. The etcd user must already be allowed to read and write `/sne/`. `snecoin` does not create users or roles. `./sne.conf` is the local file and stays out of git.

## Key layout

All keys are UTF-8. Account names match `[A-Za-z0-9_.-]{1,64}`.

| Key | Value |
| --- | --- |
| `/sne/acct/<name>` | Balance, ASCII base-10 `int64` |
| `/sne/pub/<name>` | Account public key, one `ssh-ed25519` line |
| `/sne/meta/seq` | Last assigned ledger sequence, ASCII base-10 integer |
| `/sne/log/<seq>` | One ledger record, JSON |

`<seq>` is the sequence number zero-padded to 20 digits, so key order and numeric order match. The first record uses sequence 1.

Balances are integer minor units. The client rejects a result outside `[0, 2^63-1]`.

Ledger record:

```json
{"seq":1,"op":"transfer","from":"alice","to":"bob","amount":25}
```

`op` is `open`, `credit`, `debit`, or `transfer`. `from` is present for `debit` and `transfer`. `to` is present for `open`, `credit`, and `transfer`. `amount` is the positive quantity applied by that operation. `open` stores the initial balance in `amount`. A spend also stores `revision` (the source account mod revision that was signed) and `sig` (standard base64 of the 64-byte signature). Records written before `revision` existed are left in place.

## Operations

Each mutation is one etcd transaction: compare, then write the new balance, the new sequence, and the log record together. A reader never sees a balance move without its log line.

| Command | Effect |
| --- | --- |
| `open <name> <amount> --pubkey FILE` | Create the account at `amount` and store its `ssh-ed25519` public key |
| `balance <name>` | Print the balance |
| `credit <name> <amount>` | Add `amount` |
| `debit <name> <amount> --key FILE` | Subtract `amount` when the balance covers it and the private key matches |
| `transfer <from> <to> <amount> --key FILE` | Move `amount` when the source covers it and the private key matches |
| `list` | Print every account and balance |
| `history [n]` | Print the latest `n` log records, default 20 |
| `balance <name> --from-all` | Read that account from every endpoint |

`amount` is a base-10 integer greater than zero, except `open`, which accepts zero.

`open` requires the account key to be absent and stores `/sne/pub/<name>`. `credit`, `debit`, and `transfer` require the named accounts to exist. `transfer` requires two different accounts.

A `debit` or `transfer` is rejected unless `--key` holds the unencrypted `ssh-ed25519` private key for `/sne/pub/<from>`. `snecoin` signs this text, then checks the signature before the etcd transaction:

```
snecoin/v1
<op>
<from>
<to>
<amount>
<mod_revision>
```

`<mod_revision>` is the source account's current mod revision, so a captured signature fails the compare once that account changes. The transaction also compares the public-key key. A missing public key, a missing `--key`, or a signature that does not verify leaves the balances unchanged. `credit` and receiving a transfer do not require the recipient's key. The log record for a spend stores that revision and the signature.

`balance`, `balance --from-all`, `list`, `history`, `credit`, `debit`, and `transfer` read the log before they do their own work. A `debit` or `transfer` that includes `revision` and whose `sig` does not verify against `/sne/pub/<from>` is illegal. `snecoin` prints one stderr line naming the sequence, the accounts, and the amount, then deletes that log key and reverses the balance change in one etcd transaction. `/sne/meta/seq` is not moved backward. `balance` and a one-account command only consider records that name that account. `list` and `history` consider every record. If the destination no longer holds the full amount, only the amount still there is returned to the source. There is no background watcher.

## Concurrency

etcd compares values as byte strings. The client therefore applies numeric rules itself, then commits with a compare-and-swap on mod revision so two writers cannot both succeed from the same snapshot.

1. Range-read the account keys and `/sne/meta/seq`.
2. Apply the operation in memory. Stop if an account is missing, the source balance is too small, or the sum overflows.
3. Transaction:
   - Compare each existing key's `MOD` revision to the revision just read.
   - Compare `CREATE` revision `0` when `/sne/meta/seq` or a new account key is absent.
   - On success, put the updated balances, put `seq + 1`, and put `/sne/log/<padded seq>`.
4. On a failed compare, wait a few milliseconds and retry, up to 64 attempts. The pause lets several clients commit through the same sequence key.

Range reads stay linearizable (`serializable` left false), which is the etcd default.

## HTTP API

`POST` JSON to `<client-url>/v3/kv/range` and `<client-url>/v3/kv/txn`. etcd 3.7's gateway uses proto JSON names (`mod_revision`, `request_put`, `range_end`). `bytes` fields are standard base64. `int64` fields are decimal strings.

Prefix reads set `range_end` to the etcd prefix successor: increment the last byte that is less than `0xff`, and drop the bytes after it.

Account read:

```json
{"key":"<base64 /sne/acct/alice>"}
```

Transfer transaction, shape only:

```json
{
  "compare": [
    {"key":"<from>","target":"MOD","result":"EQUAL","mod_revision":"<rev>"},
    {"key":"<to>","target":"MOD","result":"EQUAL","mod_revision":"<rev>"},
    {"key":"<seq>","target":"MOD","result":"EQUAL","mod_revision":"<rev>"}
  ],
  "success": [
    {"request_put": {"key":"<from>","value":"<new balance>"}},
    {"request_put": {"key":"<to>","value":"<new balance>"}},
    {"request_put": {"key":"<seq>","value":"<next seq>"}},
    {"request_put": {"key":"<log>","value":"<record json>"}}
  ]
}
```

A missing sequence key uses `target` `CREATE`, `create_revision` `"0"`, instead of a `MOD` compare.

## Process shape

C++17. CMake target `snecoin`. libcurl issues the HTTP calls. The modules to add on top of this template:

| Module | Job |
| --- | --- |
| `src/main.cpp` | Parse `--config`, `--endpoints`, and the command |
| `src/config.cpp` | Load the endpoint list and credentials |
| `src/etcd_client.cpp` | Base64, JSON, authentication, range, and txn calls |
| `src/ledger.cpp` | Key layout, arithmetic, retry loop |

Exit status `0` means the operation committed or the read succeeded. Exit status `2` means bad arguments. Any other failure, including a rejected debit or a lost compare after 64 attempts, exits `1` and prints one line to stderr.

## Out of scope

Watches, compaction policy, wallets, and any consensus code inside `snecoin`. etcd already orders and replicates the keys.
