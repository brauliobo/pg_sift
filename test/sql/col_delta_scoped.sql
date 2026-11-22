-- A scoped sift_col index takes a column delta, not a tombstone and a new entry, for an UPDATE that changes only columns the summary of its scope does not read (a text, a document leaf): the row stays
-- (no dead row, no stage), the scope still answers and equals the heap, a read of the patched column through the index equals the heap. An UPDATE of a column the summary reads (a measure, the member key,
-- the time) keeps the tombstone path, and the scope summary equals the heap after it. VACUUM folds the deltas; every state is checked against the heap twin. Every check prints t.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET sift.test_create_format1 = off;

CREATE TABLE entity_parents (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id)) WITH (autovacuum_enabled = false);
INSERT INTO entity_parents SELECT e, 1 + e % 7 FROM generate_series(1, 200) e;
CREATE TABLE ev (id int PRIMARY KEY, entity_id int NOT NULL, day date NOT NULL, qty int, amount numeric(12,2), note text, doc jsonb) WITH (autovacuum_enabled = false);
CREATE TABLE evh (LIKE ev INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT g, 1 + g % 200, date '2030-01-01' + g % 90, g % 13, (g % 1000) / 8.0, 'note ' || g, jsonb_build_object('flag', CASE WHEN g % 3 = 0 THEN 'a' ELSE 'b' END, 'n', g) FROM generate_series(1, 6000) g;
INSERT INTO evh SELECT * FROM ev;
SET maintenance_work_mem = '64MB';
CREATE INDEX ev_sift ON ev USING sift_col (id, entity_id, day, qty, amount, note, doc)
	WITH (key_order = 'entity_id, day', paths = 'doc.flag text', scope = 'parent_id: entity_id -> entity_parents(entity_id, parent_id)');

CREATE FUNCTION same_rows(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', a) INTO x;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', b) INTO y;
	RETURN x = y;
END $$;
CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT same_rows(replace(q, '@T', 'ev'), replace(q, '@T', 'evh')) $$;
CREATE FUNCTION dead() RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce(sum(dead_rows), 0)::bigint FROM sift.parts('ev_sift') $$;
CREATE FUNCTION scope_plan() RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) SELECT p.parent_id, count(*), sum(e.qty) FROM ev e JOIN entity_parents p USING (entity_id) GROUP BY p.parent_id' LOOP
		IF line LIKE '%SiftAggV6Scope%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE FUNCTION scoped() RETURNS boolean LANGUAGE sql AS $$
	SELECT same_rows('SELECT p.parent_id, count(*), sum(e.qty), max(e.day) FROM ev e JOIN entity_parents p USING (entity_id) GROUP BY p.parent_id',
					 'SELECT p.parent_id, count(*), sum(e.qty), max(e.day) FROM evh e JOIN entity_parents p USING (entity_id) GROUP BY p.parent_id') $$;

SELECT scope_plan() AS scope_serves_when_built;
SELECT scoped() AS scoped_when_built;
SELECT dead() AS dead_rows_when_built;

-- a note and a document leaf: the columns of no cube and no ring (disjoint rows: a row a record has moved takes the tombstone path when it is updated again)
UPDATE ev SET note = note || ' edited' WHERE id % 10 = 0;
UPDATE evh SET note = note || ' edited' WHERE id % 10 = 0;
UPDATE ev SET doc = doc || '{"flag": "c"}' WHERE id % 15 = 1;
UPDATE evh SET doc = doc || '{"flag": "c"}' WHERE id % 15 = 1;
SELECT dead() = 0 AS note_and_document_update_leaves_no_dead_row;
SELECT parity('SELECT id, note, doc FROM @T WHERE entity_id = 7 OR id % 10 = 0 OR id % 15 = 1') AS rows_read_equal_heap;
SELECT parity('SELECT doc->>''flag'', count(*) FROM @T GROUP BY 1') AS document_leaf_aggregate_equals_heap;
SELECT parity('SELECT count(*), count(note) FILTER (WHERE note LIKE ''%edited'') FROM @T') AS patched_text_counted_equals_heap;
SELECT scope_plan() AS scope_serves_after_note_update;
SELECT scoped() AS scoped_after_note_update;

-- a measure (qty), the time (day) and the member key (entity_id): the summary reads them
UPDATE ev SET qty = qty + 1 WHERE id % 20 = 0;
UPDATE evh SET qty = qty + 1 WHERE id % 20 = 0;
SELECT dead() > 0 AS measure_update_is_a_tombstone;
UPDATE ev SET day = day + 1 WHERE id % 25 = 0;
UPDATE evh SET day = day + 1 WHERE id % 25 = 0;
UPDATE ev SET entity_id = 1 + (entity_id + 3) % 200 WHERE id % 30 = 0;
UPDATE evh SET entity_id = 1 + (entity_id + 3) % 200 WHERE id % 30 = 0;
SELECT scoped() AS scoped_after_measure_time_and_member_updates;
SELECT parity('SELECT id, entity_id, day, qty, note FROM @T WHERE id % 5 = 0') AS rows_after_all_updates;

VACUUM ev;
SELECT scoped() AS scoped_after_vacuum;
SELECT parity('SELECT id, entity_id, day, qty, note, doc FROM @T WHERE id % 5 = 0 OR id % 10 = 0') AS rows_after_vacuum;
SELECT NOT quarantined AS index_not_quarantined FROM sift.status('ev_sift');
DROP TABLE ev, evh, entity_parents;
DROP FUNCTION scoped(), scope_plan(), dead(), parity(text), same_rows(text, text);
DROP EXTENSION pg_sift;
