-- <> (strategy 6) in the operator classes of sift_col whose type has an exact = (integers, floats, booleans, text, uuid, the temporal types, numeric): an index of format major 2 answers
-- col <> value from the rows whose value is not NULL and is not the constant, which is what the aggregate scan folds without a second look. The classes of ranges and networks keep their
-- containment strategies and have no <>.
DO $do$
DECLARE
	r record;
BEGIN
	FOR r IN
		SELECT DISTINCT f.oid AS fam, n.nspname AS fn, f.opfname, ao.amoplefttype AS lt, ao.amoprighttype AS rt, ne.oprname
		FROM pg_amop ao
			JOIN pg_opfamily f ON f.oid = ao.amopfamily
			JOIN pg_namespace n ON n.oid = f.opfnamespace
			JOIN pg_am a ON a.oid = f.opfmethod AND a.amname = 'sift_col'
			JOIN pg_operator eq ON eq.oid = ao.amopopr
			JOIN pg_operator ne ON ne.oid = eq.oprnegate
		WHERE ao.amopstrategy = 3 AND ao.amoplefttype = ao.amoprighttype
			AND ao.amoplefttype IN ('int2'::regtype, 'int4'::regtype, 'int8'::regtype, 'float4'::regtype, 'float8'::regtype, 'bool'::regtype,
									'text'::regtype, 'uuid'::regtype, 'date'::regtype, 'timestamp'::regtype, 'timestamptz'::regtype, 'numeric'::regtype)
			AND NOT EXISTS (SELECT 1 FROM pg_amop x WHERE x.amopfamily = f.oid AND x.amopstrategy = 6 AND x.amoplefttype = ao.amoplefttype
																								   AND x.amoprighttype = ao.amoprighttype)
	LOOP
		EXECUTE format('ALTER OPERATOR FAMILY %I.%I USING sift_col ADD OPERATOR 6 pg_catalog.%s (%s, %s)', r.fn, r.opfname, r.oprname, r.lt::regtype, r.rt::regtype);
	END LOOP;
END
$do$;

-- && (strategy 31) of anyarray: the planner offers an index of format major 2 on an array column the clause col && ARRAY[..]; the index keeps the rows whose array is not NULL and the executor
-- tests the elements. Opt-in like the classes above: CREATE INDEX ON t USING sift_col (id, tags sift.col_array_ops).
CREATE OPERATOR CLASS sift.col_array_ops FOR TYPE anyarray USING sift_col AS OPERATOR 31 pg_catalog.&& (anyarray, anyarray);
COMMENT ON OPERATOR CLASS sift.col_array_ops USING sift_col IS 'Lets the planner offer a sift_col index of format major 2 for array && array; the index keeps the rows whose array is not NULL and the executor tests the elements.';
