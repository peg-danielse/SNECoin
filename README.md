# SNECoin

SNECoin is a small ledger of account balances. The `sne` program is a C++ client. [etcd](https://github.com/etcd-io/etcd) stores the keys and copies them to every member with Raft.

`SPEC.md` is the full contract. This file is how to build and run it.

## Install

You need a C++17 compiler, CMake 3.16 or newer, and libcurl.

```bash
cmake -S . -B build
cmake --build build
./build/sne version
```

The binary is `build/sne`.

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

One client URL is enough. `sne` uses the first endpoint that answers. Extra URLs are only for failover, and for `balance --from-all`.

## Configuration

From this directory, `sne` reads `sne.conf`. If that file is absent it reads `~/.config/sne/config`. `--config FILE` picks a file explicitly. `--endpoints` replaces the URL list and leaves the credentials in place.

```
endpoints = http://127.0.0.1:2379
username = root
password = "secret"
ca_cert = /path/to/ca.pem
cert = /path/to/client.pem
key = /path/to/client-key.pem
```

Leave `username` and `password` unset when etcd auth is off. For `https://`, `ca_cert` selects a trust anchor; otherwise `sne` uses the system trust store. `cert` and `key` are a client certificate pair. `sne.conf` is gitignored.

## Commands

```bash
./build/sne open alice 100
./build/sne credit alice 25
./build/sne debit alice 10
./build/sne transfer alice bob 40
./build/sne balance alice
./build/sne balance alice --from-all
./build/sne list
./build/sne history 20
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

`--jobs N` runs N pairs at once. The default is one pair at a time. Each pair still does its two transfers in order. A later run with the same names fails on `open` until you `reset` the cluster or pass `--skip-init`.

## What `sne` says to etcd

`sne` talks to etcd's HTTP JSON gateway. It does not embed etcd and it does not run Raft itself.

| Call | Path |
| --- | --- |
| Read keys | `POST /v3/kv/range` |
| Commit a change | `POST /v3/kv/txn` |
| Log in | `POST /v3/auth/authenticate` |

Key and value bytes are standard base64. When `username` is set, `sne` sends the returned token on later calls in the `Authorization` header.

| Key | Value |
| --- | --- |
| `/sne/acct/<name>` | Balance, decimal integer |
| `/sne/meta/seq` | Last ledger sequence |
| `/sne/log/<seq>` | One JSON record, sequence padded to 20 digits |

A transfer is one transaction: compare the account revisions and the sequence, then write the new balances, the next sequence, and the log line together. If another client wins the sequence key, `sne` waits a few milliseconds and retries.

The etcd repository is [github.com/etcd-io/etcd](https://github.com/etcd-io/etcd). Its HTTP API is documented at [etcd.io/docs](https://etcd.io/docs/).
