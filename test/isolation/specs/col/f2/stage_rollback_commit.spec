# A transaction whose rows a rollback to a savepoint dropped commits without opening the index again: the rollback let go of the lock the rows took on the index, and a session that drops the
# index meanwhile does not hold the commit up.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_srb (id int, v int);
	CREATE INDEX iso_srb_idx ON iso_srb USING sift_col (id, v);
}

teardown
{
	DROP TABLE iso_srb;
}

session w
step w_rolled_back { BEGIN; SAVEPOINT a; INSERT INTO iso_srb VALUES (1, 1); ROLLBACK TO a; }
step w_commit { COMMIT; }

session d
step d_drop { BEGIN; DROP INDEX iso_srb_idx; }
step d_end { ROLLBACK; }

permutation w_rolled_back d_drop w_commit d_end
