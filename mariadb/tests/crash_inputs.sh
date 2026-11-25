#!/usr/bin/env bash
# Hostile arguments and files must end in an SQL error or NULL, never in a crash of the server or a leaked descriptor:
# NULL, negative and huge arguments of every UDF, a forged filter, and damaged manifests (fixed and fuzzed).
#   mariadb/tests/crash_inputs.sh
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=crash_inputs
DB=crash
SIFT=$SIFT_MDB_ROOT/data/.sift
PIDFILE=$SIFT_MDB_ROOT/started.pid

PID=$(cat "$PIDFILE")
alive() { kill -0 "$PID" 2>/dev/null && "$SRV" sql -e 'SELECT 1' >/dev/null 2>&1; }
revive() {   # a server that crashed is replaced by a fresh one of the same data directory
  alive && return
  while kill -0 "$PID" 2>/dev/null; do sleep 0.2; done
  "$SRV" start >/dev/null 2>&1; PID=$(cat "$PIDFILE")
}
fds() { ls "/proc/$PID/fd" | wc -l; }

# probe name statement regex: the answer matches, and the server is still the one that was running
probe() {
  local out; out=$(sqlerr "$2")
  if alive; then check_match "$1" "$3" "$out"; else check "$1: the server stays up" alive dead; revive; fi
}
ERR='^ERROR .*sift:'
NULL='^NULL$'

fresh_db ci cm cn
sql "CREATE TABLE ci (id BIGINT PRIMARY KEY, body TEXT); INSERT INTO ci VALUES (1, 'alpha beta'), (2, 'beta gamma')"
sql "CALL sift_attach('ci', 'ci', 'id', 't.body', 8)" >/dev/null
sync ci

probe "control: a top-k answers" "SELECT sift_topk('ci', 'beta', 5)" '^\[\{"id":'
probe "sift_topk with k NULL" "SELECT sift_topk('ci', 'a', NULL)" "$NULL"
probe "sift_topk with k negative" "SELECT sift_topk('ci', 'a', -5)" "$NULL"
probe "sift_topk with k zero" "SELECT sift_topk('ci', 'a', 0)" "$NULL"
probe "sift_topk with k beyond the limit" "SELECT sift_topk('ci', 'a', 2000000000)" "$ERR"
probe "sift_topk with k that wraps an int" "SELECT sift_topk('ci', 'a', 4294967297)" "$ERR"
probe "sift_ping with NULL" "SELECT sift_ping(NULL)" "$NULL"
probe "sift_ping with a negative k" "SELECT sift_ping(-1)" "$ERR"
probe "sift_ping with k beyond the limit" "SELECT sift_ping(2000000000)" "$ERR"
probe "control: sift_ping answers" "SELECT sift_ping(2)" '^\[\{"id":1000'
probe "sift_drain with seq NULL" "SELECT sift_drain('ci', NULL, 'I', 1, 'a')" "$ERR"
probe "sift_drain with key NULL" "SELECT sift_drain('ci', 1, 'I', NULL, 'a')" "$ERR"
probe "sift_drain with the index NULL" "SELECT sift_drain(NULL, 1, 'I', 1, 'a')" "$ERR"
probe "sift_load with watermark and key NULL" "SELECT sift_load('ci', NULL, NULL, 'a')" "$ERR"
probe "sift_load with watermark NULL" "SELECT sift_load('ci', NULL, 1, 'a')" "$ERR"
probe "sift_load of no rows with watermark NULL" "SELECT sift_load('ci', NULL, id, body) FROM ci WHERE id < 0" "$ERR"
probe "sift_create with a NULL cfg" "SELECT sift_create('cn', NULL)" "^(NULL|ERROR .*sift:)"
probe "sift_create with a NULL name" "SELECT sift_create(NULL, 'k1=1.2')" "^(NULL|ERROR .*sift:)"
probe "sift_tokens with a NULL text" "SELECT sift_tokens('ci', NULL)" "$NULL"
probe "sift_matches with a NULL query" "SELECT sift_matches('ci', NULL)" "$NULL"
probe "control: sift_merge with a NULL mode" "SELECT sift_merge('ci', NULL)" '^\{"segments"'
probe "control: an empty idset" "SELECT LENGTH(sift_idset(NULL))" "^16$"
FORGED="CAST(CONCAT('SIFTIDS1', UNHEX('0000000000000020')) AS BINARY)"
probe "a filter whose count wraps its length" "SELECT sift_count('ci', 'beta', $FORGED)" "$ERR"
probe "a filter that claims more keys than it holds" "SELECT sift_count('ci', 'beta', CAST(CONCAT('SIFTIDS1', UNHEX('0900000000000000')) AS BINARY))" "$ERR"
probe "control: the server answers" "SELECT 1" "^1$"

# damaged manifests: the index is opened from a copy of the directory under a name the server has not seen
MAN=$SIFT/cm/MANIFEST
patch() {   # offset hex-bytes
  printf '%b' "$(sed 's/../\\x&/g' <<<"$2")" | dd of="$MAN" bs=1 seek="$1" conv=notrunc 2>/dev/null
}
opens() {   # name regex patch-offset patch-bytes
  rm -rf "$SIFT/cm"; cp -r "$SIFT/ci" "$SIFT/cm"
  patch "$3" "$4"
  local before after i; before=$(fds)
  for i in 1 2 3 4 5 6 7 8; do sqlerr "SELECT sift_stats('cm')" >/dev/null; done
  if alive; then
    after=$(fds)
    probe "$1" "SELECT sift_stats('cm')" "$2"
    check "$1: no descriptor is left open" "$before" "$after"
  else
    check "$1: the server stays up" alive dead; revive
  fi
}
opens "a manifest whose cfg_len is 4 GiB - 1" "$ERR" 72 ffffffff
opens "a manifest whose cfg_len is 2 GiB" "$ERR" 72 ffffff7f
opens "a manifest whose cfg_len is 2 MiB" "$ERR" 72 00002000
opens "a manifest with more segments than it can hold" "$ERR" 64 79000000
opens "a manifest with 4 G segments" "$ERR" 64 ffffffff
opens "a manifest with a truncated header" "$ERR" 0 4142

rm -rf "$SIFT/cm"
size=$(stat -c %s "$SIFT/ci/MANIFEST")
RANDOM=98
for round in $(seq 1 40); do
  rm -rf "$SIFT/cm$round"; cp -r "$SIFT/ci" "$SIFT/cm$round"
  MAN=$SIFT/cm$round/MANIFEST
  for _ in 1 2 3; do patch $((RANDOM % size)) "$(printf '%02x' $((RANDOM % 256)))"; done
done
for round in $(seq 1 40); do
  out=$(sqlerr "SELECT sift_stats('cm$round')")
  if ! alive; then check "fuzzed manifest $round: the server stays up" alive dead; revive; continue; fi
  [[ $out =~ ^(ERROR\ .*sift:|\{) ]] || check "fuzzed manifest $round answers cleanly" "an error or stats" "$out"
done
check "fuzzed manifests: the server survived all of them" 1 "$(alive && echo 1 || echo 0)"
rm -rf "$SIFT"/cm*
finish
