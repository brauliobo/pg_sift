#!/usr/bin/env bash
# Live segments: small drains make small segments and never rewrite the index, the background merge keeps their number
# down, a full merge streams (bounded memory) and drops the deleted documents, and a kill -9 at any point of a merge leaves
# the last published state (the manifest is replaced atomically).
#   mariadb/tests/live.sh        ROWS (default 600000) documents for the merge tests
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=live
ROWS=${ROWS:-600000}
anon() { awk '/RssAnon/ {print $2}' "/proc/$(cat "$SIFT_MDB_ROOT/started.pid")/status"; }
# the anonymous memory of the server at its highest while the statement runs, in kB above where it started
peak_growth() {
  local pid start; pid=$(cat "$SIFT_MDB_ROOT/started.pid"); start=$(anon)
  ( while kill -0 "$pid" 2>/dev/null; do awk '/RssAnon/ {print $2}' "/proc/$pid/status"; sleep 0.01; done >"$TMP/anon" ) &
  local s=$!
  sql "$1" >/dev/null
  sleep 0.1; kill "$s"
  echo $(( $(sort -n "$TMP/anon" | tail -1) - start ))
}
TMP=$(mktemp -d /tmp/sift-mdb-live.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
files() { ls "$SIFT_MDB_ROOT/data/.sift/lv"; }

fresh_db lv
sql "CREATE TABLE lv (id BIGINT PRIMARY KEY, body TEXT)"
sql "INSERT INTO lv SELECT seq, CONCAT('base word', seq % 997, ' ', ELT(1 + seq % 7, 'red', 'green', 'blue', 'cyan', 'plum', 'gold', 'teal'), ' common') FROM seq_1_to_200"
sql "CALL sift_attach_opts('lv', 'lv', 'id', 't.body', '{\"dpp\":32,\"merge_fanin\":6}')" >/dev/null
sync lv
check "one segment after the first drain" 1 "$(stat segments lv)"

# small drains: each makes its own small segment, the segments already there are not rewritten
first=$(ls "$SIFT_MDB_ROOT"/data/.sift/lv/seg-*.sift | head -1)
sum0=$(cksum <"$first")
for i in 1 2 3; do
  sql "INSERT INTO lv VALUES (70000$i, 'extra batch$i common'), (80000$i, 'extra again$i common')"
  sql "DELETE FROM lv WHERE id = $i"
  sync lv
done
check "three small drains made small segments" "4" "$(stat segments lv)"
check "the first segment was not rewritten" "$sum0" "$(cksum <"$first")"
check "deletes are counted as dead documents" 3 "$(stat dead lv)"
check "results across the segments" "$(sql "SELECT COUNT(*) FROM lv WHERE body LIKE '%common%'")" "$(cnt common lv)"

# the background merge: past the fan-in the small segments are merged without a call
for i in 4 5 6 7 8 9; do sql "INSERT INTO lv VALUES (90000$i, 'more batch$i common')"; sync lv; done
for _ in $(seq 1 40); do [[ $(stat segments lv) -le 6 ]] && break; sleep 0.25; done
check "the background merge keeps the number of segments within the fan-in" 1 "$([[ $(stat segments lv) -le 6 ]] && echo 1)"
check "the merge counter moved" 1 "$([[ $(stat merges lv) -ge 1 ]] && echo 1)"
check "and the answers did not change" "$(sql "SELECT COUNT(*) FROM lv WHERE body LIKE '%common%'")" "$(cnt common lv)"

# a large index: bounded memory for the full merge
sql "INSERT INTO lv SELECT 10000 + seq, CONCAT('doc', seq % 5003, ' ', ELT(1 + seq % 7, 'red', 'green', 'blue', 'cyan', 'plum', 'gold', 'teal'), ' ', ELT(1 + seq % 11, 'alpha', 'beta', 'gamma', 'delta', 'omega', 'sigma', 'kappa', 'theta', 'iota', 'zeta', 'lambda'), ' w', seq % 331, ' w', seq % 1009, ' w', seq % 4001, ' w', seq % 9973, ' common filler text of the document') FROM seq_1_to_$ROWS"
sync lv
sql "DELETE FROM lv WHERE id % 10 = 0"
sync lv
want=$(sql "SELECT COUNT(*) FROM lv WHERE body LIKE '%common%'")
check "the large index answers" "$want" "$(cnt common lv)"
bytes=$(stat segments lv >/dev/null; sql "SELECT JSON_VALUE(sift_stats('lv'), '\$.bytes')")
top0=$(top 'red AND alpha AND common' 10 lv)
"$SRV" restart >/dev/null
sql "SELECT COUNT(*) FROM lv" >/dev/null
cnt common lv >/dev/null                         # the index is open: what the merge allocates is measured
t0=$(date +%s.%N)
growth=$(peak_growth "SELECT sift_merge('lv', 'full')")
echo "full merge of $bytes bytes in $(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}') s, anonymous memory +$growth kB"
check "full merge: one segment" 1 "$(stat segments lv)"
check "full merge: no dead document left" 0 "$(stat dead lv)"
check "full merge: the answers did not change" "$want" "$(cnt common lv)"
check "full merge: the ranking did not change" "$top0" "$(top 'red AND alpha AND common' 10 lv)"
# the postings are streamed; what stays in memory is the map of documents (some 50 bytes each)
docs=$(sql "SELECT JSON_VALUE(sift_stats('lv'), '\$.live')")
check "full merge: memory grows with the documents, not the postings ($growth kB for $docs documents)" 1 "$([[ $((growth * 1024)) -lt $((docs * 100)) ]] && echo 1)"

# kill -9 during a merge: the published state is intact, nothing half-written is read
n=0
for delay in 0.05 0.15 0.3; do
  n=$((n + 1)); base=$((50000000 + 1000 * n))
  sql "INSERT INTO lv SELECT $base + seq, CONCAT('late red alpha common ', seq) FROM seq_1_to_300"
  sync lv
  sql "DELETE FROM lv WHERE id BETWEEN $base AND $((base + 100))"
  sync lv
  want=$(sql "SELECT COUNT(*) FROM lv WHERE body LIKE '%common%'")
  ( sql "SELECT sift_merge('lv', 'full')" >/dev/null 2>&1 ) &
  sleep "$delay"
  kill -9 "$(cat "$SIFT_MDB_ROOT/started.pid")" 2>/dev/null; wait; sleep 0.5
  "$SRV" start >/dev/null
  check "kill -9 $delay s into a merge: the index opens with the same answers" "$want" "$(cnt common lv)"
  check "kill -9 $delay s into a merge: no temporary file is left after the restart" 0 "$(files | grep -c 'tmp\|new-')"
  sql "SELECT sift_merge('lv', 'full')" >/dev/null
  check "kill -9 $delay s into a merge: the merge can be run again" 1 "$(stat segments lv)"
  check "kill -9 $delay s into a merge: and gives the same answers" "$want" "$(cnt common lv)"
done

# the log of the index stays small: a full merge rewrites it with one event per document that is still there
check "the event log is compacted" 1 "$([[ $(command stat -c %s "$SIFT_MDB_ROOT"/data/.sift/lv/events-*.log) -le $((($(cnt common lv) + 100) * 24)) ]] && echo 1)"
finish
