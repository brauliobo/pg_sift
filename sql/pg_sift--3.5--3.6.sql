\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.6'" to load this file. \quit

-- A sift_col index whose columns carry the default class sift.col_ops (CREATE INDEX .. USING sift_col (col)) answers the filters that return rows as well as the
-- aggregates: the family of the default class has the operators of every typed class (docs/COL-SCAN.md), so the planner and the executor match WHERE id = 5,
-- IN lists, ranges and LIKE 'abc%' to such a column. The classes are catalog entries over the same bytes: an index built before gains this with no REINDEX.
DO $$
DECLARE
	r record;
BEGIN
	FOR r IN SELECT o.amopstrategy AS strat, n.nspname, p.oprname, o.amoplefttype::regtype AS lt, o.amoprighttype::regtype AS rt
			 FROM pg_amop o JOIN pg_opfamily f ON f.oid = o.amopfamily JOIN pg_am a ON a.oid = f.opfmethod
			 JOIN pg_operator p ON p.oid = o.amopopr JOIN pg_namespace n ON n.oid = p.oprnamespace
			 WHERE a.amname = 'sift_col' AND f.opfname <> 'col_ops' ORDER BY f.opfname, o.amoplefttype, o.amoprighttype, o.amopstrategy
	LOOP
		EXECUTE format('ALTER OPERATOR FAMILY sift.col_ops USING sift_col ADD OPERATOR %s %I.%s (%s, %s)', r.strat, r.nspname, r.oprname, r.lt, r.rt);
	END LOOP;
END
$$;
