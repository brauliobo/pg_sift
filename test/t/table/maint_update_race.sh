#!/usr/bin/env bash
# The rewrite of the parts of a sift_table (VACUUM, sift.maintain) beside UPDATEs that change one column of many rows. A compaction that starts in a partition with no column delta chain writes its outputs and
# then commits them under the lock: an UPDATE that commits in between puts its column deltas into a chain that names the parts the commit replaces, and the deltas of a part that is gone are dropped, so the
# UPDATE is lost. The compaction is held after its outputs are written (sift.test_phase_gate, gate SIFT_F2_GATE_COMPACT = 11) while an UPDATE commits, then released: every row the UPDATE changed is changed.
# The workers of sift stay off until then: one would compact the deleted rows of the table first and leave the VACUUM nothing to rewrite.
# Then, with the workers on, writers that add one to rows beside the rewrite of the parts, and a churn of the rows beside them, end with the sum of the rows they updated and no statement failed.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

G=7600
ROWS=${SIFT_RACE_ROWS:-16000}
SPAN=${SIFT_RACE_SPAN:-400}
ROUNDS=${SIFT_RACE_ROUNDS:-12}

plan 9
node_init mr 42 "autovacuum = off
sift.maintenance_naptime = 200
sift.async_maintenance = off
sift.dead_rewrite_pct = 60
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start mr || bail "start failed"
D=$(dir mr)
sql mr "CREATE EXTENSION pg_sift" \
	"CREATE TABLE dp (id int PRIMARY KEY, v int NOT NULL, grp int, pad text) USING sift_table WITH (autovacuum_enabled = false)" \
	"INSERT INTO dp SELECT g, 0, g % 7, repeat('x', 40) FROM generate_series(1, 4000) g" \
	"DELETE FROM dp WHERE id % 10 <> 0" >/dev/null || bail "setup failed"
parts() { sql mr "SELECT string_agg(part_gen::text, ',' ORDER BY part_gen) FROM sift.parts('dp')"; }
waiting() { sql mr "SELECT count(*) FROM pg_stat_activity WHERE query LIKE '$1%' AND wait_event_type = 'Lock' AND wait_event = 'advisory'"; }
before=$(parts)

gated() {
	rm -f "$D/H.in"; mkfifo "$D/H.in"
	psql -X -At -q -h "$D/sock" -p "$(port mr)" -U postgres -d postgres <"$D/H.in" >"$D/H.out" 2>&1 &
	exec {H}>"$D/H.in"
	echo "SELECT pg_advisory_lock($((G + 11)));" >&"$H"
	echo '\echo @@held' >&"$H"
	wait_for "grep -q '@@held' '$D/H.out'" 30 || bail "the gate was not taken"
	(sql mr "SET sift.test_phase_gate = $G" "VACUUM dp" >"$D/vacuum.out" 2>&1; echo $? >"$D/vacuum.rc") &
	check "the compaction waits with its outputs written" wait_for "[[ \$(waiting 'VACUUM dp') == 1 ]]" 60
	check "the UPDATE of the 400 rows commits meanwhile" test "$(sql mr "WITH u AS (UPDATE dp SET v = v + 1 RETURNING 1) SELECT count(*) FROM u")" == 400
	echo "SELECT pg_advisory_unlock($((G + 11)));" >&"$H"
	wait_for "[[ -f '$D/vacuum.rc' ]]" 120 || bail "the VACUUM did not finish"
	exec {H}>&-
	wait
	check "the VACUUM succeeded and replaced the parts" test "$(cat "$D/vacuum.rc")|$(parts)" != "0|$before"
}
if [[ $(sql mr "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_phase_gate'") == 1 ]]; then
	gated
else
	skip 3 "the release library has no gate"
	sql mr "UPDATE dp SET v = v + 1" "VACUUM dp" >/dev/null
fi
check "every row the UPDATE changed is changed" test "$(sql mr "SELECT sum(v) FROM dp")" == 400
sql mr "VACUUM dp" >/dev/null
check "and stays changed after the next VACUUM" test "$(sql mr "SELECT sum(v) FROM dp")" == 400

sql mr "ALTER SYSTEM SET sift.async_maintenance = on" "SELECT pg_reload_conf()" >/dev/null || bail "the workers were not turned on"
wait_for "setting_is mr sift.async_maintenance on" 30 || bail "the reload did not reach the server"
sql mr "CREATE TABLE mr (k int PRIMARY KEY, v int, grp int, pad text) USING sift_table WITH (autovacuum_enabled = false)" \
	"INSERT INTO mr SELECT g, 0, g % 7, repeat('x', 100) FROM generate_series(1, $ROWS) g" >/dev/null || bail "setup failed"
rm -f "$D"/*.n "$D"/*.err "$D/stop"
bump() { echo "WITH u AS (UPDATE mr SET v = v + 1 WHERE k BETWEEN $1 AND $2 AND k % 7 IN (1, 2, 3, 4) AND k % $3 = $4 RETURNING 1) SELECT count(*) FROM u"; }
writer() {
	local w=$1 base=$2 i lo
	for ((i = 0; i < ROUNDS; i++)); do
		lo=$((base + (i * 97) % (ROWS / 2 - SPAN)))
		sql mr "BEGIN" "$(bump "$lo" $((lo + SPAN)) 2 $((i % 2)))" "$(bump $((lo + 50)) $((lo + SPAN + 50)) 3 $((i % 3)))" "COMMIT" 2>>"$D/$w.err" | grep -E '^[0-9]+$' >>"$D/$w.n"
	done
}
churn() {
	local i=${1:-0}
	while [[ ! -e $D/stop ]]; do
		i=$((i + 1))
		sql mr "DELETE FROM mr WHERE k % 7 IN (5, 6) AND k % 5 = $((i % 5))" "INSERT INTO mr SELECT g, 0, g % 7, repeat('c', 100) FROM generate_series(1, $ROWS) g WHERE g % 7 IN (5, 6) ON CONFLICT (k) DO NOTHING" >/dev/null 2>>"$D/churn$1.err"
	done
}
maintainer() { while [[ ! -e $D/stop ]]; do sql mr "VACUUM (FREEZE) mr" "SELECT sift.maintain('mr')" "SELECT sift.merge('mr')" "VACUUM mr" "SELECT sift.promote('mr')" >/dev/null 2>>"$D/maint$1.err"; done; }
writer w1 1 & w1=$!
writer w2 $((ROWS / 2)) & w2=$!
churn 1 & cp=$!
maintainer 1 & mp=$!
maintainer 2 & mq=$!
churn 2 & cq=$!
wait "$w1" "$w2"
touch "$D/stop"
wait "$mp" "$mq" "$cp" "$cq"
expected=$(cat "$D"/w?.n | paste -sd+ | bc)
actual=$(sql mr "SELECT sum(v) FROM mr")
diag "rows updated $expected, sum of the table $actual"
[[ -z $(cat "$D"/*.err) ]] || diag "$(head -n 6 "$D"/*.err | cut -c1-240)"
check "no statement failed" test -z "$(cat "$D"/*.err)"
check "every row an UPDATE changed is in the table" test "$actual" == "$expected"
check "no backend died" test "$(grep -ac 'terminated by signal' "$D/postgres.log")" == 0
out=$(sql mr "SELECT count(*) FROM sift.check('mr')")
check "the table checks clean" test "$out" == 0
finish
