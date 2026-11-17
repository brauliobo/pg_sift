#!/usr/bin/env bash
# Two sessions intern strings into one shared value store (the strings of one column that several sift_col indexes share). The session that appends holds the store's extension lock
# only while it appends the codes of its strings; the merge of the store's runs, which follows, holds a page lock of its own and no one waits for it: another session that interns, strings of the first
# session's batch among them, gets its codes while the first is still inside its merge (stopped at a test gate, an advisory lock a test holds, so no timing is involved), and a session that
# only extends the store's file is not held back by either. The codes are the same ones a sequence of the sessions would give: one code for each string, every code reads back its string.
. "$(dirname "$0")/lib.bash"

plan 9
node_init vi 34 "autovacuum = off
sift.async_maintenance = off
sift.col_auto_merge = off"
node_start vi || bail "start failed"
D=$(dir vi)
STORE=vr_t_url_sift_values
sql vi "CREATE EXTENSION pg_sift_test CASCADE" "SET sift.test_vstore_columns = 'url'" \
	"CREATE TABLE vr_t (id int PRIMARY KEY, url text)" "CREATE INDEX vr_a ON vr_t USING sift_col (id, url)" "CREATE INDEX vr_b ON vr_t USING sift_col (url, id)" >/dev/null || bail "setup failed"
intern() { sql vi "LOAD 'pg_sift'" "SET application_name = '$1'" "${@:3}" "SELECT sift._test_vstore_intern('$STORE', array[$2])"; }
runs() { sql vi "SELECT runs FROM sift._test_vstore_info('$STORE')"; }
for i in $(seq 12); do intern fill "'f$i', 'g$i'" >/dev/null; done
check "the store has a chain of runs the next append merges" test "$(runs)" -ge 12

rm -f "$D/locked" "$D/release"
{ echo "SELECT pg_advisory_lock(20983, 12);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql vi >/dev/null &
holder=$!
wait_for "[[ -e $D/locked ]]" 30

intern first "'s1a', 's1b', 'shared'" "SET sift.test_intern_gate = on" >"$D/first.out" 2>&1 &
first=$!
wait_for "[[ \$(sql vi \"SELECT count(*) FROM pg_stat_activity WHERE application_name = 'first' AND wait_event = 'advisory'\") == 1 ]]" 60
check "the first session appended its strings and stops inside its merge" test "$(sql vi "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'first' AND wait_event = 'advisory'")" == 1

intern second "'s2a', 'shared', 's1a', 's2b'" >"$D/second.out" 2>&1 &
second=$!
check "a second session that interns new and shared strings is not held behind that merge" wait_for "! kill -0 $second 2>/dev/null" 60

intern third "'t1', 't2'" >"$D/third.out" 2>&1 &
third=$!
check "a third session with only new strings is not held either" wait_for "! kill -0 $third 2>/dev/null" 60

touch "$D/release"
wait "$holder" "$first"
check "the first session ends after the gate opens" test "$(wc -l <"$D/first.out")" -ge 1
F=($(tail -1 "$D/first.out" | tr -d '{}' | tr ',' ' '))
S=($(tail -1 "$D/second.out" | tr -d '{}' | tr ',' ' '))
check "the second session got the codes the first appended for the strings they share" test "${F[2]:-x}" == "${S[1]:-y}" -a "${F[0]:-x}" == "${S[2]:-y}"

ALL="'s1a', 's1b', 'shared', 's2a', 's2b', 't1', 't2'"
CODES=$(sql vi "SELECT unnest(sift._test_vstore_intern('$STORE', array[$ALL]))" | sort -n | uniq | wc -l)
check "every string has one code (7 strings, $CODES codes)" test "$CODES" == 7
check "every code reads back its string" test "$(sql vi "SELECT count(*) FROM unnest(array[$ALL]) WITH ORDINALITY u(s, i)
	JOIN unnest(sift._test_vstore_intern('$STORE', array[$ALL])) WITH ORDINALITY c(code, i) USING (i) WHERE sift._test_vstore_value('$STORE', c.code) = u.s")" == 7
for i in 1 2; do intern last "'z$i'" >/dev/null; done
check "the runs are merged by the appends that follow" test "$(runs)" -le 12
finish
