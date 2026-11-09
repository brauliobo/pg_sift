#!/usr/bin/env bash
# sift.col_wbuf_ref against the heap: a seeded random sequence of inserts (committed, rolled back, a savepoint rolled back), updates (HOT and not), deletes, VACUUMs (the index
# cleaned or not, FREEZE), promotions of the buffer, arena slots taken away and given back, a server that stops and starts, and a server that is killed, over one table. After every step
# the answers of the engine are the heap's (the plan of the engine against the native plan of the same table), the index verifies, and once the arena has the rows (no restart since) the
# heap is not read for them. SIFT_ARENA_SEEDS seeds (default 24, 3 quick) of SIFT_ARENA_STEPS steps (30, 12).
. "$(dirname "$0")/lib.bash"

SEEDS=$(knob SIFT_ARENA_SEEDS 24 3)
STEPS=$(knob SIFT_ARENA_STEPS 30 12)
plan "$SEEDS"
node_init fz 7 "autovacuum = off
sift.async_maintenance = off
sift.col_auto_merge = off
sift.col_wbuf_ref = on
max_parallel_workers_per_gather = 0
checkpoint_timeout = 30s"
node_start fz || bail "start failed"
D=$(dir fz)

ANSWERS="SELECT md5(concat_ws('|',
    (SELECT count(*) || ',' || sum(v) FROM a),
    (SELECT count(*) || ',' || sum(v) FROM a WHERE g = 3),
    (SELECT string_agg(id::text, ',' ORDER BY v DESC, id) FROM (SELECT id, v FROM a ORDER BY v DESC, id LIMIT 30) x),
    (SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT g, count(*), sum(v) FROM a GROUP BY g) x)))"
sql fz "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE a (id int PRIMARY KEY, g int NOT NULL, v int, pad text) WITH (autovacuum_enabled = off)" \
	"CREATE INDEX a_ix ON a USING sift_col (id, g, v)" \
	"CREATE FUNCTION answers() RETURNS text LANGUAGE sql AS \$\$ $ANSWERS \$\$" \
	"CREATE FUNCTION same_as_heap() RETURNS boolean LANGUAGE plpgsql AS \$\$ DECLARE n text; s text; BEGIN
		PERFORM set_config('sift.enable_agg', 'off', true); n := answers(); PERFORM set_config('sift.enable_agg', 'on', true); s := answers(); RETURN n = s; END \$\$" \
	>/dev/null || bail "setup failed"
rebuilt() { sql fz "SELECT rebuilt FROM sift._test_arena_stats()"; }

# one step of a seed; $1 the kind, $2 a number it uses; the id ranges of the inserts are disjoint (the step number)
step() {
	local kind=$1 n=$2 base=$(( $3 * 100000 ))
	case $kind in
	0|1) sql fz "INSERT INTO a SELECT $base + i, i % 10, i, 'x' FROM generate_series(1, $((200 + n * 7))) i" ;;
	2) sql fz "BEGIN" "INSERT INTO a SELECT $base + i, i % 10, 90000 + i, 'r' FROM generate_series(1, $((100 + n * 3))) i" "ROLLBACK" ;;
	3) sql fz "BEGIN" "INSERT INTO a SELECT $base + i, i % 10, i, 'c' FROM generate_series(1, 50) i" "SAVEPOINT s" \
		"INSERT INTO a SELECT $base + 1000 + i, i % 10, 80000 + i, 's' FROM generate_series(1, $((50 + n))) i" "ROLLBACK TO s" "COMMIT" ;;
	4) sql fz "UPDATE a SET v = v + 1 WHERE id % $((3 + n % 9)) = 0" ;;
	5) sql fz "UPDATE a SET pad = 'hot$n' WHERE id % $((2 + n % 7)) = 0" ;;
	6) sql fz "DELETE FROM a WHERE id % $((5 + n % 13)) = 0" ;;
	7) sql fz "VACUUM (INDEX_CLEANUP off) a" ;;
	8) sql fz "VACUUM a" ;;
	9) sql fz "VACUUM (FREEZE, INDEX_CLEANUP off) a" ;;
	10) sql fz "SELECT sift.col_promote('a_ix')" ;;
	11) sql fz "SELECT sift._test_arena_slots(0)" "INSERT INTO a SELECT $base + i, i % 10, i, 'n' FROM generate_series(1, 300) i" "SELECT sift._test_arena_slots(16)" ;;
	12) node_stop fz; node_start fz ;;
	13) crash fz; node_start fz ;;
	esac >/dev/null
}

BAD=
for seed in $(seq "$SEEDS"); do
	sql fz "TRUNCATE a" "SELECT sift._test_arena_gc()" "VACUUM a" >/dev/null || bail "reset failed"
	RANDOM=$seed
	trail=
	failed=
	for n in $(seq "$STEPS"); do
		kind=$((RANDOM % 14))
		# a restart empties the arena: the first read after it builds the entries
		restarted=; [[ $kind == 12 || $kind == 13 ]] && restarted=1
		step "$kind" "$n" "$n" || { failed="step $n (kind $kind) failed"; break; }
		trail="$trail $kind"
		before=$(rebuilt)
		same=$(sql fz "SELECT same_as_heap()" "SELECT sift.col_verify('a_ix', true) IS NOT NULL" | tr '\n' ' ')
		if [[ $same != "t t " ]]; then failed="after step $n (kind $kind): $same"; break; fi
		# the entries of a restart are built by the first read; a second read builds nothing (the rows decided to be gone are kept as such)
		before=$(rebuilt)
		sql fz "SELECT same_as_heap()" >/dev/null
		if [[ $(rebuilt) != "$before" ]]; then failed="after step $n (kind $kind): a second read built entries from the heap"; break; fi
	done
	if [[ -z $failed ]]; then ok "seed $seed:$trail"; else not_ok "seed $seed:$trail: $failed"; BAD=1; fi
done
finish
