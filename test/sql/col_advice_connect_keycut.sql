-- sift.col_maintenance_status of a connected index whose parent index is cut by key (key_cut) recommends REINDEX for a stale connection only for parent keys that really changed (docs/COL-CONNECT.md):
-- updates of other columns of the parent and the maintenance of its index never do. A key_cut merge rewrites only the partitions it touches, so the partitions of the parent carry different stamps
-- and no stamp tells the rows a merge wrote again from the rows inserted since: the rule counts no inserted parent rows for such a parent. It does NOT cover parent rows inserted beyond the share:
-- they are not advised for a key_cut parent (reads join for them and answer as the normal plan does); the proper count needs an on-disk change (5.0.2).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off; SET sift.col_auto_merge_partition_kb = 64;
CREATE TABLE dim_a (id int PRIMARY KEY, name text, updated_at timestamptz, score int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, 'p' || i, now(), i % 100, md5(i::text) FROM generate_series(1, 60000) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (id, name, score, pad) WITH (track_changes = 'id', key_cut = 'id');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, amount int, body text) WITH (autovacuum_enabled = false);
-- one row in ten points at a parent row the table does not have yet
INSERT INTO facts SELECT i, 1 + (i * 7919) % 66000, i % 5, 'c' || i FROM generate_series(1, 40000) i;
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

-- eight rounds of the application at 3% and at 8% of the parent rows: they change a column the index does not key, children come in and change, the tables are vacuumed and the
-- parent index is maintained; the child is not stale in any round (the second share goes on from the first, no rewrite between)
CREATE TABLE trail (pct int, round int, due text, advice text, same_rows boolean);
\o /dev/null
SELECT s FROM unnest(ARRAY[3, 8]) pct, generate_series(1, 8) r, LATERAL unnest(ARRAY[
	format('UPDATE dim_a SET updated_at = now() WHERE (id + %s * 7) %% 100 < %s', r, pct),
	format('INSERT INTO facts SELECT 100000 + %s * 1000 + i, 1 + (i * 13) %% 20000, i %% 5, ''n'' FROM generate_series(1, 200) i', pct * 10 + r),
	format('UPDATE facts SET body = body || ''x'' WHERE id BETWEEN %s * 100 AND %s * 100 + 199', r, r),
	'VACUUM dim_a, facts',
	'SELECT sift.col_maintain(''dim_a_col'')',
	format('INSERT INTO trail SELECT %s, %s, sift.col_maintenance_due(''facts_col'', true), advice(''facts_col''), same_rows()', pct, r)]) WITH ORDINALITY u(s, o)
	ORDER BY pct, r, o \gexec
\o
SELECT pct, count(*) AS rounds, count(*) FILTER (WHERE due = 'stale connection') AS rounds_stale, count(*) FILTER (WHERE advice <> '-') AS rounds_advised, bool_and(same_rows) AS same_rows
	FROM trail GROUP BY pct ORDER BY pct;

-- parent rows that came since, beyond the share (the parent rows the orphans wait for): NOT advised for a key_cut parent (the limit above), the answers stay those of the normal plan
INSERT INTO dim_a SELECT i, 'p' || i, now(), i % 100, md5(i::text) FROM generate_series(60001, 69000) i;
SELECT sift.col_maintenance_due('facts_col', true) AS due, advice('facts_col') <> '-' AS advised, same_rows() AS same_rows;
VACUUM dim_a;
SELECT sift.col_maintenance_due('facts_col', true) AS due, advice('facts_col') <> '-' AS advised, same_rows() AS same_rows;

-- real changes of the parent keys beyond the share: the keys that went and the ones that came ask for the rewrite
UPDATE dim_a SET id = id + 100000 WHERE id % 100 < 15;
VACUUM dim_a, facts;
SELECT sift.col_maintain('dim_a_col') AS parent_maintained;
SELECT sift.col_maintenance_due('facts_col', true) AS due, advice('facts_col') AS advice, same_rows() AS same_rows;
SELECT sift.col_maintain('facts_col') AS rewritten;
SELECT advice('facts_col') AS advice, sift.col_maintenance_due('facts_col', true) AS due, same_rows() AS same_rows;

DROP TABLE trail, facts, dim_a CASCADE;
DROP FUNCTION advice(regclass), same_rows();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
