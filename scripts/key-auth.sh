#!/usr/bin/env bash
# Open two accounts with different ed25519 keys. The owner can transfer.
# The other private key, and a missing key, are rejected and leave balances unchanged.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SNE="${SNE:-$ROOT/build/snecoin}"
if [[ ! -x $SNE ]]; then
  echo "key-auth: build snecoin first: cmake -S . -B build && cmake --build build" >&2
  exit 1
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
ssh-keygen -t ed25519 -f "$WORKDIR/alice" -N "" -C alice -q
ssh-keygen -t ed25519 -f "$WORKDIR/bob" -N "" -C bob -q

suffix="$(date +%s)"
alice="ka${suffix}"
bob="kb${suffix}"

"$SNE" open "$alice" 100 --pubkey "$WORKDIR/alice.pub"
"$SNE" open "$bob" 40 --pubkey "$WORKDIR/bob.pub"
"$SNE" transfer "$alice" "$bob" 15 --key "$WORKDIR/alice"

expect_reject() {
  local label=$1
  shift
  if "$@" >/tmp/snecoin-key-auth.out 2>/tmp/snecoin-key-auth.err; then
    echo "key-auth: ${label} was accepted" >&2
    exit 1
  fi
  echo "key-auth: ${label} rejected: $(cat /tmp/snecoin-key-auth.err)"
}

expect_reject "bob signing alice" "$SNE" transfer "$alice" "$bob" 5 --key "$WORKDIR/bob"
expect_reject "unsigned transfer" "$SNE" transfer "$alice" "$bob" 5
expect_reject "bob debiting alice" "$SNE" debit "$alice" 5 --key "$WORKDIR/bob"

alice_balance="$("$SNE" balance "$alice" 2>"$WORKDIR/first.err")"
bob_balance="$("$SNE" balance "$bob" 2>"$WORKDIR/bob-first.err")"
if [[ $alice_balance != 85 || $bob_balance != 55 ]]; then
  echo "key-auth: balances alice=$alice_balance bob=$bob_balance, expected 85 and 55" >&2
  exit 1
fi
if [[ -s $WORKDIR/first.err || -s $WORKDIR/bob-first.err ]]; then
  echo "key-auth: unexpected notice before tampering: $(cat "$WORKDIR/first.err" "$WORKDIR/bob-first.err")" >&2
  exit 1
fi

python3 - <<'PY'
import base64, json, urllib.request

def b64(text):
    return base64.b64encode(text.encode()).decode()

def call(path, obj):
    req = urllib.request.Request(
        "http://127.0.0.1:2379" + path,
        data=json.dumps(obj).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req) as resp:
        return json.load(resp)

call("/v3/kv/put", {"key": b64("/sne/meta/checked"), "value": b64("tampered")})
PY
tampered_balance="$("$SNE" balance "$alice" 2>"$WORKDIR/index.err")"
if [[ $tampered_balance != 85 ]]; then
  echo "key-auth: tampered index changed alice=$tampered_balance" >&2
  exit 1
fi
if ! grep -q "check index failed its signature check and was removed" "$WORKDIR/index.err"; then
  echo "key-auth: missing check-index notice: $(cat "$WORKDIR/index.err")" >&2
  exit 1
fi

# A direct etcd write can append a spend the client would have rejected.
# balance must name it, delete it, and put the 10 back. A log line with no
# revision is an older record and must stay.
bad_seq=9000000000000000001
old_seq=9000000000000000002
python3 - "$alice" "$bob" "$bad_seq" "$old_seq" <<'PY'
import base64, json, sys, urllib.request

alice, bob, bad_seq, old_seq = sys.argv[1:]
endpoint = "http://127.0.0.1:2379"

def call(path, obj):
    req = urllib.request.Request(
        endpoint + path,
        data=json.dumps(obj).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req) as resp:
        return json.load(resp)

def b64(text):
    return base64.b64encode(text.encode()).decode()

def put(key, value):
    call("/v3/kv/put", {"key": b64(key), "value": b64(value)})

sig = base64.b64encode(b"\x00" * 64).decode()
bad = {
    "seq": int(bad_seq),
    "op": "transfer",
    "from": alice,
    "to": bob,
    "amount": 10,
    "revision": 1,
    "sig": sig,
}
old = {
    "seq": int(old_seq),
    "op": "transfer",
    "from": alice,
    "to": bob,
    "amount": 10,
    "sig": sig,
}
put(f"/sne/acct/{alice}", "75")
put(f"/sne/acct/{bob}", "65")
put(f"/sne/log/{int(bad_seq):020d}", json.dumps(bad, separators=(",", ":")))
put(f"/sne/log/{int(old_seq):020d}", json.dumps(old, separators=(",", ":")))
PY

alice_balance="$("$SNE" balance "$alice" 2>"$WORKDIR/illegal.err")"
if [[ $alice_balance != 85 ]]; then
  echo "key-auth: illegal transfer left alice=$alice_balance, expected 85" >&2
  exit 1
fi
if ! grep -q "illegal transfer seq ${bad_seq} from ${alice} to ${bob} amount 10 removed" "$WORKDIR/illegal.err"; then
  echo "key-auth: missing illegal-transfer notice: $(cat "$WORKDIR/illegal.err")" >&2
  exit 1
fi
bob_balance="$("$SNE" balance "$bob" 2>"$WORKDIR/quiet.err")"
if [[ $bob_balance != 55 ]]; then
  echo "key-auth: illegal transfer left bob=$bob_balance, expected 55" >&2
  exit 1
fi
if [[ -s $WORKDIR/quiet.err ]]; then
  echo "key-auth: second inspect repeated the notice: $(cat "$WORKDIR/quiet.err")" >&2
  exit 1
fi
python3 - "$old_seq" "$bad_seq" <<'PY'
import base64, json, sys, urllib.request

old_seq, bad_seq = sys.argv[1:]

def call(path, obj):
    req = urllib.request.Request(
        "http://127.0.0.1:2379" + path,
        data=json.dumps(obj).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req) as resp:
        return json.load(resp)

def b64(text):
    return base64.b64encode(text.encode()).decode()

def present(seq):
    key = f"/sne/log/{int(seq):020d}"
    body = call("/v3/kv/range", {"key": b64(key)})
    return bool(body.get("kvs"))

if not present(old_seq):
    raise SystemExit("key-auth: unsigned older record was deleted")
if present(bad_seq):
    raise SystemExit("key-auth: illegal record is still in the log")
call("/v3/kv/deleterange", {"key": b64(f"/sne/log/{int(old_seq):020d}")})
PY
echo "key-auth: ok, $alice=85 $bob=55, illegal transfer removed"
