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
