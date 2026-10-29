-- The leaf statistics block: a segment of enough units keeps what the planner reads of its leaves (the counts, the sums, the range of
-- every leaf and a sample of the zones) after its last section, so a backend's first plan reads the block, not the zones of every unit.
-- sift.test_limits lowers the segment size from which the block is written, so a few units reach it. The index keeps the answers, the
-- block is checked against the zones (sift.col_verify), and a segment too small for it, or an index that was built without it, reads the zones.
CREATE EXTENSION pg_sift;
SET default_statistics_target = 10000;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ls_facts (id int PRIMARY KEY, grp int NOT NULL, amount numeric(12,2), label text, ts timestamptz, tags jsonb, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ls_facts SELECT i, i % 50, CASE WHEN i % 7 = 0 THEN NULL ELSE (i % 1000) / 4.0 END, 'l' || i % 40, timestamptz '2024-01-01' + i * interval '1 minute',
	jsonb_build_object('a', i % 9, 'b', 'x' || i % 5), repeat('p', 150) FROM generate_series(1, 60000) i;

-- below the default size, no block
CREATE INDEX ls_small ON ls_facts USING sift_col (id, grp, amount, label, ts, tags) WITH (paths = 'tags.a int8');
SELECT count(*) > 1 AS several_segments_or_units, bool_or(lstats) AS with_block FROM sift._test_col_formats('ls_small'::regclass) ;
SELECT sift.col_verify('ls_small', true);
DROP INDEX ls_small;

SET sift.test_limits = 'lst_min_units=2';
CREATE INDEX ls_col ON ls_facts USING sift_col (id, grp, amount, label, ts, tags) WITH (paths = 'tags.a int8');
VACUUM (FREEZE, ANALYZE) ls_facts;
SELECT bool_and(lstats) AS every_segment_has_the_block, count(*) AS segments FROM sift._test_col_formats('ls_col'::regclass);
SELECT sift.col_verify('ls_col', true);

CREATE FUNCTION chk(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE 'CREATE TEMP TABLE r_on AS ' || q;
	SET sift.enable_agg = off;
	EXECUTE 'CREATE TEMP TABLE r_off AS ' || q;
	RESET sift.enable_agg;
	n := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN n;
END $$;

-- the same answers with the block as without the pushdown
SELECT chk('SELECT grp, count(*) n, count(amount) na, sum(amount) s, min(ts) lo, max(ts) hi FROM ls_facts GROUP BY 1') AS differing_rows;
SELECT chk('SELECT label, count(DISTINCT grp) g, max(id) m FROM ls_facts WHERE ts >= timestamptz ''2024-01-20'' GROUP BY 1') AS differing_rows;
SELECT chk('SELECT (tags->>''a'')::int a, count(*) n, avg(amount) m FROM ls_facts WHERE grp < 10 GROUP BY 1') AS differing_rows;

-- more rows, a merge and a rebuild: the block follows the segment, and verify checks it
INSERT INTO ls_facts SELECT i, i % 50, (i % 1000) / 4.0, 'l' || i % 40, timestamptz '2024-01-01' + i * interval '1 minute', jsonb_build_object('a', i % 9, 'b', 'x' || i % 5), repeat('p', 150)
	FROM generate_series(60001, 90000) i;
VACUUM (FREEZE, ANALYZE) ls_facts;
SELECT sift.col_verify('ls_col', true);
SELECT chk('SELECT grp, count(*) n, sum(amount) s, max(id) m FROM ls_facts GROUP BY 1') AS differing_rows;
REINDEX INDEX ls_col;
SELECT bool_and(lstats) AS every_segment_has_the_block FROM sift._test_col_formats('ls_col'::regclass);
SELECT sift.col_verify('ls_col', true);
SELECT chk('SELECT grp, count(*) n, sum(amount) s, max(id) m FROM ls_facts GROUP BY 1') AS differing_rows;

-- a segment under the limit is written without it
RESET sift.test_limits;
REINDEX INDEX ls_col;
SELECT bool_or(lstats) AS with_block FROM sift._test_col_formats('ls_col'::regclass);
SELECT sift.col_verify('ls_col', true);

DROP FUNCTION chk(text);
DROP TABLE ls_facts;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
