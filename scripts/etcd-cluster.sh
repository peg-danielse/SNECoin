#!/usr/bin/env bash
# Start, stop, or reset the local three-member etcd cluster described in SPEC.md.
# Data lives in /tmp/snecoin-etcd and is kept across stop/start.

set -euo pipefail

ETCD_BIN="${ETCD_BIN:-$HOME/Documents/etcd/bin/etcd}"
BASE="${SNE_ETCD_DATA:-/tmp/snecoin-etcd}"
CLUSTER="sne-1=http://127.0.0.1:2380,sne-2=http://127.0.0.1:2382,sne-3=http://127.0.0.1:2384"

members=(
  "sne-1 2379 2380"
  "sne-2 2381 2382"
  "sne-3 2383 2384"
)

usage() {
  echo "Usage: scripts/etcd-cluster.sh start|stop|reset" >&2
  exit 2
}

member_alive() {
  local name=$1
  local pidfile="$BASE/$name.pid"
  [[ -f $pidfile ]] || return 1
  local pid
  pid=$(<"$pidfile")
  kill -0 "$pid" 2>/dev/null
}

running_count() {
  local count=0
  local spec name
  for spec in "${members[@]}"; do
    read -r name _ _ <<<"$spec"
    if member_alive "$name"; then
      count=$((count + 1))
    fi
  done
  echo "$count"
}

stop_members() {
  local spec name pidfile pid
  for spec in "${members[@]}"; do
    read -r name _ _ <<<"$spec"
    pidfile="$BASE/$name.pid"
    if [[ ! -f $pidfile ]]; then
      continue
    fi
    pid=$(<"$pidfile")
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done
  for spec in "${members[@]}"; do
    read -r name _ _ <<<"$spec"
    pidfile="$BASE/$name.pid"
    if [[ ! -f $pidfile ]]; then
      continue
    fi
    pid=$(<"$pidfile")
    for _ in $(seq 1 50); do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
  done
}

wait_healthy() {
  local port=$1
  local _
  for _ in $(seq 1 50); do
    if curl -sf -m 1 "http://127.0.0.1:${port}/health" >/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "etcd-cluster: member on port $port did not become healthy" >&2
  if [[ -f $BASE/sne-1.log ]]; then
    tail -n 20 "$BASE"/*.log >&2 || true
  fi
  return 1
}

start_members() {
  if [[ ! -x $ETCD_BIN ]]; then
    echo "etcd-cluster: etcd binary not found at $ETCD_BIN" >&2
    exit 1
  fi
  mkdir -p "$BASE"
  local any_data=0 any_empty=0 spec name client peer state
  for spec in "${members[@]}"; do
    read -r name _ _ <<<"$spec"
    if [[ -d $BASE/$name/member ]]; then
      any_data=1
    else
      any_empty=1
    fi
  done
  if [[ $any_data -eq 1 && $any_empty -eq 1 ]]; then
    echo "etcd-cluster: data directories disagree; run reset" >&2
    exit 1
  fi
  state=new
  if [[ $any_data -eq 1 ]]; then
    state=existing
  fi
  for spec in "${members[@]}"; do
    read -r name client peer <<<"$spec"
    "$ETCD_BIN" \
      --name "$name" \
      --data-dir "$BASE/$name" \
      --listen-client-urls "http://127.0.0.1:$client" \
      --advertise-client-urls "http://127.0.0.1:$client" \
      --listen-peer-urls "http://127.0.0.1:$peer" \
      --initial-advertise-peer-urls "http://127.0.0.1:$peer" \
      --initial-cluster "$CLUSTER" \
      --initial-cluster-token snecoin \
      --initial-cluster-state "$state" \
      --log-level error \
      >"$BASE/$name.log" 2>&1 &
    echo $! >"$BASE/$name.pid"
  done
  for spec in "${members[@]}"; do
    read -r _ client _ <<<"$spec"
    wait_healthy "$client"
  done
  echo "etcd-cluster: ready ($state) $BASE"
}

case "${1:-}" in
  start)
    count=$(running_count)
    if [[ $count -eq ${#members[@]} ]]; then
      echo "etcd-cluster: already running"
      exit 0
    fi
    if [[ $count -ne 0 ]]; then
      echo "etcd-cluster: only $count of ${#members[@]} members are running; run stop or reset" >&2
      exit 1
    fi
    start_members
    ;;
  stop)
    stop_members
    echo "etcd-cluster: stopped"
    ;;
  reset)
    stop_members
    rm -rf "$BASE"
    start_members
    ;;
  *)
    usage
    ;;
esac
