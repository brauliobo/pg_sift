-- The storage of a copy: the table itself, or its partitions (pg_partition_tree has no row for a table that is not partitioned)
CREATE FUNCTION sift.scope_relations(copy regclass) RETURNS SETOF oid LANGUAGE sql STABLE
	AS $$ SELECT copy::oid UNION SELECT relid FROM pg_partition_tree(copy) WHERE isleaf $$;

COMMENT ON FUNCTION sift.scope_relations(regclass) IS 'Internal: the relations that store a scope copy (the table, or its partitions)';
COMMENT ON FUNCTION sift.scope_partition(text, bigint) IS 'Internal: the partition of a scope key in a partitioned scope copy, created when it is missing';

-- A name for the triggers of a copy: stable text, not the oid, so a restore keeps it
CREATE FUNCTION sift.scope_trigger_name(copy text, kind text) RETURNS name LANGUAGE sql IMMUTABLE PARALLEL SAFE
	RETURN format('sift_scope_%s_%s', left(md5(copy), 12), kind)::name;

COMMENT ON FUNCTION sift.scope_trigger_name(text, text) IS 'Internal: the name of one trigger of a scope copy';

-- An option of the table's sift_col index that names columns is restricted to the columns a scope copy carries: _scope_split cuts a list outside braces, parentheses and quotes, _scope_lead is
-- the column a name starts with (doc.a, items[].k, "Q"), _scope_restrict gives NULL when nothing is left and keeps a word (auto, off, all) and an option that names no column.
CREATE FUNCTION sift._scope_split(val text, sep text) RETURNS text[] LANGUAGE sql IMMUTABLE STRICT AS $$
	SELECT array_agg(trim(e)) FROM regexp_split_to_table(val, sep || '(?=(?:[^"]*"[^"]*")*[^"]*$)(?![^{(]*[})])') e $$;
CREATE FUNCTION sift._scope_lead(entry text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT COALESCE(replace(m[1], '""', '"'), m[2]) FROM regexp_match(ltrim(entry, '+ '), '^(?:"((?:[^"]|"")*)"|([^.\[ :=>-]*))') m $$;
CREATE FUNCTION sift._scope_restrict_rollup(entry text, cols name[]) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	WITH p AS (SELECT sift._scope_split(split_part(entry, ':', 1), ',') dims, NULLIF(trim(substr(entry, NULLIF(strpos(entry, ':'), 0) + 1)), '') meas),
		 m AS (SELECT array_to_string(array_agg(e) FILTER (WHERE sift._scope_lead(e) = ANY (cols)), ', ') kept FROM p, unnest(sift._scope_split(p.meas, ',')) e)
	SELECT CASE WHEN EXISTS (SELECT 1 FROM p, unnest(p.dims) d WHERE sift._scope_lead(d) <> ALL (cols)) THEN NULL WHEN (SELECT meas FROM p) IS NULL THEN entry
				ELSE (SELECT array_to_string(dims, ', ') || ': ' || kept FROM p, m) END $$;
CREATE FUNCTION sift._scope_restrict(opt text, val text, cols name[]) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE
		WHEN lower(trim(val)) IN ('', 'auto', 'off', 'on', 'all', 'none', 'default') OR (opt = 'preagg' AND val !~ '^\s*cube\s*\(') OR opt <> ALL (ARRAY['paths', 'compression', 'preagg', 'rollup', 'rollup_leaves', 'rollup_columns', 'calendar_columns', 'hll_columns', 'return_columns']) THEN val
		WHEN opt = 'compression' THEN (SELECT string_agg(e, '; ') FROM unnest(sift._scope_split(val, ';')) e WHERE strpos(e, '=') = 0 OR sift._scope_lead(split_part(e, '=', 1)) = ANY (cols))
		WHEN opt = 'rollup_columns' THEN sift._scope_restrict_rollup(val, cols)
		WHEN opt = 'preagg' THEN (SELECT 'cube(' || string_agg(r, '; ') || ')' FROM unnest(sift._scope_split(regexp_replace(val, '^\s*cube\s*\((.*)\)\s*$', '\1'), ';')) e,
								  LATERAL (SELECT sift._scope_restrict_rollup(e, cols) r) x WHERE r IS NOT NULL)
		ELSE (SELECT string_agg(e, ', ') FROM unnest(sift._scope_split(val, ',')) e WHERE sift._scope_lead(e) = ANY (cols)) END $$;
-- The options a copy's index inherits from the index of its table as 'name = value, ..': those the caller did not name, restricted to the carried columns; one with nothing left is
-- not inherited and a NOTICE lists it. A value that starts with 'auto:' is what the build read from the documents: the copy reads its own; those keyed on the table's own key are derived again.
CREATE FUNCTION sift._scope_inherit(fidx oid, cols name[], named text[]) RETURNS text LANGUAGE plpgsql AS $$
DECLARE kept text; dropped text;
BEGIN
	SELECT string_agg(format('%s = %L', o.n, q.r), ', ' ORDER BY i) FILTER (WHERE q.r IS NOT NULL), string_agg(o.n, ', ' ORDER BY i) FILTER (WHERE q.r IS NULL) INTO kept, dropped
		FROM pg_class c, unnest(c.reloptions) WITH ORDINALITY u(x, i), LATERAL (SELECT split_part(x, '=', 1) n, substr(x, strpos(x, '=') + 1) v) o, LATERAL (SELECT sift._scope_restrict(o.n, o.v, cols) r) q
		WHERE c.oid = fidx AND x !~ '^[^=]*=auto:' AND o.n <> ALL (named) AND o.n = ANY (string_to_array('paths,compression,preagg,rollup,rollup_dims,rollup_leaves,rollup_columns,rollup_on_demand,calendar_columns,cluster_advice,cube_minmax,dict_lookup,array_lengths,hll_columns,return_columns,superunit,write_buffer_limit,dead_rewrite_pct,vacuum_truncate', ','));
	IF dropped IS NOT NULL THEN RAISE NOTICE 'scope_create: % of the index of the table name columns the copy does not carry and are not inherited', dropped
		USING DETAIL = 'The copy chooses them again from its own columns.', HINT = 'Carry the columns with columns => ARRAY[..], or give the option in options.'; END IF;
	RETURN kept;
END $$;

CREATE FUNCTION sift.scope_create(copy text, source regclass, fk name, scope_relation regclass, scope_key name, columns name[] DEFAULT NULL,
								  partition boolean DEFAULT false, member name DEFAULT NULL, order_column name DEFAULT NULL,
								  options text DEFAULT NULL, max_copies numeric DEFAULT 1.8) RETURNS bigint
	LANGUAGE plpgsql AS $$
DECLARE
	src       text := sift.scope_qualified(source);
	rel       text := sift.scope_qualified(scope_relation);
	cname     text;
	pk        name[];
	cols      name[];
	mult      float8;
	multi     float8;
	maxk      int;
	est       float8;
	key_type  text;
	ord       name := order_column;
	defs      text;
	tr        text;
	rows      bigint;
	base      text;
	opts      text;
	fidx      oid;
	mb        numeric;
	kept      text;
	named     text[] := ARRAY(SELECT m[1] FROM regexp_matches(regexp_replace(COALESCE(options, ''), '''([^'']|'''')*''', '', 'g'), '([a-z_]+)\s*=', 'g') m);
BEGIN
	member := COALESCE(member, fk);
	IF max_copies IS NULL OR max_copies < 1 THEN RAISE EXCEPTION 'scope_create: max_copies must be 1 or more, not %', COALESCE(max_copies::text, 'NULL') USING ERRCODE = 'invalid_parameter_value'; END IF;
	IF to_regclass(copy) IS NOT NULL THEN
		RAISE EXCEPTION 'scope_create: relation "%" already exists', copy USING ERRCODE = 'duplicate_table';
	END IF;
	IF (SELECT relkind FROM pg_class WHERE oid = source) NOT IN ('r', 'p') THEN
		RAISE EXCEPTION 'scope_create: "%" is not a table', src USING ERRCODE = 'wrong_object_type';
	END IF;
	IF EXISTS (SELECT 1 FROM pg_subscription_rel r WHERE r.srrelid IN (source, scope_relation)) THEN
		RAISE EXCEPTION 'scope_create: a table is the target of a logical replication subscription'
			USING DETAIL = 'The apply worker changes rows without firing statement triggers, so the copy would miss the changes.', ERRCODE = 'feature_not_supported';
	END IF;
	SELECT array_agg(a.attname ORDER BY k.o) INTO pk FROM pg_index i
		CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY k(n, o) JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.n
		WHERE i.indrelid = source AND i.indisprimary;
	IF pk IS NULL THEN
		RAISE EXCEPTION 'scope_create: table "%" has no primary key', src USING DETAIL = 'The primary key identifies the rows of the copy that a change of the table touches.', ERRCODE = 'feature_not_supported';
	END IF;
	-- the scope relation holds each pair once
	IF NOT EXISTS (SELECT 1 FROM pg_index i WHERE i.indrelid = scope_relation AND i.indisunique AND i.indpred IS NULL AND i.indnkeyatts = 2 AND
				   (SELECT array_agg(a.attname::text ORDER BY a.attname) FROM unnest(i.indkey::int2[]) k(n) JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.n)
				   = ARRAY[LEAST(member::text, scope_key::text), GREATEST(member::text, scope_key::text)]) THEN
		RAISE EXCEPTION 'scope_create: table "%" has no unique index on (%, %)', rel, member, scope_key
			USING DETAIL = 'A scope relation holds each (member, scope key) pair once: a duplicate pair would give a row two copies in one key.', ERRCODE = 'feature_not_supported';
	END IF;
	-- a pair that comes in finds the rows of its member by an index on the foreign key: one probe for each pair, where a table without it would be read once for each
	IF NOT EXISTS (SELECT 1 FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid JOIN pg_am am ON am.oid = ic.relam JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = i.indkey[0]
				   WHERE i.indrelid = source AND i.indisvalid AND i.indpred IS NULL AND am.amname = 'btree' AND a.attname = fk) THEN
		RAISE EXCEPTION 'scope_create: table "%" has no btree index that starts with "%"', src, fk USING ERRCODE = 'feature_not_supported',
			DETAIL = 'A pair that comes into the scope relation finds the rows of its member by an index on the foreign key; without it each pair would read the whole table.', HINT = format('CREATE INDEX ON %s (%I)', src, fk);
	END IF;
	SELECT format_type(a.atttypid, NULL) INTO key_type FROM pg_attribute a WHERE a.attrelid = scope_relation AND a.attname = scope_key AND NOT a.attisdropped;
	IF key_type IS NULL OR key_type NOT IN ('smallint', 'integer', 'bigint') THEN
		RAISE EXCEPTION 'scope_create: scope key "%" of "%" is not an integer column', scope_key, rel USING ERRCODE = 'datatype_mismatch';
	END IF;
	IF EXISTS (SELECT 1 FROM pg_attribute a, pg_attribute b WHERE a.attrelid = source AND a.attname = fk AND b.attrelid = scope_relation AND b.attname = member
			   AND a.atttypid <> b.atttypid) THEN
		RAISE EXCEPTION 'scope_create: "%" of "%" and "%" of "%" differ in type', fk, src, member, rel USING ERRCODE = 'datatype_mismatch';
	END IF;
	-- the sift_col index of the table gives the copy its columns and its options
	SELECT i.indexrelid INTO fidx FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am am ON am.oid = c.relam
		WHERE i.indrelid = source AND am.amname = 'sift_col' AND i.indisvalid ORDER BY i.indpred IS NOT NULL, i.indexrelid LIMIT 1;
	IF fidx IS NOT NULL THEN
		columns := COALESCE(columns, ARRAY(SELECT a.attname FROM pg_index i CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY k(n, o)
										  JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.n WHERE i.indexrelid = fidx ORDER BY k.o));
	END IF;
	-- carried columns: the primary key and the foreign key always, the rest as asked (default the columns of the table's sift_col index, else all)
	SELECT array_agg(a.attname ORDER BY a.attnum) INTO cols FROM pg_attribute a
		WHERE a.attrelid = source AND a.attnum > 0 AND NOT a.attisdropped AND (columns IS NULL OR a.attname = ANY (columns) OR a.attname = ANY (pk) OR a.attname = fk);
	IF scope_key = ANY (cols) THEN
		RAISE EXCEPTION 'scope_create: table "%" has a column named "%", the name of the scope key', src, scope_key USING ERRCODE = 'duplicate_column';
	END IF;
	IF fk <> ALL (cols) THEN
		RAISE EXCEPTION 'scope_create: table "%" has no column "%"', src, fk USING ERRCODE = 'undefined_column';
	END IF;
	IF columns IS NOT NULL AND EXISTS (SELECT 1 FROM unnest(columns) c WHERE c <> ALL (cols)) THEN
		RAISE EXCEPTION 'scope_create: table "%" has no column "%"', src, (SELECT c FROM unnest(columns) c WHERE c <> ALL (cols) LIMIT 1) USING ERRCODE = 'undefined_column';
	END IF;
	kept := sift._scope_inherit(fidx, cols, named);
	IF ord IS NULL THEN
		SELECT a.attname INTO ord FROM pg_attribute a WHERE a.attrelid = source AND a.attname = ANY (cols) AND a.atttypid IN ('timestamptz'::regtype, 'timestamp'::regtype, 'date'::regtype)
			ORDER BY a.attnum LIMIT 1;
	ELSIF ord <> ALL (cols) THEN
		RAISE EXCEPTION 'scope_create: order column "%" is not a carried column of "%"', ord, src USING ERRCODE = 'undefined_column';
	END IF;

	-- the multiplicity of the copy: its rows over the rows of the table that reach a key (each fact counts the keys of its member), the share of those rows whose member has 2 or more keys,
	-- and the size the copy would take: the table's row width for each copy row, and the sift index and the maintenance btree add 0.4 of the heap (0.34 to 0.44 from 2 to 6 copies per row)
	SELECT sum(r.copies_per_fact * r.facts)::float8 / NULLIF(sum(r.facts) FILTER (WHERE r.copies_per_fact > 0), 0),
		   sum(r.facts) FILTER (WHERE r.copies_per_fact > 1)::float8 / NULLIF(sum(r.facts) FILTER (WHERE r.copies_per_fact > 0), 0), max(r.copies_per_fact),
		   sum(r.copies_per_fact * r.facts) * pg_relation_size(source)::float8 / NULLIF(sum(r.facts), 0) * 1.4
		INTO mult, multi, maxk, est
		FROM sift.scope_replica_counts(src, fk, rel, member) r;
	IF mult > max_copies THEN
		RAISE EXCEPTION 'scope_create: a row of "%" would have % copies on average, above max_copies %', src, round(mult::numeric, 2), max_copies
			USING DETAIL = format('The copy would hold that many times the rows of the table that reach a scope key: about %s MB, +%s%% over the table and its indexes.',
								  round((est / 1048576)::numeric), round(est * 100 / (SELECT sum(pg_total_relation_size(r)) FROM sift.scope_relations(source) r) / 50) * 50),
				  HINT = format('A copy suits a multiplicity close to 1 (it stops paying at 1.84): above it the join answers the scope reads (entity path), the scope option of the sift_col index takes up to 3 entries a row (COL-SCOPE.md), counts and sums by key are cheaper from a table of them (INDEX-RECIPES.md), or pass max_copies => %s to build it anyway.', ceil(mult * 10) / 10), ERRCODE = 'feature_not_supported';
	END IF;

	-- the copy
	SELECT string_agg(format('%I %s%s', a.attname, format_type(a.atttypid, a.atttypmod), CASE WHEN a.attnotnull THEN ' NOT NULL' ELSE '' END), ', ' ORDER BY a.attnum) INTO defs
		FROM pg_attribute a WHERE a.attrelid = source AND a.attname = ANY (cols) AND NOT a.attisdropped;
	EXECUTE format('CREATE TABLE %s (%I %s NOT NULL, %s)%s', copy, scope_key, key_type, defs, CASE WHEN partition THEN format(' PARTITION BY LIST (%I)', scope_key) ELSE '' END);
	cname := sift.scope_qualified(to_regclass(copy));
	IF partition THEN
		EXECUTE format('SELECT sift.scope_partition(%L, k) FROM (SELECT DISTINCT %I::bigint k FROM %s) q', cname, scope_key, rel);
	END IF;
	EXECUTE format('INSERT INTO %s (%I, %s) SELECT s.%I, %s FROM %s f JOIN %s s ON s.%I = f.%I ORDER BY s.%I%s', cname, scope_key, sift.scope_list(cols, '')::text,
				   scope_key, sift.scope_list(cols, 'f'), src, rel, member, fk, scope_key, CASE WHEN ord IS NULL THEN '' ELSE format(', f.%I', ord) END);
	GET DIAGNOSTICS rows = ROW_COUNT;

	opts := CASE WHEN 'key_order' = ANY (named) THEN '' WHEN partition THEN CASE WHEN ord IS NULL THEN '' ELSE format('key_order = %L', ord) END
				 ELSE format('key_order = %L', concat_ws(', ', scope_key, ord)) END;
	opts := concat_ws(', ', NULLIF(opts, ''), kept, NULLIF(options, ''));
	SELECT relname INTO base FROM pg_class WHERE oid = to_regclass(cname);
	EXECUTE format('CREATE INDEX %I ON %s USING sift_col (%I, %s)%s', left(base, 55) || '_col', cname, scope_key, sift.scope_list(cols, '')::text, CASE WHEN opts = '' THEN '' ELSE ' WITH (' || opts || ')' END);
	-- what a change of a fact looks up in the copy (all its copies share the primary key) and the insert checks for a copy a change of the other table made in the same statement;
	-- a pair that goes is found by the sift index on (scope key, foreign key)
	EXECUTE format('CREATE INDEX %I ON %s (%s)', left(base, 54) || '_maint', cname,
				   array_to_string(ARRAY(SELECT format('%I', k) FROM unnest(pk) k), ', '));

	-- ANALYZE may run in a function, VACUUM may not: the pages of the copy are all-visible only after the caller's VACUUM. The scope relation is analyzed too: a pair table that
	-- only the copy's creation or a bulk load has filled has no statistics until autovacuum, and a trigger statement that joins it is then planned for millions of rows
	EXECUTE format('ANALYZE %s, %s', cname, rel);

	INSERT INTO sift.scope_copies VALUES (cname, src, fk, rel, member, scope_key, pk, cols, partition);
	FOR tr IN SELECT unnest(ARRAY['ins', 'upd', 'del']) LOOP
		EXECUTE format('CREATE TRIGGER %I AFTER %s ON %s REFERENCING %s AS %s FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_fact_trg(%L)',
					   sift.scope_trigger_name(cname, 'f' || tr), CASE tr WHEN 'ins' THEN 'INSERT' WHEN 'upd' THEN 'UPDATE' ELSE 'DELETE' END, src,
					   CASE tr WHEN 'ins' THEN 'NEW TABLE' WHEN 'upd' THEN 'OLD TABLE AS old_rows NEW TABLE' ELSE 'OLD TABLE' END,
					   CASE tr WHEN 'del' THEN 'old_rows' ELSE 'new_rows' END, cname);
		EXECUTE format('CREATE TRIGGER %I AFTER %s ON %s REFERENCING %s AS %s FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_pairs_trg(%L)',
					   sift.scope_trigger_name(cname, 'p' || tr), CASE tr WHEN 'ins' THEN 'INSERT' WHEN 'upd' THEN 'UPDATE' ELSE 'DELETE' END, rel,
					   CASE tr WHEN 'ins' THEN 'NEW TABLE' WHEN 'upd' THEN 'OLD TABLE AS old_rows NEW TABLE' ELSE 'OLD TABLE' END,
					   CASE tr WHEN 'del' THEN 'old_rows' ELSE 'new_rows' END, cname);
	END LOOP;
	EXECUTE format('CREATE TRIGGER %I AFTER TRUNCATE ON %s FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_truncate_trg(%L)', sift.scope_trigger_name(cname, 'ft'), src, cname);
	EXECUTE format('CREATE TRIGGER %I AFTER TRUNCATE ON %s FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_truncate_trg(%L)', sift.scope_trigger_name(cname, 'pt'), rel, cname);
	SELECT sum(pg_total_relation_size(r)) INTO est FROM sift.scope_relations(to_regclass(cname)) r;
	RAISE NOTICE 'scope copy %: % rows, % copies per row of "%" that reaches a key', cname, rows, round(mult::numeric, 2), src
		USING DETAIL = format('%s of those rows have 2 or more copies, at most %s; the copy takes %s MB with its indexes, +%s%% over the table and its indexes.',
							  round(COALESCE(multi, 0) * 100) || '%', maxk, round((est / 1048576.0)::numeric, 1), round(est * 100 / (SELECT sum(pg_total_relation_size(r)) FROM sift.scope_relations(source) r) / 50) * 50);
	IF fidx IS NULL AND columns IS NULL THEN
		SELECT round(sum(pg_relation_size(r)) / 1048576.0, 1) INTO mb FROM sift.scope_relations(to_regclass(cname)) r;
		RAISE NOTICE 'scope copy %: no sift_col index on "%", all % columns carried, table of % MB', cname, src, cardinality(cols), mb
			USING DETAIL = 'A copy carries the columns of the sift_col index of its table, or every column when there is none, and its indexes come on top of the table.',
				  HINT = 'Drop the copy and build it with columns => ARRAY[..] to carry the columns the reads need.';
	END IF;
	RAISE NOTICE 'scope copy %: run VACUUM % now', cname, cname
		USING DETAIL = 'VACUUM cannot run inside a function. A copy that is not vacuumed has an empty visibility map: a read checks every row it keeps in the heap.',
			  HINT = 'sift.scope_status shows whether a copy was vacuumed.';
	RETURN rows;
END $$;

