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
COMMENT ON FUNCTION sift.scope_create(text, regclass, name, regclass, name, name[], boolean, name, name, text, numeric) IS 'Creates a copy of a table keyed by a scope key reached through (member, scope key) pairs, with a sift_col index by the key and triggers that keep it exact; returns the rows. Needs a primary key and a btree index on the foreign key. Refuses above max_copies copies per row (default 1.8). Takes the columns and options of the table''s sift_col index. VACUUM it afterwards. See INDEX-RECIPES';

