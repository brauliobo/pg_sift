\echo Use "ALTER EXTENSION pg_sift UPDATE TO '5.0'" to load this file. \quit

-- Scope copies: a copy of a fact table keyed by a scope key (tenant, group) that the facts reach through a scope relation, a table of unique (member key, scope key) pairs.
-- The copy carries one row per (fact row, scope key of its foreign key), has a sift_col index keyed by the scope key, and is kept exact by statement triggers
-- with transition tables on the fact table and on the scope relation, in the transaction of the change.
-- Names are stored as text (schema-qualified, quoted): a dump restores them by name.
CREATE TABLE sift.scope_copies (
	copy           text PRIMARY KEY,
	source         text   NOT NULL,
	fk             name   NOT NULL,
	scope_relation text   NOT NULL,
	member         name   NOT NULL,
	scope_key      name   NOT NULL,
	key_columns    name[] NOT NULL,
	columns        name[] NOT NULL,
	partitioned    boolean NOT NULL
);
SELECT pg_catalog.pg_extension_config_dump('sift.scope_copies', '');

COMMENT ON TABLE sift.scope_copies IS 'The scope copies made by sift.scope_create: the fact table, its foreign key, the scope relation and the columns carried';

-- Fragments of the statements the triggers run: a list of columns with a prefix, and a comparison of two prefixes joined by a separator
CREATE FUNCTION sift.scope_list(cols name[], prefix text) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
	RETURN (SELECT string_agg(format(CASE prefix WHEN '' THEN '%s%I' ELSE '%s.%I' END, prefix, c), ', ' ORDER BY o) FROM unnest(cols) WITH ORDINALITY AS u(c, o));
CREATE FUNCTION sift.scope_cmp(cols name[], a text, b text, op text, sep text) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
	RETURN (SELECT string_agg(format('%s.%I %s %s.%I', a, c, op, b, c), sep ORDER BY o) FROM unnest(cols) WITH ORDINALITY AS u(c, o));

COMMENT ON FUNCTION sift.scope_list(name[], text) IS 'Internal: the columns of a list with a table prefix, comma separated, for the statements of the scope triggers';
COMMENT ON FUNCTION sift.scope_cmp(name[], text, text, text, text) IS 'Internal: the columns of a list compared with an operator between two table prefixes, joined by a separator, for the statements of the scope triggers';

CREATE FUNCTION sift.scope_qualified(rel regclass) RETURNS text LANGUAGE sql STABLE PARALLEL SAFE
	RETURN (SELECT format('%I.%I', n.nspname, c.relname) FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = rel);

COMMENT ON FUNCTION sift.scope_qualified(regclass) IS 'Internal: the schema-qualified, quoted name of a relation';

-- A copy is exact only for writers that see every committed row when their statement starts: the snapshot of a REPEATABLE READ or SERIALIZABLE transaction misses the pairs
-- and facts that others committed after it began, so the triggers refuse to run there.
CREATE FUNCTION sift.scope_refuse_isolation(copy text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	RAISE EXCEPTION 'scope copies need READ COMMITTED writers' USING ERRCODE = 'feature_not_supported',
		DETAIL = format('The scope copy "%s" is kept by statement triggers that read the snapshot of the transaction, which is %s.', copy, current_setting('transaction_isolation')),
		HINT = 'run the change in a READ COMMITTED transaction, or drop the copy with sift.scope_drop';
END $$;
COMMENT ON FUNCTION sift.scope_refuse_isolation(text) IS 'Internal: the error of a scope copy trigger that runs in a REPEATABLE READ or SERIALIZABLE transaction';

-- The rows of a transition table that a trigger applies to the copy. An UPDATE applies only the rows whose columns changed: the set difference of the two transition tables over
-- those columns, one hash (or sort) pass of both tables whatever the planner estimates for them, in the order of the keys so that the probes of the copy that follow find the
-- pages they read before. A NOT EXISTS over the two tables joined by key and compared by column plans as a nested loop that compares every old row with every new one
-- when the estimate of a transition table is low.
CREATE FUNCTION sift.scope_changed(keys name[], cols name[], op text, side text) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
	RETURN CASE WHEN op <> 'UPDATE' THEN side || '_rows'
				ELSE format('(SELECT %1$s FROM %2$s_rows EXCEPT ALL SELECT %1$s FROM %3$s_rows ORDER BY %4$s)', sift.scope_list(cols, ''), side, CASE side WHEN 'old' THEN 'new' ELSE 'old' END,
							sift.scope_list(keys, '')) END;
COMMENT ON FUNCTION sift.scope_changed(name[], name[], text, text) IS 'Internal: the transition table (side old or new) that a trigger of a scope copy applies; for an UPDATE the sub-select of its rows whose columns are not among the other side''s, in key order';

-- The INSERT of the copy's rows that a statement trigger runs for the rows of src (sift.scope_changed). A row is added unless the copy has it: the other
-- table's trigger may have made it in the same statement. The probe of the copy is a lateral sub-select with LIMIT 1, which PostgreSQL can only run as an index probe of the
-- maintenance index for each row (an anti join it chose by estimate read the whole copy when the scope relation had no statistics).
CREATE FUNCTION sift.scope_fact_insert(c sift.scope_copies, src text) RETURNS text LANGUAGE sql STABLE AS $$
	SELECT format('INSERT INTO %s (%I, %s) SELECT s.%I, %s FROM %s n JOIN %s s ON s.%I = n.%I LEFT JOIN LATERAL (SELECT true AS present FROM %s x WHERE %s AND x.%I = s.%I LIMIT 1) h ON true WHERE h.present IS NULL',
		c.copy, c.scope_key, sift.scope_list(c.columns, ''), c.scope_key, sift.scope_list(c.columns, 'n'), src, c.scope_relation, c.member, c.fk,
		c.copy, sift.scope_cmp(c.key_columns, 'x', 'n', '=', ' AND '), c.scope_key, c.scope_key) $$;
CREATE FUNCTION sift.scope_pairs_insert(c sift.scope_copies, src text) RETURNS text LANGUAGE sql STABLE AS $$
	SELECT format('INSERT INTO %s (%I, %s) SELECT n.%I, %s FROM %s n JOIN %s f ON f.%I = n.%I LEFT JOIN LATERAL (SELECT true AS present FROM %s x WHERE %s AND x.%I = n.%I LIMIT 1) h ON true WHERE h.present IS NULL',
		c.copy, c.scope_key, sift.scope_list(c.columns, ''), c.scope_key, sift.scope_list(c.columns, 'f'), src, c.source, c.fk, c.member,
		c.copy, sift.scope_cmp(c.key_columns, 'x', 'f', '=', ' AND '), c.scope_key, c.scope_key) $$;
COMMENT ON FUNCTION sift.scope_fact_insert(sift.scope_copies, text) IS 'Internal: the INSERT of the copy''s rows that the trigger of the table of a scope copy runs for the rows of its transition table (sift.scope_changed)';
COMMENT ON FUNCTION sift.scope_pairs_insert(sift.scope_copies, text) IS 'Internal: the INSERT of the copy''s rows that the trigger of the scope relation of a scope copy runs for the rows of its transition table (sift.scope_changed)';

-- The DELETE of the copy's rows that a statement trigger of op runs: the rows of the old transition table whose copies go (sift.scope_changed)
CREATE FUNCTION sift.scope_fact_delete(c sift.scope_copies, op text) RETURNS text LANGUAGE sql STABLE AS $$
	SELECT format('DELETE FROM %s d USING %s o WHERE %s', c.copy, sift.scope_changed(c.key_columns, c.columns, op, 'old'), sift.scope_cmp(c.key_columns, 'd', 'o', '=', ' AND ')) $$;
-- The copy's rows of the pairs that go. A pair whose member has no fact in the table has no copy row, and a statement over all the pairs read the whole copy for as few as 1,000 of
-- them (600 M instructions, 1,000 and 10,000 pairs alike): scope_pairs_gone lists the pairs whose member has a fact (a probe of the table's index on the foreign key, 8,000
-- instructions each), in arrays whose length the planner knows, and scope_pairs_delete deletes their copy rows. A semi join, not a join, so that PostgreSQL hashes the pairs and
-- not the copy: from about 400 pairs (1.14 copies per fact, 25 facts for each member) it reads the copy once, below that it probes it for each pair.
CREATE FUNCTION sift.scope_pairs_gone(c sift.scope_copies, op text) RETURNS text LANGUAGE sql STABLE AS $$
	SELECT format('SELECT array_agg(o.%1$I)::text, array_agg(o.%2$I)::text FROM %3$s o WHERE EXISTS (SELECT 1 FROM %4$s f WHERE f.%5$I = o.%1$I)',
		c.member, c.scope_key, sift.scope_changed(ARRAY[c.member, c.scope_key], ARRAY[c.member, c.scope_key], op, 'old'), c.source, c.fk) $$;
CREATE FUNCTION sift.scope_pairs_delete(c sift.scope_copies) RETURNS text LANGUAGE sql STABLE AS $$
	SELECT format('DELETE FROM %s d WHERE EXISTS (SELECT 1 FROM unnest($1::%s[], $2::%s[]) x(m, k) WHERE d.%I = x.m AND d.%I = x.k)', c.copy,
		(SELECT format_type(a.atttypid, a.atttypmod) FROM pg_attribute a WHERE a.attrelid = c.copy::regclass AND a.attname = c.fk),
		(SELECT format_type(a.atttypid, a.atttypmod) FROM pg_attribute a WHERE a.attrelid = c.copy::regclass AND a.attname = c.scope_key), c.fk, c.scope_key) $$;
COMMENT ON FUNCTION sift.scope_fact_delete(sift.scope_copies, text) IS 'Internal: the DELETE of the copy''s rows that the trigger of the table of a scope copy runs for its transition table old_rows (op: the trigger''s operation)';
COMMENT ON FUNCTION sift.scope_pairs_gone(sift.scope_copies, text) IS 'Internal: the SELECT of the arrays of the pairs of the rows that go (op: the trigger''s operation) whose member has a fact in the table of a scope copy';
COMMENT ON FUNCTION sift.scope_pairs_delete(sift.scope_copies) IS 'Internal: the DELETE of the copy''s rows of the pairs, in the arrays $1 (members) and $2 (scope keys), that scope_pairs_gone listed';

-- The statements that keep a copy exact. A partitioned copy gets the partition of a scope key that appears in the scope relation when the pair is inserted.
CREATE FUNCTION sift.scope_fact_trg() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
	c        sift.scope_copies;
	src      text;
	k        bigint;
BEGIN
	IF current_setting('transaction_isolation') <> 'read committed' THEN PERFORM sift.scope_refuse_isolation(TG_ARGV[0]); END IF;
	SELECT * INTO STRICT c FROM sift.scope_copies WHERE copy = TG_ARGV[0];
	-- a transaction does not see the uncommitted rows of the other table's: the writers of the facts share a lock that the writers of the pairs take alone
	PERFORM pg_advisory_xact_lock_shared(hashtext('sift.scope_copy'), hashtext(c.copy));
	IF TG_OP IN ('UPDATE', 'DELETE') THEN
		EXECUTE sift.scope_fact_delete(c, TG_OP);
	END IF;
	IF TG_OP IN ('INSERT', 'UPDATE') THEN
		src := sift.scope_changed(c.key_columns, c.columns, TG_OP, 'new');
		-- a statement that changes the facts and the pairs fires this trigger first, on pairs whose partition their own trigger has not made yet
		IF c.partitioned THEN
			FOR k IN EXECUTE format('SELECT DISTINCT s.%I FROM %s n JOIN %s s ON s.%I = n.%I', c.scope_key, src, c.scope_relation, c.member, c.fk) LOOP
				PERFORM sift.scope_partition(c.copy, k);
			END LOOP;
		END IF;
		EXECUTE sift.scope_fact_insert(c, src);
	END IF;
	RETURN NULL;
END $$;

CREATE FUNCTION sift.scope_pairs_trg() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
	c        sift.scope_copies;
	src      text;
	k        bigint;
	ms       text;
	ks       text;
BEGIN
	IF current_setting('transaction_isolation') <> 'read committed' THEN PERFORM sift.scope_refuse_isolation(TG_ARGV[0]); END IF;
	SELECT * INTO STRICT c FROM sift.scope_copies WHERE copy = TG_ARGV[0];
	PERFORM pg_advisory_xact_lock(hashtext('sift.scope_copy'), hashtext(c.copy));
	IF TG_OP IN ('UPDATE', 'DELETE') THEN
		EXECUTE sift.scope_pairs_gone(c, TG_OP) INTO ms, ks;
		IF ms IS NOT NULL THEN
			EXECUTE sift.scope_pairs_delete(c) USING ms, ks;
		END IF;
	END IF;
	IF TG_OP IN ('INSERT', 'UPDATE') THEN
		src := sift.scope_changed(ARRAY[c.member, c.scope_key], ARRAY[c.member, c.scope_key], TG_OP, 'new');
		IF c.partitioned THEN
			FOR k IN EXECUTE format('SELECT DISTINCT n.%I FROM %s n', c.scope_key, src) LOOP
				PERFORM sift.scope_partition(c.copy, k);
			END LOOP;
		END IF;
		EXECUTE sift.scope_pairs_insert(c, src);
	END IF;
	RETURN NULL;
END $$;

CREATE FUNCTION sift.scope_truncate_trg() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	IF current_setting('transaction_isolation') <> 'read committed' THEN PERFORM sift.scope_refuse_isolation(TG_ARGV[0]); END IF;
	EXECUTE format('TRUNCATE %s', TG_ARGV[0]);
	RETURN NULL;
END $$;

CREATE FUNCTION sift.scope_partition(copy text, scope_key bigint) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	name text := format('%s_p%s', copy, replace(scope_key::text, '-', 'm'));
BEGIN
	-- the name of the partition is the copy's, so a quoted copy name keeps its quotes out of the middle
	IF to_regclass(name) IS NULL THEN
		EXECUTE format('CREATE TABLE %s PARTITION OF %s FOR VALUES IN (%s)', name, copy, scope_key);
	END IF;
END $$;

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
								  round((est / 1048576)::numeric), round(est * 100 / (SELECT sum(pg_total_relation_size(r)) FROM sift.scope_relations(source) r))),
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
							  round(COALESCE(multi, 0) * 100) || '%', maxk, round((est / 1048576.0)::numeric, 1), round(est * 100 / (SELECT sum(pg_total_relation_size(r)) FROM sift.scope_relations(source) r)));
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

-- How many copies each fact has: the facts of the table by the number of scope keys their foreign key has in the scope relation (0: the fact reaches no key and has no copy).
-- The copy holds a fact that many times, so the sum of copies_per_fact * facts is its row count.
CREATE FUNCTION sift.scope_replica_counts(source text, fk name, scope_relation text, member name) RETURNS TABLE (copies_per_fact int, facts bigint)
	LANGUAGE plpgsql STABLE AS $$
BEGIN
	RETURN QUERY EXECUTE format('SELECT COALESCE(p.k, 0)::int, sum(f.n)::bigint FROM (SELECT %2$I m, count(*) n FROM %1$s GROUP BY 1) f
		LEFT JOIN (SELECT %4$I m, count(*) k FROM %3$s GROUP BY 1) p USING (m) GROUP BY 1 ORDER BY 1', source, fk, scope_relation, member);
END $$;

CREATE FUNCTION sift.scope_replicas(copy text) RETURNS TABLE (copies_per_fact int, facts bigint) LANGUAGE plpgsql STABLE AS $$
DECLARE
	c sift.scope_copies;
BEGIN
	SELECT * INTO c FROM sift.scope_copies s WHERE s.copy = sift.scope_qualified(to_regclass(scope_replicas.copy));
	IF NOT FOUND THEN
		RAISE EXCEPTION 'scope_replicas: "%" is not a scope copy', scope_replicas.copy USING ERRCODE = 'undefined_table';
	END IF;
	RETURN QUERY SELECT * FROM sift.scope_replica_counts(c.source, c.fk, c.scope_relation, c.member);
END $$;

-- The WHERE condition that reads a copy over several scope keys without counting a fact twice: a fact counts under the first requested key that holds its member, so a fact of a
-- member in two requested keys is read once (scope_key IN (a, b) reads it in both). alias names the copy in the query that uses the condition (default: the copy's name).
CREATE FUNCTION sift.scope_read_keys(copy text, keys bigint[], alias text DEFAULT NULL) RETURNS text LANGUAGE plpgsql STABLE AS $$
DECLARE
	c sift.scope_copies;
BEGIN
	SELECT * INTO c FROM sift.scope_copies s WHERE s.copy = sift.scope_qualified(to_regclass(scope_read_keys.copy));
	IF NOT FOUND THEN
		RAISE EXCEPTION 'scope_read_keys: "%" is not a scope copy', scope_read_keys.copy USING ERRCODE = 'undefined_table';
	END IF;
	alias := COALESCE(quote_ident(alias), c.copy);
	RETURN COALESCE((SELECT string_agg(format('(%1$s.%2$I = %3$s%4$s)', alias, c.scope_key, q.k, CASE WHEN q.r = 1 THEN '' ELSE
			format(' AND NOT EXISTS (SELECT 1 FROM %s scope_p WHERE scope_p.%I = %s.%I AND scope_p.%I = ANY (%L::bigint[]))', c.scope_relation, c.member, alias, c.fk, c.scope_key, q.before) END),
			' OR ' ORDER BY q.r)
		FROM (SELECT d.k, d.r, array_agg(d.k) OVER (ORDER BY d.r ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS before
			  FROM (SELECT u.k, row_number() OVER (ORDER BY min(u.o)) AS r FROM unnest(keys) WITH ORDINALITY u(k, o) WHERE u.k IS NOT NULL GROUP BY u.k) d) q), 'false');
END $$;

-- The rows of a copy that differ from the join of the table with the scope relation: 'missing' (the join has it, the copy does not), 'extra' (the copy has a row the join does not),
-- 'differs' (the carried columns are not those of the table), 'duplicate' (the copy holds a fact twice in one key) and 'logical replication target' (the table or the scope
-- relation is the target of a subscription, which keeps the copy behind). No row: the copy is exact.
-- With keys, only those keys are checked, and so is the contract of a read over several keys (sift.scope_read_keys): the facts it reads, each once, are the facts of the members
-- of the keys ('multi_key_missing' for a fact it does not read, 'multi_key_extra' for a fact it reads that the members do not have, or reads twice; scope_key is null there).
CREATE FUNCTION sift.scope_verify(copy text, keys bigint[] DEFAULT NULL) RETURNS TABLE (problem text, scope_key bigint, row_key jsonb) LANGUAGE plpgsql STABLE AS $$
DECLARE
	c      sift.scope_copies;
	row_of text;
BEGIN
	SELECT * INTO c FROM sift.scope_copies s WHERE s.copy = sift.scope_qualified(to_regclass(scope_verify.copy));
	IF NOT FOUND THEN
		RAISE EXCEPTION 'scope_verify: "%" is not a scope copy', scope_verify.copy USING ERRCODE = 'undefined_table';
	END IF;
	-- an apply worker fires no trigger: a subscription made after the copy leaves it behind, and the rows below are its symptom
	RETURN QUERY SELECT 'logical replication target', NULL::bigint, jsonb_build_object('table', sift.scope_qualified(r.srrelid),
		'detail', 'apply workers do not fire the copy triggers; rebuild the copy with sift.scope_drop and sift.scope_create')
		FROM pg_subscription_rel r WHERE r.srrelid IN (c.source::regclass, c.scope_relation::regclass) ORDER BY 3;
	row_of := (SELECT string_agg(format('%L, COALESCE(a.%I, e.%I)', k, k, k), ', ') FROM unnest(c.key_columns) k);
	RETURN QUERY EXECUTE format($q$
		SELECT CASE WHEN a.%1$I IS NULL THEN 'missing' WHEN e.%1$I IS NULL THEN 'extra' ELSE 'differs' END, COALESCE(a.%1$I, e.%1$I)::bigint, jsonb_build_object(%4$s)
		FROM (SELECT s.%7$I AS %1$I, %5$s FROM %2$s f JOIN %3$s s ON s.%6$I = f.%8$I WHERE $1 IS NULL OR s.%7$I = ANY ($1)) e
		FULL JOIN (SELECT * FROM %9$s WHERE $1 IS NULL OR %1$I = ANY ($1)) a ON a.%1$I = e.%1$I AND %10$s
		WHERE a.%1$I IS NULL OR e.%1$I IS NULL OR %11$s
		UNION ALL
		SELECT 'duplicate', a.%1$I::bigint, jsonb_build_object(%12$s) FROM %9$s a WHERE $1 IS NULL OR a.%1$I = ANY ($1) GROUP BY a.%1$I, %13$s HAVING count(*) > 1$q$,
		c.scope_key, c.source, c.scope_relation, row_of, sift.scope_list(c.columns, 'f'), c.member, c.scope_key, c.fk, c.copy,
		sift.scope_cmp(c.key_columns, 'a', 'e', '=', ' AND '),
		'(' || (SELECT string_agg(format('a.%1$I IS DISTINCT FROM e.%1$I', k), ' OR ') FROM unnest(c.columns) k) || ')',
		(SELECT string_agg(format('%L, a.%I', k, k), ', ') FROM unnest(c.key_columns) k), sift.scope_list(c.key_columns, 'a')) USING keys;
	IF keys IS NOT NULL THEN
		RETURN QUERY EXECUTE format($q$
			SELECT CASE WHEN a.%1$I IS NULL THEN 'multi_key_missing' ELSE 'multi_key_extra' END, NULL::bigint, jsonb_build_object(%2$s)
			FROM (SELECT %3$s FROM %4$s f WHERE f.%5$I IN (SELECT s.%6$I FROM %7$s s WHERE s.%8$I = ANY ($1))) e
			FULL JOIN (SELECT %3$s, count(*) n FROM %9$s f WHERE %10$s GROUP BY %3$s) a ON %11$s
			WHERE a.%1$I IS NULL OR e.%1$I IS NULL OR a.n > 1$q$,
			c.key_columns[1], row_of, sift.scope_list(c.key_columns, ''), c.source, c.fk, c.member, c.scope_relation, c.scope_key, c.copy,
			sift.scope_read_keys(c.copy, keys, 'f'), sift.scope_cmp(c.key_columns, 'a', 'e', '=', ' AND ')) USING keys;
	END IF;
END $$;

COMMENT ON FUNCTION sift.scope_replica_counts(text, name, text, name) IS 'Internal: the facts of a table by the number of scope keys their foreign key has in a scope relation';
COMMENT ON FUNCTION sift.scope_read_keys(text, bigint[], text) IS 'The WHERE condition that reads a scope copy over several keys with each fact once: a fact counts under the first requested key that holds its member';
COMMENT ON FUNCTION sift.scope_replicas(text) IS 'The facts of a scope copy by how many copies each has (the number of scope keys of its foreign key; 0: none): the multiplicity of the copy';
COMMENT ON FUNCTION sift.scope_verify(text, bigint[]) IS 'The rows of a scope copy that differ from the join of its table with the scope relation (missing, extra, differs, duplicate) and a logical replication target row for each table a subscription writes; no rows: the copy is exact. With keys, only those keys and the read over them through sift.scope_read_keys (multi_key_missing, multi_key_extra)';

-- Whether the copies were vacuumed: the pages of a leaf of a copy that the last VACUUM or ANALYZE found all-visible, over its pages; none of them means never vacuumed.
-- Whether the copy and its scope relation have statistics (analyzed): without them the statements of the triggers are planned for far more rows than they get
CREATE VIEW sift.scope_status AS
	SELECT s.copy, sum(c.relpages)::bigint AS pages, sum(c.relallvisible)::bigint AS all_visible_pages, sum(c.relallvisible) > 0 AS vacuumed,
		   bool_and(c.reltuples >= 0) AND (SELECT p.reltuples >= 0 FROM pg_class p WHERE p.oid = s.scope_relation::regclass) AS analyzed
	FROM sift.scope_copies s CROSS JOIN LATERAL sift.scope_relations(to_regclass(s.copy)) r JOIN pg_class c ON c.oid = r
	GROUP BY s.copy;

COMMENT ON VIEW sift.scope_status IS 'One row for each scope copy: its pages, its all-visible pages, whether it was vacuumed (not vacuumed: a read checks the rows in the heap, run VACUUM on the copy) and whether it and its scope relation have statistics (not analyzed: run ANALYZE on them)';

-- Drops the triggers and the copy
CREATE FUNCTION sift.scope_drop(copy text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	c sift.scope_copies;
	t text;
BEGIN
	SELECT * INTO c FROM sift.scope_copies s WHERE s.copy = sift.scope_qualified(to_regclass(scope_drop.copy));
	IF NOT FOUND THEN
		RAISE EXCEPTION 'scope_drop: "%" is not a scope copy', scope_drop.copy USING ERRCODE = 'undefined_table';
	END IF;
	FOREACH t IN ARRAY ARRAY['fins', 'fupd', 'fdel', 'ft'] LOOP
		EXECUTE format('DROP TRIGGER IF EXISTS %I ON %s', sift.scope_trigger_name(c.copy, t), c.source);
	END LOOP;
	FOREACH t IN ARRAY ARRAY['pins', 'pupd', 'pdel', 'pt'] LOOP
		EXECUTE format('DROP TRIGGER IF EXISTS %I ON %s', sift.scope_trigger_name(c.copy, t), c.scope_relation);
	END LOOP;
	DELETE FROM sift.scope_copies s WHERE s.copy = c.copy;
	EXECUTE format('DROP TABLE %s', c.copy);
END $$;

COMMENT ON FUNCTION sift.scope_drop(text) IS 'Drops a scope copy with its triggers on the table and on the scope relation';
COMMENT ON FUNCTION sift.scope_create(text, regclass, name, regclass, name, name[], boolean, name, name, text, numeric) IS 'Creates a copy of a table keyed by a scope key reached through a scope relation of (member, scope key) pairs, with a sift_col index by the scope key and triggers that keep it exact; returns the rows. Takes the columns and options of the table''s sift_col index. Above max_copies copies per row (default 1.8) it refuses; a larger max_copies builds it. VACUUM it afterwards. See INDEX-RECIPES';

-- Pairs kept from a view: the scope relation of a copy (or a table read on its own) is a table of unique (member key, scope key) pairs that a view defines over
-- source tables (assignments, group membership). sift.scope_refresh_pairs recomputes the pairs of named member ids or scope keys from the view and applies the difference;
-- sift.scope_follow_pairs puts statement triggers on the source tables that call it for the member ids or scope keys a change names, in the transaction of the change,
-- so the pairs stay equal to the view and a copy built on them (sift.scope_create) stays equal to the join.
CREATE TABLE sift.scope_follows (
	pairs         text   NOT NULL,
	definition    text   NOT NULL,
	source        text   NOT NULL,
	refresh_by    text   NOT NULL CHECK (refresh_by IN ('member', 'key', 'pair')),
	source_column name   NOT NULL,
	expires       name,
	swept_at      timestamptz NOT NULL,
	PRIMARY KEY (pairs, source, source_column)
);
SELECT pg_catalog.pg_extension_config_dump('sift.scope_follows', '');

COMMENT ON TABLE sift.scope_follows IS 'The tables that sift.scope_follow_pairs follows: for each pair table, its defining view, a source table of the view, the column that names the member ids or scope keys a change touches, and the column of the source that expires pairs';

-- The condition on the columns of a view or of the pair table (qualified by q) that names the rows of a refresh: $1 are the member ids or the scope keys, $2 the scope keys of a pair mode
CREATE FUNCTION sift.scope_pairs_where(by text, cols name[], everything boolean, q text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN everything THEN 'true'
		WHEN by = 'member' THEN format('%1$s%2$I = ANY ($1)', q, cols[1])
		WHEN by = 'key' THEN format('%1$s%3$I = ANY ($1)', q, cols[1], cols[2])
		ELSE format('%1$s%2$I = ANY ($1) AND %1$s%3$I = ANY ($2) AND EXISTS (SELECT 1 FROM unnest($1, $2) u(m, k) WHERE u.m = %1$s%2$I AND u.k = %1$s%3$I)', q, cols[1], cols[2]) END
$$;

-- The pairs of the named member ids, scope keys or (member, key) pairs (by = 'member', 'key' or 'pair': ids are the members and keys the scope keys, element by element; ids NULL: all)
-- as the view gives them: the pairs the table has for them are read once and compared with the view's, the missing ones are inserted and the extra ones deleted, in one statement.
-- The first column of the view is the member, the second the scope key; the pair table has columns of those names. One refresh of a pair table at a time: a later
-- one waits for the commit of the earlier and reads what it wrote, so two writers of different source tables never leave a pair that neither saw.
CREATE FUNCTION sift.scope_refresh_pairs(pairs regclass, definition regclass, by text, ids bigint[] DEFAULT NULL, keys bigint[] DEFAULT NULL) RETURNS TABLE (inserted bigint, deleted bigint)
	LANGUAGE plpgsql AS $$
DECLARE
	cols   name[];
	target text := sift.scope_qualified(pairs);
BEGIN
	SELECT array_agg(a.attname ORDER BY a.attnum) INTO cols FROM pg_attribute a WHERE a.attrelid = definition AND a.attnum > 0 AND NOT a.attisdropped;
	IF cardinality(cols) <> 2 THEN
		RAISE EXCEPTION 'scope_refresh_pairs: "%" must have two columns, the member and the scope key', definition USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF by NOT IN ('member', 'key', 'pair') THEN
		RAISE EXCEPTION 'scope_refresh_pairs: by is "%", it must be ''member'', ''key'' or ''pair''', by USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF by = 'pair' AND ids IS NOT NULL AND cardinality(ids) IS DISTINCT FROM cardinality(keys) THEN
		RAISE EXCEPTION 'scope_refresh_pairs: by pair takes as many keys as ids' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF (SELECT count(*) FROM pg_attribute a WHERE a.attrelid = pairs AND a.attname = ANY (cols) AND NOT a.attisdropped) <> 2 THEN
		RAISE EXCEPTION 'scope_refresh_pairs: table % has no columns %, %', target, cols[1], cols[2] USING ERRCODE = 'undefined_column';
	END IF;
	PERFORM pg_advisory_xact_lock(hashtextextended('sift.scope_pairs ' || target, 0));
	RETURN QUERY EXECUTE format($q$
		WITH want AS MATERIALIZED (SELECT DISTINCT %1$I, %2$I FROM %3$s WHERE %5$s),
		     have AS MATERIALIZED (SELECT ctid AS tid, %1$I, %2$I FROM %4$s p WHERE %6$s),
		     gone AS (DELETE FROM %4$s WHERE ctid = ANY (ARRAY(SELECT h.tid FROM have h WHERE NOT EXISTS (SELECT 1 FROM want w WHERE w.%1$I = h.%1$I AND w.%2$I = h.%2$I))) RETURNING 1),
		     ins  AS (INSERT INTO %4$s (%1$I, %2$I) SELECT w.%1$I, w.%2$I FROM want w
		              WHERE NOT EXISTS (SELECT 1 FROM have h WHERE h.%1$I = w.%1$I AND h.%2$I = w.%2$I) ON CONFLICT DO NOTHING RETURNING 1)
		SELECT (SELECT count(*) FROM ins), (SELECT count(*) FROM gone)$q$,
		cols[1], cols[2], sift.scope_qualified(definition), target,
		sift.scope_pairs_where(by, cols, ids IS NULL, ''), sift.scope_pairs_where(by, cols, ids IS NULL, 'p.')) USING ids, keys;
	-- a refresh of all the pairs fills a new or emptied table: the statements that join it are planned from its statistics, which autovacuum would only make later
	IF ids IS NULL THEN
		EXECUTE format('ANALYZE %s', target);
	END IF;
END $$;

-- What the rows of a followed table (the SELECT `changed`, with the columns of the table) name: a SELECT of the member ids or scope keys of source_column or, in pair mode, of the
-- members and the scope keys of the rows, element by element (a row may name only one of them). A transition table is read only by the trigger's own statements: its caller runs this.
CREATE FUNCTION sift.scope_names_sql(f sift.scope_follows, changed text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN f.refresh_by <> 'pair'
		THEN format('SELECT COALESCE(array_agg(DISTINCT %1$I::bigint), ''{}''), NULL::bigint[] FROM (%2$s) t WHERE %1$I IS NOT NULL', f.source_column, changed)
		ELSE format('SELECT COALESCE(array_agg(m), ''{}''), COALESCE(array_agg(k), ''{}'') FROM (SELECT DISTINCT %1$I::bigint m, %2$I::bigint k FROM (%3$s) t) s', v.m, v.k, changed) END
	FROM (SELECT (array_agg(a.attname ORDER BY a.attnum))[1] m, (array_agg(a.attname ORDER BY a.attnum))[2] k FROM pg_attribute a
		  WHERE a.attrelid = f.definition::regclass AND a.attnum > 0 AND NOT a.attisdropped) v
$$;

-- Refreshes what scope_names_sql named: the member ids or scope keys, or in pair mode the (member, key) pairs of the rows that name both; a row that names only its key or only its
-- member refreshes that one, as it can contribute to any pair of it
CREATE FUNCTION sift.scope_refresh_named(f sift.scope_follows, ids bigint[], keys bigint[]) RETURNS TABLE (inserted bigint, deleted bigint) LANGUAGE plpgsql AS $$
DECLARE
	r record;
	x record;
BEGIN
	inserted := 0; deleted := 0;
	FOR x IN SELECT f.refresh_by AS mode, ids AS a, NULL::bigint[] AS b WHERE f.refresh_by <> 'pair'
			 UNION ALL SELECT 'pair', array_agg(u.m), array_agg(u.k) FROM unnest(ids, keys) u(m, k) WHERE f.refresh_by = 'pair' AND u.m IS NOT NULL AND u.k IS NOT NULL
			 UNION ALL SELECT 'key', array_agg(DISTINCT u.k), NULL FROM unnest(ids, keys) u(m, k) WHERE f.refresh_by = 'pair' AND u.m IS NULL AND u.k IS NOT NULL
			 UNION ALL SELECT 'member', array_agg(DISTINCT u.m), NULL FROM unnest(ids, keys) u(m, k) WHERE f.refresh_by = 'pair' AND u.k IS NULL AND u.m IS NOT NULL LOOP
		IF cardinality(x.a) > 0 THEN
			SELECT * INTO r FROM sift.scope_refresh_pairs(f.pairs::regclass, f.definition::regclass, x.mode, x.a, x.b);
			inserted := inserted + r.inserted; deleted := deleted + r.deleted;
		END IF;
	END LOOP;
	RETURN NEXT;
END $$;

-- The statement trigger of a followed table: the rows it changed (the old and the new ones of an update) are what is refreshed; a TRUNCATE refreshes all the pairs
CREATE FUNCTION sift.scope_follow_trg() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
	f    sift.scope_follows;
	ids  bigint[];
	keys bigint[];
BEGIN
	SELECT * INTO STRICT f FROM sift.scope_follows s WHERE s.pairs = TG_ARGV[0] AND s.source = TG_ARGV[1] AND s.source_column = TG_ARGV[2];
	IF TG_OP = 'TRUNCATE' THEN
		PERFORM sift.scope_refresh_pairs(f.pairs::regclass, f.definition::regclass, f.refresh_by);
	ELSE
		EXECUTE sift.scope_names_sql(f, CASE TG_OP WHEN 'INSERT' THEN 'SELECT * FROM new_rows' WHEN 'DELETE' THEN 'SELECT * FROM old_rows'
											ELSE 'SELECT * FROM old_rows UNION ALL SELECT * FROM new_rows' END) INTO ids, keys;
		PERFORM sift.scope_refresh_named(f, ids, keys);
	END IF;
	RETURN NULL;
END $$;

-- Makes the pairs equal to the view and keeps them so: statement triggers with transition tables on the source table (insert, update, delete, truncate) refresh the members
-- ('member') or the keys ('key') named by source_column in the transaction of each change. expires names a date or timestamp column of the source that ends a pair
-- (the view tests it against now()): sift.scope_expire_pairs applies the expiries that happened with no write.
CREATE FUNCTION sift.scope_follow_pairs(pairs regclass, definition regclass, source regclass, by text, source_column name, expires name DEFAULT NULL)
	RETURNS TABLE (inserted bigint, deleted bigint) LANGUAGE plpgsql AS $$
DECLARE
	target text := sift.scope_qualified(pairs);
	src    text := sift.scope_qualified(source);
	tr     text;
BEGIN
	IF (SELECT relkind FROM pg_class WHERE oid = source) NOT IN ('r', 'p') THEN
		RAISE EXCEPTION 'scope_follow_pairs: "%" is not a table', src USING ERRCODE = 'wrong_object_type';
	END IF;
	IF by NOT IN ('member', 'key', 'pair') THEN
		RAISE EXCEPTION 'scope_follow_pairs: by is "%", it must be ''member'', ''key'' or ''pair''', by USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF by = 'pair' AND (SELECT count(*) FROM pg_attribute a, pg_attribute v WHERE a.attrelid = source AND NOT a.attisdropped AND v.attrelid = definition AND v.attnum IN (1, 2)
						AND v.attname = a.attname AND a.atttypid IN ('int2'::regtype, 'int4'::regtype, 'int8'::regtype)) <> 2 THEN
		RAISE EXCEPTION 'scope_follow_pairs: by pair needs the two columns of the view as integer columns of "%"', src USING ERRCODE = 'undefined_column';
	END IF;
	IF by = 'pair' AND source_column <> (SELECT attname FROM pg_attribute WHERE attrelid = definition AND attnum = 2) THEN
		RAISE EXCEPTION 'scope_follow_pairs: by pair takes the scope key column of the view as source_column' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF NOT EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = source AND a.attname = source_column AND NOT a.attisdropped
				   AND a.atttypid IN ('int2'::regtype, 'int4'::regtype, 'int8'::regtype)) THEN
		RAISE EXCEPTION 'scope_follow_pairs: table % has no integer column "%"', src, source_column USING ERRCODE = 'undefined_column';
	END IF;
	IF expires IS NOT NULL AND NOT EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = source AND a.attname = expires AND NOT a.attisdropped
										   AND a.atttypid IN ('date'::regtype, 'timestamp'::regtype, 'timestamptz'::regtype)) THEN
		RAISE EXCEPTION 'scope_follow_pairs: table % has no date or timestamp column "%"', src, expires USING ERRCODE = 'undefined_column';
	END IF;
	IF EXISTS (SELECT 1 FROM pg_subscription_rel r WHERE r.srrelid IN (source, pairs)) THEN
		RAISE EXCEPTION 'scope_follow_pairs: a table is the target of a logical replication subscription'
			USING DETAIL = 'The apply worker changes rows without firing statement triggers, so the pairs would miss the changes.', ERRCODE = 'feature_not_supported';
	END IF;
	INSERT INTO sift.scope_follows VALUES (target, sift.scope_qualified(definition), src, by, source_column, expires, now());
	FOR tr IN SELECT unnest(ARRAY['ins', 'upd', 'del']) LOOP
		EXECUTE format('CREATE TRIGGER %I AFTER %s ON %s REFERENCING %s AS %s FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_follow_trg(%L, %L, %L)',
					   sift.scope_trigger_name(target || ' ' || src || ' ' || source_column, 's' || tr), CASE tr WHEN 'ins' THEN 'INSERT' WHEN 'upd' THEN 'UPDATE' ELSE 'DELETE' END, src,
					   CASE tr WHEN 'ins' THEN 'NEW TABLE' WHEN 'upd' THEN 'OLD TABLE AS old_rows NEW TABLE' ELSE 'OLD TABLE' END,
					   CASE tr WHEN 'del' THEN 'old_rows' ELSE 'new_rows' END, target, src, source_column);
	END LOOP;
	EXECUTE format('CREATE TRIGGER %I AFTER TRUNCATE ON %s FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_follow_trg(%L, %L, %L)',
				   sift.scope_trigger_name(target || ' ' || src || ' ' || source_column, 'st'), src, target, src, source_column);
	-- the triggers hold the source against writers until this transaction ends, so the full refresh below is the state they continue from
	RETURN QUERY SELECT * FROM sift.scope_refresh_pairs(pairs, definition, by);
END $$;

-- The pairs that ended by a date since the last call: for each followed table with an expires column, the member ids or scope keys of its rows that expired from swept_at minus overlap
-- up to now() are refreshed. A transaction that started before a sweep and commits after it can add a pair that has already expired: overlap (an upper bound of the
-- duration of a write transaction of the source) makes the next sweeps look at those expiries again.
CREATE FUNCTION sift.scope_expire_pairs(pairs regclass, overlap interval DEFAULT '1 hour') RETURNS TABLE (inserted bigint, deleted bigint) LANGUAGE plpgsql AS $$
DECLARE
	f      sift.scope_follows;
	ids    bigint[];
	keys   bigint[];
	r      record;
	typ    text;
BEGIN
	inserted := 0; deleted := 0;
	FOR f IN SELECT * FROM sift.scope_follows s WHERE s.pairs = sift.scope_qualified(scope_expire_pairs.pairs) AND s.expires IS NOT NULL ORDER BY s.source FOR UPDATE LOOP
		SELECT format_type(a.atttypid, NULL) INTO typ FROM pg_attribute a WHERE a.attrelid = f.source::regclass AND a.attname = f.expires;
		EXECUTE sift.scope_names_sql(f, format('SELECT * FROM %s WHERE %I BETWEEN %L::%s AND %L::%s', f.source, f.expires, f.swept_at - overlap, typ, now(), typ)) INTO ids, keys;
		SELECT * INTO r FROM sift.scope_refresh_named(f, ids, keys);
		inserted := inserted + r.inserted; deleted := deleted + r.deleted;
		UPDATE sift.scope_follows s SET swept_at = now() WHERE s.pairs = f.pairs AND s.source = f.source AND s.source_column = f.source_column;
	END LOOP;
	IF NOT FOUND THEN
		RAISE EXCEPTION 'scope_expire_pairs: no source of "%" has an expires column', scope_expire_pairs.pairs USING ERRCODE = 'undefined_object';
	END IF;
	RETURN NEXT;
END $$;

-- Stops following: drops the triggers of one source table, or of every source of the pair table
CREATE FUNCTION sift.scope_unfollow_pairs(pairs regclass, source regclass DEFAULT NULL) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	f sift.scope_follows;
	t text;
BEGIN
	FOR f IN WITH d AS (DELETE FROM sift.scope_follows s WHERE s.pairs = sift.scope_qualified(scope_unfollow_pairs.pairs)
						AND (scope_unfollow_pairs.source IS NULL OR s.source = sift.scope_qualified(scope_unfollow_pairs.source)) RETURNING s.*) SELECT * FROM d LOOP
		FOREACH t IN ARRAY ARRAY['sins', 'supd', 'sdel', 'st'] LOOP
			EXECUTE format('DROP TRIGGER IF EXISTS %I ON %s', sift.scope_trigger_name(f.pairs || ' ' || f.source || ' ' || f.source_column, t), f.source);
		END LOOP;
	END LOOP;
	IF NOT FOUND THEN
		RAISE EXCEPTION 'scope_unfollow_pairs: "%" follows no such table', scope_unfollow_pairs.pairs USING ERRCODE = 'undefined_object';
	END IF;
END $$;

COMMENT ON FUNCTION sift.scope_refresh_pairs(regclass, regclass, text, bigint[], bigint[]) IS 'Recomputes the pairs of the named member ids, scope keys or (member, key) pairs (by = member, key or pair, the ids and, for pair, the keys element by element; ids NULL: all) of a table of unique (member, scope key) pairs from a view of the same two columns, inserting the missing pairs and deleting the extra ones in one statement; returns the counts';
COMMENT ON FUNCTION sift.scope_follow_pairs(regclass, regclass, source regclass, text, name, name) IS 'Makes a table of (member, scope key) pairs equal to its view and keeps it so: statement triggers on a source table refresh what a change names (by = member ids, scope keys or, for pair, the (member, key) pairs of the rows), in the transaction of the change; returns the pairs the first refresh inserted and deleted';
COMMENT ON FUNCTION sift.scope_expire_pairs(regclass, interval) IS 'Applies the expiries by date that happened with no write to the source tables of followed pairs: refreshes what the rows whose expires column passed since the last call name; returns the counts';
COMMENT ON FUNCTION sift.scope_unfollow_pairs(regclass, regclass) IS 'Drops the triggers that keep a table of pairs equal to its view, of one source table or of all of them';
COMMENT ON FUNCTION sift.scope_follow_trg() IS 'Internal: the statement trigger of a source table of followed pairs; refreshes what the changed rows name';
COMMENT ON FUNCTION sift.scope_names_sql(sift.scope_follows, text) IS 'Internal: the SELECT of the member ids, scope keys or pairs that the rows of a followed table (a SELECT of its columns) name';
COMMENT ON FUNCTION sift.scope_refresh_named(sift.scope_follows, bigint[], bigint[]) IS 'Internal: refreshes the member ids, scope keys or pairs that scope_names_sql named';
COMMENT ON FUNCTION sift.scope_pairs_where(text, name[], boolean, text) IS 'Internal: the condition that names the rows a refresh of the pairs reads';
COMMENT ON FUNCTION sift.scope_fact_trg() IS 'Internal: the statement trigger of the table of a scope copy; applies the inserted, updated and deleted rows to the copy; refuses REPEATABLE READ and SERIALIZABLE transactions';
COMMENT ON FUNCTION sift.scope_pairs_trg() IS 'Internal: the statement trigger of the scope relation of a scope copy; applies the inserted, updated and deleted pairs to the copy; refuses REPEATABLE READ and SERIALIZABLE transactions';
COMMENT ON FUNCTION sift.scope_truncate_trg() IS 'Internal: the TRUNCATE trigger of the table and of the scope relation of a scope copy; empties the copy; refuses REPEATABLE READ and SERIALIZABLE transactions';

CREATE FUNCTION sift.col_build_lock() RETURNS event_trigger
	AS 'MODULE_PATHNAME', 'sift_col_build_lock' LANGUAGE C;
CREATE EVENT TRIGGER sift_col_build_lock ON ddl_command_start WHEN TAG IN ('CREATE INDEX') EXECUTE FUNCTION sift.col_build_lock();
COMMENT ON FUNCTION sift.col_build_lock() IS 'Internal: the event trigger before CREATE INDEX of a sift_col index; takes the table lock that the build of its change log needs first, so that two builds on one table do not deadlock';
-- Named projections: what a sift_col index stores (columns, derived expressions, filter, rollups), defined once and used by several indexes.
-- sift.projection_index creates the index from the definitions, or, when an index of the table with the same layout already holds them, builds nothing and records that
-- the index serves them: the index lists the names in its option projection, which a dump keeps. The definitions are extension configuration (dumped as data, like the scope copies);
-- the names are resolved by sift.projection_status, never by CREATE INDEX, because a restore creates the indexes before the data of the definitions is there and pg_upgrade never has it.
CREATE TABLE sift.projections (
	name             text   PRIMARY KEY CHECK (name ~ '^[a-z_][a-z0-9_$]*$'),
	source           text   NOT NULL,
	leaves           text[] NOT NULL CHECK (cardinality(leaves) > 0),
	rollups          text,
	filter           text,
	options          jsonb  NOT NULL DEFAULT '{}',
	canonical        text[] NOT NULL,
	canonical_filter text
);
SELECT pg_catalog.pg_extension_config_dump('sift.projections', '');

COMMENT ON TABLE sift.projections IS 'The named projections of sift.projection_create: the table, the columns and expressions, the rollups, the filter and the other options of an index, with the deparsed forms an index is compared with';

CREATE FUNCTION sift.projection_filter_served(source regclass, filter text, index regclass) RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_col_projection_filter_served' LANGUAGE C STRICT STABLE;
COMMENT ON FUNCTION sift.projection_filter_served(regclass, text, regclass) IS 'Internal: whether the index keeps every row that satisfies the filter and either no other row or the columns to test the filter on the rows it keeps';

-- The value of a reloption of an index (NULL when it is not set), and the projections its option lists
CREATE FUNCTION sift.projection_option(idx oid, key text) RETURNS text LANGUAGE sql STABLE PARALLEL SAFE
	RETURN (SELECT substr(o, length(key) + 2) FROM pg_catalog.pg_class c, unnest(c.reloptions) o WHERE c.oid = idx AND starts_with(o, key || '='));
CREATE FUNCTION sift.projection_names(idx oid) RETURNS text[] LANGUAGE sql STABLE PARALLEL SAFE
	RETURN COALESCE(string_to_array(replace(sift.projection_option(idx, 'projection'), ' ', ''), ','), '{}');
COMMENT ON FUNCTION sift.projection_option(oid, text) IS 'Internal: the value of a reloption of an index, NULL when it is not set';
COMMENT ON FUNCTION sift.projection_names(oid) IS 'Internal: the projections an index lists in its option projection';

-- The sift_col indexes and the projections each lists
CREATE FUNCTION sift.projection_listed() RETURNS TABLE (idx oid, name text) LANGUAGE sql STABLE PARALLEL SAFE AS $$
	SELECT c.oid, n FROM pg_catalog.pg_class c JOIN pg_catalog.pg_am a ON a.oid = c.relam AND a.amname = 'sift_col', unnest(sift.projection_names(c.oid)) n
	WHERE c.relkind = 'i'
$$;
CREATE FUNCTION sift.projection_indexes(name text) RETURNS SETOF oid LANGUAGE sql STABLE PARALLEL SAFE AS $$
	SELECT l.idx FROM sift.projection_listed() l WHERE l.name = $1 ORDER BY l.idx
$$;
COMMENT ON FUNCTION sift.projection_listed() IS 'Internal: each sift_col index with each projection its option projection lists';
COMMENT ON FUNCTION sift.projection_indexes(text) IS 'The sift_col indexes whose option projection lists the name';

-- The WITH list of an index made from projections: the names, the rollups, the options the definitions share and the layout of this index
CREATE FUNCTION sift.projection_with(names text[], rollups text, options jsonb, layout jsonb) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
	RETURN (SELECT string_agg(format('%I = %L', k, v), ', ' ORDER BY o, k) FROM (
		SELECT 1 AS o, 'projection' AS k, array_to_string(names, ', ') AS v WHERE cardinality(names) > 0
		UNION ALL SELECT 2, 'rollup_columns', rollups WHERE rollups IS NOT NULL
		UNION ALL SELECT 3, e.key, e.value FROM jsonb_each_text(options) e
		UNION ALL SELECT 4, e.key, e.value FROM jsonb_each_text(layout) e) w);
COMMENT ON FUNCTION sift.projection_with(text[], text, jsonb, jsonb) IS 'Internal: the WITH list of an index made from projections';

-- The deparsed columns and filter of a definition: an empty temporary copy of the table gets the index, whose own deparsing is what an index of the table is compared with later.
-- It also checks the columns, the expressions, the filter and the options of the definition. The copy is dropped.
CREATE FUNCTION sift.projection_probe(source text, leaves text[], rollups text, filter text, options jsonb, OUT canonical text[], OUT canonical_filter text)
	LANGUAGE plpgsql SET client_min_messages = warning AS $$
DECLARE
	idx oid;
BEGIN
	EXECUTE format('CREATE TEMPORARY TABLE sift_projection_probe (LIKE %s)', source);
	EXECUTE format('CREATE INDEX sift_projection_probe_i ON pg_temp.sift_projection_probe USING sift_col (%s)%s%s', array_to_string(leaves, ', '),
				   COALESCE(' WITH (' || sift.projection_with(ARRAY[]::text[], rollups, options, '{}') || ')', ''), COALESCE(' WHERE ' || filter, ''));
	idx := 'pg_temp.sift_projection_probe_i'::regclass;
	canonical := ARRAY(SELECT pg_get_indexdef(idx, k, true) FROM generate_series(1, (SELECT indnatts FROM pg_index WHERE indexrelid = idx)) k ORDER BY k);
	canonical_filter := pg_get_expr((SELECT indpred FROM pg_index WHERE indexrelid = idx), (SELECT indrelid FROM pg_index WHERE indexrelid = idx), true);
	DROP TABLE pg_temp.sift_projection_probe;
END $$;
COMMENT ON FUNCTION sift.projection_probe(text, text[], text, text, jsonb) IS 'Internal: checks a definition on an empty temporary copy of the table and returns the deparsed columns and filter';

-- A rollup of an index holds the rollup a projection wants: the same dimensions, and every measure the projection names (none named: none named either, the defaults are the same).
-- 'a, b' or 'a, b: m1, m2'; an automatic choice is written 'auto: a, b'.
CREATE FUNCTION sift.projection_rollup_holds(have text, want text) RETURNS boolean LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
	WITH p AS (
		SELECT regexp_split_to_array(regexp_replace(COALESCE($1, ''), '^\s*auto:', ''), ':') AS h, regexp_split_to_array(regexp_replace($2, '^\s*auto:', ''), ':') AS w
	), q AS (
		SELECT ARRAY(SELECT trim(t) FROM unnest(string_to_array(h[1], ',')) t ORDER BY 1) AS hd, ARRAY(SELECT trim(t) FROM unnest(string_to_array(h[2], ',')) t WHERE h[2] IS NOT NULL) AS hm,
			   ARRAY(SELECT trim(t) FROM unnest(string_to_array(w[1], ',')) t ORDER BY 1) AS wd, ARRAY(SELECT trim(t) FROM unnest(string_to_array(w[2], ',')) t WHERE w[2] IS NOT NULL) AS wm
		FROM p)
	SELECT $2 IS NULL OR (hd = wd AND CASE WHEN cardinality(wm) = 0 THEN cardinality(hm) = 0 ELSE wm <@ hm END) FROM q
$$;
COMMENT ON FUNCTION sift.projection_rollup_holds(text, text) IS 'Internal: whether the rollup_columns of an index hold the rollup a projection wants (the same dimensions, every measure it names)';

-- What an index must keep of a projection, in one place: columns, filter implied, rollups, options. NULL when it holds, else the first reason.
CREATE FUNCTION sift.projection_holds(idx oid, pname text) RETURNS text LANGUAGE plpgsql STABLE AS $$
DECLARE
	p     sift.projections;
	ix    pg_index;
	have  text;
	o     record;
BEGIN
	SELECT * INTO p FROM sift.projections WHERE name = pname;
	IF NOT FOUND THEN RETURN format('the projection "%s" is not defined', pname); END IF;
	SELECT * INTO ix FROM pg_index WHERE indexrelid = idx;
	IF ix.indrelid IS DISTINCT FROM to_regclass(p.source)::oid THEN RETURN format('the index is not on %s', p.source); END IF;
	IF NOT (ix.indisvalid AND ix.indisready) THEN RETURN 'the index is not valid'; END IF;
	FOREACH have IN ARRAY p.canonical LOOP
		IF NOT EXISTS (SELECT 1 FROM generate_series(1, ix.indnatts) k WHERE pg_get_indexdef(idx, k, true) = have) THEN
			RETURN format('the index does not hold %s', have);
		END IF;
	END LOOP;
	IF NOT sift.projection_filter_served(ix.indrelid, COALESCE(p.filter, 'true'), idx) THEN
		RETURN format('the index keeps other rows than the filter or lacks its columns (%s)', COALESCE(pg_get_expr(ix.indpred, ix.indrelid, true), 'no predicate'));
	END IF;
	IF NOT sift.projection_rollup_holds(sift.projection_option(idx, 'rollup_columns'), p.rollups) THEN
		RETURN format('the index does not roll up %s', p.rollups);
	END IF;
	FOR o IN SELECT key, value FROM jsonb_each_text(p.options) LOOP
		IF sift.projection_option(idx, o.key) IS DISTINCT FROM o.value THEN
			RETURN format('the option %s of the index is not %s', o.key, o.value);
		END IF;
	END LOOP;
	RETURN NULL;
END $$;
COMMENT ON FUNCTION sift.projection_holds(oid, text) IS 'Whether a sift_col index still keeps what a projection needs (its columns, its filter implied, its rollups and options): NULL when it does, else the reason';

-- The layout an index of the same table must have to serve a new one: the options that cut and order the rows. Absent and an automatic choice are the same.
CREATE FUNCTION sift.projection_same_layout(idx oid, layout jsonb) RETURNS boolean LANGUAGE sql STABLE AS $$
	SELECT NOT EXISTS (
		SELECT 1 FROM unnest(ARRAY['key_order', 'key_cut', 'order_array_columns', 'order_array_group', 'order_layout']) k
		WHERE COALESCE(NULLIF(regexp_replace(COALESCE(sift.projection_option(idx, k), ''), '^auto.*', ''), 'off'), '') IS DISTINCT FROM COALESCE(layout ->> k, ''))
$$;
COMMENT ON FUNCTION sift.projection_same_layout(oid, jsonb) IS 'Internal: whether an index cuts and orders its rows as the layout says (an automatic choice counts as none)';

CREATE FUNCTION sift.projection_serves(idx oid, names text[], layout jsonb) RETURNS text LANGUAGE plpgsql STABLE AS $$
DECLARE
	n   text;
	why text;
BEGIN
	IF NOT sift.projection_same_layout(idx, layout) THEN RETURN 'the index has another layout'; END IF;
	FOREACH n IN ARRAY names LOOP
		why := sift.projection_holds(idx, n);
		IF why IS NOT NULL THEN RETURN why; END IF;
	END LOOP;
	RETURN NULL;
END $$;
COMMENT ON FUNCTION sift.projection_serves(oid, text[], jsonb) IS 'Internal: NULL when an index with that layout holds every projection of the list, else the reason';

-- A definition that an index lists changes its columns or filter only through an index that is made again: the guard backs the functions for direct changes of the table
CREATE FUNCTION sift.projection_guard() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
	n  text;
	ix text;
BEGIN
	IF TG_OP = 'UPDATE' AND ROW(OLD.name, OLD.source, OLD.leaves, OLD.filter) IS NOT DISTINCT FROM ROW(NEW.name, NEW.source, NEW.leaves, NEW.filter) THEN RETURN NEW; END IF;
	FOR n IN SELECT p.name FROM sift.projections p WHERE TG_OP = 'TRUNCATE' OR p.name = OLD.name LOOP
		SELECT string_agg(i::regclass::text, ', ') INTO ix FROM sift.projection_indexes(n) i;
		IF ix IS NOT NULL THEN
			RAISE EXCEPTION 'the projection "%" is held by %', n, ix USING ERRCODE = 'dependent_objects_still_exist',
				HINT = CASE WHEN TG_OP = 'UPDATE' THEN 'create the changed definition under another name and its indexes with sift.projection_index_sql, then drop these indexes and this definition'
							ELSE 'drop these indexes, or ALTER INDEX .. RESET (projection), first' END;
		END IF;
	END LOOP;
	RETURN CASE WHEN TG_OP = 'DELETE' THEN OLD WHEN TG_OP = 'TRUNCATE' THEN NULL ELSE NEW END;
END $$;
COMMENT ON FUNCTION sift.projection_guard() IS 'Internal: the trigger of sift.projections that refuses to delete a definition, or change its table, columns or filter, while an index lists it';
CREATE TRIGGER projection_guard_row BEFORE UPDATE OR DELETE ON sift.projections FOR EACH ROW EXECUTE FUNCTION sift.projection_guard();
CREATE TRIGGER projection_guard_truncate BEFORE TRUNCATE ON sift.projections FOR EACH STATEMENT EXECUTE FUNCTION sift.projection_guard();

CREATE FUNCTION sift.projection_create(name text, source regclass, columns text[], rollups text DEFAULT NULL, filter text DEFAULT NULL,
									   options jsonb DEFAULT '{}') RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	src   text := sift.scope_qualified(source);
	bad   text;
	pr    record;
BEGIN
	IF (SELECT relkind FROM pg_class WHERE oid = source) NOT IN ('r', 'p') THEN
		RAISE EXCEPTION 'projection_create: "%" is not a table', src USING ERRCODE = 'wrong_object_type';
	END IF;
	IF columns IS NULL OR cardinality(columns) = 0 THEN
		RAISE EXCEPTION 'projection_create: a projection needs at least one column' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	SELECT string_agg(k, ', ') INTO bad FROM jsonb_object_keys(options) k
		WHERE k IN ('projection', 'rollup_columns', 'rollup', 'rollup_dims', 'key_order', 'key_cut', 'key_cut_rows', 'order_columns', 'order_array_columns', 'order_array_group',
					'order_layout', 'point_tid_columns', 'track_changes', 'connect');
	IF bad IS NOT NULL THEN
		RAISE EXCEPTION 'projection_create: the options % belong to the index or to the rollups argument', bad USING ERRCODE = 'invalid_parameter_value',
			HINT = 'the layout of an index (key_order, key_cut, order arrays) is the layout argument of sift.projection_index';
	END IF;
	SELECT * INTO pr FROM sift.projection_probe(src, columns, rollups, filter, options);
	INSERT INTO sift.projections VALUES (name, src, columns, rollups, filter, options, pr.canonical, pr.canonical_filter);
END $$;
COMMENT ON FUNCTION sift.projection_create(text, regclass, text[], text, text, jsonb) IS 'Defines a named projection: the columns and expressions an index of the table stores, its rollups (rollup_columns), its filter (a partial index predicate) and other options of the index; checked on an empty copy of the table';

CREATE FUNCTION sift.projection_replace(name text, columns text[], rollups text DEFAULT NULL, filter text DEFAULT NULL, options jsonb DEFAULT '{}') RETURNS void
	LANGUAGE plpgsql AS $$
DECLARE
	old   sift.projections;
	pr    record;
	ix    oid;
	opt   text;
BEGIN
	SELECT * INTO old FROM sift.projections p WHERE p.name = projection_replace.name FOR UPDATE;
	IF NOT FOUND THEN
		RAISE EXCEPTION 'projection_replace: the projection "%" is not defined', name USING ERRCODE = 'undefined_object';
	END IF;
	SELECT * INTO pr FROM sift.projection_probe(old.source, columns, rollups, filter, options);
	UPDATE sift.projections p SET leaves = columns, rollups = projection_replace.rollups, filter = projection_replace.filter, options = projection_replace.options,
		canonical = pr.canonical, canonical_filter = pr.canonical_filter WHERE p.name = projection_replace.name;
	IF old.rollups IS NOT DISTINCT FROM rollups AND old.options = options THEN RETURN; END IF;
	FOR ix IN SELECT * FROM sift.projection_indexes(name) LOOP
		IF sift.projection_names(ix) = ARRAY[name] THEN
			IF sift.projection_with(ARRAY[]::text[], rollups, options, '{}') IS NOT NULL THEN
				EXECUTE format('ALTER INDEX %s SET (%s)', ix::regclass, sift.projection_with(ARRAY[]::text[], rollups, options, '{}'));
			END IF;
			FOR opt IN SELECT k FROM jsonb_object_keys(old.options) k WHERE NOT options ? k LOOP
				EXECUTE format('ALTER INDEX %s RESET (%I)', ix::regclass, opt);
			END LOOP;
			IF rollups IS NULL AND old.rollups IS NOT NULL THEN EXECUTE format('ALTER INDEX %s RESET (rollup_columns)', ix::regclass); END IF;
			RAISE NOTICE 'index "%" holds the projection "%": REINDEX INDEX % writes it with the new rollups and options', ix::regclass, name, ix::regclass;
		ELSE
			RAISE NOTICE 'index "%" holds other projections too: its options stay, sift.projection_status checks "%"', ix::regclass, name;
		END IF;
	END LOOP;
END $$;
COMMENT ON FUNCTION sift.projection_replace(text, text[], text, text, jsonb) IS 'Replaces a projection. The columns and the filter of a projection that an index lists cannot change: build the changed one under another name (sift.projection_index_sql). New rollups and options are set on the indexes that hold only this projection, which then need REINDEX';

CREATE FUNCTION sift.projection_drop(name text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DELETE FROM sift.projections p WHERE p.name = projection_drop.name;
	IF NOT FOUND THEN
		RAISE EXCEPTION 'projection_drop: the projection "%" is not defined', name USING ERRCODE = 'undefined_object';
	END IF;
END $$;
COMMENT ON FUNCTION sift.projection_drop(text) IS 'Drops a projection definition; refused while an index lists it';

-- The one statement that makes an index from projections: the CREATE INDEX of the union of their columns, or the ALTER INDEX that records that an existing index serves them. Their
-- tables, filters, rollups and options must be the same; layout is the options of this index that cut and order its rows (key_order, key_cut, order_array_group, ..).
CREATE FUNCTION sift.projection_plan(index text, projections text[], layout jsonb, conc boolean, OUT serving oid, OUT statement text) LANGUAGE plpgsql STABLE AS $$
DECLARE
	d      sift.projections[];
	p      sift.projections;
	n      text;
	have   text[] := '{}';
	cols   text[] := '{}';
	i      int;
	named  oid;
BEGIN
	IF projections IS NULL OR cardinality(projections) = 0 THEN
		RAISE EXCEPTION 'projection_index: no projection is named' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	FOREACH n IN ARRAY projections LOOP
		SELECT * INTO p FROM sift.projections WHERE name = n;
		IF NOT FOUND THEN
			RAISE EXCEPTION 'projection_index: the projection "%" is not defined', n USING ERRCODE = 'undefined_object';
		END IF;
		d := d || p;
	END LOOP;
	IF EXISTS (SELECT 1 FROM unnest(d) x WHERE (x.source, x.filter, x.rollups, x.options) IS DISTINCT FROM (d[1].source, d[1].filter, d[1].rollups, d[1].options)) THEN
		RAISE EXCEPTION 'projection_index: the projections % differ in table, filter, rollups or options', projections USING ERRCODE = 'invalid_parameter_value',
			HINT = 'one index holds projections with the same table, filter, rollups and options: make one index of each kind';
	END IF;
	IF EXISTS (SELECT 1 FROM jsonb_object_keys(layout) k WHERE k IN ('projection', 'rollup_columns')) THEN
		RAISE EXCEPTION 'projection_index: the layout cannot set projection or rollup_columns' USING ERRCODE = 'invalid_parameter_value';
	END IF;
	SELECT i.indexrelid INTO serving FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_col'
		WHERE i.indrelid = to_regclass(d[1].source) AND sift.projection_serves(i.indexrelid, projections, layout) IS NULL
		ORDER BY (c.relname = index) DESC, pg_relation_size(i.indexrelid), c.relname LIMIT 1;
	IF serving IS NOT NULL THEN
		statement := format('ALTER INDEX %s SET (projection = %L)', serving::regclass,
							array_to_string(ARRAY(SELECT DISTINCT u FROM unnest(sift.projection_names(serving) || projections) u ORDER BY u), ', '));
		RETURN;
	END IF;
	IF to_regclass(format('%s.%I', (SELECT nspname FROM pg_namespace WHERE oid = (SELECT relnamespace FROM pg_class WHERE oid = to_regclass(d[1].source))), index)) IS NOT NULL THEN
		RAISE EXCEPTION 'projection_index: relation "%" already exists and does not hold the projections', index USING ERRCODE = 'duplicate_table';
	END IF;
	FOR i IN 1 .. cardinality(d) LOOP
		FOREACH n IN ARRAY d[i].leaves LOOP
			IF NOT (d[i].canonical[array_position(d[i].leaves, n)] = ANY (have)) THEN
				cols := cols || n;
				have := have || d[i].canonical[array_position(d[i].leaves, n)];
			END IF;
		END LOOP;
	END LOOP;
	statement := format('CREATE INDEX %s%I ON %s USING sift_col (%s) WITH (%s)%s', CASE WHEN conc THEN 'CONCURRENTLY ' ELSE '' END, index, d[1].source,
						array_to_string(cols, ', '), sift.projection_with(projections, d[1].rollups, d[1].options, layout), COALESCE(' WHERE ' || d[1].filter, ''));
END $$;
COMMENT ON FUNCTION sift.projection_plan(text, text[], jsonb, boolean) IS 'Internal: the index that serves a list of projections with a layout, or the statement that makes it';

CREATE FUNCTION sift.projection_index(index text, projections text[], layout jsonb DEFAULT '{}') RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	pl record;
BEGIN
	SELECT * INTO pl FROM sift.projection_plan(index, projections, layout, false);
	EXECUTE pl.statement;
	IF pl.serving IS NULL THEN RETURN index; END IF;
	RAISE NOTICE 'the index "%" already holds the projections %: no index is built', pl.serving::regclass, array_to_string(projections, ', ')
		USING HINT = 'its columns, filter and rollups cover them with the same layout; sift.projection_status lists it';
	RETURN pl.serving::regclass::text;
END $$;
COMMENT ON FUNCTION sift.projection_index(text, text[], jsonb) IS 'Makes the sift_col index of one or more projections of one table, or, when an index of the table with the same layout already holds them, builds none and records that it serves them; returns the index';

CREATE FUNCTION sift.projection_index_sql(index text, projections text[], layout jsonb DEFAULT '{}', concurrently boolean DEFAULT true) RETURNS text
	LANGUAGE sql STABLE AS $$ SELECT statement FROM sift.projection_plan($1, $2, $3, $4) $$;
COMMENT ON FUNCTION sift.projection_index_sql(text, text[], jsonb, boolean) IS 'The statement sift.projection_index runs, for CREATE INDEX CONCURRENTLY and for the recipe that replaces a projection: run it yourself';

CREATE VIEW sift.projection_status AS
	SELECT p.name AS projection, p.source, l.index, CASE WHEN l.index IS NULL OR l.reason IS NOT NULL THEN 'broken' ELSE 'ok' END AS state,
		   COALESCE(l.reason, CASE WHEN l.index IS NULL THEN 'no index lists it' END) AS reason, l.shared, l.size
	FROM sift.projections p LEFT JOIN LATERAL (
		SELECT i::regclass::text AS index, sift.projection_holds(i, p.name) AS reason, cardinality(sift.projection_names(i)) AS shared, pg_relation_size(i) AS size
		FROM sift.projection_indexes(p.name) i) l ON true
	UNION ALL
	SELECT l.name, NULL, l.idx::regclass::text, 'broken', 'the projection is not defined', cardinality(sift.projection_names(l.idx)), pg_relation_size(l.idx)
	FROM sift.projection_listed() l WHERE NOT EXISTS (SELECT 1 FROM sift.projections p WHERE p.name = l.name)
	ORDER BY 1, 3;
COMMENT ON VIEW sift.projection_status IS 'Each projection with the indexes that list it: ok while the index still holds its columns, its filter, its rollups and its options, broken (with the reason) when it does not or when no index lists it; shared: how many projections the index lists';
