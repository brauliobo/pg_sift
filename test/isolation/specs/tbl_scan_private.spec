# A scan of a sift_table goes on with the state it opened with while another scan of the same table in the same backend refreshes the
# table's cache: a cursor is half read, the table is extended, maintained (the write buffers are promoted) and vacuumed, a second scan
# reads the new directory, and the cursor returns the rows its snapshot sees.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_sp (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
	CREATE INDEX iso_sp_id ON iso_sp (id);
	INSERT INTO iso_sp SELECT g, 'row ' || md5(g::text) FROM generate_series(1, 3000) g;
	DELETE FROM iso_sp WHERE id % 10 = 0;

	CREATE FUNCTION iso_sp_open(scan text) RETURNS void LANGUAGE plpgsql AS $$
	DECLARE
		c refcursor := 'c1';
	BEGIN
		PERFORM set_config('enable_seqscan', (scan = 'seq')::text, true);
		PERFORM set_config('enable_bitmapscan', (scan = 'bmp')::text, true);
		PERFORM set_config('enable_indexscan', false::text, true);
		OPEN c FOR SELECT id FROM iso_sp WHERE id BETWEEN 1 AND 6000;
	END $$;

	CREATE FUNCTION iso_sp_fetch(n int, OUT fetched int, OUT idsum bigint) LANGUAGE plpgsql AS $$
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
	DROP TABLE iso_sp;
	DROP FUNCTION iso_sp_open(text);
	DROP FUNCTION iso_sp_fetch(int);
}

session r
step ropen_seq	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT iso_sp_open('seq'); SELECT * FROM iso_sp_fetch(1000); }
step ropen_bmp	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT iso_sp_open('bmp'); SELECT * FROM iso_sp_fetch(1000); }
step rscan	{ SELECT count(*) AS second_scan FROM iso_sp; }
step rrest	{ SELECT * FROM iso_sp_fetch(100000); }
step rend	{ COMMIT; }

session w
step wmod	{ INSERT INTO iso_sp SELECT g, 'new ' || g FROM generate_series(3001, 6000) g; DELETE FROM iso_sp WHERE id % 7 = 0; }
step wmaint	{ SELECT busy FROM sift.table_maintain('iso_sp'); }
step wvac	{ VACUUM iso_sp; }

permutation ropen_seq wmod wmaint wvac rscan rrest rend
permutation ropen_bmp wmod wmaint wvac rscan rrest rend
permutation ropen_seq wmod wmaint rscan rrest rend
permutation ropen_bmp wmod wmaint rscan rrest rend
permutation ropen_bmp wmod rscan rrest rend
