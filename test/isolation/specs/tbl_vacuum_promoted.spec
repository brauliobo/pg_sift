# A VACUUM of a sift_table that waited for the maintenance lock while another backend promoted the write buffers it had listed must clear the dead
# rows of the groups the promotion made before it drops their tombstones: an old version that an UPDATE committed long ago stays invisible.
# requires: tam gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_tv (id int PRIMARY KEY, v int NOT NULL, pad text) USING sift_table;
	INSERT INTO iso_tv SELECT i, 0, 'p' FROM generate_series(1, 3000) i;
	UPDATE iso_tv SET v = 1;
}

teardown
{
	DROP TABLE iso_tv;
}

session s1
setup		{ SET sift.table_auto_defrag = off; SET sift.async_maintenance = off; }
step s1hold	{ BEGIN; SELECT sift._test_tbl_hold_maint('iso_tv'); }
step s1maint	{ SELECT sift._test_tbl_promote_held('iso_tv'); }
step s1end	{ COMMIT; }

session s2
setup		{ SET sift.table_auto_defrag = off; SET sift.async_maintenance = off; }
step s2vacuum	{ VACUUM iso_tv; }
step s2check	{ SELECT v, count(*) AS versions, count(DISTINCT id) AS ids, min(id), max(id) FROM iso_tv GROUP BY v ORDER BY v; }

session s3
setup		{ SET sift.table_auto_defrag = off; SET sift.async_maintenance = off; }
step s3fill	{ INSERT INTO iso_tv SELECT 10000 + i, 2, repeat('x', 300) FROM generate_series(1, 30000) i; }

permutation s1hold s2vacuum s3fill s1maint s1end s2check
