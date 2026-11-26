# VACUUM FULL and CLUSTER of a sift_table rewrite the rows a snapshot that is still open may read: a REPEATABLE READ or SERIALIZABLE transaction that took its snapshot before a
# delete, an insert and an update still sees the table as it was after the rewrite (the deleted rows, the old values, none of the new rows), a new snapshot sees what
# committed, and the rewritten table takes VACUUM and new writes. CLUSTER in the transaction that deleted and updated rows keeps the versions it replaced for the older snapshot.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
	SET sift.test_create_format1 = off;
	CREATE TABLE iso_rw (id int, k int, v int) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO iso_rw SELECT g, g, 0 FROM generate_series(1, 600) g;
	CREATE INDEX iso_rw_k ON iso_rw (k);
	CREATE TABLE iso_rw_other (x int);
}

teardown
{
	DROP TABLE iso_rw, iso_rw_other;
}

session s1
step s1_rr { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_rw_other; }
step s1_ser { BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT count(*) FROM iso_rw_other; }
step s1_read { SELECT count(*), sum(k), sum(v), max(id) FROM iso_rw; }
step s1_end { COMMIT; }

session s2
step s2_write { DELETE FROM iso_rw WHERE id % 3 = 0; INSERT INTO iso_rw SELECT g, g, 0 FROM generate_series(601, 700) g; UPDATE iso_rw SET v = v + 1 WHERE id % 5 = 0; }
step s2_full { VACUUM FULL iso_rw; }
step s2_cluster { CLUSTER iso_rw USING iso_rw_k; }
step s2_begin { BEGIN; }
step s2_own { DELETE FROM iso_rw WHERE id % 3 = 0; UPDATE iso_rw SET v = v + 1 WHERE id % 5 = 0; }
step s2_commit { COMMIT; }
step s2_vac { VACUUM iso_rw; }
step s2_more { UPDATE iso_rw SET v = v + 10 WHERE id % 7 = 0; DELETE FROM iso_rw WHERE id % 11 = 0; }
step s2_read { SELECT count(*), sum(k), sum(v), max(id) FROM iso_rw; }

permutation s1_rr s2_write s2_full s1_read s1_end s2_read s2_vac s2_more s2_read
permutation s1_ser s2_write s2_cluster s1_read s2_more s1_read s1_end s2_vac s2_read
permutation s2_write s2_full s2_read s1_rr s1_read s1_end
permutation s1_rr s2_begin s2_own s2_cluster s2_read s2_commit s1_read s1_end s2_read
