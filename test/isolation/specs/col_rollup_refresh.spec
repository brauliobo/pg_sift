# The on-demand refresh of the rollups (docs/COL-PREAGG.md section 7) against writers and maintenance: a SELECT that scans exact units for lack of
# their rollup queues their segments and writes nothing; the refresh rewrites a queued segment under the maintenance lock, so a writer in flight does not
# wait for it and its rows are read as before.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_ru (id int, lines jsonb, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_ru SELECT g, (SELECT jsonb_agg(jsonb_build_object('a', 'a' || (g + e) % 4, 'b', 'b' || (g * e) % 3, 'q', (g * e) % 9) ORDER BY e)
	                              FROM generate_series(1, g % 4 + 1) e), repeat('p', 100) FROM generate_series(1, 24000) g;
	-- the braces of a path would end the block of the spec
	DO $$ BEGIN EXECUTE format('CREATE INDEX iso_ru_idx ON iso_ru USING sift_col (id, lines) WITH (paths = %L, write_buffer_limit = 4096, rollup = ''off'')',
	                           'lines[]' || chr(123) || 'a text, b text, q int4' || chr(125)); END $$;
}

setup
{
	VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) iso_ru;
}

setup
{
	ALTER INDEX iso_ru_idx SET (rollup = 'lines[].a, lines[].b');
	CREATE FUNCTION iso_ru_q(OUT units int, OUT pre int, OUT miss int, OUT queued int, OUT agree bool) LANGUAGE plpgsql AS $$
	DECLARE l text; q text := 'SELECT e.a, e.b, count(*), sum(e.q) FROM iso_ru d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, q int) GROUP BY 1, 2'; x text; y text;
	BEGIN
		PERFORM set_config('sift.col_rollup_worker', 'off', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		PERFORM set_config('cpu_tuple_cost', '0.5', true);
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
			pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
			miss := coalesce(substring(l from 'Units Without The Rollup: (\d+)')::int, miss);
			queued := coalesce(substring(l from 'Rollup Refresh Queued: (\d+)')::int, queued);
		END LOOP;
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO x;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO y;
		agree := x IS NOT DISTINCT FROM y;
	END $$;
}

teardown
{
	DROP TABLE iso_ru;
	DROP FUNCTION iso_ru_q();
}

session s1
step s1q	{ SELECT pre = 0 AS none_from_rollups, miss = units AS all_missing, queued = units AS all_queued, agree FROM iso_ru_q(); }
step s1r	{ SELECT pre = units AS all_from_rollups, miss AS missing, agree FROM iso_ru_q(); }
step s1d	{ SELECT pre < units AS some_scanned, pre > 0 AS some_from_rollups, miss AS missing, agree FROM iso_ru_q(); }

session s2
step s2b	{ BEGIN; }
step s2u	{ UPDATE iso_ru SET lines = jsonb_build_array(jsonb_build_object('a', 'a1', 'b', 'b1', 'q', 5)) WHERE id <= 1000; }
step s2c	{ COMMIT; }

session s3
step s3q	{ SELECT count(*) > 0 AS waiting FROM sift.col_rollup_requests; }
step s3r	{ SELECT sift.col_rollup_refresh('iso_ru_idx') > 0 AS refreshed; }
step s3n	{ SELECT count(*) AS waiting FROM sift.col_rollup_requests; }

# the SELECT queues, the refresh serves, the next SELECT answers from the rollups
permutation s1q s3q s3r s3n s1r
# a writer in flight does not wait for the refresh and its update, committed afterwards, makes units dirty, which are scanned and not asked for
permutation s1q s2b s2u s3r s2c s1d
# a second refresh finds nothing left to rewrite
permutation s1q s3r s3r s3n s1r
