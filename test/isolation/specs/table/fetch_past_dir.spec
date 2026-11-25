# The heap check of a BM25 index on a sift_table (sift_tbl_filter_page) goes by the state its fetch scan holds: a cursor is half read, other sessions extend the
# table past the directory the scan began with (inserts the index scan does not return), promote the write buffers and vacuum, and the cursor, which asks
# for pages past its directory when its own session extends the table too, returns the rows its snapshot sees.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_fp (id int, body text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_fp SELECT g, 'common ' || CASE WHEN g % 2 = 0 THEN 'even' ELSE 'odd' END || ' row ' || g FROM generate_series(1, 3000) g;
	CREATE INDEX iso_fp_idx ON iso_fp USING sift (body);
	DELETE FROM iso_fp WHERE id % 10 = 0;

	CREATE FUNCTION iso_fp_open() RETURNS void LANGUAGE plpgsql AS $$
	DECLARE
		c refcursor := 'c1';
	BEGIN
		PERFORM set_config('enable_seqscan', 'false', true);
		PERFORM set_config('enable_bitmapscan', 'false', true);
		OPEN c FOR SELECT id FROM iso_fp WHERE body @@> 'common';
	END $$;

	CREATE FUNCTION iso_fp_fetch(n int, OUT fetched int, OUT idsum bigint) LANGUAGE plpgsql AS $$
	DECLARE
		c refcursor := 'c1';
		r int;
	BEGIN
		fetched := 0;
		idsum := 0;
		WHILE fetched < n LOOP
			FETCH c INTO r;
			EXIT WHEN NOT FOUND;
			fetched := fetched + 1;
			idsum := idsum + r;
		END LOOP;
	END $$;
}

teardown
{
	DROP TABLE iso_fp;
	DROP FUNCTION iso_fp_open();
	DROP FUNCTION iso_fp_fetch(int);
}

session r
step ropen	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT iso_fp_open(); SELECT * FROM iso_fp_fetch(1000); }
step rown	{ INSERT INTO iso_fp SELECT g, 'common own ' || g FROM generate_series(7001, 7500) g; }
step rcount	{ SELECT count(*) AS count_scan FROM iso_fp WHERE body @@> 'common'; }
step rrest	{ SELECT * FROM iso_fp_fetch(100000); }
step rend	{ COMMIT; }

session w
step wmod	{ INSERT INTO iso_fp SELECT g, 'common new ' || g FROM generate_series(3001, 6000) g; DELETE FROM iso_fp WHERE id % 7 = 0; }
step wmaint	{ SELECT busy FROM sift.table_maintain('iso_fp'); }
step wvac	{ VACUUM iso_fp; }

permutation ropen wmod wmaint rown rcount rrest rend
permutation ropen wmod wmaint wvac rown rcount rrest rend
permutation ropen rown wmod wmaint rrest rend
