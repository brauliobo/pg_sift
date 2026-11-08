# The first child index connected to a parent index makes the journal of the parent, which holds the writers of the parent table out; that starts after the child has read its table
# (the build waits to enter the write phase for the advisory lock sift.test_phase_gate + 3, which another session holds) and lasts to the end of the transaction. A second child of the
# same parent finds the journal made and holds nobody out.
# requires: build gates col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_cl_dim (id int PRIMARY KEY, category_id int NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO iso_cl_dim SELECT i, i % 10 FROM generate_series(1, 500) i;
	CREATE INDEX iso_cl_dim_c ON iso_cl_dim USING sift_col (id, category_id);
	CREATE TABLE iso_cl_fact (id int PRIMARY KEY, dim_id int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = off);
	INSERT INTO iso_cl_fact SELECT i, 1 + i % 500, i FROM generate_series(1, 2000) i;
}

teardown
{
	DROP TABLE iso_cl_fact, iso_cl_dim;
}

session s1
step s1begin	{ BEGIN; }
step s1build	{ SET sift.test_phase_gate = 1000;
		  CREATE INDEX iso_cl_fact_c ON iso_cl_fact USING sift_col (id, dim_id, v) WITH (connect = 'dim_id -> iso_cl_dim(id) carries category_id'); }
step s1build2	{ CREATE INDEX iso_cl_fact_c2 ON iso_cl_fact USING sift_col (id, dim_id) WITH (connect = 'dim_id -> iso_cl_dim(id) carries category_id'); }
step s1commit	{ COMMIT; }

session s2
step s2h	{ SELECT pg_advisory_lock(1003); }
step s2r	{ SELECT pg_advisory_unlock(1003); }

session s3
step s3upd	{ UPDATE iso_cl_dim SET category_id = category_id + 1 WHERE id = 7; }
step s3upd2	{ UPDATE iso_cl_dim SET category_id = category_id + 1 WHERE id = 8; }

permutation s2h s1begin s1build s3upd s2r s3upd2 s1commit s1begin s1build2 s3upd s1commit
