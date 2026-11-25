# The visibility map of a sift_table (table/f2/vm/vm.h) under concurrent sessions: an index-only scan of a secondary index must answer what the table does while INSERT, UPDATE (a
# column delta and a key change), DELETE, a prepared DELETE, TRUNCATE and VACUUM interleave. vm_ios() reads through the index only, vm_tbl() through the table only; the two
# columns of a read are the same count unless a bit of the map is true of a row that is not. A snapshot that began before a delete keeps the rows it saw.
# Run by test/t/tbl_vm.sh on a server that preloads pg_sift (the records of the map are those of a resource manager): a server that does not never sets a bit.

setup
{
	CREATE TABLE vm_t (id int, k int, v int, pad text) USING sift_table;
	INSERT INTO vm_t SELECT g, g, 0, md5(g::text) FROM generate_series(1, 6000) g;
	CREATE INDEX vm_t_k ON vm_t (k);
	CREATE FUNCTION vm_ios() RETURNS bigint LANGUAGE plpgsql SET enable_seqscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS
	$$ DECLARE n bigint; BEGIN EXECUTE 'SELECT count(*) FROM vm_t WHERE k >= 0' INTO n; RETURN n; END $$;
	CREATE FUNCTION vm_tbl() RETURNS bigint LANGUAGE plpgsql SET enable_indexonlyscan = off SET enable_indexscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS
	$$ DECLARE n bigint; BEGIN EXECUTE 'SELECT count(*) FROM vm_t WHERE k >= 0' INTO n; RETURN n; END $$;
}

setup
{
	VACUUM ANALYZE vm_t;
}

teardown
{
	DROP TABLE vm_t;
	DROP FUNCTION vm_ios();
	DROP FUNCTION vm_tbl();
}

session s1
step s1_snap { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM vm_t; }
step s1_read { SELECT vm_ios() AS ios, vm_tbl() AS tbl; }
step s1_end { COMMIT; }

session s2
step s2_del { DELETE FROM vm_t WHERE id % 7 = 0; }
step s2_upd { UPDATE vm_t SET v = v + 1 WHERE id % 5 = 0; }
step s2_key { UPDATE vm_t SET k = k + 100000 WHERE id % 11 = 0; }
step s2_ins { INSERT INTO vm_t SELECT g, g, 0, 'n' FROM generate_series(6001, 6500) g; }
step s2_begin { BEGIN; }
step s2_del_open { DELETE FROM vm_t WHERE id % 3 = 0; }
step s2_prep { PREPARE TRANSACTION 'vm_iso'; }
step s2_commitp { COMMIT PREPARED 'vm_iso'; }
step s2_commit { COMMIT; }
step s2_trunc { TRUNCATE vm_t; }
step s2_refill { INSERT INTO vm_t SELECT g, g, 0, 'r' FROM generate_series(1, 3000) g; }

session s3
step s3_vac { VACUUM vm_t; }
step s3_read { SELECT vm_ios() AS ios, vm_tbl() AS tbl; }

permutation s2_del s3_vac s3_read
permutation s1_snap s2_del s3_vac s1_read s1_end s3_read
permutation s2_begin s2_del_open s3_vac s3_read s2_commit s3_read
permutation s2_begin s2_del_open s2_prep s3_vac s3_read s2_commitp s3_read s3_vac s3_read
permutation s2_upd s3_vac s3_read s2_key s3_read s3_vac s3_read
permutation s2_ins s3_vac s2_del s3_read s3_vac s3_read
permutation s2_trunc s2_refill s3_vac s3_read s2_del s3_read
