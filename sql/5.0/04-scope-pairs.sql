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
