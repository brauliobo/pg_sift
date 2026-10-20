# The xmax of a row of a sift_table that another transaction deletes, updates or locks and has not ended is that transaction's, as in a heap
# twin written by the same statements: a reader that joins the two tables sees the same xmax and xmin for the same row, in the write buffer and
# in a column group (VACUUM FREEZE moved the rows there, where the sift_table reads FrozenTransactionId and a heap its raw xid), and again after
# the transaction aborted or committed.
# requires: tam

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE sa_h (id int PRIMARY KEY, v text);
	CREATE TABLE sa_s (id int PRIMARY KEY, v text) USING sift_table WITH (autovacuum_enabled = false);
	INSERT INTO sa_h SELECT g, 'row' FROM generate_series(1, 4) g;
	INSERT INTO sa_s SELECT g, 'row' FROM generate_series(1, 4) g;
}

teardown
{
	DROP TABLE sa_h, sa_s;
}

session writer
step wbegin	{ BEGIN; }
step wdelete	{ DELETE FROM sa_h WHERE id = 1; DELETE FROM sa_s WHERE id = 1; }
step wupdate	{ UPDATE sa_h SET v = 'upd' WHERE id = 2; UPDATE sa_s SET v = 'upd' WHERE id = 2; }
step wlock	{ SELECT id FROM sa_h WHERE id = 3 FOR SHARE; SELECT id FROM sa_s WHERE id = 3 FOR SHARE; }
step wcommit	{ COMMIT; }
step wabort	{ ROLLBACK; }

session freezer
step vfreeze	{ VACUUM FREEZE sa_h, sa_s; }

session reader
step rxmax	{ SELECT h.id, h.xmax::text <> '0' AS heap_has_xmax, h.xmax = s.xmax AS same_xmax, h.xmin = s.xmin AS same_xmin
		FROM sa_h h JOIN sa_s s USING (id) ORDER BY id; }
step rfrozen	{ SELECT h.id, h.xmax = s.xmax AS same_xmax, s.xmin::text = '2' AS sift_frozen FROM sa_h h JOIN sa_s s USING (id) ORDER BY id; }

permutation wbegin wdelete wupdate wlock rxmax wabort rxmax
permutation wbegin wdelete wupdate wlock rxmax wcommit rxmax
permutation vfreeze wbegin wdelete wupdate wlock rfrozen wabort rfrozen
