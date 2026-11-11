-- The relation of pairs of a scope may hold a pair in several rows (a multiset: the application keeps one row for each source of the pair, not one for each pair), declared with a btree on (dim_a_id, tenant_id) that
-- is not unique: a pair exists while at least one visible row, satisfying the predicate of the btree, holds it. The journal names every dim_a_id one of whose rows was inserted or ended, a list that holds those whose
-- distinct pairs changed, so a duplicate that is inserted or deleted costs a patch and never a wrong answer; every place that lists the pairs lists them once. Table-driven: each state of the pairs (the states
-- accumulate) by each read shape; every cell is md5 equal to the plain statement with sift.enable_agg off, reads the scope units where a top-N does not forbid it, and reads no more rows than the changed dim_a_id
-- have and the buffers hold. The statements an application writes are planned: a semi join (IN, EXISTS) is the scope read, a join and a read grouped by the key count a fact once for each row and keep the join,
-- with their reason. Counts only, no timings.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
-- the narrowed read is taken whatever it costs: the price is the subject of col_scope_patch_price
SET sift.test_scope_patch = on;
-- page costs that keep a SiftAgg plan the cheaper one whenever it is generated: the plan then depends on the shape of the statement
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

-- 10 keys; dim_a_id d is in the key 1 + d % 10 and, when d % 7 = 0, in a second one; one pair in seven of the dim_a_id with d % 7 = 3 is a second row (row_src 2): 1.13 rows for a pair.
-- 301..320 have events and no pair, 20 events in 100 have none
CREATE TABLE tenant_pairs (id serial PRIMARY KEY, row_src int NOT NULL, dim_a_id int NOT NULL, tenant_id int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO tenant_pairs (row_src, dim_a_id, tenant_id) SELECT 1, d, 1 + d % 10 FROM generate_series(1, 300) d;
INSERT INTO tenant_pairs (row_src, dim_a_id, tenant_id) SELECT 1, d, 1 + (d * 3 + 1) % 10 FROM generate_series(1, 300) d WHERE d % 7 = 0;
INSERT INTO tenant_pairs (row_src, dim_a_id, tenant_id) SELECT 2, d, 1 + d % 10 FROM generate_series(1, 300) d WHERE d % 7 = 3;
CREATE INDEX tenant_pairs_pair ON tenant_pairs (dim_a_id, tenant_id);
CREATE INDEX tenant_pairs_journal ON tenant_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
SELECT count(*) AS rows, count(DISTINCT (dim_a_id, tenant_id)) AS pairs, round(count(*)::numeric / count(DISTINCT (dim_a_id, tenant_id)), 2) AS rows_a_pair FROM tenant_pairs;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 100 < 20 THEN NULL ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_pairs(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_pairs;

\ir test/scope_multiset_helpers.sql

-- each state: the statements that make it (they accumulate), and then the reads; a pair that changed is a changed pair of the next reads until the index is rebuilt
CREATE TABLE states (n int, label text, stmt text);
INSERT INTO states VALUES
	(1, 'built', 'SELECT 1'),
	(2, 'a duplicate row of pairs that exist (dim_a_id with events, in one key and in two)',
		'INSERT INTO tenant_pairs (row_src, dim_a_id, tenant_id) SELECT 3, dim_a_id, tenant_id FROM tenant_pairs WHERE dim_a_id IN (20, 30, 35, 42) AND row_src = 1'),
	(3, 'one of two duplicate rows deleted: the pair stays', 'DELETE FROM tenant_pairs WHERE id IN (SELECT min(id) FROM tenant_pairs WHERE dim_a_id IN (3, 31) GROUP BY dim_a_id, tenant_id HAVING count(*) > 1)'),
	(4, 'the last row of a pair deleted: the pair is gone', 'DELETE FROM tenant_pairs WHERE dim_a_id IN (10, 11)'),
	(5, 'a duplicate added, then the first row deleted: the pair stays',
		'INSERT INTO tenant_pairs (row_src, dim_a_id, tenant_id) SELECT 4, dim_a_id, tenant_id FROM tenant_pairs WHERE dim_a_id IN (51, 52); DELETE FROM tenant_pairs WHERE id IN (SELECT min(id) FROM tenant_pairs WHERE dim_a_id IN (51, 52) GROUP BY dim_a_id, tenant_id)'),
	(6, 'the key of one of two duplicate rows moved: the old key stays, the new one is added',
		'UPDATE tenant_pairs SET tenant_id = 1 + tenant_id % 10 WHERE id IN (SELECT max(id) FROM tenant_pairs WHERE dim_a_id IN (17, 24) GROUP BY dim_a_id, tenant_id HAVING count(*) > 1)'),
	(7, 'a column that no btree holds updated on a duplicate row: nothing changes', 'UPDATE tenant_pairs SET row_src = 9 WHERE dim_a_id IN (38, 45)'),
	(8, 'buffered events of dim_a_id with duplicate rows, of ones whose pairs changed and of ones that did not',
		'INSERT INTO events SELECT 6000 + i, CASE i % 4 WHEN 0 THEN 3 WHEN 1 THEN 17 WHEN 2 THEN 30 ELSE 100 + i % 50 END, timestamptz ''2026-02-05'', 4, ''u'' FROM generate_series(1, 120) i'),
	(9, 'promoted (the buffers are in the segments; the changed pairs still leave their dim_a_id out of the units)', 'SELECT sift.col_promote(''events_col'')'),
	(10, 'merged', 'SELECT sift.col_merge(''events_col'', true)'),
	(11, 'the btree of the pairs rebuilt (the index keeps the one it recorded)', 'REINDEX INDEX tenant_pairs_pair'),
	(12, 'rebuilt (nothing changed since the stamp)', 'REINDEX INDEX events_col'),
	(13, 'a duplicate after the rebuild, and a buffered event of it', 'INSERT INTO tenant_pairs (row_src, dim_a_id, tenant_id) VALUES (5, 5, 6), (5, 5, 6), (5, 301, 6); INSERT INTO events VALUES (7000, 5, timestamptz ''2026-02-06'', 1, ''v''), (7001, 301, timestamptz ''2026-02-06'', 1, ''v'')');
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), stmt, 'SELECT * FROM probe(''events'', ''events_col'', ''tenant_pairs'')']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec
SELECT sift.col_verify('events_col', true);

-- the reads of the last state again with the reader listing the pairs of the changed dim_a_id from the relation (1) and by probing the btree (2), then the index rebuilt with the fill reading the relation
-- once (1) and probing the btree (2): the same rows, the units, and the entries of the verifier
SELECT c FROM generate_series(1, 2) m, LATERAL unnest(ARRAY[format('SET sift.test_conn_fill = %s', m), 'SELECT * FROM probe(''events'', ''events_col'', ''tenant_pairs'')', 'REINDEX INDEX events_col',
	'SELECT * FROM probe(''events'', ''events_col'', ''tenant_pairs'')', 'SELECT sift.col_verify(''events_col'', true)']) WITH ORDINALITY u(c, o) ORDER BY m, o \gexec
RESET sift.test_conn_fill;

-- the statements an application writes. A semi join counts a fact once whatever the rows of the pair: the scope read takes it or the price keeps the join, the rows are the join's. A join counts a fact once
-- for each row, and a read grouped by the key a row for each: the scope read counts a pair once, so they keep the join and say why (sift.explain_why, arm scope)
CREATE TABLE forms (n int, label text, q text);
INSERT INTO forms VALUES
	(1, 'IN', 'SELECT sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_pairs WHERE tenant_id = 3)'),
	(2, 'EXISTS', 'SELECT sum(amount) AS total, count(*) AS n FROM events e WHERE EXISTS (SELECT 1 FROM tenant_pairs s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = 3)'),
	(3, 'IN, two keys', 'SELECT sum(amount) AS total, count(*) AS n FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_pairs WHERE tenant_id IN (3, 5))'),
	(4, 'join', 'SELECT sum(amount) AS total, count(*) AS n FROM events e JOIN tenant_pairs s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3'),
	(5, 'join, the key first', 'SELECT sum(amount) AS total, count(*) AS n FROM tenant_pairs s JOIN events e ON e.dim_a_id = s.dim_a_id WHERE 3 = s.tenant_id'),
	(6, 'join, two keys', 'SELECT sum(amount) AS total, count(*) AS n FROM events e JOIN tenant_pairs s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id IN (3, 5)'),
	(7, 'join grouped by the key', 'SELECT s.tenant_id, sum(amount) AS total, count(*) AS n FROM events e JOIN tenant_pairs s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id IN (3, 5, 9) GROUP BY s.tenant_id'),
	(8, 'join grouped by a column of the facts', 'SELECT amount, count(*) AS n FROM events e JOIN tenant_pairs s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3 GROUP BY amount');

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text; plan text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF plan ~ 'Scope Index: ' THEN
		RETURN 'scope read';
	END IF;
	SELECT regexp_replace(reason, '^.*the join runs as written \((.*)\)$', '\1') INTO r FROM sift.explain_why(q) WHERE arm = 'scope';
	RETURN CASE WHEN r IS NOT NULL THEN 'join, ' || r ELSE 'join' END;
END $$;
CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION same_rows(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; c text;
BEGIN
	a := rows_md5(q);
	PERFORM set_config('sift.agg_scope', 'off', false);
	b := rows_md5(q);
	PERFORM set_config('sift.enable_agg', 'off', false);
	c := rows_md5(q);
	PERFORM set_config('sift.agg_scope', 'on', false);
	PERFORM set_config('sift.enable_agg', 'on', false);
	RETURN a = b AND b = c;
END $$;
-- a pair in several rows makes the join count a fact once for each: the statements that count a pair once and the join differ
SELECT (SELECT count(*) FROM events e JOIN tenant_pairs s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 3) >
	(SELECT count(*) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_pairs WHERE tenant_id = 3)) AS join_counts_more_than_in;
SELECT n, label, plan_of(q) AS plan, same_rows(q) AS same_rows FROM forms ORDER BY n;

-- the testing markers by the same rule: a read for each pair is the join's meaning and is refused for the pairs of a multiset, a semi join is read
EXPLAIN (COSTS OFF) SELECT count(*) FROM events f WHERE sift.in_scope_pairs('events_col'::regclass, 'tenant_id', f.dim_a_id, 3);
EXPLAIN (COSTS OFF) SELECT count(*) FROM events f WHERE sift.in_scope_pairs('events_col'::regclass, 'tenant_id', f.dim_a_id, '{3,5}'::int8[]);
EXPLAIN (COSTS OFF) SELECT count(*) FROM events f WHERE sift.in_scope('events_col'::regclass, 'tenant_id', f.dim_a_id, 3);

-- the same planned statements after the pairs changed again and a rebuild: IN is the scope read and the join is the join
INSERT INTO tenant_pairs (row_src, dim_a_id, tenant_id) SELECT 6, dim_a_id, tenant_id FROM tenant_pairs WHERE tenant_id = 3 AND row_src = 1;
SELECT n, label, plan_of(q) AS plan, same_rows(q) AS same_rows FROM forms ORDER BY n;
REINDEX INDEX events_col;
SELECT n, label, plan_of(q) AS plan, same_rows(q) AS same_rows FROM forms ORDER BY n;
SELECT sift.col_verify('events_col', true);

-- the scope copy (sift.scope_create) keeps each fact once for each pair, and builds its copies from a join with the relation: a pair in several rows would give a fact two copies in one key, so the copy
-- still needs the unique btree and refuses the relation of a multiset
SELECT sift.scope_create('events_copy', 'events', 'dim_a_id', 'tenant_pairs', 'tenant_id');

-- the declaration: a unique btree before a non-unique one, then the partial rule; USING names the other one; the NOTICE says the pairs are a multiset
CREATE TABLE decl_facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz);
INSERT INTO decl_facts SELECT i, 1 + i % 60, timestamptz '2026-01-01' + i * interval '1 hour' FROM generate_series(1, 300) i;
CREATE TABLE decl_pairs (id serial PRIMARY KEY, dim_a_id int NOT NULL, tenant_id int NOT NULL);
INSERT INTO decl_pairs (dim_a_id, tenant_id) SELECT 1 + i % 50, 1 + i % 7 FROM generate_series(1, 200) i;
CREATE INDEX decl_pairs_a ON decl_pairs (dim_a_id, tenant_id);
CREATE INDEX decl_pairs_b ON decl_pairs (tenant_id, dim_a_id);
CREATE INDEX decl_pairs_journal ON decl_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
SET client_min_messages = notice;
CREATE INDEX decl_facts_col ON decl_facts USING sift_col (id, dim_a_id, created_at) WITH (scope = 'tenant_id: dim_a_id -> decl_pairs(dim_a_id, tenant_id)');
SET client_min_messages = warning;
SELECT c.relname AS btree, i.indisunique AS is_unique FROM pg_depend d JOIN pg_class c ON c.oid = d.refobjid JOIN pg_index i ON i.indexrelid = c.oid
	WHERE d.objid = 'decl_facts_col'::regclass AND d.refclassid = 'pg_class'::regclass AND c.relam = (SELECT oid FROM pg_am WHERE amname = 'btree') ORDER BY 1;
DROP INDEX decl_facts_col;
-- a unique btree on the two columns wins over a non-unique one, whatever the oids
CREATE TABLE decl_unique (dim_a_id int NOT NULL, tenant_id int NOT NULL);
INSERT INTO decl_unique SELECT DISTINCT 1 + i % 50, 1 + i % 7 FROM generate_series(1, 200) i;
CREATE INDEX decl_unique_plain ON decl_unique (dim_a_id, tenant_id);
CREATE UNIQUE INDEX decl_unique_pair ON decl_unique (tenant_id, dim_a_id);
CREATE INDEX decl_unique_journal ON decl_unique USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
SET client_min_messages = notice;
CREATE INDEX decl_facts_col ON decl_facts USING sift_col (id, dim_a_id, created_at) WITH (scope = 'tenant_id: dim_a_id -> decl_unique(dim_a_id, tenant_id)');
SET client_min_messages = warning;
DROP INDEX decl_facts_col;
-- two partial btrees that are not unique: which rows are pairs would be a guess, USING names one
CREATE TABLE decl_soft (id serial PRIMARY KEY, dim_a_id int NOT NULL, tenant_id int NOT NULL, deleted_at timestamptz);
CREATE INDEX decl_soft_active ON decl_soft (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE INDEX decl_soft_other ON decl_soft (tenant_id, dim_a_id) WHERE id > 0;
CREATE INDEX decl_soft_journal ON decl_soft USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX decl_facts_col ON decl_facts USING sift_col (id, dim_a_id, created_at) WITH (scope = 'tenant_id: dim_a_id -> decl_soft(dim_a_id, tenant_id)');
SET client_min_messages = notice;
CREATE INDEX decl_facts_col ON decl_facts USING sift_col (id, dim_a_id, created_at) WITH (scope = 'tenant_id: dim_a_id -> decl_soft(dim_a_id, tenant_id) using decl_soft_other');
SET client_min_messages = warning;
-- a btree of other columns, a deferrable unique one and an included column are no pairs
CREATE TABLE decl_bad (dim_a_id int NOT NULL, tenant_id int NOT NULL, other int);
CREATE INDEX decl_bad_three ON decl_bad (dim_a_id, tenant_id, other);
CREATE INDEX decl_bad_include ON decl_bad (dim_a_id, tenant_id) INCLUDE (other);
CREATE INDEX decl_bad_journal ON decl_bad USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX decl_bad_facts ON decl_facts USING sift_col (id, dim_a_id, created_at) WITH (scope = 'tenant_id: dim_a_id -> decl_bad(dim_a_id, tenant_id)');

DROP FUNCTION probe(text, text, text);
DROP TABLE shapes, states, forms;
DROP FUNCTION plan_of(text), rows_md5(text), same_rows(text), ms_run(text, text, text, text), changed_rows(text, text);
DROP TABLE decl_facts, decl_pairs, decl_unique, decl_soft, decl_bad CASCADE;
DROP TABLE events;
DROP TABLE tenant_pairs;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
