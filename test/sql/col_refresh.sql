-- sift.col_refresh (docs/COL-PREAGG.md section 7): a change of the options that decide the pre-aggregates (cube_minmax, preagg, rollup) takes effect for
-- the units written afterwards; the refresh rewrites the segments written before, one at a time, with the same rows and the same answers. The planner prices
-- the index for the units that hold the section: the share of them that have it is a counter read by sift._test_col_pre_stats. Read back by sift.col_verify; the damaged-blob
-- regress of the census it takes is in col_preagg_minmax.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE rf (id int, kind int, grp text, val bigint, amt numeric(10, 2), pad text) WITH (autovacuum_enabled = off);
INSERT INTO rf SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'), CASE WHEN g % 7 = 0 THEN NULL ELSE (g * 977) % 100000 - 50000 END,
                      ((g * 91) % 20000) / 100.0 - 50, repeat('p', 100) FROM generate_series(1, 20000) g;
CREATE INDEX rf_idx ON rf USING sift_col (id, kind, grp, val, amt) WITH (rollup_dims = 'off', write_buffer_limit = 4096, cube_minmax = false, superunit = 0);
-- four segments: the build and three promotions
INSERT INTO rf SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'), (g * 977) % 100000 - 50000, ((g * 91) % 20000) / 100.0 - 50, repeat('p', 100) FROM generate_series(20001, 25000) g;
SELECT sift.col_promote('rf_idx') >= 0 AS promoted;
INSERT INTO rf SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'), (g * 977) % 100000 - 50000, ((g * 91) % 20000) / 100.0 - 50, repeat('p', 100) FROM generate_series(25001, 30000) g;
SELECT sift.col_promote('rf_idx') >= 0 AS promoted;
INSERT INTO rf SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'), (g * 977) % 100000 - 50000, ((g * 91) % 20000) / 100.0 - 50, repeat('p', 100) FROM generate_series(30001, 35000) g;
SELECT sift.col_promote('rf_idx') >= 0 AS promoted;
VACUUM (freeze, disable_page_skipping) rf;
SELECT count(*) AS segments FROM sift._test_col_formats('rf_idx');

-- what a query does: the units answered from the groups, and whether the scan and the normal plan agree
CREATE FUNCTION rf_q(q text, OUT units int, OUT pre int, OUT agree bool) LANGUAGE plpgsql AS $$
DECLARE l text; x text; y text;
BEGIN
	PERFORM set_config('seq_page_cost', '10000', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
	END LOOP;
	pre := coalesce(pre, 0);
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO x;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO y;
	agree := x IS NOT DISTINCT FROM y;
END $$;
SELECT md5(string_agg(x::text, E'\n' ORDER BY x::text)) AS answer FROM (SELECT kind, min(val), max(val), min(amt), max(amt), count(*) FROM rf GROUP BY kind) x \gset before_
SELECT md5(string_agg(x::text, E'\n' ORDER BY x::text)) AS answer FROM (SELECT grp, sum(val), count(*) FROM rf GROUP BY grp) x \gset before2_

-- nothing is stale yet: the options and the segments agree
SELECT * FROM sift.col_refresh('rf_idx', 0);
SELECT units > 0 AS has_units, pre = 0 AS min_max_scanned, agree FROM rf_q('SELECT kind, min(val), max(val), min(amt), max(amt), count(*) FROM rf GROUP BY kind');

-- the planner counts the units that hold the histogram of a leaf and its min and max: every unit has the first, none the second
SELECT dim_units = units AS all_have_hist, mm_units = 0 AS none_have_minmax FROM sift._test_col_pre_stats('rf_idx') WHERE leaf = 1;

-- the option is set: the segments written before do not hold the min and max, a count shows how many, and the dry run changes nothing
ALTER INDEX rf_idx SET (cube_minmax = true);
SELECT dim_units = units AS all_have_hist, mm_units = 0 AS none_have_minmax FROM sift._test_col_pre_stats('rf_idx') WHERE leaf = 1;
SELECT * FROM sift.col_refresh('rf_idx', 0);
SELECT pre = 0 AS min_max_scanned, agree FROM rf_q('SELECT kind, min(val), max(val), min(amt), max(amt), count(*) FROM rf GROUP BY kind');
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('rf_idx');

-- two of the four, then the rest: the answers are the same at each step, and the planner counts the units that have the min and max: some, then all
SELECT * FROM sift.col_refresh('rf_idx', 2);
SELECT mm_units > 0 AND mm_units < units AS some_have_minmax FROM sift._test_col_pre_stats('rf_idx') WHERE leaf = 1;
SELECT pre > 0 AND pre < units AS some_from_groups, agree FROM rf_q('SELECT kind, min(val), max(val), min(amt), max(amt), count(*) FROM rf GROUP BY kind');
SELECT md5(string_agg(x::text, E'\n' ORDER BY x::text)) = :'before_answer' AS same_answer FROM (SELECT kind, min(val), max(val), min(amt), max(amt), count(*) FROM rf GROUP BY kind) x;
SELECT sift.col_verify('rf_idx', true);
SELECT * FROM sift.col_refresh('rf_idx');
SELECT pre = units AS all_from_groups, agree FROM rf_q('SELECT kind, min(val), max(val), min(amt), max(amt), count(*) FROM rf GROUP BY kind');
SELECT md5(string_agg(x::text, E'\n' ORDER BY x::text)) = :'before_answer' AS same_answer FROM (SELECT kind, min(val), max(val), min(amt), max(amt), count(*) FROM rf GROUP BY kind) x;
SELECT md5(string_agg(x::text, E'\n' ORDER BY x::text)) = :'before2_answer' AS same_answer FROM (SELECT grp, sum(val), count(*) FROM rf GROUP BY grp) x;
SELECT mm_units = units AS all_have_minmax FROM sift._test_col_pre_stats('rf_idx') WHERE leaf = 1;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors, count(*) AS segments FROM sift._test_col_formats('rf_idx');
SELECT sift.col_verify('rf_idx', true);
SELECT rows = 35000 AS rows_kept FROM sift.col_index_info('rf_idx');

-- the rows are the same: a second call finds nothing to do
SELECT * FROM sift.col_refresh('rf_idx');

-- the option is switched off again: the min and max go with the rewrite, and so do the dead rows of the segments it rewrites (as in a merge)
ALTER INDEX rf_idx SET (cube_minmax = false);
DELETE FROM rf WHERE id % 40 = 0;
VACUUM (freeze, index_cleanup on, disable_page_skipping) rf;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('rf_idx');
SELECT * FROM sift.col_refresh('rf_idx', 0);
SELECT * FROM sift.col_refresh('rf_idx');
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('rf_idx');
SELECT mm_units = 0 AS none_have_minmax FROM sift._test_col_pre_stats('rf_idx') WHERE leaf = 1;
SELECT pre = 0 AS min_max_scanned, agree FROM rf_q('SELECT kind, min(val), max(val), min(amt), max(amt), count(*) FROM rf GROUP BY kind');
SELECT rows = (SELECT count(*) FROM rf) AS rows_are_the_heaps FROM sift.col_index_info('rf_idx');
SELECT sift.col_verify('rf_idx', true);

-- a setting that keeps older libraries reading: a segment of a minor above the setting is left as written (a rewrite would drop what only that minor
-- holds), and an index written at the setting is rewritten at it, so that the older library reads it
ALTER INDEX rf_idx SET (cube_minmax = true);
SELECT * FROM sift.col_refresh('rf_idx');
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('rf_idx');
SET sift.col_format_minor = 4;
SELECT * FROM sift.col_refresh('rf_idx', 0);
SET sift.col_format_minor = 3;
SELECT * FROM sift.col_refresh('rf_idx');
CREATE INDEX rf3_idx ON rf USING sift_col (id, kind, grp, val, amt) WITH (rollup_dims = 'off', write_buffer_limit = 4096, cube_minmax = false, superunit = 0);
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('rf3_idx');
ALTER INDEX rf3_idx SET (preagg = 'cube(kind: val)');
SELECT (SELECT remaining FROM sift.col_refresh('rf3_idx', 0)) > 0 AS stale;
SELECT refreshed > 0 AS rewrote, remaining FROM sift.col_refresh('rf3_idx');
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('rf3_idx');
SELECT sift.col_verify('rf3_idx', true);
RESET sift.col_format_minor;
DROP INDEX rf3_idx;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('rf_idx');
SELECT sift.col_verify('rf_idx', true);

-- the cube of a configured dim and measures: a change of the option reaches the old segments, the answers do not change
ALTER INDEX rf_idx SET (cube_minmax = false, preagg = 'cube(kind: val, amt; grp: val)');
SELECT (SELECT remaining FROM sift.col_refresh('rf_idx', 0)) > 0 AS stale;
SELECT refreshed > 0 AS rewrote, remaining FROM sift.col_refresh('rf_idx');
SELECT * FROM sift.col_refresh('rf_idx');
SELECT pre = units AS all_from_groups, agree FROM rf_q('SELECT kind, sum(val), sum(amt), count(*) FROM rf GROUP BY kind');
SELECT pre = units AS all_from_groups, agree FROM rf_q('SELECT grp, sum(val), count(*) FROM rf GROUP BY grp');
ALTER INDEX rf_idx SET (preagg = 'cube(kind: val)');
SELECT (SELECT remaining FROM sift.col_refresh('rf_idx', 0)) > 0 AS stale;
SELECT refreshed > 0 AS rewrote FROM sift.col_refresh('rf_idx');
SELECT pre = 0 AS group_by_grp_scanned, agree FROM rf_q('SELECT grp, sum(val), count(*) FROM rf GROUP BY grp');
SELECT pre = units AS group_by_kind_from_groups, agree FROM rf_q('SELECT kind, sum(val), count(*) FROM rf GROUP BY kind');
SELECT sift.col_verify('rf_idx', true);
ALTER INDEX rf_idx RESET (preagg);
SELECT refreshed > 0 AS rewrote FROM sift.col_refresh('rf_idx');
SELECT * FROM sift.col_refresh('rf_idx');
SELECT sift.col_verify('rf_idx', true);

-- the rollups of a list: the option is set after the build, the segments written before get them
CREATE TABLE rfl (id int, lines jsonb, pad text) WITH (autovacuum_enabled = off);
INSERT INTO rfl SELECT g, (SELECT jsonb_agg(jsonb_build_object('a', 'a' || (g + e) % 4, 'b', 'b' || (g * e) % 3, 'q', (g * e) % 9) ORDER BY e)
                           FROM generate_series(1, g % 4 + 1) e), repeat('p', 100) FROM generate_series(1, 12000) g;
DO $$ BEGIN EXECUTE format('CREATE INDEX rfl_idx ON rfl USING sift_col (id, lines) WITH (rollup_dims = ''off'', paths = %L, write_buffer_limit = 4096, rollup = ''off'')',
                           'lines[]' || chr(123) || 'a text, b text, q int4' || chr(125)); END $$;
INSERT INTO rfl SELECT g, (SELECT jsonb_agg(jsonb_build_object('a', 'a' || (g + e) % 4, 'b', 'b' || (g * e) % 3, 'q', (g * e) % 9) ORDER BY e)
                           FROM generate_series(1, g % 4 + 1) e), repeat('p', 100) FROM generate_series(12001, 16000) g;
SELECT sift.col_promote('rfl_idx') >= 0 AS promoted;
VACUUM (freeze, disable_page_skipping) rfl;
ALTER INDEX rfl_idx SET (rollup = 'lines[].a, lines[].b');
SELECT (SELECT remaining FROM sift.col_refresh('rfl_idx', 0)) = 2 AS two_stale;
SELECT * FROM sift.col_refresh('rfl_idx', 1);
SELECT * FROM sift.col_refresh('rfl_idx');
SELECT * FROM sift.col_refresh('rfl_idx');
SELECT pre = units AS all_from_rollups, agree FROM rf_q('SELECT e.a, e.b, count(*), sum(e.q) FROM rfl d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, q int) GROUP BY 1, 2');
SELECT sift.col_verify('rfl_idx', true);
ALTER INDEX rfl_idx SET (rollup = 'off');
SELECT * FROM sift.col_refresh('rfl_idx');
SELECT pre = 0 AS none_from_rollups, agree FROM rf_q('SELECT e.a, e.b, count(*), sum(e.q) FROM rfl d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, q int) GROUP BY 1, 2');
SELECT sift.col_verify('rfl_idx', true);

-- what it refuses
SELECT sift.col_refresh('rf');
SELECT sift.col_refresh('rf_idx', -1);
SELECT sift.col_refresh(NULL);

DROP TABLE rf, rfl;
DROP FUNCTION rf_q(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
