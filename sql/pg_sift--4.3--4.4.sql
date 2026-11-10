\echo Use "ALTER EXTENSION pg_sift UPDATE TO '4.4'" to load this file. \quit

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

-- The INSERT of the copy's rows that a statement trigger runs for the transition table new_rows; same: more conditions on its rows. A row is added unless the copy has it: the other
-- table's trigger may have made it in the same statement. The probe of the copy is a lateral sub-select with LIMIT 1, which PostgreSQL can only run as an index probe of the
-- maintenance index for each row (an anti join it chose by estimate read the whole copy when the scope relation had no statistics).
CREATE FUNCTION sift.scope_fact_insert(c sift.scope_copies, same text) RETURNS text LANGUAGE sql STABLE AS $$
	SELECT format('INSERT INTO %s (%I, %s) SELECT s.%I, %s FROM new_rows n JOIN %s s ON s.%I = n.%I LEFT JOIN LATERAL (SELECT true AS present FROM %s x WHERE %s AND x.%I = s.%I LIMIT 1) h ON true WHERE h.present IS NULL%s',
		c.copy, c.scope_key, sift.scope_list(c.columns, ''), c.scope_key, sift.scope_list(c.columns, 'n'), c.scope_relation, c.member, c.fk,
		c.copy, sift.scope_cmp(c.key_columns, 'x', 'n', '=', ' AND '), c.scope_key, c.scope_key, same) $$;
CREATE FUNCTION sift.scope_pairs_insert(c sift.scope_copies, same text) RETURNS text LANGUAGE sql STABLE AS $$
	SELECT format('INSERT INTO %s (%I, %s) SELECT n.%I, %s FROM new_rows n JOIN %s f ON f.%I = n.%I LEFT JOIN LATERAL (SELECT true AS present FROM %s x WHERE %s AND x.%I = n.%I LIMIT 1) h ON true WHERE h.present IS NULL%s',
		c.copy, c.scope_key, sift.scope_list(c.columns, ''), c.scope_key, sift.scope_list(c.columns, 'f'), c.source, c.fk, c.member,
		c.copy, sift.scope_cmp(c.key_columns, 'x', 'f', '=', ' AND '), c.scope_key, c.scope_key, same) $$;
COMMENT ON FUNCTION sift.scope_fact_insert(sift.scope_copies, text) IS 'Internal: the INSERT of the copy''s rows that the trigger of the table of a scope copy runs for its transition table new_rows';
COMMENT ON FUNCTION sift.scope_pairs_insert(sift.scope_copies, text) IS 'Internal: the INSERT of the copy''s rows that the trigger of the scope relation of a scope copy runs for its transition table new_rows';

-- The statements that keep a copy exact. A partitioned copy gets the partition of a scope key that appears in the scope relation when the pair is inserted.
CREATE FUNCTION sift.scope_fact_trg() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
	c        sift.scope_copies;
	same     text;
	k        bigint;
BEGIN
	IF current_setting('transaction_isolation') <> 'read committed' THEN PERFORM sift.scope_refuse_isolation(TG_ARGV[0]); END IF;
	SELECT * INTO STRICT c FROM sift.scope_copies WHERE copy = TG_ARGV[0];
	-- a transaction does not see the uncommitted rows of the other table's: the writers of the facts share a lock that the writers of the pairs take alone
	PERFORM pg_advisory_xact_lock_shared(hashtext('sift.scope_copy'), hashtext(c.copy));
	IF TG_OP IN ('UPDATE', 'DELETE') THEN
		-- an updated row whose carried columns did not change keeps its copies
		same := CASE WHEN TG_OP = 'UPDATE' THEN format(' AND NOT EXISTS (SELECT 1 FROM new_rows n WHERE %s AND %s)',
			sift.scope_cmp(c.key_columns, 'n', 'o', '=', ' AND '), sift.scope_cmp(c.columns, 'n', 'o', 'IS NOT DISTINCT FROM', ' AND ')) ELSE '' END;
		EXECUTE format('DELETE FROM %s d USING old_rows o WHERE %s%s', c.copy,
			sift.scope_cmp(c.key_columns, 'd', 'o', '=', ' AND '), same);
	END IF;
	IF TG_OP IN ('INSERT', 'UPDATE') THEN
		same := CASE WHEN TG_OP = 'UPDATE' THEN format(' AND NOT EXISTS (SELECT 1 FROM old_rows o WHERE %s AND %s)',
			sift.scope_cmp(c.key_columns, 'o', 'n', '=', ' AND '), sift.scope_cmp(c.columns, 'o', 'n', 'IS NOT DISTINCT FROM', ' AND ')) ELSE '' END;
		-- a statement that changes the facts and the pairs fires this trigger first, on pairs whose partition their own trigger has not made yet
		IF c.partitioned THEN
			FOR k IN EXECUTE format('SELECT DISTINCT s.%I FROM new_rows n JOIN %s s ON s.%I = n.%I WHERE true%s', c.scope_key, c.scope_relation, c.member, c.fk, same) LOOP
				PERFORM sift.scope_partition(c.copy, k);
			END LOOP;
		END IF;
		EXECUTE sift.scope_fact_insert(c, same);
	END IF;
	RETURN NULL;
END $$;

CREATE FUNCTION sift.scope_pairs_trg() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
	c        sift.scope_copies;
	same     text;
	k        bigint;
BEGIN
	IF current_setting('transaction_isolation') <> 'read committed' THEN PERFORM sift.scope_refuse_isolation(TG_ARGV[0]); END IF;
	SELECT * INTO STRICT c FROM sift.scope_copies WHERE copy = TG_ARGV[0];
	PERFORM pg_advisory_xact_lock(hashtext('sift.scope_copy'), hashtext(c.copy));
	IF TG_OP IN ('UPDATE', 'DELETE') THEN
		same := CASE WHEN TG_OP = 'UPDATE' THEN format(' AND NOT EXISTS (SELECT 1 FROM new_rows n WHERE n.%1$I = o.%1$I AND n.%2$I = o.%2$I)', c.member, c.scope_key) ELSE '' END;
		EXECUTE format('DELETE FROM %s d USING old_rows o WHERE d.%I = o.%I AND d.%I = o.%I%s', c.copy, c.fk, c.member, c.scope_key, c.scope_key, same);
	END IF;
	IF TG_OP IN ('INSERT', 'UPDATE') THEN
		same := CASE WHEN TG_OP = 'UPDATE' THEN format(' AND NOT EXISTS (SELECT 1 FROM old_rows o WHERE n.%1$I = o.%1$I AND n.%2$I = o.%2$I)', c.member, c.scope_key) ELSE '' END;
		IF c.partitioned THEN
			FOR k IN EXECUTE format('SELECT DISTINCT n.%I FROM new_rows n WHERE true%s', c.scope_key, same) LOOP
				PERFORM sift.scope_partition(c.copy, k);
			END LOOP;
		END IF;
		EXECUTE sift.scope_pairs_insert(c, same);
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

CREATE FUNCTION sift.scope_create(copy text, source regclass, fk name, scope_relation regclass, scope_key name, columns name[] DEFAULT NULL,
								  partition boolean DEFAULT false, member name DEFAULT NULL, order_column name DEFAULT NULL,
								  options text DEFAULT NULL) RETURNS bigint
	LANGUAGE plpgsql AS $$
DECLARE
	cap       CONSTANT float8 := 1.8;
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
	-- the options of a sift_col index that describe its columns; those keyed on the table's own key (key_*, order_*, point_*, count_columns, track_changes, connect) are derived again for the scope key
	inherit   CONSTANT text[] := ARRAY['paths', 'compression', 'preagg', 'rollup', 'rollup_dims', 'rollup_leaves', 'rollup_columns', 'rollup_on_demand', 'calendar_columns',
									   'cluster_advice', 'cube_minmax', 'dict_lookup', 'array_lengths', 'hll_columns', 'return_columns', 'superunit',
									   'write_buffer_limit', 'dead_rewrite_pct', 'vacuum_truncate'];
BEGIN
	member := COALESCE(member, fk);
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
		-- a value that starts with 'auto:' is what the build read from the documents: the copy reads its own
		SELECT string_agg(format('%s = %L', split_part(o, '=', 1), substr(o, strpos(o, '=') + 1)), ', ') INTO kept FROM pg_class c, unnest(c.reloptions) o
			WHERE c.oid = fidx AND split_part(o, '=', 1) = ANY (inherit) AND split_part(o, '=', 1) <> ALL (named) AND o !~ '^[^=]*=auto:';
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
	IF mult > cap THEN
		RAISE EXCEPTION 'scope_create: a row of "%" would have % copies on average (the limit is %)', src, round(mult::numeric, 2), cap
			USING DETAIL = format('The copy would hold that many times the rows of the table that reach a scope key: about %s MB, +%s%% over the table and its indexes.',
								  round((est / 1048576)::numeric), round(est * 100 / (SELECT sum(pg_total_relation_size(r)) FROM sift.scope_relations(source) r))),
				  HINT = 'Key the copy by a scope relation whose members belong to fewer keys, or read the table through the join.', ERRCODE = 'feature_not_supported';
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
							  round(multi * 100) || '%', maxk, round((est / 1048576.0)::numeric, 1), round(est * 100 / (SELECT sum(pg_total_relation_size(r)) FROM sift.scope_relations(source) r)));
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
COMMENT ON FUNCTION sift.scope_create(text, regclass, name, regclass, name, name[], boolean, name, name, text) IS 'Creates a copy of a table keyed by a scope key reached through a scope relation of (member, scope key) pairs, with a sift_col index by the scope key and triggers that keep it exact; returns the rows. It takes the columns and the column options (paths, ..) of the table''s sift_col index; options adds or replaces some. VACUUM it afterwards: sift.scope_status tells. See INDEX-RECIPES';

-- Pairs kept from a view: the scope relation of a copy (or a table read on its own) is a table of unique (member key, scope key) pairs that a view defines over
-- source tables (assignments, group membership). sift.scope_refresh_pairs recomputes the pairs of named member ids or scope keys from the view and applies the difference;
-- sift.scope_follow_pairs puts statement triggers on the source tables that call it for the member ids or scope keys a change names, in the transaction of the change,
-- so the pairs stay equal to the view and a copy built on them (sift.scope_create) stays equal to the join.
CREATE TABLE sift.scope_follows (
	pairs         text   NOT NULL,
	definition    text   NOT NULL,
	source        text   NOT NULL,
	refresh_by    text   NOT NULL CHECK (refresh_by IN ('member', 'key')),
	source_column name   NOT NULL,
	expires       name,
	swept_at      timestamptz NOT NULL,
	PRIMARY KEY (pairs, source, source_column)
);
SELECT pg_catalog.pg_extension_config_dump('sift.scope_follows', '');

COMMENT ON TABLE sift.scope_follows IS 'The tables that sift.scope_follow_pairs follows: for each pair table, its defining view, a source table of the view, the column that names the member ids or scope keys a change touches, and the column of the source that expires pairs';

-- The pairs of the named member ids or scope keys (by = 'member' or 'key'; ids NULL: all) as the view gives them: the pairs the table has for them are read once and compared with the
-- view's, the missing ones are inserted and the extra ones deleted, in one statement.
-- The first column of the view is the member, the second the scope key; the pair table has columns of those names. One refresh of a pair table at a time: a later
-- one waits for the commit of the earlier and reads what it wrote, so two writers of different source tables never leave a pair that neither saw.
CREATE FUNCTION sift.scope_refresh_pairs(pairs regclass, definition regclass, by text, ids bigint[] DEFAULT NULL) RETURNS TABLE (inserted bigint, deleted bigint)
	LANGUAGE plpgsql AS $$
DECLARE
	cols   name[];
	target text := sift.scope_qualified(pairs);
	col    name;
BEGIN
	SELECT array_agg(a.attname ORDER BY a.attnum) INTO cols FROM pg_attribute a WHERE a.attrelid = definition AND a.attnum > 0 AND NOT a.attisdropped;
	IF cardinality(cols) <> 2 THEN
		RAISE EXCEPTION 'scope_refresh_pairs: "%" must have two columns, the member and the scope key', definition USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF by NOT IN ('member', 'key') THEN
		RAISE EXCEPTION 'scope_refresh_pairs: by is "%", it must be ''member'' or ''key''', by USING ERRCODE = 'invalid_parameter_value';
	END IF;
	IF (SELECT count(*) FROM pg_attribute a WHERE a.attrelid = pairs AND a.attname = ANY (cols) AND NOT a.attisdropped) <> 2 THEN
		RAISE EXCEPTION 'scope_refresh_pairs: table % has no columns %, %', target, cols[1], cols[2] USING ERRCODE = 'undefined_column';
	END IF;
	col := cols[CASE by WHEN 'member' THEN 1 ELSE 2 END];
	PERFORM pg_advisory_xact_lock(hashtextextended('sift.scope_pairs ' || target, 0));
	RETURN QUERY EXECUTE format($q$
		WITH want AS MATERIALIZED (SELECT DISTINCT %1$I, %2$I FROM %3$s%5$s),
		     have AS MATERIALIZED (SELECT ctid AS tid, %1$I, %2$I FROM %4$s p WHERE %6$s),
		     gone AS (DELETE FROM %4$s WHERE ctid = ANY (ARRAY(SELECT h.tid FROM have h WHERE NOT EXISTS (SELECT 1 FROM want w WHERE w.%1$I = h.%1$I AND w.%2$I = h.%2$I))) RETURNING 1),
		     ins  AS (INSERT INTO %4$s (%1$I, %2$I) SELECT w.%1$I, w.%2$I FROM want w
		              WHERE NOT EXISTS (SELECT 1 FROM have h WHERE h.%1$I = w.%1$I AND h.%2$I = w.%2$I) ON CONFLICT DO NOTHING RETURNING 1)
		SELECT (SELECT count(*) FROM ins), (SELECT count(*) FROM gone)$q$,
		cols[1], cols[2], sift.scope_qualified(definition), target,
		CASE WHEN ids IS NULL THEN '' ELSE format(' WHERE %I = ANY ($1)', col) END, CASE WHEN ids IS NULL THEN 'true' ELSE format('p.%I = ANY ($1)', col) END) USING ids;
	-- a refresh of all the pairs fills a new or emptied table: the statements that join it are planned from its statistics, which autovacuum would only make later
	IF ids IS NULL THEN
		EXECUTE format('ANALYZE %s', target);
	END IF;
END $$;

-- The member ids or scope keys that a change to the rows of a followed table names: the column of the old and new rows
CREATE FUNCTION sift.scope_follow_trg() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
	f   sift.scope_follows;
	ids bigint[];
BEGIN
	SELECT * INTO STRICT f FROM sift.scope_follows s WHERE s.pairs = TG_ARGV[0] AND s.source = TG_ARGV[1] AND s.source_column = TG_ARGV[2];
	IF TG_OP <> 'TRUNCATE' THEN
		EXECUTE format('SELECT COALESCE(array_agg(DISTINCT c), ''{}'') FROM (%s) t(c) WHERE c IS NOT NULL',
			CASE TG_OP WHEN 'INSERT' THEN format('SELECT %I::bigint FROM new_rows', f.source_column) WHEN 'DELETE' THEN format('SELECT %I::bigint FROM old_rows', f.source_column)
			ELSE format('SELECT %1$I::bigint FROM old_rows UNION ALL SELECT %1$I::bigint FROM new_rows', f.source_column) END) INTO ids;
		IF cardinality(ids) = 0 THEN
			RETURN NULL;
		END IF;
	END IF;
	PERFORM sift.scope_refresh_pairs(f.pairs::regclass, f.definition::regclass, f.refresh_by, ids);
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
	IF by NOT IN ('member', 'key') THEN
		RAISE EXCEPTION 'scope_follow_pairs: by is "%", it must be ''member'' or ''key''', by USING ERRCODE = 'invalid_parameter_value';
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
	r      record;
	typ    text;
BEGIN
	inserted := 0; deleted := 0;
	FOR f IN SELECT * FROM sift.scope_follows s WHERE s.pairs = sift.scope_qualified(scope_expire_pairs.pairs) AND s.expires IS NOT NULL ORDER BY s.source FOR UPDATE LOOP
		SELECT format_type(a.atttypid, NULL) INTO typ FROM pg_attribute a WHERE a.attrelid = f.source::regclass AND a.attname = f.expires;
		EXECUTE format('SELECT COALESCE(array_agg(DISTINCT %1$I::bigint), ''{}'') FROM %2$s WHERE %3$I BETWEEN ($1 - $2)::%4$s AND $3::%4$s AND %1$I IS NOT NULL',
					   f.source_column, f.source, f.expires, typ) INTO ids USING f.swept_at, overlap, now();
		IF cardinality(ids) > 0 THEN
			SELECT * INTO r FROM sift.scope_refresh_pairs(f.pairs::regclass, f.definition::regclass, f.refresh_by, ids);
			inserted := inserted + r.inserted; deleted := deleted + r.deleted;
		END IF;
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

COMMENT ON FUNCTION sift.scope_refresh_pairs(regclass, regclass, text, bigint[]) IS 'Recomputes the pairs of the named member ids or scope keys (by = member or key; ids NULL: all) of a table of unique (member, scope key) pairs from a view of the same two columns, inserting the missing pairs and deleting the extra ones in one statement; returns the counts';
COMMENT ON FUNCTION sift.scope_follow_pairs(regclass, regclass, source regclass, text, name, name) IS 'Makes a table of (member, scope key) pairs equal to its view and keeps it so: statement triggers on a source table of the view refresh the member ids or scope keys that a change names, in the transaction of the change, and so keep a scope copy built on the pairs exact; returns the pairs the first refresh inserted and deleted';
COMMENT ON FUNCTION sift.scope_expire_pairs(regclass, interval) IS 'Applies the expiries by date that happened with no write to the source tables of followed pairs: refreshes the member ids or scope keys of the rows whose expires column passed since the last call; returns the counts';
COMMENT ON FUNCTION sift.scope_unfollow_pairs(regclass, regclass) IS 'Drops the triggers that keep a table of pairs equal to its view, of one source table or of all of them';
COMMENT ON FUNCTION sift.scope_follow_trg() IS 'Internal: the statement trigger of a source table of followed pairs; refreshes the member ids or scope keys that the changed rows name';
COMMENT ON FUNCTION sift.scope_fact_trg() IS 'Internal: the statement trigger of the table of a scope copy; applies the inserted, updated and deleted rows to the copy; refuses REPEATABLE READ and SERIALIZABLE transactions';
COMMENT ON FUNCTION sift.scope_pairs_trg() IS 'Internal: the statement trigger of the scope relation of a scope copy; applies the inserted, updated and deleted pairs to the copy; refuses REPEATABLE READ and SERIALIZABLE transactions';
COMMENT ON FUNCTION sift.scope_truncate_trg() IS 'Internal: the TRUNCATE trigger of the table and of the scope relation of a scope copy; empties the copy; refuses REPEATABLE READ and SERIALIZABLE transactions';
