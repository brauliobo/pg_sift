# sift.upgrade_table rewrites the versions of a Format 1 sift_table that a snapshot still open may read, as VACUUM FULL does: a REPEATABLE READ transaction that took its snapshot before a
# delete, an insert and an update still sees the table as it was after the upgrade, a new snapshot sees what committed, and the upgraded table takes VACUUM and new writes. An upgrade in
# the transaction that deleted and updated rows keeps the versions it replaced for the older snapshot.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = on;
	CREATE TABLE iso_up (id int, k int, v int) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_up SELECT g, g, 0 FROM generate_series(1, 600) g;
	CREATE INDEX iso_up_k ON iso_up (k);
	CREATE TABLE iso_up_other (x int);
}

teardown
{
	DROP TABLE iso_up, iso_up_other;
}

session s1
step s1_rr { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_up_other; }
step s1_read { SELECT count(*), sum(k), sum(v), max(id) FROM iso_up; }
step s1_end { COMMIT; }

session s2
step s2_write { DELETE FROM iso_up WHERE id % 3 = 0; INSERT INTO iso_up SELECT g, g, 0 FROM generate_series(601, 700) g; UPDATE iso_up SET v = v + 1 WHERE id % 5 = 0; }
step s2_upgrade { SET client_min_messages = warning; SELECT sift.upgrade_table('iso_up'); }
step s2_begin { BEGIN; }
step s2_own { DELETE FROM iso_up WHERE id % 3 = 0; UPDATE iso_up SET v = v + 1 WHERE id % 5 = 0; }
step s2_commit { COMMIT; }
step s2_vac { VACUUM iso_up; }
step s2_more { UPDATE iso_up SET v = v + 10 WHERE id % 7 = 0; DELETE FROM iso_up WHERE id % 11 = 0; }
step s2_read { SELECT count(*), sum(k), sum(v), max(id), (sift.index_format('iso_up')).major FROM iso_up; }

permutation s1_rr s2_write s2_upgrade s1_read s1_end s2_read s2_vac s2_more s2_read
permutation s2_write s2_upgrade s2_read s1_rr s1_read s1_end
permutation s1_rr s2_begin s2_own s2_upgrade s2_read s2_commit s1_read s1_end s2_read
