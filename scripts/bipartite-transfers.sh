#!/usr/bin/env bash
# Open 1000 accounts in two sets of 500. Each aNNN sends its whole balance to
# bNNN, then bNNN sends that same amount back.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SNE="${SNE:-$ROOT/build/snecoin}"
INITIAL=100
SKIP_INIT=0
JOBS=1
PAIRS=500

usage() {
  echo "Usage: scripts/bipartite-transfers.sh [--skip-init] [--jobs N] [initial-balance]"
}

while [[ $# -gt 0 ]]; do
  case $1 in
    --skip-init)
      SKIP_INIT=1
      shift
      ;;
    --jobs)
      if [[ $# -lt 2 ]]; then
        echo "bipartite: --jobs requires a number" >&2
        exit 2
      fi
      JOBS=$2
      shift 2
      ;;
    --jobs=*)
      JOBS=${1#--jobs=}
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "bipartite: unknown option '$1'" >&2
      usage >&2
      exit 2
      ;;
    *)
      if [[ -n ${POSITIONAL:-} ]]; then
        usage >&2
        exit 2
      fi
      POSITIONAL=$1
      shift
      ;;
  esac
done
if [[ $# -gt 0 ]]; then
  usage >&2
  exit 2
fi

if [[ ! -x $SNE ]]; then
  echo "bipartite: build snecoin first: cmake -S . -B build && cmake --build build" >&2
  exit 1
fi
LOAD_KEY="${SNE_LOAD_KEY:-/tmp/snecoin-etcd/load_ed25519}"
mkdir -p "$(dirname "$LOAD_KEY")"
if [[ ! -f $LOAD_KEY || ! -f ${LOAD_KEY}.pub ]]; then
  ssh-keygen -t ed25519 -f "$LOAD_KEY" -N "" -C snecoin-load -q
fi
if [[ $SKIP_INIT -eq 0 ]]; then
  INITIAL="${POSITIONAL:-100}"
  if ! [[ $INITIAL =~ ^[1-9][0-9]*$ ]]; then
    echo "bipartite: initial balance must be a positive integer" >&2
    exit 2
  fi
elif [[ -n ${POSITIONAL:-} ]]; then
  echo "bipartite: --skip-init does not take a balance" >&2
  exit 2
fi
if ! [[ $JOBS =~ ^[1-9][0-9]*$ ]]; then
  echo "bipartite: --jobs must be a positive integer" >&2
  exit 2
fi

name() {
  printf '%s%03d' "$1" "$2"
}

failed=0
pids=()
done_count=0

phase=opened

reap_one() {
  if ! wait "$1"; then
    failed=1
  fi
  done_count=$((done_count + 1))
  if ((done_count % 50 == 0 || done_count == PAIRS)); then
    echo "bipartite: $phase $done_count / $PAIRS pairs"
  fi
}

launch() {
  "$@" &
  pids+=("$!")
  if ((${#pids[@]} >= JOBS)); then
    reap_one "${pids[0]}"
    pids=("${pids[@]:1}")
  fi
}

drain() {
  local pid
  if ((${#pids[@]} == 0)); then
    return
  fi
  for pid in "${pids[@]}"; do
    reap_one "$pid"
  done
  pids=()
}

open_pair() {
  local i=$1
  "$SNE" open "$(name a "$i")" "$INITIAL" --pubkey "${LOAD_KEY}.pub"
  "$SNE" open "$(name b "$i")" "$INITIAL" --pubkey "${LOAD_KEY}.pub"
}

transfer_pair() {
  local i=$1
  local from to amount
  from="$(name a "$i")"
  to="$(name b "$i")"
  amount="$("$SNE" balance "$from")"
  "$SNE" transfer "$from" "$to" "$amount" --key "$LOAD_KEY"
  "$SNE" transfer "$to" "$from" "$amount" --key "$LOAD_KEY"
}

if [[ $SKIP_INIT -eq 0 ]]; then
  echo "bipartite: opening $((PAIRS * 2)) accounts at balance $INITIAL ($JOBS at a time)"
  for ((i = 0; i < PAIRS; i++)); do
    launch open_pair "$i"
  done
  drain
  if ((failed != 0)); then
    echo "bipartite: one or more accounts failed to open" >&2
    exit 1
  fi
else
  echo "bipartite: skipping account creation"
fi

before_left="$("$SNE" balance "$(name a 0)")"
before_right="$("$SNE" balance "$(name b 0)")"
done_count=0
phase=transferred
echo "bipartite: moving each aNNN balance to bNNN and back ($JOBS at a time)"
for ((i = 0; i < PAIRS; i++)); do
  launch transfer_pair "$i"
done
drain
if ((failed != 0)); then
  echo "bipartite: one or more pairs failed" >&2
  exit 1
fi

left="$("$SNE" balance "$(name a 0)")"
right="$("$SNE" balance "$(name b 0)")"
if [[ $left != "$before_left" || $right != "$before_right" ]]; then
  echo "bipartite: a000=$left b000=$right, expected $before_left and $before_right" >&2
  exit 1
fi
echo "bipartite: done, 500 pairs moved $before_left each way"
