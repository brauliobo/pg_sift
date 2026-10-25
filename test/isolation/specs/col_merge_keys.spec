# The merge of every segment of an index with key_columns = 'auto' writes the option (the key list the churn made worth keeping): a writer in flight when the option
# changes, and a reader in a transaction, do not wait for it, the rows of both are read as before, and the index verifies after.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_mk (id int, set_id int, v int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_mk SELECT i, i / 500, i % 100, repeat('p', 200) FROM generate_series(0, 29999) i;
}

setup { VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) iso_mk; }

setup
{
	CREATE INDEX iso_mk_idx ON iso_mk USING sift_col (id, set_id, v);
	UPDATE iso_mk SET set_id = (id::bigint * 7919 % 600)::int;
}

setup { VACUUM iso_mk; }

setup
{
	SELECT sift.col_promote('iso_mk_idx');
	UPDATE iso_mk SET set_id = (id::bigint * 104729 % 600)::int;
}

setup { VACUUM iso_mk; }

setup
{
	SELECT sift.col_promote('iso_mk_idx');
	CREATE FUNCTION iso_mk_q(OUT agree bool) LANGUAGE plpgsql AS $$
	DECLARE q text := 'SELECT set_id, count(DISTINCT id), count(*) FROM iso_mk WHERE set_id IN (5, 77, 410) GROUP BY 1'; x text; y text;
	BEGIN
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO x;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO y;
		agree := x IS NOT DISTINCT FROM y;
	END $$;
}

teardown
{
	DROP TABLE iso_mk;
	DROP FUNCTION iso_mk_q();
}

session s1
step s1b	{ BEGIN; }
step s1i	{ INSERT INTO iso_mk SELECT g, g % 600, g % 100, repeat('p', 200) FROM generate_series(30001, 30400) g; }
step s1j	{ INSERT INTO iso_mk SELECT g, g % 600, g % 100, repeat('p', 200) FROM generate_series(30401, 30800) g; }
step s1c	{ COMMIT; }

session s2
step s2r	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS rows_seen FROM iso_mk; }
step s2q	{ SELECT agree FROM iso_mk_q(); }
step s2c	{ COMMIT; }

session s3
step s3o	{ SELECT coalesce(reloptions::text, '') AS before FROM pg_class WHERE relname = 'iso_mk_idx'; }
step s3m	{ SELECT sift.col_merge('iso_mk_idx', true) > 0 AS merged; }
step s3p	{ SELECT reloptions::text AS after FROM pg_class WHERE relname = 'iso_mk_idx'; }
step s3v	{ SELECT 1 AS verified FROM (SELECT sift.col_verify('iso_mk_idx', true)) v; }

permutation s3o s1b s1i s2r s3m s1j s1c s2q s2c s3p s3v
