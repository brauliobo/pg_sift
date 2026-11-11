-- GROUP BY the key of the scope relation ("every tenant at once"): the join `f JOIN pairs s ON s.member = f.fk GROUP BY s.key` as an application writes it is read from the scope units of every key,
-- grouped by the derived key of each pair, and the node runs the join as written where the reader refuses the units or the join costs less (agg/plan_scope.c, agg/scope.c). The key is the column
-- of the scope relation, of its type (int2, int4, int8): the derived leaf is an int8 that holds the stored key, so a key column of the narrower types is read exactly. Table-driven: every form
-- (extra group keys, HAVING, ORDER BY the key, a top-N over the groups, a list of keys, count(DISTINCT), no group) by each key type, in each state of the pairs and the events (clean, buffered
-- rows, pairs inserted, ended and moved, promoted); each is md5 equal to the join under sift.agg_scope = off and to the normal plan, says how it was planned (Scope Keys, Scope Group Key) and what the
-- group column's type is. The refusals say their reason (sift.explain_why, slug scope).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.scope_max_multiplicity = 8;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
-- the units whatever they cost; the node's choice has its own section below
SET sift.test_scope_patch = on;

-- 40 tenants of each key type, the extremes of the type among them; member m is in 1 to 3 of them, the members 191..200 in none and 201..210 have facts and no pair
CREATE TABLE tenants (n int PRIMARY KEY, k2 int2, k4 int4, k8 int8);
INSERT INTO tenants SELECT n, (n - 20) * 1000 + n % 7, (n - 20) * 100000000 + n, (n - 20) * 200000000000000000::int8 / 5 + n FROM generate_series(1, 40) n;
UPDATE tenants SET k2 = -32768, k4 = -2147483648, k8 = -4611686018427387904 WHERE n = 1;
UPDATE tenants SET k2 = 32767, k4 = 2147483647, k8 = 4611686018427387904 WHERE n = 40;
CREATE TABLE pairs2 (dim_a_id int NOT NULL, tenant int2 NOT NULL, PRIMARY KEY (dim_a_id, tenant));
CREATE TABLE pairs4 (dim_a_id int NOT NULL, tenant int4 NOT NULL, PRIMARY KEY (dim_a_id, tenant));
CREATE TABLE pairs8 (dim_a_id int NOT NULL, tenant int8 NOT NULL, PRIMARY KEY (dim_a_id, tenant));
INSERT INTO pairs2 SELECT m, k2 FROM generate_series(1, 190) m JOIN tenants ON n = 1 + (m * 7) % 40 OR (m % 3 = 0 AND n = 1 + (m * 13 + 5) % 40) OR (m % 5 = 0 AND n = 1 + (m * 3) % 40);
INSERT INTO pairs4 SELECT m, k4 FROM generate_series(1, 190) m JOIN tenants ON n = 1 + (m * 7) % 40 OR (m % 3 = 0 AND n = 1 + (m * 13 + 5) % 40) OR (m % 5 = 0 AND n = 1 + (m * 3) % 40);
INSERT INTO pairs8 SELECT m, k8 FROM generate_series(1, 190) m JOIN tenants ON n = 1 + (m * 7) % 40 OR (m % 3 = 0 AND n = 1 + (m * 13 + 5) % 40) OR (m % 5 = 0 AND n = 1 + (m * 3) % 40);
CREATE TABLE pairsc (dim_a_id int NOT NULL, tenant int4 NOT NULL, PRIMARY KEY (dim_a_id, tenant));
INSERT INTO pairsc SELECT dim_a_id, tenant FROM pairs4;
CREATE INDEX pairs2_journal ON pairs2 USING sift_col (dim_a_id, tenant) WITH (track_changes = 'ctid');
CREATE INDEX pairs4_journal ON pairs4 USING sift_col (dim_a_id, tenant) WITH (track_changes = 'ctid');
CREATE INDEX pairs8_journal ON pairs8 USING sift_col (dim_a_id, tenant) WITH (track_changes = 'ctid');
CREATE INDEX pairsc_journal ON pairsc USING sift_col (dim_a_id, tenant) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, at timestamptz, kind text, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 210 END, timestamptz '2026-01-01' + (i % 300) * interval '1 day', 'k' || i % 3, i % 10, repeat('x', 60)
FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, at, kind, v)
	WITH (scope = 'k2: dim_a_id -> pairs2(dim_a_id, tenant); k4: dim_a_id -> pairs4(dim_a_id, tenant); k8: dim_a_id -> pairs8(dim_a_id, tenant); kc: dim_a_id -> pairsc(dim_a_id, tenant) cols(v)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events, pairs2, pairs4, pairs8, pairsc;

-- the rows of a statement as the md5 of their text (an EXECUTE of a prepared statement through a table)
CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	IF q ~ '^EXECUTE' THEN
		EXECUTE 'CREATE TEMP TABLE gk_rows AS ' || q;
		q := 'SELECT * FROM gk_rows';
	END IF;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO r;
	DROP TABLE IF EXISTS gk_rows;
	RETURN r;
END $$;

-- what a statement did: the plan's scope lines (Scope Keys, Scope Group Key, the fallback the node ran the join for), whether its rows are those of the join (sift.agg_scope = off) and of the
-- normal plan (sift.enable_agg = off), and the type of its first column
CREATE FUNCTION gk(q text, OUT planned text, OUT same boolean, OUT col_type text) LANGUAGE plpgsql AS $$
DECLARE
	l text;
	a text;
	b text;
	c text;
BEGIN
	planned := '';
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_scope = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Scope (Keys|Group Key|Fallback)' THEN
			planned := planned || regexp_replace(regexp_replace(l, '^ *Scope ', ''), 'Keys: (\$|ARRAY).*', 'Keys: parameter') || '; ';
		END IF;
	END LOOP;
	planned := coalesce(nullif(rtrim(planned, '; '), ''), 'no scope read');
	a := rows_md5(q);
	IF q !~ '^EXECUTE' THEN
		EXECUTE 'SELECT pg_typeof(x.k)::text FROM (SELECT * FROM (' || q || ') y(k)) x LIMIT 1' INTO col_type;
	END IF;
	SET LOCAL sift.agg_scope = off;
	b := rows_md5(q);
	SET LOCAL sift.enable_agg = off;
	c := rows_md5(q);
	RESET sift.enable_agg;
	RESET sift.agg_scope;
	same := a = b AND b = c;
END $$;

-- the forms: %1$s the pairs relation, %2$s the key type's tenant list for the IN forms, %3$s one key
CREATE TABLE forms (n int, label text, stmt text);
INSERT INTO forms VALUES
	(1, 'count and sum by the key', 'SELECT s.tenant, count(*), sum(v) FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant'),
	(2, 'the key and kind, min, max, avg', 'SELECT s.tenant, kind, min(id), max(v), avg(v) FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant, kind'),
	(3, 'HAVING', 'SELECT s.tenant, count(*) FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant HAVING count(*) > 300'),
	(4, 'ORDER BY the key', 'SELECT s.tenant, count(*) FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant ORDER BY s.tenant DESC'),
	(5, 'top 3 groups by count', 'SELECT s.tenant, count(*) n FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant ORDER BY n DESC, s.tenant LIMIT 3'),
	(6, 'the key and the month', 'SELECT s.tenant, extract(month FROM at) m, sum(v) FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id WHERE v > 3 AND at >= timestamptz ''2026-03-01'' GROUP BY s.tenant, m'),
	(7, 'count(DISTINCT) by the key', 'SELECT s.tenant, count(DISTINCT f.dim_a_id), count(DISTINCT v) FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant'),
	(8, 'SELECT DISTINCT the key', 'SELECT DISTINCT s.tenant FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id'),
	(9, 'a list of keys', 'SELECT s.tenant, count(*) FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id WHERE s.tenant IN (%2$s) GROUP BY s.tenant'),
	(10, 'one key', 'SELECT s.tenant, count(*) FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id WHERE s.tenant = %3$s GROUP BY s.tenant'),
	(11, 'a row for each pair, no group', 'SELECT count(*), sum(v), min(id) FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id'),
	(12, 'a group by another column of the facts', 'SELECT kind, count(*) FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id GROUP BY kind'),
	(13, 'the latest 5 rows of every key (a top-N, a row for each pair)', 'SELECT f.id, f.at FROM events f JOIN %1$s s ON s.dim_a_id = f.dim_a_id ORDER BY f.at DESC, f.id DESC LIMIT 5');

-- the values of the tenants ns of the key type k ('2', '4', '8'; 'c' is int4 too) as a list
CREATE FUNCTION keys_of(k text, ns int[]) RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(CASE k WHEN '2' THEN k2::text WHEN '8' THEN k8::text ELSE k4::text END, ', ' ORDER BY n) FROM tenants WHERE n = ANY (ns)
$$;

-- what a form over the key type k says of its plan and its rows
CREATE FUNCTION probe(k text) RETURNS TABLE (n int, label text, planned text, same boolean, col_type text) LANGUAGE sql AS $$
	SELECT f.n, f.label, r.planned, r.same, r.col_type FROM forms f, LATERAL gk(format(f.stmt, 'pairs' || k, keys_of(k, ARRAY[1, 5, 20, 40]), keys_of(k, ARRAY[5]))) r ORDER BY f.n
$$;

\echo int2
SELECT * FROM probe('2');
\echo int4
SELECT * FROM probe('4');
\echo int8
SELECT * FROM probe('8');
-- a scope that keeps only the column v (and the member key and the time column): a statement that reads another one runs the join, which says why
\echo cols(v)
SELECT * FROM probe('c');

-- the states the pairs and the events go through: the rows stay the join's, and the node runs the join where the reader refuses (a top-N cannot take the delta of a changed pair)
CREATE TABLE states (n int, label text, stmt text);
INSERT INTO states VALUES
	(1, 'buffered events (dim_b with pairs, without, and none)',
		'INSERT INTO events SELECT 6000 + i, CASE WHEN i % 40 = 0 THEN NULL ELSE 1 + (i * 37) % 210 END, timestamptz ''2026-02-01'', ''k1'', i % 10, ''y'' FROM generate_series(1, 120) i'),
	(2, 'pairs inserted, ended and moved (every type), the events as they were',
		'INSERT INTO pairs2 SELECT m, k2 FROM generate_series(1, 30) m, tenants WHERE n = 3 + m % 5 ON CONFLICT DO NOTHING;
		 INSERT INTO pairs4 SELECT m, k4 FROM generate_series(1, 30) m, tenants WHERE n = 3 + m % 5 ON CONFLICT DO NOTHING;
		 INSERT INTO pairs8 SELECT m, k8 FROM generate_series(1, 30) m, tenants WHERE n = 3 + m % 5 ON CONFLICT DO NOTHING;
		 DELETE FROM pairsc WHERE dim_a_id % 11 = 0; DELETE FROM pairs2 WHERE dim_a_id % 11 = 0; DELETE FROM pairs4 WHERE dim_a_id % 11 = 0; DELETE FROM pairs8 WHERE dim_a_id % 11 = 0;
		 WITH d AS (DELETE FROM pairs2 WHERE dim_a_id % 13 = 0 RETURNING dim_a_id, tenant) INSERT INTO pairs2 SELECT dim_a_id, (SELECT k2 FROM tenants WHERE n = 1 + (dim_a_id % 40)) FROM d ON CONFLICT DO NOTHING;
		 WITH d AS (DELETE FROM pairs4 WHERE dim_a_id % 13 = 0 RETURNING dim_a_id, tenant) INSERT INTO pairs4 SELECT dim_a_id, (SELECT k4 FROM tenants WHERE n = 1 + (dim_a_id % 40)) FROM d ON CONFLICT DO NOTHING;
		 WITH d AS (DELETE FROM pairs8 WHERE dim_a_id % 13 = 0 RETURNING dim_a_id, tenant) INSERT INTO pairs8 SELECT dim_a_id, (SELECT k8 FROM tenants WHERE n = 1 + (dim_a_id % 40)) FROM d ON CONFLICT DO NOTHING'),
	(3, 'buffered events of dim_b whose pairs changed',
		'INSERT INTO events SELECT 6200 + i, 1 + (i * 11) % 60, timestamptz ''2026-02-02'', ''k2'', 3, ''z'' FROM generate_series(1, 80) i'),
	(4, 'promoted', 'SELECT sift.col_promote(''events_col'')'),
	(5, 'rebuilt (nothing changed since the stamp)', 'REINDEX INDEX events_col');
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), stmt, 'SELECT k, count(*) FILTER (WHERE same) AS same, count(*) FILTER (WHERE planned ~ ''Group Key'') AS read_by_key, count(*) FILTER (WHERE planned ~ ''Fallback'') AS join_run FROM unnest(ARRAY[''2'', ''4'', ''8'', ''c'']) k, LATERAL probe(k) GROUP BY k ORDER BY k']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec
SELECT sift.col_verify('events_col', true);

-- the other statements over the pairs: each refusal keeps the join and says why (sift.explain_why, slug scope); the keys of a table of keys are read once when the scan starts
CREATE TABLE others (n int, label text, stmt text);
INSERT INTO others VALUES
	(1, 'the key in an expression', 'SELECT s.tenant / 2, count(*) FROM events f JOIN pairs4 s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant / 2'),
	(2, 'the key as an aggregate argument', 'SELECT max(s.tenant), count(*) FROM events f JOIN pairs4 s ON s.dim_a_id = f.dim_a_id'),
	(3, 'the key in a HAVING aggregate', 'SELECT s.tenant, count(*) FROM events f JOIN pairs4 s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant HAVING max(s.tenant) > 3'),
	(4, 'ROLLUP the key', 'SELECT s.tenant, count(*) FROM events f JOIN pairs4 s ON s.dim_a_id = f.dim_a_id GROUP BY ROLLUP (s.tenant)'),
	(5, 'a function of the key', 'SELECT s.tenant, pg_typeof(s.tenant), count(*) FROM events f JOIN pairs4 s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant'),
	(6, 'every key of an IN (a row counts once)', 'SELECT kind, count(*) FROM events f WHERE f.dim_a_id IN (SELECT dim_a_id FROM pairs4) GROUP BY kind'),
	(7, 'a range of keys', 'SELECT s.tenant, count(*) FROM events f JOIN pairs4 s ON s.dim_a_id = f.dim_a_id WHERE s.tenant > 0 GROUP BY s.tenant'),
	(8, 'a table of keys, grouped by the key (read)', 'SELECT s.tenant, count(*) FROM events f JOIN pairs4 s ON s.dim_a_id = f.dim_a_id WHERE s.tenant IN (SELECT k4 FROM tenants WHERE n < 4) GROUP BY s.tenant'),
	(9, 'the key and a table joined on it', 'SELECT s.tenant, t.n, count(*) FROM events f JOIN pairs4 s ON s.dim_a_id = f.dim_a_id JOIN tenants t ON t.k4 = s.tenant GROUP BY s.tenant, t.n'),
	(10, 'a LEFT JOIN to the pairs', 'SELECT s.tenant, count(*) FROM events f LEFT JOIN pairs4 s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant');
CREATE FUNCTION why(q text) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce((SELECT substring(r::text FROM 'the join runs as written \((.*)\)",') FROM sift.explain_why(q) r WHERE r::text ~ '^\(scope,' LIMIT 1), 'no scope refusal')
$$;
SELECT x.n, x.label, r.planned, r.same, why(x.stmt) FROM others x, LATERAL gk(x.stmt) r ORDER BY x.n;

-- a prepared statement: the generic plan has the keys of each execution (an array parameter), a group by the key too
SET plan_cache_mode = force_generic_plan;
PREPARE by_keys(int4[]) AS SELECT s.tenant, count(*), sum(v) FROM events f JOIN pairs4 s ON s.dim_a_id = f.dim_a_id WHERE s.tenant = ANY ($1) GROUP BY s.tenant;
SELECT planned, same FROM gk('EXECUTE by_keys(ARRAY[-2147483648, 0, 2147483647])');
SELECT planned, same FROM gk('EXECUTE by_keys(ARRAY[]::int4[])');
DEALLOCATE by_keys;
RESET plan_cache_mode;

-- the node's choice: with the price in charge the node runs the join where it costs less than the units for the keys it finds, and says so; the rows are the join's
SET sift.test_scope_patch = off;
SELECT f.n, r.planned, r.same FROM forms f, LATERAL gk(format(f.stmt, 'pairs4', '', '')) r WHERE f.n IN (1, 11) ORDER BY f.n;
-- a group by the key through the join of the units: the same statement with the choice off reads the units
SET sift.test_scope_patch = on;
SELECT f.n, r.planned, r.same FROM forms f, LATERAL gk(format(f.stmt, 'pairs4', '', '')) r WHERE f.n IN (1, 11) ORDER BY f.n;

-- soft deleted pairs: the pairs are the rows of a partial unique btree, so a join that does not state its predicate counts pairs the scope does not have: it keeps the join and says so; with the predicate
-- (in the WHERE or in a view) it is the scope read
CREATE TABLE sd_pairs (dim_a_id int NOT NULL, tenant int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO sd_pairs SELECT m, 1 + (m + j) % 6, CASE WHEN (m + j) % 4 = 0 THEN now() END FROM generate_series(1, 150) m, generate_series(0, m % 2) j ON CONFLICT DO NOTHING;
CREATE UNIQUE INDEX sd_pairs_active ON sd_pairs (dim_a_id, tenant) WHERE deleted_at IS NULL;
CREATE INDEX sd_pairs_journal ON sd_pairs USING sift_col (dim_a_id, tenant) WITH (track_changes = 'ctid');
CREATE TABLE sd_events (id int PRIMARY KEY, dim_a_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO sd_events SELECT i, 1 + i % 150, i % 50 FROM generate_series(1, 6000) i;
CREATE INDEX sd_events_col ON sd_events USING sift_col (id, dim_a_id, v) WITH (scope = 'tenant: dim_a_id -> sd_pairs(dim_a_id, tenant)');
REINDEX INDEX sd_events_col;
CREATE VIEW sd_active AS SELECT dim_a_id, tenant FROM sd_pairs WHERE deleted_at IS NULL;
VACUUM (FREEZE, ANALYZE) sd_events, sd_pairs;
CREATE TABLE sd_forms (n int, label text, stmt text);
INSERT INTO sd_forms VALUES
	(1, 'without the predicate', 'SELECT s.tenant, count(*), sum(v) FROM sd_events f JOIN sd_pairs s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant'),
	(2, 'with the predicate', 'SELECT s.tenant, count(*), sum(v) FROM sd_events f JOIN sd_pairs s ON s.dim_a_id = f.dim_a_id WHERE s.deleted_at IS NULL GROUP BY s.tenant'),
	(3, 'through a view of the predicate', 'SELECT s.tenant, count(*), sum(v) FROM sd_events f JOIN sd_active s ON s.dim_a_id = f.dim_a_id GROUP BY s.tenant'),
	(4, 'one key without the predicate', 'SELECT count(*), sum(v) FROM sd_events f JOIN sd_pairs s ON s.dim_a_id = f.dim_a_id WHERE s.tenant = 3');
SELECT x.n, x.label, r.planned, r.same, why(x.stmt) FROM sd_forms x, LATERAL gk(x.stmt) r ORDER BY x.n;

DROP TABLE forms, states, others, sd_forms;
DROP VIEW sd_active;
DROP TABLE sd_events, sd_pairs;
DROP FUNCTION gk(text), probe(text), why(text), keys_of(text, int[]), rows_md5(text);
DROP TABLE events, pairs2, pairs4, pairs8, pairsc, tenants;
DROP EXTENSION pg_sift;
