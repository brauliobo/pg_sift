# CREATE INDEX CONCURRENTLY on a sift_table whose index entries do not fit in maintenance_work_mem (200000 rows against its 1 MB minimum): the rows written while the first pass is
# running are not in the index, and the validation inserts them by merging the sorted addresses of the table with the sorted addresses of the index. The first pass is held in the
# index expression on an advisory lock while another session inserts and deletes rows; the index then holds exactly the rows of the table.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_cic (id int, k int) USING sift_table;
}

setup
{
	INSERT INTO iso_cic SELECT i, i FROM generate_series(1, 200000) i;
}

setup
{
	CREATE FUNCTION iso_cic_f(k int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$
	BEGIN
		PERFORM pg_advisory_lock_shared(7301);
		PERFORM pg_advisory_unlock_shared(7301);
		RETURN k;
	END $$;
}

teardown
{
	DROP TABLE iso_cic;
	DROP FUNCTION iso_cic_f(int);
}

session s1
setup	{ SET maintenance_work_mem = '1MB'; SET client_min_messages = warning; }
step s1cic	{ CREATE INDEX CONCURRENTLY iso_cic_idx ON iso_cic (iso_cic_f(k)); }

session s2
setup	{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s2lock	{ SELECT pg_advisory_lock(7301); }
step s2unlock	{ SELECT pg_advisory_unlock(7301); }
step s2valid	{ SELECT indisvalid, indisready FROM pg_index WHERE indexrelid = 'iso_cic_idx'::regclass; }
step s2index	{ SELECT count(*) AS indexed FROM iso_cic WHERE k >= 0 AND iso_cic_f(k) >= 0; }
step s2point	{ SELECT count(*) AS late_rows FROM iso_cic WHERE iso_cic_f(k) BETWEEN 200001 AND 200050; }
step s2gone	{ SELECT count(*) AS deleted_rows FROM iso_cic WHERE iso_cic_f(k) BETWEEN 1 AND 10; }

session s3
step s3ins	{ INSERT INTO iso_cic SELECT 200000 + i, 200000 + i FROM generate_series(1, 50) i; }
step s3del	{ DELETE FROM iso_cic WHERE id <= 10; }
step s3count	{ SELECT count(*) AS rows_of_table FROM iso_cic; }

permutation s2lock s1cic s3ins s3del s2unlock s2valid s2index s2point s2gone s3count
