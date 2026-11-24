-- FUNCTION 1 of the typed families of sift_col is the comparison function of the btree family of the same pair of types (btint4cmp, bttextcmp, date_cmp, ...): the standard two-argument
-- support function an ordering access method and a row comparison ((a, b) > ($1, $2)) ask the family for. The placeholder of sift.col_ops (one argument) stays where it is: it is released,
-- and only DDL looks at it. Safe to run again.
DO $do$
DECLARE
	r record;
BEGIN
	FOR r IN
		SELECT DISTINCT f.oid AS fam, n.nspname AS fn, f.opfname, ao.amoplefttype AS lt, ao.amoprighttype AS rt, bp.amproc::regprocedure AS cmp
		FROM pg_amop ao
			JOIN pg_opfamily f ON f.oid = ao.amopfamily
			JOIN pg_namespace n ON n.oid = f.opfnamespace
			JOIN pg_am a ON a.oid = f.opfmethod AND a.amname = 'sift_col'
			JOIN pg_opclass bc ON bc.opcintype = ao.amoplefttype AND bc.opcdefault
			JOIN pg_am ba ON ba.oid = bc.opcmethod AND ba.amname = 'btree'
			JOIN pg_amproc bp ON bp.amprocfamily = bc.opcfamily AND bp.amprocnum = 1
								 AND bp.amproclefttype = ao.amoplefttype AND bp.amprocrighttype = ao.amoprighttype
		WHERE n.nspname = 'sift' AND f.opfname IN ('col_integer_ops', 'col_float_ops', 'col_bool_ops', 'col_date_ops', 'col_timestamp_ops', 'col_timestamptz_ops',
												 'col_text_ops', 'col_uuid_ops', 'col_numeric_ops', 'col_bpchar_ops')
			AND ao.amopstrategy = 3
			AND NOT EXISTS (SELECT 1 FROM pg_amproc x WHERE x.amprocfamily = f.oid AND x.amprocnum = 1
																			AND x.amproclefttype = ao.amoplefttype AND x.amprocrighttype = ao.amoprighttype)
	LOOP
		EXECUTE format('ALTER OPERATOR FAMILY %I.%I USING sift_col ADD FUNCTION 1 (%s, %s) %s', r.fn, r.opfname, r.lt::regtype, r.rt::regtype, r.cmp);
	END LOOP;
END
$do$;
