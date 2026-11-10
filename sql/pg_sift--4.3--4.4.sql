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

-- The statements that keep a copy exact. A partitioned copy gets the partition of a scope key that appears in the scope relation when the pair is inserted.
CREATE FUNCTION sift.scope_fact_trg() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
	c        sift.scope_copies;
	same     text;
BEGIN
	SELECT * INTO STRICT c FROM sift.scope_copies WHERE copy = TG_ARGV[0];
	IF TG_OP IN ('UPDATE', 'DELETE') THEN
		-- an updated row whose carried columns did not change keeps its copies
		same := CASE WHEN TG_OP = 'UPDATE' THEN format(' AND NOT EXISTS (SELECT 1 FROM new_rows n WHERE %s AND %s)',
			sift.scope_cmp(c.key_columns, 'n', 'o', '=', ' AND '), sift.scope_cmp(c.columns, 'n', 'o', 'IS NOT DISTINCT FROM', ' AND ')) ELSE '' END;
		EXECUTE format('DELETE FROM %s d USING old_rows o WHERE %s AND %s%s', c.copy,
			sift.scope_cmp(ARRAY[c.fk], 'd', 'o', '=', ''), sift.scope_cmp(c.key_columns, 'd', 'o', '=', ' AND '), same);
	END IF;
	IF TG_OP IN ('INSERT', 'UPDATE') THEN
		same := CASE WHEN TG_OP = 'UPDATE' THEN format(' AND NOT EXISTS (SELECT 1 FROM old_rows o WHERE %s AND %s)',
			sift.scope_cmp(c.key_columns, 'o', 'n', '=', ' AND '), sift.scope_cmp(c.columns, 'o', 'n', 'IS NOT DISTINCT FROM', ' AND ')) ELSE '' END;
		EXECUTE format('INSERT INTO %s (%I, %s) SELECT s.%I, %s FROM new_rows n JOIN %s s ON s.%I = n.%I WHERE true%s ON CONFLICT DO NOTHING', c.copy, c.scope_key,
			sift.scope_list(c.columns, '')::text, c.scope_key, sift.scope_list(c.columns, 'n'), c.scope_relation, c.member, c.fk, same);
	END IF;
	RETURN NULL;
END $$;

CREATE FUNCTION sift.scope_pairs_trg() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
	c        sift.scope_copies;
	same     text;
	k        bigint;
BEGIN
	SELECT * INTO STRICT c FROM sift.scope_copies WHERE copy = TG_ARGV[0];
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
		EXECUTE format('INSERT INTO %s (%I, %s) SELECT n.%I, %s FROM new_rows n JOIN %s f ON f.%I = n.%I WHERE true%s ON CONFLICT DO NOTHING', c.copy, c.scope_key,
			sift.scope_list(c.columns, '')::text, c.scope_key, sift.scope_list(c.columns, 'f'), c.source, c.fk, c.member, same);
	END IF;
	RETURN NULL;
END $$;

CREATE FUNCTION sift.scope_truncate_trg() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
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

COMMENT ON FUNCTION sift.scope_partition(text, bigint) IS 'Internal: the partition of a scope key in a partitioned scope copy, created when it is missing';

-- A name for the triggers of a copy: stable text, not the oid, so a restore keeps it
CREATE FUNCTION sift.scope_trigger_name(copy text, kind text) RETURNS name LANGUAGE sql IMMUTABLE PARALLEL SAFE
	RETURN format('sift_scope_%s_%s', left(md5(copy), 12), kind)::name;

COMMENT ON FUNCTION sift.scope_trigger_name(text, text) IS 'Internal: the name of one trigger of a scope copy';

CREATE FUNCTION sift.scope_create(copy text, source regclass, fk name, scope_relation regclass, scope_key name, columns name[] DEFAULT NULL,
								  partition boolean DEFAULT false, member name DEFAULT NULL, order_column name DEFAULT NULL) RETURNS bigint
	LANGUAGE plpgsql AS $$
DECLARE
	cap       CONSTANT float8 := 3.0;
	src       text := sift.scope_qualified(source);
	rel       text := sift.scope_qualified(scope_relation);
	cname     text;
	pk        name[];
	cols      name[];
	mult      float8;
	key_type  text;
	ord       name := order_column;
	defs      text;
	tr        text;
	rows      bigint;
	base      text;
	opts      text;
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
	-- carried columns: the primary key and the foreign key always, the rest as asked (default all)
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

	-- replication of the copy: the rows a fact has, weighted by its rows, over the facts that reach a key
	EXECUTE format('SELECT sum(f.n * p.k)::float8 / NULLIF(sum(f.n), 0) FROM (SELECT %2$I m, count(*) n FROM %1$s GROUP BY 1) f JOIN (SELECT %4$I m, count(*) k FROM %3$s GROUP BY 1) p USING (m)',
				   src, fk, rel, member) INTO mult;
	IF mult > cap THEN
		RAISE EXCEPTION 'scope_create: a row of "%" would have % copies on average (the limit is %)', src, round(mult::numeric, 2), cap
			USING DETAIL = 'The copy would hold that many times the rows of the table that reach a scope key and its index grows with them.',
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

	opts := CASE WHEN partition THEN COALESCE(format('key_order = %L', ord), '') ELSE format('key_order = %L', concat_ws(', ', scope_key, ord)) END;
	SELECT relname INTO base FROM pg_class WHERE oid = to_regclass(cname);
	EXECUTE format('CREATE INDEX %I ON %s USING sift_col (%I, %s)%s', left(base, 55) || '_col', cname, scope_key, sift.scope_list(cols, '')::text, CASE WHEN opts = '' THEN '' ELSE ' WITH (' || opts || ')' END);
	-- what a change of the facts or of the pairs looks up in the copy: the foreign key and the primary key identify its rows
	EXECUTE format('CREATE UNIQUE INDEX %I ON %s (%I, %s, %I)', left(base, 54) || '_maint', cname, fk,
				   array_to_string(ARRAY(SELECT format('%I', k) FROM unnest(pk) k), ', '), scope_key);

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
	RAISE NOTICE 'scope copy %: % rows, % copies per row of "%" that reaches a key', cname, rows, round(mult::numeric, 2), src;
	RETURN rows;
END $$;

-- The rows of a copy that differ from the join of the table with the scope relation: 'missing' (the join has it, the copy does not), 'extra' (the copy has a row the join does not)
-- and 'differs' (the carried columns are not those of the table). No row: the copy is exact.
CREATE FUNCTION sift.scope_verify(copy text) RETURNS TABLE (problem text, scope_key bigint, row_key jsonb) LANGUAGE plpgsql STABLE AS $$
DECLARE
	c sift.scope_copies;
BEGIN
	SELECT * INTO c FROM sift.scope_copies s WHERE s.copy = sift.scope_qualified(to_regclass(scope_verify.copy));
	IF NOT FOUND THEN
		RAISE EXCEPTION 'scope_verify: "%" is not a scope copy', scope_verify.copy USING ERRCODE = 'undefined_table';
	END IF;
	RETURN QUERY EXECUTE format($q$
		SELECT CASE WHEN a.%1$I IS NULL THEN 'missing' WHEN e.%1$I IS NULL THEN 'extra' ELSE 'differs' END, COALESCE(a.%1$I, e.%1$I)::bigint,
			   jsonb_build_object(%4$s)
		FROM (SELECT s.%7$I AS %1$I, %5$s FROM %2$s f JOIN %3$s s ON s.%6$I = f.%8$I) e
		FULL JOIN %9$s a ON a.%1$I = e.%1$I AND %10$s
		WHERE a.%1$I IS NULL OR e.%1$I IS NULL OR %11$s$q$,
		c.scope_key, c.source, c.scope_relation,
		(SELECT string_agg(format('%L, COALESCE(a.%I, e.%I)', k, k, k), ', ') FROM unnest(c.key_columns) k),
		sift.scope_list(c.columns, 'f'), c.member, c.scope_key, c.fk, c.copy,
		sift.scope_cmp(c.key_columns, 'a', 'e', '=', ' AND '),
		'(' || (SELECT string_agg(format('a.%1$I IS DISTINCT FROM e.%1$I', k), ' OR ') FROM unnest(c.columns) k) || ')');
END $$;

COMMENT ON FUNCTION sift.scope_verify(text) IS 'The rows of a scope copy that differ from the join of its table with the scope relation (missing, extra or differs); no rows: the copy is exact';

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
COMMENT ON FUNCTION sift.scope_create(text, regclass, name, regclass, name, name[], boolean, name, name) IS 'Creates a copy of a table keyed by a scope key reached through a scope relation of (member, scope key) pairs, with a sift_col index keyed by the scope key and triggers that keep it exact; returns the rows of the copy';

COMMENT ON FUNCTION sift.scope_fact_trg() IS 'Internal: the statement trigger of the table of a scope copy; applies the inserted, updated and deleted rows to the copy';
COMMENT ON FUNCTION sift.scope_pairs_trg() IS 'Internal: the statement trigger of the scope relation of a scope copy; applies the inserted, updated and deleted pairs to the copy';
COMMENT ON FUNCTION sift.scope_truncate_trg() IS 'Internal: the TRUNCATE trigger of the table and of the scope relation of a scope copy; empties the copy';
