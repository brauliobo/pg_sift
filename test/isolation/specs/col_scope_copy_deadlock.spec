# Two transactions that each change the facts and then the pairs of a scope copy deadlock: the writers of the facts share the lock of the copy and a writer of the pairs
# takes it alone, so each waits for the other's shared hold. PostgreSQL ends exactly one with a deadlock error (the one to retry), the other commits, and the copy
# is exact after it (the pairs first in both transactions cannot deadlock). Documented in docs/INDEX-RECIPES.md, 'Keys above the fact table'.
# requires: col

setup
{
	CREATE TABLE iso_dl_facts (id int PRIMARY KEY, dim_a_id int NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL) WITH (autovacuum_enabled = off);
	CREATE TABLE iso_dl_pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id)) WITH (autovacuum_enabled = off);
	INSERT INTO iso_dl_pairs VALUES (1, 1), (2, 1), (3, 1), (3, 2);
	INSERT INTO iso_dl_facts SELECT i, 1 + i % 4, i % 3, i FROM generate_series(1, 8) i;
	SELECT count(*) FROM (SELECT sift.scope_create('iso_dl_copy', 'iso_dl_facts', 'dim_a_id', 'iso_dl_pairs', 'scope_id')) q;
}

teardown
{
	SELECT sift.scope_drop('iso_dl_copy');
	DROP TABLE iso_dl_facts, iso_dl_pairs;
}

session s1
step s1_begin  { BEGIN; }
step s1_fact   { INSERT INTO iso_dl_facts VALUES (101, 4, 1, 10); }
step s1_pair   { INSERT INTO iso_dl_pairs VALUES (4, 2); }
step s1_commit { COMMIT; }

session s2
step s2_begin  { BEGIN; }
step s2_fact   { INSERT INTO iso_dl_facts VALUES (102, 2, 1, 20); }
step s2_pair   { INSERT INTO iso_dl_pairs VALUES (2, 2); }
step s2_commit { COMMIT; }

session s3
step s3_verify { SELECT problem, scope_key, row_key FROM sift.scope_verify('iso_dl_copy'); }

permutation s1_begin s2_begin s1_fact s2_fact s1_pair s2_pair(s1_pair) s1_commit s2_commit s3_verify
# the pairs first in both transactions cannot deadlock: the second waits for the first to commit
permutation s1_begin s2_begin s1_pair s2_pair(s1_commit) s1_fact s1_commit s2_fact s2_commit s3_verify
