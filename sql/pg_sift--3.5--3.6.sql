\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.6'" to load this file. \quit

-- A date column compared with a timestamp or timestamptz (WHERE d < x + interval '1 day', d >= now() - interval '30 days'), and a timestamp or timestamptz column
-- compared with a date, are the same bounds of the column (the date is its midnight, in the session's zone against a timestamptz): the classes of the three types hold
-- those operators too, with btree's strategy numbers.
DO $$
DECLARE
	p text[];
	n int;
	list text;
	ops text[] := ARRAY['<', '<=', '=', '>=', '>'];
BEGIN
	FOREACH p SLICE 1 IN ARRAY ARRAY[['date', 'timestamp'], ['date', 'timestamptz'], ['timestamp', 'date'], ['timestamptz', 'date']] LOOP
		list := '';
		FOR n IN 1..5 LOOP
			list := list || format('%sOPERATOR %s pg_catalog.%s (%s, %s)', CASE WHEN n > 1 THEN ', ' ELSE '' END, n, ops[n], p[1], p[2]);
		END LOOP;
		EXECUTE format('ALTER OPERATOR FAMILY sift.col_%s_ops USING sift_col ADD %s', p[1], list);
	END LOOP;
END
$$;

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

-- The same family has the containment operators of the ranges and networks (docs/CONTAINMENT.md): a sift_col index with such a column returns the rows of
-- WHERE r @> x, r && range, ip <<= net and r = x, ip = x from the integer lanes of the column (col/shape.c) as it does for a number. Strategies: 3 =, for a range 10 @>,
-- 11 <@, 12 &&, 13 <<, 14 >>, 15 &<, 16 &>, 17 -|- and 18 @> an element, for a network 21 <<, 22 <<=, 23 >>, 24 >>=, 25 &&. An index built by 3.5 or older has no lanes
-- for them: it asks for a REINDEX, as for the lanes themselves.
ALTER OPERATOR FAMILY sift.col_ops USING sift_col ADD
	OPERATOR 3 pg_catalog.= (anyrange, anyrange), OPERATOR 10 pg_catalog.@> (anyrange, anyrange), OPERATOR 11 pg_catalog.<@ (anyrange, anyrange),
	OPERATOR 12 pg_catalog.&& (anyrange, anyrange), OPERATOR 13 pg_catalog.<< (anyrange, anyrange), OPERATOR 14 pg_catalog.>> (anyrange, anyrange),
	OPERATOR 15 pg_catalog.&< (anyrange, anyrange), OPERATOR 16 pg_catalog.&> (anyrange, anyrange), OPERATOR 17 pg_catalog.-|- (anyrange, anyrange),
	OPERATOR 18 pg_catalog.@> (anyrange, anyelement),
	OPERATOR 3 pg_catalog.= (inet, inet), OPERATOR 21 pg_catalog.<< (inet, inet), OPERATOR 22 pg_catalog.<<= (inet, inet), OPERATOR 23 pg_catalog.>> (inet, inet),
	OPERATOR 24 pg_catalog.>>= (inet, inet), OPERATOR 25 pg_catalog.&& (inet, inet);

-- sift.explain_why answers a statement with no aggregate: the row index
COMMENT ON FUNCTION sift.explain_why(text) IS 'Why each candidate path of the aggregate pushdown kept or left the normal plan for one SELECT, planned and not run: one row per arm with the first reason it gave up; a statement with no aggregate gets the row index: whether the plan reads a sift_col index and, if not, no_source, where or cost.';
