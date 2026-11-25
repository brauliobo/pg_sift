# UNIQUE on a sift index: the insert of a key another transaction has inserted or deleted waits for it, and goes on
# or fails by what that transaction does; keys that differ do not wait.
# requires: build insert

setup
{
	CREATE TABLE iso_uq (id int, body text);
	INSERT INTO iso_uq VALUES (1, 'existing key');
	CREATE UNIQUE INDEX iso_uq_idx ON iso_uq USING sift (body);
}

teardown
{
	DROP TABLE iso_uq;
}

session s1
step s1b	{ BEGIN; }
step s1i	{ INSERT INTO iso_uq VALUES (2, 'contended key'); }
step s1d	{ DELETE FROM iso_uq WHERE id = 1; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }

session s2
step s2b	{ BEGIN; }
step s2i	{ INSERT INTO iso_uq VALUES (3, 'contended key'); }
step s2e	{ INSERT INTO iso_uq VALUES (4, 'existing key'); }
step s2o	{ INSERT INTO iso_uq VALUES (5, 'other key'); }
step s2c	{ COMMIT; }

session s3
step s3check	{ SELECT id, body FROM iso_uq ORDER BY id; }

# the second insert waits, then fails: the first committed
permutation s1b s2b s1i s2i(s1c) s1c s2c s3check
# the first rolled back: the second goes on
permutation s1b s2b s1i s2i(s1r) s1r s2c s3check
# another key does not wait
permutation s1b s2b s1i s2o s1c s2c s3check
# a key deleted by an open transaction is free when it commits, and taken when it rolls back
permutation s1b s2b s1d s2e(s1c) s1c s2c s3check
permutation s1b s2b s1d s2e(s1r) s1r s2c s3check
