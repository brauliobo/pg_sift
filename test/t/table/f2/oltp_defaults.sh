#!/usr/bin/env bash
# The OLTP defaults of a Format 2.0 sift_table: an UPDATE of half the columns or a 3 KB value is a column delta (the ctid stays, no index entry), a statement that locks 10,000 or 32,000 rows
# does not hold the partition against a writer of another row, and a short session takes one virtual block, a bulk insert the virtual blocks of its rows and not 8,192.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

done_testing
node_init od 1 "autovacuum = off
sift.async_maintenance = off
max_locks_per_transaction = 256"
node_start od || bail "start failed"
sql od "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

eq() { local desc=$1 got=$2 want=$3; out="got '$got', want '$want'"; check "$desc" test "$got" = "$want"; out=; }
le() { local desc=$1 got=$2 max=$3; out="got '$got', at most '$max'"; check "$desc" test "$got" -le "$max"; out=; }
ge() { local desc=$1 got=$2 min=$3; out="got '$got', at least '$min'"; check "$desc" test "$got" -ge "$min"; out=; }

# 1. column deltas: two columns and 3 KB values keep the address of the row
sql od "CREATE TABLE o (id int PRIMARY KEY, status text, updated_at timestamptz, payload text) USING sift_table WITH (autovacuum_enabled = false)" \
	"CREATE TABLE oh (id int PRIMARY KEY, status text, updated_at timestamptz, payload text) USING heap" \
	"INSERT INTO o SELECT i, 'new', timestamptz '2026-01-01', repeat('a', 100) FROM generate_series(1, 3000) i" \
	"INSERT INTO oh SELECT * FROM o" "SELECT busy FROM sift.table_maintain('o')" >/dev/null || bail "table o failed"

ctid_of() { sql od "SELECT ctid FROM o WHERE id = $1"; }
same_rows() { sql od "SELECT NOT EXISTS (SELECT * FROM o EXCEPT SELECT * FROM oh) AND NOT EXISTS (SELECT * FROM oh EXCEPT SELECT * FROM o)"; }
both() { sql od "$1" >/dev/null; sql od "${1/UPDATE o /UPDATE oh }" >/dev/null; }
keeps_ctid() {
	local c0 c1
	c0=$(ctid_of "$2")
	both "UPDATE o SET $3 WHERE id = $2"
	c1=$(ctid_of "$2")
	if [[ $1 == keeps ]]; then eq "$4" "$c1" "$c0"; else check "$4" test "$c1" != "$c0"; fi
}

sess_open od rr
sess_say rr "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 'snap', status, md5(payload) FROM o WHERE id = 5; \\echo seen1"
wait_for "grep -q '^seen1\$' $(dir od)/rr.out" 60
OLD=$(sql od "SELECT 'snap', status, md5(payload) FROM o WHERE id = 5")

keeps_ctid keeps 5 "status = 'paid', payload = repeat('p', 3000)" "two of four columns and a 3 KB value keep the ctid"
keeps_ctid keeps 6 "payload = repeat('q', 3000)" "one column of 3 KB keeps the ctid"
C7=$(ctid_of 7)
both "UPDATE o SET id = 70007, payload = repeat('r', 3000) WHERE id = 7"
check "an indexed column moves the row" test "$(ctid_of 70007)" != "$C7"
keeps_ctid moves 8 "payload = repeat('s', 5000)" "a value over 3,840 bytes moves the row"
eq "the heap's rows after the updates" "$(same_rows)" t
eq "the index finds the updated row" "$(sql od "SET enable_seqscan = off; SELECT count(*) FROM o WHERE id = 5 AND status = 'paid'")" 1

sess_say rr "SELECT 'snap', status, md5(payload) FROM o WHERE id = 5; \\echo seen2"
wait_for "grep -q '^seen2\$' $(dir od)/rr.out" 60
eq "a snapshot opened before the update sees the old row" "$(grep -c "^$OLD\$" "$(dir od)/rr.out")" 2
sess_say rr "COMMIT;"
sess_close rr

C5=$(ctid_of 5)
sql od "SELECT busy FROM sift.table_maintain('o')" >/dev/null
eq "the folded delta keeps the ctid" "$(ctid_of 5)" "$C5"
eq "the heap's rows after the fold" "$(same_rows)" t
eq "the payload after the fold" "$(sql od "SELECT length(payload) || status FROM o WHERE id = 5")" 3000paid

# 2. row locks: a statement of 10,000 rows and one of 32,000 hold no partition
sql od "CREATE TABLE l (id int PRIMARY KEY, val int) USING sift_table WITH (autovacuum_enabled = false)" \
	"INSERT INTO l SELECT i, 0 FROM generate_series(1, 40000) i" "SELECT busy FROM sift.table_maintain('l')" >/dev/null || bail "table l failed"

T0=$SECONDS
race od "UPDATE l SET val = val + 1 WHERE id BETWEEN 1 AND 10000" "UPDATE l SET val = val + 1 WHERE id = 20000"
eq "an update of 10,000 rows does not block a writer of another row" "$RACE_WAITED" 0
diag "10,000 rows: $((SECONDS - T0)) s"
T0=$SECONDS
race od "SELECT count(*) FROM (SELECT 1 FROM l WHERE id BETWEEN 1 AND 32000 FOR UPDATE) s" "UPDATE l SET val = val + 1 WHERE id = 40000"
eq "32,000 locked rows do not block a writer of another row" "$RACE_WAITED" 0
diag "32,000 rows: $((SECONDS - T0)) s"
race od "SELECT count(*) FROM (SELECT 1 FROM l WHERE id BETWEEN 1 AND 33000 FOR UPDATE) s" "UPDATE l SET val = val + 1 WHERE id = 39000"
eq "33,000 locked rows hold the partition" "$RACE_WAITED" 1
eq "the rows updated" "$(sql od "SELECT count(*) FILTER (WHERE val = 1) FROM l")" 10003
eq "no row lock is left" "$(sql od "SELECT count(*) FROM pg_locks WHERE locktype = 'page' AND mode IN ('ExclusiveLock', 'AccessExclusiveLock')")" 0

# 3. virtual blocks
sql od "CREATE TABLE vb (id int, v text) USING sift_table WITH (autovacuum_enabled = false)" >/dev/null || bail "table vb failed"
next_vblock() { sql od "SELECT 4294967294 - vblocks_left FROM sift.space('vb')"; }

V0=$(next_vblock)
{ for i in $(seq 1 100); do echo "INSERT INTO vb VALUES ($i, 'x');"; echo '\c -'; done; } | sql od >/dev/null
V1=$(next_vblock)
le "100 sessions of one row take at most 100 virtual blocks" "$((V1 - V0))" 100
diag "100 sessions: $((V1 - V0)) virtual blocks"

sql od "INSERT INTO vb SELECT i, 'y' FROM generate_series(1, 50000) i" >/dev/null
V2=$(next_vblock)
ge "50,000 rows of one session take the virtual blocks of their rows" "$((V2 - V1))" 172
le "50,000 rows of one session ramp to large reservations" "$((V2 - V1))" 512
diag "50,000 rows of one session: $((V2 - V1)) virtual blocks"

seq 1 100 | sql od "COPY vb (id) FROM STDIN"
V3=$(next_vblock)
le "a COPY of 100 rows takes at most 4 virtual blocks" "$((V3 - V2))" 4
diag "COPY of 100 rows: $((V3 - V2)) virtual blocks"

seq 1 50000 | sql od "COPY vb (id) FROM STDIN"
V4=$(next_vblock)
ge "a COPY of 50,000 rows takes the virtual blocks of its rows" "$((V4 - V3))" 172
le "a COPY of 50,000 rows takes at most 512 virtual blocks" "$((V4 - V3))" 512
diag "COPY of 50,000 rows: $((V4 - V3)) virtual blocks"
eq "every row is there" "$(sql od "SELECT count(*), count(DISTINCT ctid) FROM vb")" "100200|100200"
