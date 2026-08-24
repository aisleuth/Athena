#!/usr/bin/env bash
# Fixed-position search benchmark and accuracy gate.
#
#   tools/bench.sh                    run and print a table
#   tools/bench.sh --save FILE        also snapshot "name bestmove score" lines
#   tools/bench.sh --check FILE       fail (exit 1) if bestmove/score drifted
#   tools/bench.sh --check            check the versioned reference snapshot
#
# Nodes and time are informational; the gate compares only bestmove and score
# so pure speedups pass and accuracy changes are surfaced.
set -u
cd "$(dirname "$0")/.."
ENGINE=${ENGINE:-./build/src/athena}
POSITIONS=tools/bench_positions.txt
MODE=${1:-}
FILE=${2:-tools/bench_expected.txt}
LIMIT=${BENCH_TIMEOUT:-120}

[ -x "$ENGINE" ] || { echo "engine not built: $ENGINE" >&2; exit 2; }

results=""
total_nodes=0
total_ms=0
printf '%-18s %-9s %10s %12s %8s\n' name bestmove score nodes ms
while IFS='|' read -r name poscmd depth; do
  case "$name" in ''|'#'*) continue;; esac
  tmpdir=$(mktemp -d)
  out="$tmpdir/output"
  fifo="$tmpdir/input"
  mkfifo "$fifo"
  "$ENGINE" < "$fifo" > "$out" 2>/dev/null &
  pid=$!
  exec 3> "$fifo"
  printf '%s\ngo depth %s\n' "$poscmd" "$depth" >&3
  waited=0
  until grep -q '^bestmove' "$out" 2>/dev/null; do
    sleep 0.2
    waited=$((waited + 1))
    if [ $waited -ge $((LIMIT * 5)) ] || ! kill -0 $pid 2>/dev/null; then break; fi
  done
  printf 'quit\n' >&3 2>/dev/null
  exec 3>&-
  wait $pid 2>/dev/null
  rm -f "$fifo"
  info=$(awk '/^info depth/{last=$0} END{print last}' "$out")
  best=$(awk '/^bestmove/{print $2; exit}' "$out")
  score=$(printf '%s' "$info" | sed -nE 's/.*score (cp|mate) (-?[0-9]+).*/\1 \2/p')
  nodes=$(printf '%s' "$info" | sed -nE 's/.* nodes ([0-9]+).*/\1/p')
  ms=$(printf '%s' "$info" | sed -nE 's/.* time ([0-9]+).*/\1/p')
  rm -f "$out"
  rmdir "$tmpdir"
  printf '%-18s %-9s %10s %12s %8s\n' "$name" "${best:-?}" "${score:-?}" "${nodes:-?}" "${ms:-?}"
  results="${results}${name} ${best:-?} ${score:-?}\n"
  total_nodes=$((total_nodes + ${nodes:-0}))
  total_ms=$((total_ms + ${ms:-0}))
done < "$POSITIONS"
echo "total: ${total_nodes} nodes, ${total_ms} ms"

case "$MODE" in
  --save)
    printf '%b' "$results" > "$FILE"
    echo "snapshot saved to $FILE" ;;
  --check)
    if printf '%b' "$results" | diff -u "$FILE" - ; then
      echo "bench: OK (matches $FILE)"
    else
      echo "bench: DRIFT vs $FILE" >&2
      exit 1
    fi ;;
esac
