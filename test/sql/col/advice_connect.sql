-- sift.col_maintenance_status of a connected index recommends REINDEX for a stale connection only when parent keys really changed or parent rows really came in (docs/COL-CONNECT.md): updates of other
-- columns of the parent and the maintenance of its index never do (the stamps of the child hold the insert horizon of the parent, which merges leave alone), and the reads give the rows of the normal plan either way.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
-- Format 1: the connection stamps of the child segments and the insert horizon of the parent (sift.test_conn_no_horizon) are Format 1; a Format 2.0 child keeps no
-- connection state, the key journal of the parent vouches for the dimension at read time (col/f2/connect.h).
SET sift.test_create_format1 = on;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET work_mem = '16MB';
CREATE TABLE dim_a (id int PRIMARY KEY, name text, updated_at timestamptz, score int) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, 'p' || i, now(), i % 100 FROM generate_series(1, 20000) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, name, score) WITH (track_changes = 'id');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, amount int, body text) WITH (autovacuum_enabled = false);
-- one row in ten points at a parent row the table does not have yet
INSERT INTO facts SELECT i, 1 + (i * 7919) % 22000, i % 5, 'c' || i FROM generate_series(1, 40000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, amount) WITH (track_changes = 'id', connect = 'dim_a_id -> dim_a(id)');
VACUUM dim_a, facts;
SELECT sift.col_maintain('facts_col') AS filled;

CREATE FUNCTION advice(idx regclass) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce((SELECT string_agg(b, '; ') FROM sift.col_maintenance_status(idx) s, unnest(s.blocked_by) b WHERE b LIKE 'REINDEX recommended%'), '-') $$;
CREATE FUNCTION same_rows() RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
	q text := 'SELECT p.score, count(*), sum(r.amount), count(p.id) FROM facts r LEFT JOIN dim_a p ON p.id = r.dim_a_id GROUP BY 1 ORDER BY 1';
	a text;
	b text;
BEGIN
	SET LOCAL sift.agg_force = on;
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
	RETURN a = b;
END $$;
SELECT advice('facts_col') AS advice, sift.col_maintenance_due('facts_col', true) AS due, same_rows() AS same_rows;

-- eight rounds of the application at 3% and at 8% of the parent rows: they change columns the index does not key (one of them one the index holds, so that every version gets an entry of its own), children come in and change, the tables are vacuumed and the
-- parent index is maintained; the child is not stale in any round (the second share goes on from the first, no rewrite between)
CREATE TABLE trail (pct int, round int, due text, advice text, same_rows boolean);
\o /dev/null
SELECT s FROM unnest(ARRAY[3, 8]) pct, generate_series(1, 8) r, LATERAL unnest(ARRAY[
	format('UPDATE dim_a SET updated_at = now() WHERE (id + %s * 7) %% 100 < %s', r, pct),
	format('UPDATE dim_a SET score = (score + 1) %% 100 WHERE (id + %s * 11) %% 100 < %s', r, pct),
	format('INSERT INTO facts SELECT 100000 + %s * 1000 + i, 1 + (i * 13) %% 20000, i %% 5, ''n'' FROM generate_series(1, 200) i', pct * 10 + r),
	format('UPDATE facts SET body = body || ''x'' WHERE id BETWEEN %s * 100 AND %s * 100 + 199', r, r),
	'VACUUM dim_a, facts',
	'SELECT sift.col_maintain(''dim_a_col'')',
	format('INSERT INTO trail SELECT %s, %s, sift.col_maintenance_due(''facts_col'', true), advice(''facts_col''), same_rows()', pct, r)]) WITH ORDINALITY u(s, o)
	ORDER BY pct, r, o \gexec
\o
SELECT pct, count(*) AS rounds, count(*) FILTER (WHERE due = 'stale connection') AS rounds_stale, count(*) FILTER (WHERE advice <> '-') AS rounds_advised, string_agg(DISTINCT due, ', ') AS dues, bool_and(same_rows) AS same_rows
	FROM trail GROUP BY pct ORDER BY pct;

-- parent rows that came since, beyond the share (the parent rows the orphans wait for): in the write buffer, in the segment its promotion wrote and in the base segment of the parent that a
-- merge put them into, they ask for the rewrite; a child whose segments were stamped before the horizon existed (sift.test_conn_no_horizon) is told only by the keys
INSERT INTO dim_a SELECT i, 'p' || i, now(), i % 100 FROM generate_series(20001, 23000) i;
SELECT sift.col_maintenance_due('facts_col', true) AS due, advice('facts_col') <> '-' AS advised, same_rows() AS same_rows;
VACUUM dim_a;
SELECT sift.col_maintenance_due('facts_col', true) AS due, advice('facts_col') <> '-' AS advised, same_rows() AS same_rows;
SELECT sift.col_maintain('dim_a_col') AS parent_maintained;
SELECT sift.col_maintenance_due('facts_col', true) AS due, advice('facts_col') <> '-' AS advised, same_rows() AS same_rows;
SELECT sift.col_maintain('facts_col') AS rewritten;
SELECT advice('facts_col') AS advice, sift.col_maintenance_due('facts_col', true) AS due, same_rows() AS same_rows;

-- real changes of the parent keys beyond the share: the keys that went and the ones that came ask for the rewrite
UPDATE dim_a SET id = id + 100000 WHERE id % 100 < 15;
VACUUM dim_a, facts;
SELECT sift.col_maintain('dim_a_col') AS parent_maintained;
SELECT sift.col_maintenance_due('facts_col', true) AS due, advice('facts_col') AS advice, same_rows() AS same_rows;
SELECT sift.col_maintain('facts_col') AS rewritten;
SELECT advice('facts_col') AS advice, sift.col_maintenance_due('facts_col', true) AS due, same_rows() AS same_rows;

-- a child stamped before the horizon existed says nothing of the rows that came: not advised for them, whatever came; stamped again, it is told
SET sift.test_conn_no_horizon = on;
REINDEX INDEX facts_col;
RESET sift.test_conn_no_horizon;
INSERT INTO dim_a SELECT i, 'p' || i, now(), i % 100 FROM generate_series(30001, 36000) i;
SELECT sift.col_maintenance_due('facts_col', true) AS due, advice('facts_col') <> '-' AS advised, same_rows() AS same_rows;
REINDEX INDEX facts_col;
INSERT INTO dim_a SELECT i, 'p' || i, now(), i % 100 FROM generate_series(36001, 42000) i;
SELECT sift.col_maintenance_due('facts_col', true) AS due, advice('facts_col') <> '-' AS advised, same_rows() AS same_rows;

DROP TABLE trail, facts, dim_a CASCADE;
DROP FUNCTION advice(regclass), same_rows();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
