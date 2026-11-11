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
-- The INSERT for new pairs reads the facts of each pair's member by the table's index on the foreign key, one probe for each pair (a lateral sub-select with OFFSET 0 only runs as a nested
-- loop: a hash join read the whole table from about 10,000 pairs whose members have no facts, 810 M instructions against 106 M), in the order of the primary key, as a scan gives them.
CREATE FUNCTION sift.scope_pairs_insert(c sift.scope_copies, src text) RETURNS text LANGUAGE sql STABLE AS $$
	SELECT format('INSERT INTO %1$s (%2$I, %3$s) SELECT s.%2$I, %3$s FROM (SELECT n.%2$I, %4$s FROM %5$s n CROSS JOIN LATERAL (SELECT %4$s FROM %6$s f WHERE f.%7$I = n.%8$I OFFSET 0) f ORDER BY %9$s) s LEFT JOIN LATERAL (SELECT true AS present FROM %1$s x WHERE %10$s AND x.%2$I = s.%2$I LIMIT 1) h ON true WHERE h.present IS NULL',
		c.copy, c.scope_key, sift.scope_list(c.columns, ''), sift.scope_list(c.columns, 'f'), src, c.source, c.fk, c.member,
		sift.scope_list(c.key_columns, 'f'), sift.scope_cmp(c.key_columns, 'x', 's', '=', ' AND ')) $$;
COMMENT ON FUNCTION sift.scope_fact_insert(sift.scope_copies, text) IS 'Internal: the INSERT of the copy''s rows that the trigger of the table of a scope copy runs for the rows of its transition table (sift.scope_changed)';
COMMENT ON FUNCTION sift.scope_pairs_insert(sift.scope_copies, text) IS 'Internal: the INSERT of the copy''s rows that the trigger of the scope relation of a scope copy runs for the rows of its transition table (sift.scope_changed)';

-- The DELETE of the copy's rows that a statement trigger of op runs: the rows of the old transition table whose copies go (sift.scope_changed)
CREATE FUNCTION sift.scope_fact_delete(c sift.scope_copies, op text) RETURNS text LANGUAGE sql STABLE AS $$
	SELECT format('DELETE FROM %s d USING %s o WHERE %s', c.copy, sift.scope_changed(c.key_columns, c.columns, op, 'old'), sift.scope_cmp(c.key_columns, 'd', 'o', '=', ' AND ')) $$;
-- The copy's rows of the pairs that go. A pair whose member has no fact has no copy row, and a statement over all the pairs read the whole copy for as few as 1,000 of them (600 M instructions):
-- scope_pairs_gone lists the pairs whose member has a fact (a probe of the table's index on the foreign key, 8,000 instructions each), in arrays whose length the planner knows, and scope_pairs_delete
-- deletes their copy rows, by a semi join so that PostgreSQL hashes the pairs and not the copy: from about 400 pairs (25 facts for each member) it reads the copy once, below that it probes it.
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
	ms       text; ks text;
BEGIN
	IF current_setting('transaction_isolation') <> 'read committed' THEN PERFORM sift.scope_refuse_isolation(TG_ARGV[0]); END IF;
	SELECT * INTO STRICT c FROM sift.scope_copies WHERE copy = TG_ARGV[0];
	PERFORM pg_advisory_xact_lock(hashtext('sift.scope_copy'), hashtext(c.copy));
	IF TG_OP IN ('UPDATE', 'DELETE') THEN
		EXECUTE sift.scope_pairs_gone(c, TG_OP) INTO ms, ks;
		IF ms IS NOT NULL THEN EXECUTE sift.scope_pairs_delete(c) USING ms, ks; END IF;
	END IF;
	IF TG_OP IN ('INSERT', 'UPDATE') THEN
		src := sift.scope_changed(ARRAY[c.member, c.scope_key], ARRAY[c.member, c.scope_key], TG_OP, 'new');
		IF c.partitioned THEN EXECUTE format('SELECT sift.scope_partition(%L, k) FROM (SELECT DISTINCT n.%I AS k FROM %s n) q', c.copy, c.scope_key, src); END IF;
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

