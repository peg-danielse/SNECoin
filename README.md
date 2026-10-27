# SNECoin

SNECoin is a small ledger of account balances for Security & Network Engineering. The `snecoin` program is a C++ client. [etcd](https://github.com/etcd-io/etcd) stores the keys and copies them to every member with Raft.

`SPEC.md` is the full contract. This file is how to build and run it.

## Install

You need a C++17 compiler, CMake 3.16 or newer, libcurl, and OpenSSL (`libcrypto`, used to sign with ed25519).

```bash
cmake -S . -B build
cmake --build build
./build/snecoin version
```

The binary is `build/snecoin`.

etcd is separate. Install it from the [etcd repository](https://github.com/etcd-io/etcd), or use a tree you already have. `scripts/etcd-cluster.sh` looks for `~/Documents/etcd/bin/etcd`. Set `ETCD_BIN` if the binary lives somewhere else.

## Local cluster

```bash
scripts/etcd-cluster.sh start
scripts/etcd-cluster.sh stop
scripts/etcd-cluster.sh reset
```

`start` launches three members on localhost:

| Member | Client URL |
| --- | --- |
| sne-1 | `http://127.0.0.1:2379` |
| sne-2 | `http://127.0.0.1:2381` |
| sne-3 | `http://127.0.0.1:2383` |

`stop` keeps the data in `/tmp/snecoin-etcd`. `reset` deletes that directory and bootstraps again.

One client URL is enough. `snecoin` uses the first endpoint that answers. Extra URLs are only for failover, and for `balance --from-all`.

## Configuration

From this directory, `snecoin` reads `sne.conf`. If that file is absent it reads `~/.config/sne/config`. `--config FILE` picks a file explicitly. `--endpoints` replaces the URL list and leaves the credentials in place.

```
endpoints = http://127.0.0.1:2379
username = root
password = "secret"
ca_cert = /path/to/ca.pem
cert = /path/to/client.pem
key = /path/to/client-key.pem
```

Leave `username` and `password` unset when etcd auth is off. For `https://`, `ca_cert` selects a trust anchor; otherwise `snecoin` uses the system trust store. `cert` and `key` are a client certificate pair. `sne.conf` is gitignored.

## Commands

```bash
ssh-keygen -t ed25519 -f alice_ed25519 -N ""
ssh-keygen -t ed25519 -f bob_ed25519 -N ""
./build/snecoin open alice 100 --pubkey alice_ed25519.pub
./build/snecoin open bob 40 --pubkey bob_ed25519.pub
./build/snecoin credit alice 25
./build/snecoin debit alice 10 --key alice_ed25519
./build/snecoin transfer alice bob 40 --key alice_ed25519
./build/snecoin balance alice
./build/snecoin balance bob
./build/snecoin balance alice --from-all
./build/snecoin list
./build/snecoin history 20
```

`open` stores one `ssh-ed25519` public key at `/sne/pub/<name>`. Bob is opened with his own key before Alice transfers to him. `debit` and `transfer` are rejected unless `--key` is the unencrypted private key for the source account. `credit` does not need a key. `scripts/key-auth.sh` generates two keys and checks that one cannot spend the other's balance.

A spend is signed over this text, including the source account's current mod revision:

```
snecoin/v1
<op>
<from>
<to>
<amount>
<mod_revision>
```

The log line stores that revision and the signature. `snecoin` checks the signature before it commits, so a spend signed with the wrong key never changes a balance.

If a debit or transfer was written straight into etcd with a signature that does not verify, `snecoin` notices the next time someone inspects the ledger or moves funds: `balance`, `list`, `history`, `credit`, `debit`, or `transfer`. It prints one stderr line, deletes that log record, and puts the amount back when the accounts still hold it. `balance` only looks at records that name that account. `list` and `history` look at every record. There is no background watcher. Older log lines that have no signature are left as they are. The notice looks like this:

```
snecoin: illegal transfer seq 12 from alice to bob amount 40 removed
```

Amounts are non-negative integers. `open` accepts zero. Every other amount is greater than zero. Account names match `[A-Za-z0-9_.-]{1,64}`.

Exit status `0` means the read or the commit succeeded. Exit status `2` means bad arguments. Anything else, including a rejected debit, exits `1` and prints one line to stderr.

## Load generation

`scripts/bipartite-transfers.sh` opens 1,000 accounts, `a000`–`a499` and `b000`–`b499`, then moves each left-hand balance to its pair and back.

```bash
scripts/etcd-cluster.sh start
scripts/bipartite-transfers.sh              # start each account at 100
scripts/bipartite-transfers.sh 250          # or another positive balance
scripts/bipartite-transfers.sh --skip-init  # accounts already exist
scripts/bipartite-transfers.sh --skip-init --jobs 16
```

`--jobs N` runs N pairs at once. The default is one pair at a time. Each pair still does its two transfers in order. The script keeps one ed25519 key at `/tmp/snecoin-etcd/load_ed25519` and signs every spend with it. A later run with the same names fails on `open` until you `reset` the cluster or pass `--skip-init`. `--skip-init` only works for accounts that were opened with that key.

`scripts/key-auth.sh` is the smaller check. It opens two accounts with different keys, rejects a spend signed by the other key, then writes one bad signature directly into etcd and checks that the next `balance` removes it and restores both balances.

## What `snecoin` says to etcd

`snecoin` talks to etcd's HTTP JSON gateway. It does not embed etcd and it does not run Raft itself.

| Call | Path |
| --- | --- |
| Read keys | `POST /v3/kv/range` |
| Commit a change | `POST /v3/kv/txn` |
| Log in | `POST /v3/auth/authenticate` |

Key and value bytes are standard base64. When `username` is set, `snecoin` sends the returned token on later calls in the `Authorization` header.

| Key | Value |
| --- | --- |
| `/sne/acct/<name>` | Balance, decimal integer |
| `/sne/pub/<name>` | One `ssh-ed25519` public key |
| `/sne/meta/seq` | Last ledger sequence |
| `/sne/log/<seq>` | One JSON record, sequence padded to 20 digits |

A transfer is one transaction: compare the account revisions, the public key, and the sequence, then write the new balances, the next sequence, and the signed log line together. If another client wins the sequence key, `snecoin` waits a few milliseconds and retries. Removing an illegal record is also one transaction: delete `/sne/log/<seq>` and reverse the balances. The sequence counter stays where it is.

The etcd repository is [github.com/etcd-io/etcd](https://github.com/etcd-io/etcd). Its HTTP API is documented at [etcd.io/docs](https://etcd.io/docs/).
