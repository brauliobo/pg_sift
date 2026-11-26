-- The caches of the values of an expression or of the rows of a lookup join are a memo, not a table the answer needs: with no cache memory at all
-- (sift.backend_cache_kb and sift.shared_cache_kb 0) the rows are the same. Every query runs as the scan and as the normal plan; the rows must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, kind text, big bigint, doc jsonb, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, 'k' || (abs(hashint4(i)) % 700), (i % 3000)::bigint * 1000003,
       jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('cat', 'cat_' || i % 9, 'score', (i % 400) / 4.0),
                                                     jsonb_build_object('cat', 'cat_' || (i + 3) % 9, 'score', (i % 250) / 2.0))),
       (i % 1000) / 10.0
FROM generate_series(1, 12000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, kind, big, doc, amount) WITH (paths = 'doc.parts[]{cat text, score float8}');
VACUUM (FREEZE, ANALYZE) ev;
CREATE TABLE maps (kind text, label text) WITH (autovacuum_enabled = false);
INSERT INTO maps SELECT 'k' || k, 'L' || k % 300 FROM generate_series(0, 699) k WHERE k % 11 <> 0;
CREATE INDEX maps_idx ON maps (kind);
CREATE TABLE groups (label text, grp text) WITH (autovacuum_enabled = false);
INSERT INTO groups SELECT 'L' || l, 'G' || l % 40 FROM generate_series(0, 299) l WHERE l % 13 <> 0;
CREATE INDEX groups_idx ON groups (label);
VACUUM (FREEZE, ANALYZE) maps;
VACUUM (FREEZE, ANALYZE) groups;

CREATE TABLE cases (n int, shape text, q text);
INSERT INTO cases VALUES
	(1, 'lookup join, text key',    'SELECT coalesce(m.label, ''none'') AS label, count(*) AS n, sum(ev.amount) AS total FROM ev LEFT JOIN maps m ON m.kind = ev.kind GROUP BY 1'),
	(2, 'lookup join on a lookup',  'SELECT coalesce(g.grp, ''none'') AS grp, count(*) AS n FROM ev LEFT JOIN maps m ON m.kind = ev.kind LEFT JOIN groups g ON g.label = m.label GROUP BY 1'),
	(3, 'inner lookup join',        'SELECT m.label, count(*) AS n, sum(ev.amount) AS total FROM ev JOIN maps m ON m.kind = ev.kind GROUP BY 1'),
	(4, 'expression, two leaves',   'SELECT o.cat || '':'' || (o.score::int % 50)::text AS k, count(*) AS n FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.doc[''parts'']) o(cat text, score float8) GROUP BY 1'),
	(5, 'expression, one wide leaf', 'SELECT (o.score * 3)::int % 97 AS k, count(*) AS n, sum(o.score) AS total FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.doc[''parts'']) o(cat text, score float8) GROUP BY 1'),
	(6, 'expression in a filter',   'SELECT o.cat, count(*) AS n FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.doc[''parts'']) o(cat text, score float8) WHERE (o.score::int % 7) <> 3 GROUP BY 1'),
	(7, 'expression, text and wide leaf', 'SELECT substr(kind, 1, 3) || (big % 7)::text AS k, count(*) AS n FROM ev GROUP BY 1'),
	(8, 'filter on a text leaf',        'SELECT count(*) AS n, sum(amount) AS total FROM ev WHERE kind <> ''k'' || (id % 50)::text');

-- the scan and the normal plan give the same rows
CREATE FUNCTION check_case(q text, OUT pushed bool, OUT differences bigint, OUT nrows bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
	SELECT count(*) INTO nrows FROM r_off;
	SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
	pushed := false;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		pushed := pushed OR l LIKE '%Custom Scan (SiftAgg%';
	END LOOP;
	DROP TABLE r_off, r_on;
END $$;

-- the caches as they are
SELECT n, shape, c.pushed, c.nrows, c.differences FROM cases, LATERAL check_case(q) c ORDER BY n;

-- no cache memory: the rows stay the same
SET sift.backend_cache_kb = 0;
SET sift.shared_cache_kb = 0;
SELECT n, shape, c.pushed, c.nrows, c.differences FROM cases, LATERAL check_case(q) c ORDER BY n;

-- a numeric leaf whose 3,000 distinct values lie far apart (900,000 positions), joined to a lookup
CREATE TABLE sparse (id int PRIMARY KEY, k int, amount numeric(12, 2)) WITH (autovacuum_enabled = false);
INSERT INTO sparse SELECT i, i % 40, (i % 3000) * 3 + (i % 3000) % 7 / 100.0 FROM generate_series(1, 6000) i;
CREATE INDEX sparse_col ON sparse USING sift_col (id, k, amount);
CREATE TABLE cats (id int PRIMARY KEY, category_id int) WITH (autovacuum_enabled = false);
INSERT INTO cats SELECT k, 1 + k % 6 FROM generate_series(0, 39) k;
VACUUM (FREEZE, ANALYZE) sparse, cats;
SELECT c.pushed, c.nrows, c.differences
FROM check_case('SELECT cats.category_id, sum(sparse.amount * cats.category_id) AS total FROM sparse JOIN cats ON cats.id = sparse.k GROUP BY 1') c;
DROP TABLE sparse, cats;

-- workers with no cache memory
SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SELECT n, shape, c.pushed, c.nrows, c.differences FROM cases, LATERAL check_case(q) c ORDER BY n;

RESET sift.backend_cache_kb;
RESET sift.shared_cache_kb;
DROP TABLE ev, maps, groups, cases;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
DROP FUNCTION check_case;
