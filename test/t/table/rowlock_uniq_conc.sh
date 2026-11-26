#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# Writers of disjoint rows of a sift_table and of a UNIQUE sift_col index do not meet in shared memory or in the lock manager. A transaction that holds a few rows takes no page lock of the lock
# manager for the intention of its row locks (they are shared-memory records until a partition or the relation is held); 16 clients that update disjoint rows add up exactly and scale; 16 clients that
# insert into a UNIQUE index leave one row of each key, and the same keys inserted again are refused or skipped. A request for a coarse lock waits for the transaction that holds a row without a lock manager
# entry, and a cycle through that wait is a deadlock that the server detects. 32 transactions that each hold 5000 keys of a UNIQUE index unflushed (more than the stripes of the registry) stage
# them without writing a part, and 10 that stage a row of one key under a deferred check all go on.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 17
node_init rc 2 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off
max_connections = 100
deadlock_timeout = 200ms"
node_start rc || bail "start failed"
D=$(dir rc)
CONN=(-h "$D/sock" -p "$(port rc)" -U postgres -d postgres)
sql rc "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE h (id int PRIMARY KEY, v bigint NOT NULL) USING sift_table" \
	"INSERT INTO h SELECT g, 0 FROM generate_series(1, 16000) g" \
	"CREATE TABLE big (k int PRIMARY KEY, v int NOT NULL) USING sift_table" \
	"INSERT INTO big SELECT g, 0 FROM generate_series(1, 40000) g" \
	"CREATE SEQUENCE useq" \
	"CREATE TABLE u (id bigint, c int)" \
	"CREATE UNIQUE INDEX u_i ON u USING sift_col (id)" \
	"CREATE TABLE uc (id int, c int)" \
	"CREATE UNIQUE INDEX uc_i ON uc USING sift_col (id)" \
	"CREATE TABLE pt (id int, t int NOT NULL, v int NOT NULL) PARTITION BY RANGE (t)" \
	"DO \$\$ BEGIN FOR i IN 0..15 LOOP
		EXECUTE format('CREATE TABLE pt_%s PARTITION OF pt FOR VALUES FROM (%s) TO (%s) USING sift_table', i, i, i + 1);
	END LOOP; END \$\$" \
	"INSERT INTO pt SELECT g, g % 16, 0 FROM generate_series(1, 1600) g" \
	"CREATE TABLE bu (id int)" \
	"CREATE UNIQUE INDEX bu_i ON bu USING sift_col (id)" \
	"CREATE TABLE dk (id int, pad text)" \
	"ALTER TABLE dk ALTER pad SET STORAGE PLAIN" \
	"INSERT INTO dk SELECT -g, repeat('x', 3000) FROM generate_series(1, 2400) g" \
	"CREATE UNIQUE INDEX dk_i ON dk USING sift_col (id)" \
	"CREATE FUNCTION partial_insert(regclass, int) RETURNS text AS 'pg_sift', 'sift_test_unique_partial_insert' LANGUAGE C STRICT" >/dev/null || bail "setup failed"

intent_locks() { sql rc "SELECT count(*) FROM pg_locks WHERE locktype = 'page' AND relation = 'h'::regclass AND page BETWEEN -12288 AND -4097"; }
tps() { sed -n 's/^tps = \([0-9.]*\) .*/\1/p' "$1" | head -n 1; }
bench() { pgbench -n -c "$1" -j 4 -T "${4:-1}" -f "$2" "${CONN[@]}" >"$3" 2>&1; }

cat >"$D/upd.pgb" <<'EOF'
\set id (:client_id % 16) * 1000 + random(1, 1000)
UPDATE h SET v = v + 1 WHERE id = :id;
SELECT xmax FROM h WHERE id = :id;
EOF
cat >"$D/lock.pgb" <<'EOF'
\set id (:client_id % 16) * 1000 + random(1, 1000)
BEGIN;
SELECT id FROM h WHERE id = :id FOR UPDATE;
COMMIT;
EOF
cat >"$D/ins.pgb" <<'EOF'
INSERT INTO u SELECT nextval('useq'), :client_id;
EOF
cat >"$D/dup.pgb" <<'EOF'
\set k random(1, 400)
INSERT INTO uc VALUES (:k, :client_id) ON CONFLICT DO NOTHING;
EOF

sess_open rc ta
sess_say ta "BEGIN; UPDATE h SET v = v + 1 WHERE id = 1; SELECT xmax = 0 FROM h WHERE id = 1; \\echo held"
wait_for "grep -q '^held\$' $D/ta.out" 60 || bail "the writer did not hold its row"
out=$(intent_locks)
check "a transaction that holds a row takes no page lock of the lock manager for its intention" test "$out" -eq 0
sess_say ta "COMMIT;"
sess_close ta
sql rc "UPDATE h SET v = 0 WHERE id = 1" >/dev/null

# A row of each of 16 partitioned sift_tables in one transaction, by one client and by 16 at once: the intentions of the 16 relations and of their partitions are records, no page lock
own_intents="SELECT count(*) FROM pg_locks WHERE locktype = 'page' AND pid = pg_backend_pid() AND page BETWEEN -12288 AND -4097"
cat >"$D/parts.pgb" <<EOF
BEGIN;
UPDATE pt SET v = v + 1 WHERE id BETWEEN :client_id * 64 + 1 AND :client_id * 64 + 16;
$own_intents \\gset
\\if :count
SELECT 1 / 0;
\\endif
COMMIT;
EOF
out=$(sql rc "BEGIN" "UPDATE pt SET v = v + 1 WHERE id <= 16" "$own_intents" "ROLLBACK")
check "a transaction that holds a row of each of 16 partitions takes no page lock for its intentions" test "$out" -eq 0
bench 16 "$D/parts.pgb" "$D/parts16.out"; brc=$?
out=$(cat "$D/parts16.out")
check "neither do 16 clients that each hold a row of each of the 16 partitions" test "$brc" -eq 0

bench 1 "$D/lock.pgb" "$D/lock1.out"
bench 16 "$D/lock.pgb" "$D/lock16.out"
sql rc "UPDATE h SET v = 0" >/dev/null
polls=0
bench 16 "$D/upd.pgb" "$D/upd16.out" 2 &
bpid=$!
for _ in 1 2 3 4 5 6; do sleep 0.25; polls=$((polls + $(intent_locks))); done
wait "$bpid"; brc=$?
tx=$(sed -n 's/^number of transactions actually processed: //p' "$D/upd16.out")
out=$(sql rc "SELECT sum(v) FROM h")
check "16 clients that update disjoint rows add up exactly" test "$out" == "$tx"
check "and none of them failed or met a deadlock" test "$brc" -eq 0 -a "$(grep -c 'deadlock detected' "$D/postgres.log")" -eq 0
check "no page lock for the intention of the row locks was seen while they ran" test "$polls" -eq 0
t1=$(tps "$D/lock1.out"); t16=$(tps "$D/lock16.out")
out="1 client $t1 tps, 16 clients $t16 tps"
check "16 clients that lock disjoint rows run at 3 times the rate of one or better" awk -v a="$t1" -v b="$t16" 'BEGIN { exit !(b >= 3 * a) }'

bench 16 "$D/ins.pgb" "$D/ins.out"
tx=$(sed -n 's/^number of transactions actually processed: //p' "$D/ins.out")
out=$(sql rc "SELECT count(*) || ' ' || count(DISTINCT id) FROM u")
check "16 clients that insert into a UNIQUE index leave a row for each insert" test "$out" == "$tx $tx"
bench 16 "$D/dup.pgb" "$D/dup.out"
out=$(sql rc "SELECT count(*) || ' ' || count(DISTINCT id) FROM uc")
check "16 clients that insert the same 400 keys leave one row of each" test "$out" == "400 400"

sess_open rc ea; sess_open rc eb
sess_say ea "SET application_name = 'ea'; BEGIN; UPDATE big SET v = 1 WHERE k = 40000; \\echo held"
sess_say eb "SET application_name = 'eb';"
wait_for "grep -q '^held\$' $D/ea.out && [[ \$(sess_count rc eb true) == 1 ]]" 60 || bail "the two sessions did not start"
sess_say eb "BEGIN; SELECT count(*) FROM (SELECT k FROM big WHERE k <= 36000 FOR UPDATE) s; \\echo locked"
wait_for "[[ \$(sess_count rc eb \"wait_event_type = 'Lock'\") == 1 ]]" 60
out=$(sess_count rc eb "wait_event_type = 'Lock'")
check "a request for the partition waits for the writer that holds a row without a lock manager entry" test "$out" -eq 1
sess_say ea "UPDATE big SET v = 2 WHERE k = 1; \\echo ended"
wait_for "grep -q 'deadlock detected' $D/ea.out $D/eb.out" 60
check "the cycle through that wait is a deadlock the server finds, one session is its victim" test "$(cat "$D/ea.out" "$D/eb.out" | grep -c 'deadlock detected')" -eq 1
sess_say ea "ROLLBACK;"; sess_say eb "ROLLBACK;"
sess_close ea; sess_close eb
out=$(sql rc "SELECT count(*) || ' ' || sum(v) FROM big")
check "and neither of them changed a row" test "$out" == "40000 0"
out=$(intent_locks)
check "no lock of the intention is left" test "$out" -eq 0
# 32 x 5000 keys held at once fill the 512 entries of every stripe of the registry: none of them sends its stage to the partitions to make room
for k in $(seq 0 31); do
	sess_open rc "b$k"
	sess_say "b$k" "BEGIN;
SELECT sift._test_stage_flushes() AS f0 \\gset
INSERT INTO bu SELECT g FROM generate_series($((k * 5000 + 1)), $(((k + 1) * 5000))) g;
SELECT 'flushes=' || (sift._test_stage_flushes() - :f0);"
done
wait_for "[[ \$(cat $D/b*.out | grep -c '^flushes=') == 32 ]]" 120 || diag "$(cat "$D"/b*.out | grep -v '^flushes=' | head -n 5)"
out=$(cat "$D"/b*.out | grep -v '^flushes=0$' | head -n 5)
check "32 transactions that hold 5000 unique keys each write none of them before they commit" test -z "$out"
for k in $(seq 0 31); do sess_say "b$k" "COMMIT;"; sess_close "b$k"; done
out=$(sql rc "SELECT count(*) || ' ' || count(DISTINCT id) FROM bu")
check "and every key is there once" test "$out" == "160000 160000"

# 10 transactions that stage a row of one key under a deferred check (at the heap tuple 1000/1 of another key): the registry reports them all, the check judges each
for k in $(seq 0 9); do
	sess_open rc "dq$k"
	sess_say "dq$k" "BEGIN; SELECT 'staged ' || partial_insert('dk_i', 1);"
	wait_for "grep -q '^staged\|ERROR' $D/dq$k.out" 60
done
out=$(cat "$D"/dq*.out | grep -v '^staged ' | head -n 5)
check "10 transactions stage a row of one key under a deferred check" test -z "$out"
for k in $(seq 0 9); do sess_say "dq$k" "ROLLBACK;"; sess_close "dq$k"; done
out=$(sql rc "SELECT count(*) FROM pg_stat_activity WHERE state = 'idle in transaction'")
check "and they all end" test "$out" -eq 0
finish
