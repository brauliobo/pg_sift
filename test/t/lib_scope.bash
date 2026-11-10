# Fixtures of the scope copy tests (col_scope_copy_*): the tables facts and pairs, a plain copy (cflat) and a partitioned one (cpart), scope_same(copy) that
# says whether a copy is exact, and scope_churn NAME that writes the files of the pgbench scripts that change facts and pairs in transactions.
# Ids belong to 0 to 6 scope keys: id e has none (e % 10 = 0), two (6), three (7), four (8), six (9) or one key; the ids 41 to 45 have facts and no pair.

SCOPE_ROWS=$(knob SIFT_SCOPE_ROWS 6000 3000)

SCOPE_SAME_SQL="CREATE FUNCTION scope_same(copy text) RETURNS bool LANGUAGE plpgsql AS \$\$
	DECLARE k int; a text; b text;
	BEGIN
		IF EXISTS (SELECT 1 FROM sift.scope_verify(copy)) THEN RETURN false; END IF;
		FOR k IN 0..9 LOOP
			PERFORM set_config('sift.enable_agg', 'on', true);
			PERFORM set_config('sift.agg_force', 'on', true);
			EXECUTE format('SELECT coalesce(md5(string_agg(t::text, ''|'' ORDER BY t::text)), ''-'') FROM (SELECT kind, count(*), sum(amount), max(id) FROM %s WHERE scope_id = %s GROUP BY kind) t', copy, k) INTO a;
			PERFORM set_config('sift.enable_agg', 'off', true);
			EXECUTE format('SELECT coalesce(md5(string_agg(t::text, ''|'' ORDER BY t::text)), ''-'') FROM (SELECT kind, count(*), sum(amount), max(id) FROM facts JOIN pairs USING (dim_a_id) WHERE scope_id = %s GROUP BY kind) t', k) INTO b;
			IF a IS DISTINCT FROM b THEN RETURN false; END IF;
		END LOOP;
		RETURN true;
	END \$\$"

# scope_setup NODE: the extension, the tables, the rows and the two copies; scope_same(copy) is true when the verify of the copy returns no row and every key from 0 to 9 (empty
# ones too) answers by the sift plan as the join does by the plain plan
scope_setup() {
	sql "$1" "CREATE EXTENSION pg_sift" \
		"CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL, note text) WITH (autovacuum_enabled = false)" \
		"CREATE TABLE pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id)) WITH (autovacuum_enabled = false)" \
		"INSERT INTO pairs SELECT e, k FROM generate_series(1, 40) e, generate_series(1, CASE e % 10 WHEN 0 THEN 0 WHEN 6 THEN 2 WHEN 7 THEN 3 WHEN 8 THEN 4 WHEN 9 THEN 6 ELSE 1 END) k" \
		"INSERT INTO facts SELECT i, 1 + i % 45, timestamptz '2026-01-01' + i * interval '1 hour', i % 5, i % 97, 'n' || i % 7 FROM generate_series(1, $SCOPE_ROWS) i" \
		"SELECT sift.scope_create('cflat', 'facts', 'dim_a_id', 'pairs', 'scope_id')" \
		"SELECT sift.scope_create('cpart', 'facts', 'dim_a_id', 'pairs', 'scope_id', partition => true)" \
		"$SCOPE_SAME_SQL" >/dev/null
}

# scope_answers NODE COPY: the answers of every key from 0 to 9 of a copy by the sift plan, as one string
scope_answers() {
	sql "$1" "SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.agg_force = on;
		SELECT string_agg(k || '=' || coalesce((SELECT md5(string_agg(t::text, '|' ORDER BY t::text)) FROM (SELECT kind, count(*), sum(amount), max(id) FROM $2 WHERE scope_id = k GROUP BY kind) t), '-'), ',' ORDER BY k)
		FROM generate_series(0, 9) k" | tail -n 1
}

# scope_exact NODE: true when both copies are exact
scope_exact() { [[ $(sql "$1" "SELECT scope_same('cflat') AND scope_same('cpart')") == t ]]; }

# scope_files NAME: the pgbench scripts of the node's directory that change the facts and the pairs, each in a transaction (statements that cannot fail on a key or an id)
scope_files() {
	local d
	d=$(dir "$1")
	cat >"$d/sf.sql" <<'EOF'
\set id random(1, 100000)
\set m random(1, 48)
BEGIN;
INSERT INTO facts VALUES (:id, :m, now(), :id % 5, :id % 97, 'x') ON CONFLICT (id) DO UPDATE SET dim_a_id = EXCLUDED.dim_a_id, amount = facts.amount + 1;
UPDATE facts SET amount = amount + 1, note = 'y' WHERE id BETWEEN :id AND :id + 20;
DELETE FROM facts WHERE id = :id + 30;
COMMIT;
EOF
	cat >"$d/sp.sql" <<'EOF'
\set m random(1, 48)
\set k random(1, 8)
\set k2 random(1, 8)
BEGIN;
INSERT INTO pairs VALUES (:m, :k) ON CONFLICT DO NOTHING;
UPDATE pairs SET scope_id = :k2 WHERE dim_a_id = :m AND scope_id = :k AND NOT EXISTS (SELECT 1 FROM pairs q WHERE q.dim_a_id = :m AND q.scope_id = :k2);
DELETE FROM pairs WHERE dim_a_id = :m + 1 AND scope_id = :k2;
COMMIT;
EOF
	cat >"$d/sb.sql" <<'EOF'
\set id random(1, 100000)
\set m random(1, 48)
\set k random(1, 8)
BEGIN;
WITH p AS (INSERT INTO pairs VALUES (:m, :k) ON CONFLICT DO NOTHING RETURNING 1)
	INSERT INTO facts VALUES (:id, :m, now(), 1, :id % 97, 'z') ON CONFLICT (id) DO UPDATE SET dim_a_id = EXCLUDED.dim_a_id;
WITH d AS (DELETE FROM pairs WHERE dim_a_id = :m AND scope_id = :k + 1 RETURNING 1) UPDATE facts SET amount = amount + 1 WHERE id BETWEEN :id AND :id + 5;
COMMIT;
EOF
	echo "-f $d/sf.sql@4 -f $d/sp.sql@3 -f $d/sb.sql@3"
}
