-- The one place that turns the rows of a relation of pairs into pairs (sift_scope_pairs_distinct) under every way the pairs are known: the fill of the scope units reads the relation once or probes its btree
-- for the dim_a_id of each batch, the reader lists the pairs of the keys by probing the btree by key, from the leaves of the journal index, or from the relation. A pair that is thousands of rows (runs
-- of one value that span many leaf pages of the btree, with deduplication off so that each row is an entry), a btree in either order, a partial one: the entries are the join over the DISTINCT pairs,
-- each pair once. sift.test_conn_fill picks the way the fill and the reader know the pairs: 1 the relation read once, 2 the btree probed.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
SET sift.test_scope_patch = on;
SET sift.scope_max_multiplicity = 100;

-- dim_a_id d (1..2000) is in the key 1 + d % 40, in a second row when d % 3 = 0 and in three when d % 9 = 0; 7 has runs of 12,000 rows of two pairs, 8 one of 9,000 rows
CREATE TABLE pairs_ab (id serial PRIMARY KEY, dim_a_id int NOT NULL, tenant_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO pairs_ab (dim_a_id, tenant_id) SELECT d, 1 + d % 40 FROM generate_series(1, 2000) d WHERE d NOT IN (7, 8);
INSERT INTO pairs_ab (dim_a_id, tenant_id) SELECT d, 1 + d % 40 FROM generate_series(1, 2000) d WHERE d % 3 = 0;
INSERT INTO pairs_ab (dim_a_id, tenant_id) SELECT d, 1 + d % 40 FROM generate_series(1, 2000) d, generate_series(1, 2) WHERE d % 9 = 0 AND d <> 7;
INSERT INTO pairs_ab (dim_a_id, tenant_id) SELECT 7, 3 FROM generate_series(1, 12000);
INSERT INTO pairs_ab (dim_a_id, tenant_id) SELECT 7, 4 FROM generate_series(1, 12000);
INSERT INTO pairs_ab (dim_a_id, tenant_id) SELECT 8, 3 FROM generate_series(1, 9000);
-- rows soft deleted: the pair of 11 and of 14 survive in their other row
UPDATE pairs_ab SET deleted_at = now() WHERE dim_a_id IN (11, 14, 15) AND id IN (SELECT min(id) FROM pairs_ab WHERE dim_a_id IN (11, 14, 15) GROUP BY dim_a_id);
CREATE TABLE pairs_ba (LIKE pairs_ab INCLUDING DEFAULTS);
INSERT INTO pairs_ba SELECT * FROM pairs_ab;
CREATE TABLE pairs_soft (LIKE pairs_ab INCLUDING DEFAULTS);
INSERT INTO pairs_soft SELECT * FROM pairs_ab;
CREATE INDEX pairs_ab_pair ON pairs_ab (dim_a_id, tenant_id) WITH (deduplicate_items = off);
CREATE INDEX pairs_ba_pair ON pairs_ba (tenant_id, dim_a_id) WITH (deduplicate_items = off);
CREATE INDEX pairs_soft_active ON pairs_soft (dim_a_id, tenant_id) WITH (deduplicate_items = off) WHERE deleted_at IS NULL;
CREATE INDEX pairs_ab_journal ON pairs_ab USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX pairs_ba_journal ON pairs_ba USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX pairs_soft_journal ON pairs_soft USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
VACUUM (ANALYZE) pairs_ab, pairs_ba, pairs_soft;
-- a run of one value spans many leaf pages of its btree
SELECT pg_relation_size('pairs_ab_pair') / 8192 > 100 AS run_spans_many_pages, (SELECT count(*) FROM pairs_ab WHERE dim_a_id = 7 AND tenant_id = 3) AS rows_of_the_run,
	(SELECT count(*) FROM (SELECT DISTINCT dim_a_id, tenant_id FROM pairs_ab) p) AS pairs, (SELECT count(*) FROM pairs_ab) AS rows;

-- the entries the fill gives for the dim_a_id of the facts (NULLs, unknown ones and the heavy ones), by the way the pairs are known (0: the price chooses), against the join over the distinct pairs
CREATE TABLE facts AS
	SELECT i AS ordinal, CASE WHEN i % 11 = 0 THEN NULL WHEN i % 13 = 0 THEN 100000 + i WHEN i % 17 = 0 THEN 7 + i % 2 ELSE 1 + (i * 7919) % 2000 END::int8 AS fk FROM generate_series(0, 4999) i;
CREATE FUNCTION expand_md5(tbl regclass, btree regclass, member_col text, key_col text, mode int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	EXECUTE format('SET sift.test_conn_fill = %s', mode);
	SELECT md5(string_agg(e.ordinal || ':' || e.key, ',' ORDER BY e.ordinal, e.key)) INTO r
		FROM sift._test_scope_expand(tbl, btree, member_col, key_col, (SELECT array_agg(fk ORDER BY ordinal) FROM facts)) e;
	RETURN r;
END $$;
CREATE FUNCTION joined_md5(pairs text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	EXECUTE format('SELECT md5(string_agg(f.ordinal || '':'' || p.tenant_id, '','' ORDER BY f.ordinal, p.tenant_id)) FROM facts f JOIN (SELECT DISTINCT dim_a_id, tenant_id FROM %s) p ON p.dim_a_id = f.fk', pairs) INTO r;
	RETURN r;
END $$;
SELECT 'btree (dim_a_id, tenant_id)' AS btree, expand_md5('pairs_ab', 'pairs_ab_pair', 'dim_a_id', 'tenant_id', m) = joined_md5('pairs_ab') AS is_the_join_over_distinct_pairs, m AS way
	FROM generate_series(0, 2) m ORDER BY m;
SELECT 'btree (tenant_id, dim_a_id)' AS btree, expand_md5('pairs_ba', 'pairs_ba_pair', 'dim_a_id', 'tenant_id', m) = joined_md5('pairs_ba') AS is_the_join_over_distinct_pairs, m AS way
	FROM generate_series(0, 2) m ORDER BY m;
SELECT 'partial btree' AS btree, expand_md5('pairs_soft', 'pairs_soft_active', 'dim_a_id', 'tenant_id', m) = joined_md5('(SELECT * FROM pairs_soft WHERE deleted_at IS NULL)') AS is_the_join_over_distinct_pairs, m AS way
	FROM generate_series(0, 2) m ORDER BY m;
-- no entry twice: a row of the facts has each key once
CREATE FUNCTION entries_of(tbl regclass, btree regclass, mode int, OUT entries bigint, OUT distinct_entries bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('SET sift.test_conn_fill = %s', mode);
	SELECT count(*), count(DISTINCT (e.ordinal, e.key)) INTO entries, distinct_entries
		FROM sift._test_scope_expand(tbl, btree, 'dim_a_id', 'tenant_id', (SELECT array_agg(fk ORDER BY ordinal) FROM facts)) e;
END $$;
SELECT m AS way, e.* FROM generate_series(0, 2) m, LATERAL entries_of('pairs_ab', 'pairs_ab_pair', m) e ORDER BY m;
SET sift.test_conn_fill = 0;
-- a small memory leaves the relation to the probe
SET maintenance_work_mem = '1MB';
SELECT expand_md5('pairs_ab', 'pairs_ab_pair', 'dim_a_id', 'tenant_id', 0) = joined_md5('pairs_ab') AS small_memory_is_the_join;
RESET maintenance_work_mem;
SET sift.test_conn_fill = 0;

-- the reader over the same relations: the scope units of dim_a_id of 1..2200 and the changed pairs, for the relation read by key (btree on (tenant_id, dim_a_id): probed by key), from the leaves of
-- its journal index, and from the relation (a partial btree: the plain journal lists the soft deleted rows too)
-- one table of events for each relation, each with the index that declares its scope: the marker names the scanned index
CREATE TABLE events_ab (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events_ab SELECT i, CASE WHEN i % 100 < 15 THEN NULL ELSE 1 + (i * 7919) % 2200 END, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 8000) i;
INSERT INTO events_ab SELECT 8000 + i, 7 + i % 2, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, 'p' FROM generate_series(1, 300) i;
CREATE TABLE events_ba (LIKE events_ab INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE events_soft (LIKE events_ab INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO events_ba SELECT * FROM events_ab;
INSERT INTO events_soft SELECT * FROM events_ab;
CREATE INDEX events_ab_col ON events_ab USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> pairs_ab(dim_a_id, tenant_id)');
CREATE INDEX events_ba_col ON events_ba USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> pairs_ba(dim_a_id, tenant_id)');
CREATE INDEX events_soft_col ON events_soft USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> pairs_soft(dim_a_id, tenant_id)');
REINDEX INDEX events_ab_col;
REINDEX INDEX events_ba_col;
REINDEX INDEX events_soft_col;
VACUUM (FREEZE, ANALYZE) events_ab, events_ba, events_soft;
\ir test/scope_multiset_helpers.sql

CREATE FUNCTION probes() RETURNS TABLE (btree text, n int, label text, verdict text, route text, delta text, bounded boolean) LANGUAGE sql AS $$
	SELECT 'ab', p.* FROM probe('events_ab', 'events_ab_col', 'pairs_ab') p
	UNION ALL SELECT 'ba', p.* FROM probe('events_ba', 'events_ba_col', 'pairs_ba') p
	UNION ALL SELECT 'soft', p.* FROM probe('events_soft', 'events_soft_col', '(SELECT * FROM pairs_soft WHERE deleted_at IS NULL)') p
	ORDER BY 1, 2
$$;
-- the statement of a state is run on the three relations (PAIRS stands for each)
CREATE FUNCTION edit_all(stmt text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	t text;
BEGIN
	FOREACH t IN ARRAY ARRAY['pairs_ab', 'pairs_ba', 'pairs_soft'] LOOP
		EXECUTE replace(stmt, 'PAIRS', t);
	END LOOP;
END $$;
-- each state (they accumulate) by each read shape on each relation: the cells, how many are md5 equal to the plain statement, read the units (a top-N of a changed pair does not) and are bounded
CREATE TABLE states (n int, label text, stmt text);
INSERT INTO states VALUES
	(1, 'built', 'SELECT 1'),
	(2, 'three more rows of a run of 12,000', 'INSERT INTO PAIRS (dim_a_id, tenant_id) SELECT 7, 3 FROM generate_series(1, 3)'),
	(3, '1,000 rows of the run deleted, the other pair of the dim_a_id untouched', 'DELETE FROM PAIRS WHERE id IN (SELECT id FROM PAIRS WHERE dim_a_id = 7 AND tenant_id = 3 ORDER BY id LIMIT 1000)'),
	(4, 'the last 9,000 rows of a pair deleted: the pair is gone', 'DELETE FROM PAIRS WHERE dim_a_id = 8'),
	(5, 'a run of 12,000 rows moved to another key, one row of it left', 'UPDATE PAIRS SET tenant_id = 5 WHERE dim_a_id = 7 AND tenant_id = 4 AND id <> (SELECT min(id) FROM PAIRS WHERE dim_a_id = 7 AND tenant_id = 4)');
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), format('SELECT edit_all(%L)', stmt),
	'SELECT btree, count(*) AS cells, count(*) FILTER (WHERE verdict = ''same'') AS same, count(*) FILTER (WHERE route = ''units'') AS units, count(*) FILTER (WHERE bounded) AS bounded FROM probes() GROUP BY btree ORDER BY btree'])
	WITH ORDINALITY u(c, o) ORDER BY n, o \gexec
-- and buffered events of the heavy dim_a_id: the delta copies each once for each distinct pair
INSERT INTO events_ab SELECT 9000 + i, 7 + i % 3, timestamptz '2026-02-05', 4, 'b' FROM generate_series(1, 60) i;
INSERT INTO events_ba SELECT * FROM events_ab WHERE id >= 9000;
INSERT INTO events_soft SELECT * FROM events_ab WHERE id >= 9000;
SELECT btree, count(*) AS cells, count(*) FILTER (WHERE verdict = 'same') AS same, count(*) FILTER (WHERE route = 'units') AS units, count(*) FILTER (WHERE bounded) AS bounded FROM probes() GROUP BY btree ORDER BY btree;
SELECT sift.col_verify('events_ab_col', true), sift.col_verify('events_ba_col', true), sift.col_verify('events_soft_col', true);

DROP FUNCTION edit_all(text), probes(), probe(text, text, text), expand_md5(regclass, regclass, text, text, int), joined_md5(text), entries_of(regclass, regclass, int);
DROP TABLE shapes, states, facts;
DROP FUNCTION ms_run(text, text, text, text), changed_rows(text, text);
DROP TABLE events_ab, events_ba, events_soft;
DROP TABLE pairs_ab, pairs_ba, pairs_soft;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
