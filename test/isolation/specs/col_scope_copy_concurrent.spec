# A scope copy stays exact when a transaction that changes the facts and a transaction that changes the pairs overlap: neither sees the uncommitted rows of the
# other, so each trigger would apply its change to a state the other is about to change. The triggers of the two tables exclude each other (the facts share a
# lock, the pairs take it alone): the later statement waits for the transaction of the earlier one and then reads its rows.
# requires: col

setup
{
	CREATE TABLE iso_sc_facts (id int PRIMARY KEY, dim_a_id int NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL) WITH (autovacuum_enabled = off);
	CREATE INDEX ON iso_sc_facts (dim_a_id);
	CREATE TABLE iso_sc_pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id)) WITH (autovacuum_enabled = off);
	INSERT INTO iso_sc_pairs VALUES (1, 1), (2, 1), (3, 1), (3, 2);
	INSERT INTO iso_sc_facts SELECT i, 1 + i % 4, i % 3, i FROM generate_series(1, 8) i;
	SELECT count(*) FROM (SELECT sift.scope_create('iso_sc_copy', 'iso_sc_facts', 'dim_a_id', 'iso_sc_pairs', 'scope_id')) q;
}

teardown
{
	SELECT sift.scope_drop('iso_sc_copy');
	DROP TABLE iso_sc_facts, iso_sc_pairs;
}

session s1
step s1_begin  { BEGIN; }
step s1_insert { INSERT INTO iso_sc_facts VALUES (101, 4, 1, 10), (102, 2, 1, 20); }
step s1_delete { DELETE FROM iso_sc_facts WHERE id = 3; }
step s1_commit { COMMIT; }

session s2
step s2_begin  { BEGIN; }
step s2_add    { INSERT INTO iso_sc_pairs VALUES (4, 2); }
step s2_remove { DELETE FROM iso_sc_pairs WHERE dim_a_id = 2; }
step s2_commit { COMMIT; }

session s3
step s3_verify { SELECT problem, scope_key, row_key FROM sift.scope_verify('iso_sc_copy'); }

permutation s1_begin s1_insert s2_add s1_commit s3_verify
permutation s2_begin s2_add s1_begin s1_insert s2_commit s1_commit s3_verify
permutation s1_begin s1_delete s2_add s1_commit s3_verify
permutation s1_begin s1_insert s2_remove s1_commit s3_verify
permutation s2_begin s2_remove s1_begin s1_insert s2_commit s1_commit s3_verify
