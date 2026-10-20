# sift_table under concurrent writers: row-level conflicts at READ COMMITTED (the waiter re-evaluates the updated row),
# REPEATABLE READ and SERIALIZABLE failures, row locks, uncommitted rows, unique waits and index maintenance.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_tbl (id int PRIMARY KEY, v int, note text) USING sift_table;
	INSERT INTO iso_tbl VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, 'c');
}

teardown
{
	DROP TABLE iso_tbl;
}

session s1
step s1b	{ BEGIN; }
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM iso_tbl; }
step s1ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT sum(v) FROM iso_tbl WHERE id IN (1, 2); }
step s1u	{ UPDATE iso_tbl SET v = v + 1 WHERE id = 1; }
step s1d	{ DELETE FROM iso_tbl WHERE id = 1; }
step s1i	{ INSERT INTO iso_tbl VALUES (4, 40, 'd'); }
step s1i1	{ INSERT INTO iso_tbl VALUES (10, 11, 'dup'); }
step s1w	{ UPDATE iso_tbl SET v = 0 WHERE id = 2; }
step s1lock	{ SELECT id FROM iso_tbl WHERE id = 1 FOR UPDATE; }
step s1c	{ COMMIT; }
step s1a	{ ROLLBACK; }

session s2
step s2b	{ BEGIN; }
step s2ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SELECT sum(v) FROM iso_tbl WHERE id IN (1, 2); }
step s2u	{ UPDATE iso_tbl SET v = v * 2 WHERE id = 1; }
step s2ukey	{ UPDATE iso_tbl SET id = 5 WHERE id = 1; }
step s2d	{ DELETE FROM iso_tbl WHERE id = 1; }
step s2lock	{ SELECT id FROM iso_tbl WHERE id = 1 FOR UPDATE; }
step s2qual	{ SELECT id FROM iso_tbl WHERE v = 10 FOR UPDATE; }
step s2share	{ SELECT id FROM iso_tbl WHERE id = 1 FOR SHARE; }
step s2nowait	{ SELECT id FROM iso_tbl WHERE id = 1 FOR UPDATE NOWAIT; }
step s2skip	{ SELECT id FROM iso_tbl ORDER BY id FOR UPDATE SKIP LOCKED; }
step s2i1	{ INSERT INTO iso_tbl VALUES (10, 12, 'dup2'); }
step s2w	{ UPDATE iso_tbl SET v = 0 WHERE id = 1; }
step s2ins	{ INSERT INTO iso_tbl VALUES (10, 99, 'again') ON CONFLICT (id) DO UPDATE SET v = excluded.v; }
step s2c	{ COMMIT; }
step s2sel	{ SELECT id, v, note FROM iso_tbl ORDER BY id; }

# Locking follows updated versions and rechecks the original predicate, including a multi-hop chain
permutation s1b s2b s1u s2qual s1c s2c
permutation s1b s2b s1u s2qual s1u s1c s2c

# READ COMMITTED: the second updater waits, then applies its change on the new version
permutation s1b s2b s1u s2u s1c s2c s2sel
# the waiter finds the row gone
permutation s1b s2b s1d s2u s1c s2c s2sel
# the waiter's delete finds the row updated and deletes the new version
permutation s1b s2b s1u s2d s1c s2c s2sel
# the writer rolls back: the waiter proceeds on the old version
permutation s1b s2b s1u s2u s1a s2c s2sel
# an updater of the key column moves the row; the other waiter sees it gone
permutation s1b s2b s2ukey s1u s2c s1c s2sel

# REPEATABLE READ: first-updater-wins
permutation s1rr s2b s2u s2c s1u s1a s2sel
permutation s1rr s2b s2d s2c s1u s1a s2sel

# SERIALIZABLE: write skew
permutation s1ser s2ser s1w s2w s1c s2c s2sel

# row locks
permutation s1b s2b s1lock s2lock s1c s2c
permutation s1b s2b s1lock s2nowait s1c s2c
permutation s1b s2b s1lock s2skip s1c s2c
permutation s1b s2b s1lock s2share s1c s2c
permutation s1b s2b s1lock s2u s1c s2c s2sel

# uncommitted rows are invisible; unique waits for the inserter
permutation s1b s1i s2sel s1c s2sel
permutation s1b s1i1 s2i1 s1a s2sel
permutation s1b s1i1 s2i1 s1c s2sel
permutation s1b s1i1 s2ins s1c s2sel
