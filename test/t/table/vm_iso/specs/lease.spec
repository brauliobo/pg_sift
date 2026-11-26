# The vblocks a running insert still fills (table/f2/vm/lease.c) under VACUUM: the 280 rows of s1_few are a part that leaves its vblock partly empty, and the rows a transaction then
# staged in memory in that vblock and the next keep their index entries through a VACUUM that runs before the commit, and a session whose table moved to another file (SET TABLESPACE, by itself or another session) takes a new run for its next rows, so that a vblock VACUUM made all visible
# gets no row of a transaction that rolls back. vm_ios() reads through the index only, vm_tbl() through the table only: the two are the same count unless an index entry or a bit is wrong.
# Run by test/t/table/vm.sh on a server that preloads pg_sift, with the tablespace vm_ts.

setup
{
	CREATE TABLE vm_l (id int, k int) USING sift_table;
	INSERT INTO vm_l SELECT g, g FROM generate_series(1, 1000) g;
	CREATE INDEX vm_l_k ON vm_l (k);
	CREATE FUNCTION vm_ios() RETURNS bigint LANGUAGE plpgsql SET enable_seqscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS
	$$ DECLARE n bigint; BEGIN EXECUTE 'SELECT count(*) FROM vm_l WHERE k >= 0' INTO n; RETURN n; END $$;
	CREATE FUNCTION vm_tbl() RETURNS bigint LANGUAGE plpgsql SET enable_indexonlyscan = off SET enable_indexscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS
	$$ DECLARE n bigint; BEGIN EXECUTE 'SELECT count(*) FROM vm_l WHERE k >= 0' INTO n; RETURN n; END $$;
}

setup
{
	VACUUM vm_l;
}

teardown
{
	DROP TABLE vm_l;
	DROP FUNCTION vm_ios();
	DROP FUNCTION vm_tbl();
}

session s1
step s1_few { INSERT INTO vm_l SELECT g, g FROM generate_series(2001, 2280) g; }
step s1_begin { BEGIN; }
step s1_more { INSERT INTO vm_l SELECT g, g FROM generate_series(3001, 3020) g; }
step s1_commit { COMMIT; }
step s1_move { ALTER TABLE vm_l SET TABLESPACE vm_ts; }
step s1_abort { ROLLBACK; }

session s2
step s2_move { ALTER TABLE vm_l SET TABLESPACE vm_ts; }
step s2_vac { VACUUM vm_l; }
step s2_read { SELECT vm_ios() AS ios, vm_tbl() AS tbl; }

permutation s1_few s1_begin s1_more s2_vac s1_commit s2_read
permutation s1_few s2_move s2_vac s1_begin s1_more s1_abort s2_read
permutation s1_begin s1_few s1_move s1_more s1_commit s2_vac s1_begin s1_more s1_abort s2_read
