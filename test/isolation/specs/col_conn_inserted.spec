# sift_conn_inserted_since (docs/COL-CONNECT.md): another session's insert into the parent index that is still open counts (its transaction may commit), the same insert after it
# aborted does not, and after it committed it does again; a stamp taken after the commit does not see it.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_conn_dim (id int PRIMARY KEY, category_id int NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO iso_conn_dim SELECT i, i % 10 FROM generate_series(1, 500) i;
	CREATE INDEX iso_conn_dim_c ON iso_conn_dim USING sift_col (id, category_id);
	SELECT sift._test_conn_enable('iso_conn_dim_c');
	CREATE TABLE iso_conn_stamp (lin int8, hor xid);
}

teardown
{
	DROP TABLE iso_conn_dim, iso_conn_stamp;
}

session s1
step s1stamp	{ INSERT INTO iso_conn_stamp SELECT lineage, horizon FROM sift._test_conn_take('iso_conn_dim_c'); }
step s1ask	{ SELECT sift._test_conn_inserted_since('iso_conn_dim_c', lin, hor) AS inserted_since FROM iso_conn_stamp; }
step s1restamp	{ DELETE FROM iso_conn_stamp; INSERT INTO iso_conn_stamp SELECT lineage, horizon FROM sift._test_conn_take('iso_conn_dim_c'); }

session s2
step s2b	{ BEGIN; }
step s2i	{ INSERT INTO iso_conn_dim VALUES (1001, 1); }
step s2flush	{ SAVEPOINT x; }
step s2commit	{ COMMIT; }
step s2abort	{ ROLLBACK; }

permutation s1stamp s1ask s2b s2i s2flush s1ask s2abort s1ask
permutation s1stamp s2b s2i s2flush s1ask s2commit s1ask s1restamp s1ask
