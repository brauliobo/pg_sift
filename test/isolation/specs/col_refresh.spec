# sift.col_refresh (docs/COL-PREAGG.md section 7) against readers and writers: the rewrite of a segment replaces it under the maintenance lock, so a
# reader in a transaction, a writer in flight and a second refresh do not wait for it, and every answer is the one of the heap before, during and after.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_rf (id int, kind int, val bigint, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_rf SELECT g, g % 6, (g * 977) % 100000 - 50000, repeat('p', 100) FROM generate_series(1, 12000) g;
	CREATE INDEX iso_rf_idx ON iso_rf USING sift_col (id, kind, val) WITH (write_buffer_limit = 4096, cube_minmax = false, superunit = 0);
	INSERT INTO iso_rf SELECT g, g % 6, (g * 977) % 100000 - 50000, repeat('p', 100) FROM generate_series(12001, 18000) g;
	SELECT sift.col_promote('iso_rf_idx');
	INSERT INTO iso_rf SELECT g, g % 6, (g * 977) % 100000 - 50000, repeat('p', 100) FROM generate_series(18001, 24000) g;
	SELECT sift.col_promote('iso_rf_idx');
}

setup
{
	VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) iso_rf;
}

setup
{
	ALTER INDEX iso_rf_idx SET (cube_minmax = true);
	CREATE FUNCTION iso_rf_q(OUT units int, OUT pre int, OUT agree bool) LANGUAGE plpgsql AS $$
	DECLARE l text; q text := 'SELECT kind, min(val), max(val), count(*) FROM iso_rf GROUP BY kind'; x text; y text;
	BEGIN
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		PERFORM set_config('seq_page_cost', '10000', true);
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
			pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
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
	DROP TABLE iso_rf;
	DROP FUNCTION iso_rf_q();
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1q	{ SELECT pre < units AS some_scanned, agree FROM iso_rf_q(); }
step s1a	{ SELECT agree FROM iso_rf_q(); }
step s1r	{ SELECT pre = units AS all_from_groups, agree FROM iso_rf_q(); }
step s1d	{ SELECT pre < units AS some_scanned, pre > 0 AS some_from_groups, agree FROM iso_rf_q(); }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN; }
step s2i	{ INSERT INTO iso_rf SELECT g, g % 6, g, repeat('p', 100) FROM generate_series(24001, 24500) g; }
step s2u	{ UPDATE iso_rf SET val = val + 1 WHERE id <= 1000; }
step s2c	{ COMMIT; }

session s3
step s3p	{ SELECT remaining AS stale FROM sift.col_refresh('iso_rf_idx', 0); }
step s3r	{ SELECT refreshed, remaining FROM sift.col_refresh('iso_rf_idx'); }
step s3o	{ SELECT refreshed, remaining FROM sift.col_refresh('iso_rf_idx', 1); }

# the units are scanned for lack of the min and max, the refresh finds the three segments stale and the next query answers from the groups
permutation s3p s1q s3r s1r
# a reader in a transaction is not waited for, and answers as the heap does before and after the rewrite
permutation s1b s1a s3r s1a s1c s1r
# a writer in flight does not wait for the refresh; what it inserts is read after it commits
permutation s1q s2b s2i s3r s2c s1d
# the same for an update, whose rows make their units dirty (scanned) and are not asked of the refresh
permutation s1q s2b s2u s3r s2c s1d
# one segment at a time, then the rest, then nothing
permutation s3o s1d s3r s1r s3r
