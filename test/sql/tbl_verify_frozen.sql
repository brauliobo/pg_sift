-- A group written by a committed COPY carries its writer's xid; VACUUM freezes it (the owner clears the flag of the version section, the section
-- stays in the file). sift._test_tbl_verify then has no versions to check, and the scans read the group as frozen: the answers are the heap's.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET sift.table_bulk_groups = on;

CREATE TABLE twin AS SELECT g AS id, g % 7 AS grp, 'row ' || g AS v, lpad(g::text, 100, '0') AS pad FROM generate_series(1, 20000) g;
CREATE TABLE t (id int, grp int, v text, pad text) USING sift_table;
COPY t FROM PROGRAM 'seq 1 20000 | awk ''{ printf "%d\t%d\trow %d\t%0100d\n", $1, $1 % 7, $1, $1 }''';

CREATE FUNCTION answers(tbl regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || $q$:$q$ || sum(id) || $q$:$q$ || md5(string_agg(grp::text || v || pad, $q$,$q$ ORDER BY id)) FROM %s', tbl) INTO r;
	RETURN r;
END $$;

-- written by the COPY: unfrozen groups, versions that agree with their summaries
SELECT sift._test_tbl_tagged('t') > 0 AS unfrozen_groups, sift._test_tbl_verify('t') AS verified_before;
VACUUM t;
-- frozen by VACUUM: no group with a writer left, the rows verify and answer as the heap twin
SELECT sift._test_tbl_tagged('t') AS unfrozen_groups_after, sift._test_tbl_verify('t') AS verified_after;
SELECT answers('t') = answers('twin') AS same_rows;
SELECT count(*) AS differing_groups FROM (
	(SELECT grp, count(*), sum(id) FROM t GROUP BY grp EXCEPT SELECT grp, count(*), sum(id) FROM twin GROUP BY grp)
	UNION ALL
	(SELECT grp, count(*), sum(id) FROM twin GROUP BY grp EXCEPT SELECT grp, count(*), sum(id) FROM t GROUP BY grp)) d;

DROP TABLE t, twin;
DROP FUNCTION answers(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
