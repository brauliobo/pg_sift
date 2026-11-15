-- Pairs kept from a view: the pair table of a scope index (or a table read on its own) is a table of unique (member key, scope key) pairs that a view defines over
-- source tables (assignments, group membership). sift.scope_refresh_pairs recomputes the pairs of named member ids or scope keys from the view and applies the difference;
-- sift.scope_follow_pairs puts statement triggers on the source tables that call it for the member ids or scope keys a change names, in the transaction of the change,
-- so the pairs stay equal to the view and a scope index on them stays equal to the join.
CREATE FUNCTION sift.scope_qualified(rel regclass) RETURNS text LANGUAGE sql STABLE PARALLEL SAFE
	RETURN (SELECT format('%I.%I', n.nspname, c.relname) FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = rel);

COMMENT ON FUNCTION sift.scope_qualified(regclass) IS 'Internal: the schema-qualified, quoted name of a relation';

-- The tables are kept by identity: a regclass is the oid of the table, so a rename or a move to another schema leaves it as it was (it dumps and restores as the name the table has then; the
-- columns are names, which the event trigger sift.col_names_follow keeps when one is renamed: the column of the source that names the members or the keys (and, by pair, the members),
-- the column that expires pairs, and the columns of the pair table that hold the member and the scope key (the columns of the view are found by position). The triggers of a follow are
-- found by the name they carry, trigger_base with a suffix, which the dump keeps: a trigger has no arguments that a restore would leave naming oids.
CREATE TABLE sift.scope_follows (
	pairs         regclass NOT NULL,
	definition    regclass NOT NULL,
	source        regclass NOT NULL,
	trigger_base  name     NOT NULL,
	refresh_by    text     NOT NULL CHECK (refresh_by IN ('member', 'key', 'pair')),
	source_column name     NOT NULL,
	source_member name,
	member_column name     NOT NULL,
	key_column    name     NOT NULL,
	expires       name,
	swept_at      timestamptz NOT NULL,
	PRIMARY KEY (pairs, source, source_column),
	UNIQUE (source, trigger_base)
);
-- a row of a table that is gone (a stale oid prints as a number) is never written
SELECT pg_catalog.pg_extension_config_dump('sift.scope_follows', 'WHERE source::pg_catalog.oid IN (SELECT oid FROM pg_catalog.pg_class) AND pairs::pg_catalog.oid IN (SELECT oid FROM pg_catalog.pg_class) AND definition::pg_catalog.oid IN (SELECT oid FROM pg_catalog.pg_class)');

COMMENT ON TABLE sift.scope_follows IS 'The tables that sift.scope_follow_pairs follows: for each pair table, its defining view, a source table of the view, the base of the names of its triggers, the column that names the member ids or scope keys a change touches (by pair, the one that names the members too), the columns of the pair table that hold the member and the scope key, and the column of the source that expires pairs';

-- The condition on the columns of a view or of the pair table (qualified by q) that names the rows of a refresh: $1 are the member ids or the scope keys, $2 the scope keys of a pair mode
CREATE FUNCTION sift.scope_pairs_where(by text, cols name[], everything boolean, q text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN everything THEN 'true'
		WHEN by = 'member' THEN format('%1$s%2$I = ANY ($1)', q, cols[1])
		WHEN by = 'key' THEN format('%1$s%3$I = ANY ($1)', q, cols[1], cols[2])
		ELSE format('%1$s%2$I = ANY ($1) AND %1$s%3$I = ANY ($2) AND EXISTS (SELECT 1 FROM unnest($1, $2) u(m, k) WHERE u.m = %1$s%2$I AND u.k = %1$s%3$I)', q, cols[1], cols[2]) END
$$;

-- The columns of a pair table that hold the member and the scope key: the ones the follow of the pair table recorded (they follow the rename of a column), else the columns of the view
CREATE FUNCTION sift._pair_columns(pairs regclass, definition regclass) RETURNS name[] LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
	SELECT COALESCE((SELECT ARRAY[f.member_column, f.key_column] FROM sift.scope_follows f WHERE f.pairs = $1 AND f.definition = $2 ORDER BY f.source LIMIT 1),
					(SELECT array_agg(a.attname ORDER BY a.attnum) FROM pg_attribute a WHERE a.attrelid = $2 AND a.attnum > 0 AND NOT a.attisdropped))
$$;
COMMENT ON FUNCTION sift._pair_columns(regclass, regclass) IS 'Internal: the columns of the pair table that hold the member and the scope key';

-- The pairs of the named member ids, scope keys or (member, key) pairs (by = 'member', 'key' or 'pair': ids are the members and keys the scope keys, element by element; ids NULL: all)
-- as the view gives them: the pairs the table has for them are read once and compared with the view's, the missing ones are inserted and the extra ones deleted, in one statement.
-- The first column of the view is the member, the second the scope key; the pair table has columns of those names. One refresh of a pair table at a time: a later
-- one waits for the commit of the earlier and reads what it wrote, so two writers of different source tables never leave a pair that neither saw.
CREATE FUNCTION sift.scope_refresh_pairs(pairs regclass, definition regclass, by text, ids bigint[] DEFAULT NULL, keys bigint[] DEFAULT NULL) RETURNS TABLE (inserted bigint, deleted bigint)
	LANGUAGE plpgsql AS $$
DECLARE
	cols   name[];
	pcols  name[] := sift._pair_columns(pairs, definition);
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
	IF (SELECT count(*) FROM pg_attribute a WHERE a.attrelid = pairs AND a.attname = ANY (pcols) AND NOT a.attisdropped) <> 2 THEN
		RAISE EXCEPTION 'scope_refresh_pairs: table % has no columns %, %', target, pcols[1], pcols[2] USING ERRCODE = 'undefined_column';
	END IF;
	PERFORM pg_advisory_xact_lock(hashtext('sift.scope_pairs'), pairs::oid::int);
	RETURN QUERY EXECUTE format($q$
		WITH want AS MATERIALIZED (SELECT DISTINCT %1$I AS m, %2$I AS k FROM %3$s WHERE %5$s),
		     have AS MATERIALIZED (SELECT ctid AS tid, %7$I AS m, %8$I AS k FROM %4$s p WHERE %6$s),
		     gone AS (DELETE FROM %4$s WHERE ctid = ANY (ARRAY(SELECT h.tid FROM have h WHERE NOT EXISTS (SELECT 1 FROM want w WHERE w.m = h.m AND w.k = h.k))) RETURNING 1),
		     ins  AS (INSERT INTO %4$s (%7$I, %8$I) SELECT w.m, w.k FROM want w
		              WHERE NOT EXISTS (SELECT 1 FROM have h WHERE h.m = w.m AND h.k = w.k) ON CONFLICT DO NOTHING RETURNING 1)
		SELECT (SELECT count(*) FROM ins), (SELECT count(*) FROM gone)$q$,
		cols[1], cols[2], sift.scope_qualified(definition), target,
		sift.scope_pairs_where(by, cols, ids IS NULL, ''), sift.scope_pairs_where(by, pcols, ids IS NULL, 'p.'), pcols[1], pcols[2]) USING ids, keys;
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
		ELSE format('SELECT COALESCE(array_agg(m), ''{}''), COALESCE(array_agg(k), ''{}'') FROM (SELECT DISTINCT %1$I::bigint m, %2$I::bigint k FROM (%3$s) t) s', f.source_member, f.source_column, changed) END
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
			SELECT * INTO r FROM sift.scope_refresh_pairs(f.pairs, f.definition, x.mode, x.a, x.b);
			inserted := inserted + r.inserted; deleted := deleted + r.deleted;
		END IF;
	END LOOP;
	RETURN NEXT;
END $$;

-- The statement trigger of a followed table: the rows it changed (the old and the new ones of an update) are what is refreshed; a TRUNCATE refreshes all the pairs
-- (it finds its follow by the table it fires on and its own name)
CREATE FUNCTION sift.scope_follow_trg() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
	f    sift.scope_follows;
	ids  bigint[];
	keys bigint[];
BEGIN
	SELECT * INTO STRICT f FROM sift.scope_follows s WHERE s.source::oid = TG_RELID AND s.trigger_base = regexp_replace(TG_NAME, '_s(ins|upd|del|t)$', '');
	IF TG_OP = 'TRUNCATE' THEN
		PERFORM sift.scope_refresh_pairs(f.pairs, f.definition, f.refresh_by);
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
	src    text := sift.scope_qualified(source);
	base   name := format('sift_scope_%s', left(md5(format('%s %s %s', pairs::oid, source::oid, source_column)), 12));
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
	INSERT INTO sift.scope_follows SELECT pairs, definition, source, base, by, source_column, CASE WHEN by = 'pair' THEN v.m END, v.m, v.k, expires, now()
		FROM (SELECT (array_agg(a.attname ORDER BY a.attnum))[1] m, (array_agg(a.attname ORDER BY a.attnum))[2] k FROM pg_attribute a
			  WHERE a.attrelid = definition AND a.attnum > 0 AND NOT a.attisdropped) v;
	FOR tr IN SELECT unnest(ARRAY['ins', 'upd', 'del']) LOOP
		EXECUTE format('CREATE TRIGGER %I AFTER %s ON %s REFERENCING %s AS %s FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_follow_trg()',
					   base || '_s' || tr, CASE tr WHEN 'ins' THEN 'INSERT' WHEN 'upd' THEN 'UPDATE' ELSE 'DELETE' END, src,
					   CASE tr WHEN 'ins' THEN 'NEW TABLE' WHEN 'upd' THEN 'OLD TABLE AS old_rows NEW TABLE' ELSE 'OLD TABLE' END,
					   CASE tr WHEN 'del' THEN 'old_rows' ELSE 'new_rows' END);
	END LOOP;
	EXECUTE format('CREATE TRIGGER %I AFTER TRUNCATE ON %s FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_follow_trg()', base || '_st', src);
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
	FOR f IN SELECT * FROM sift.scope_follows s WHERE s.pairs = scope_expire_pairs.pairs AND s.expires IS NOT NULL ORDER BY s.source FOR UPDATE LOOP
		SELECT format_type(a.atttypid, NULL) INTO typ FROM pg_attribute a WHERE a.attrelid = f.source AND a.attname = f.expires;
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
	FOR f IN WITH d AS (DELETE FROM sift.scope_follows s WHERE s.pairs = scope_unfollow_pairs.pairs
						AND (scope_unfollow_pairs.source IS NULL OR s.source = scope_unfollow_pairs.source) RETURNING s.*) SELECT * FROM d LOOP
		FOREACH t IN ARRAY ARRAY['sins', 'supd', 'sdel', 'st'] LOOP
			EXECUTE format('DROP TRIGGER IF EXISTS %I ON %s', f.trigger_base || '_' || t, f.source);
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
