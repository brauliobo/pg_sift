# REINDEX of a sift_col index that tracks changes does not hold the table against its readers: it keeps the triggers of its log when they are as the option makes them and writes the baseline
# again, so it takes the ShareLock of any index build and not the lock that drops a trigger (a session that has the table open for reading made it wait, and a read that held the table
# while the index was rebuilt deadlocked with it).
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_ri (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_ri SELECT i, i % 10 FROM generate_series(1, 2000) i;
	CREATE INDEX iso_ri_col ON iso_ri USING sift_col (id, v);
}

teardown
{
	DROP TABLE iso_ri;
}

session reader
step rlock	{ BEGIN; LOCK TABLE iso_ri IN ACCESS SHARE MODE; }
step rread	{ SELECT count(*) AS rows FROM iso_ri; }
step rend	{ COMMIT; }

session builder
step reindex	{ REINDEX INDEX iso_ri_col; }

permutation rlock reindex rread rend
