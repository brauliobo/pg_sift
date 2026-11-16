-- Every function of the extension that takes NULL (not STRICT) is called with each argument NULL in turn and with all of them NULL, the others being a value of the type: a
-- NULL argument is an answer (NULL or an error), never a crash. sift.suggest_paths and sift._names_expr, _names_exprs and _names_option dereferenced the NULL text (signal 11).
CREATE EXTENSION pg_sift;
CREATE TABLE null_args_t (a int PRIMARY KEY, b text);
INSERT INTO null_args_t SELECT i, 'x' FROM generate_series(1, 10) i;

CREATE TEMP TABLE null_calls AS
WITH fns AS (
	SELECT p.oid, p.proname, p.proretset, p.pronargs, p.proargtypes::oid[] AS types
	FROM pg_proc p JOIN pg_depend d ON d.objid = p.oid AND d.deptype = 'e'
	JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'pg_sift'
	JOIN pg_language l ON l.oid = p.prolang AND l.lanname = 'c'
	WHERE NOT p.proisstrict AND p.pronargs > 0 AND p.prokind = 'f' AND p.proname NOT LIKE '\_test%'
		AND NOT EXISTS (SELECT 1 FROM unnest(p.proargtypes::oid[]) t WHERE t::regtype::text NOT IN
			('regclass', 'text', 'integer', 'bigint', 'name', 'text[]', 'boolean', 'xid', 'anyelement', 'bigint[]'))
)
SELECT f.oid::regproc AS fn, nul,
	format(CASE WHEN f.proretset THEN 'SELECT count(*) FROM %s(%s)' ELSE 'SELECT %s(%s)' END, f.oid::regproc,
		string_agg(CASE WHEN k = nul OR nul > f.pronargs THEN 'NULL::' || format_type(t, NULL) ELSE
			CASE t::regtype::text WHEN 'regclass' THEN '''null_args_t''::regclass' WHEN 'text' THEN '''a''' WHEN 'integer' THEN '1' WHEN 'bigint' THEN '1'
				WHEN 'name' THEN '''a''::name' WHEN 'text[]' THEN 'ARRAY[''a'']' WHEN 'boolean' THEN 'true' WHEN 'xid' THEN '''1''::xid'
				WHEN 'anyelement' THEN '1' WHEN 'bigint[]' THEN 'ARRAY[1::bigint]' END END, ', ' ORDER BY k)) AS stmt
FROM fns f CROSS JOIN LATERAL unnest(f.types) WITH ORDINALITY AS a(t, k) CROSS JOIN LATERAL generate_series(1, f.pronargs + 1) AS nul
GROUP BY f.oid, f.proname, f.proretset, f.pronargs, nul;

DO $$
DECLARE q text;
BEGIN
	FOR q IN SELECT stmt FROM null_calls ORDER BY fn::text, nul LOOP
		BEGIN
			EXECUTE q;
		EXCEPTION WHEN OTHERS THEN
			NULL;
		END;
	END LOOP;
END $$;

SELECT count(DISTINCT fn) > 10 AS functions, count(*) > 50 AS calls FROM null_calls;
SELECT sift.suggest_paths(NULL, 'b'), sift.suggest_paths('null_args_t', NULL), sift._names_expr('a', NULL, 'c'), sift._names_exprs(ARRAY['a'], 'a', NULL),
	sift._names_option('o', 'v', NULL, 'c');

DROP TABLE null_args_t;
DROP EXTENSION pg_sift;
