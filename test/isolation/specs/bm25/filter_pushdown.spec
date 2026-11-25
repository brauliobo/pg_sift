# Ranked scans that take a bitmap of the other quals as their filter, while another session
# updates the filter column, deletes, inserts and vacuums: the cursor answers with its snapshot,
# whatever the bitmap saw. The bitmap is built when the cursor is first fetched, after the snapshot.
# requires: build insert vacuum scan verify score

setup
{
	CREATE TABLE iso_flt (id int, tenant int, body text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_flt SELECT g, g % 10, 'common ' || CASE WHEN g % 3 = 0 THEN 'rare ' ELSE '' END || 'text ' || g
		FROM generate_series(1, 3000) g;
	CREATE INDEX iso_flt_idx ON iso_flt USING sift (body);
	CREATE INDEX iso_flt_ten ON iso_flt (tenant);

	-- the drained cursor rows are the reference matches under the filter, in non-increasing score order
	CREATE FUNCTION iso_flt_ok(q text, flt text) RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		ref int[];
	BEGIN
		EXECUTE format('SELECT array_agg(id ORDER BY id) FROM iso_flt WHERE sift_ref.match(body, %L) AND (%s)', q, flt) INTO ref;
		RETURN (SELECT array_agg(id ORDER BY id) FROM pg_temp.fetched) IS NOT DISTINCT FROM ref
			AND NOT EXISTS (SELECT 1 FROM pg_temp.fetched a JOIN pg_temp.fetched b ON b.n = a.n + 1 WHERE b.score > a.score);
	END $$;

	-- does the ranked scan take the filter as a bitmap
	CREATE FUNCTION iso_flt_plan(q text, flt text) RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		l text;
		p text := '';
	BEGIN
		IF current_setting('iso.mode', true) IS DISTINCT FROM 'sift' THEN
			RETURN true;
		END IF;
		FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM iso_flt WHERE body @@> %L AND (%s)
			ORDER BY sift.score(ctid) DESC', q, flt) LOOP
			p := p || l;
		END LOOP;
		RETURN p ~ 'SiftScan' AND p ~ 'Bitmap Index Scan';
	END $$;

	-- a statement of its own, with the filter pushed down, against the reference
	CREATE FUNCTION iso_flt_now(q text, flt text) RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE
		got int[];
		ref int[];
	BEGIN
		PERFORM set_config('enable_seqscan', 'off', true);
		EXECUTE format('SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM iso_flt WHERE body @@> %L AND (%s)
			ORDER BY sift.score(ctid) DESC) t', q, flt) INTO got;
		PERFORM set_config('enable_seqscan', 'on', true);
		EXECUTE format('SELECT array_agg(id ORDER BY id) FROM iso_flt WHERE sift_ref.match(body, %L) AND (%s)', q, flt) INTO ref;
		RETURN got IS NOT DISTINCT FROM ref;
	END $$;
}

teardown
{
	DROP FUNCTION iso_flt_ok(text, text), iso_flt_plan(text, text), iso_flt_now(text, text);
	DROP TABLE iso_flt;
}

session s1
setup		{ CREATE TEMP TABLE IF NOT EXISTS fetched (n serial, id int, score real); TRUNCATE fetched RESTART IDENTITY; SET enable_seqscan = off; SET sift.filter_pushdown = force; }
step s1plan	{ SELECT iso_flt_plan('common', 'tenant < 3') AS pushed_down; }
step s1ranked	{ BEGIN ISOLATION LEVEL REPEATABLE READ; DECLARE c CURSOR FOR SELECT id, sift.score(ctid) AS score FROM iso_flt WHERE body @@> 'common' AND tenant < 3 ORDER BY sift.score(ctid) DESC; }
step s1f	{ SELECT iso_fetch('c', 50) AS fetched; }
step s1rest	{ SELECT iso_fetch('c', 100000) > 0 AS more; }
step s1check	{ SELECT iso_flt_ok('common', 'tenant < 3') AS ok; }
step s1c	{ COMMIT; }
step s1now	{ SELECT iso_flt_now('common OR rare', 'tenant < 3') AS agree, iso_flt_now('rare', 'tenant IN (1, 2, 11)') AS agree_rare; }

session s2
setup		{ SET sift.filter_pushdown = force; }
step s2b	{ BEGIN; }
step s2c	{ COMMIT; }
step s2i	{ INSERT INTO iso_flt SELECT g, g % 4, 'common reused rare ' || g FROM generate_series(5001, 6500) g; }
step s2u	{ UPDATE iso_flt SET tenant = tenant + 5 WHERE tenant < 3 AND id % 7 = 0; UPDATE iso_flt SET tenant = 1 WHERE tenant = 6; }
step s2d	{ DELETE FROM iso_flt WHERE id % 4 = 0; }
step s2v	{ VACUUM iso_flt; }
step s2check	{ SELECT iso_flt_now('common OR rare', 'tenant < 3') AS agree, iso_flt_now('rare', 'tenant IN (1, 2, 11)') AS agree_rare, iso_verify('iso_flt_idx') AS verified; }

# the filter column changes, rows go and come, and VACUUM removes entries, between the snapshot and the first fetch
permutation s1plan s1ranked s2u s2d s2i s2v s1f s1rest s1check s1c s2check
# the same after the first batch
permutation s1plan s1ranked s1f s2u s2d s2v s2i s1rest s1check s1c s2check
# entries that VACUUM removed before the scan, and their reused slots
permutation s2d s2u s2v s2i s1plan s1ranked s1f s1rest s1check s1c s2check
# a writer that has not committed is invisible, and visible once it has
permutation s2b s2i s2u s1plan s1now s2c s1now s2check
