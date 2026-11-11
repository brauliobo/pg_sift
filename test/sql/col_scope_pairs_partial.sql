-- The pairs of a scope may be the rows of a PARTIAL unique btree of the pair relation: WHERE deleted_at IS NULL makes the rows that are not soft deleted the pairs, and the all-rows unique
-- btree (the primary key) may stay beside it. Each state of the relation is read from the scope segments less the members that changed, with the answer of the join on the active rows:
-- soft delete, restore, insert, move, delete, an update of a column no index holds, the rebuild of the btree. The choice of the btree, the journal that must hear of every pair, and the
-- dependency that keeps it.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE FUNCTION try(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE hint text; BEGIN EXECUTE stmt; RETURN 'ok';
   EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT; RETURN sqlerrm || coalesce(' [' || hint || ']', ''); END $$;

-- the groups 1..21 read from the scope and by the join on the active rows: how many differ, and how many were read from the entity rows instead of the units
CREATE FUNCTION compare(OUT differ int, OUT fell_back int) LANGUAGE plpgsql AS $$
DECLARE
	k int;
	a record;
	b record;
	p jsonb;
BEGIN
	differ := 0;
	fell_back := 0;
	FOR k IN 1..21 LOOP
		EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*), sum(amount) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''group_id'', f.dim_a_id, %s)', k) INTO p;
		IF jsonb_path_exists(p, '$.**."Scope Fallback"') THEN fell_back := fell_back + 1; END IF;
		EXECUTE format('SELECT count(*) c, coalesce(sum(amount), 0) s FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''group_id'', f.dim_a_id, %s)', k) INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE format('SELECT count(*) c, coalesce(sum(amount), 0) s FROM facts f WHERE f.dim_a_id IN (SELECT dim_a_id FROM memberships WHERE group_id = %s AND deleted_at IS NULL)', k) INTO b;
		SET LOCAL sift.enable_agg = on;
		IF a <> b THEN differ := differ + 1; END IF;
	END LOOP;
END $$;
-- the records the journal index holds: ended pairs (4) and inserted pairs (5)
CREATE FUNCTION journal(OUT ended bigint, OUT inserted bigint) LANGUAGE plpgsql AS
$$ BEGIN SELECT count(*) FILTER (WHERE kind = 4), count(*) FILTER (WHERE kind = 5) INTO ended, inserted FROM sift._test_col_log_entries('memberships_journal') WHERE kind IN (4, 5); END $$;

CREATE TABLE memberships (group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz, note int, PRIMARY KEY (group_id, dim_a_id)) WITH (autovacuum_enabled = false);
INSERT INTO memberships SELECT 1 + e % 20, e, CASE WHEN e % 9 = 0 THEN now() END FROM generate_series(1, 400) e;
CREATE UNIQUE INDEX memberships_active ON memberships (dim_a_id, group_id) WHERE deleted_at IS NULL;
CREATE INDEX memberships_journal ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid') WHERE deleted_at IS NULL;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + i % 450, timestamptz '2026-01-01' + (i % 100) * interval '1 day', i % 10 FROM generate_series(1, 20000) i;

-- the partial unique btree wins over the primary key, and its predicate is said; the journal index is partial on the same predicate
SET client_min_messages = notice;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'group_id: dim_a_id -> memberships(dim_a_id, group_id)');
SET client_min_messages = warning;
REINDEX INDEX facts_col;
VACUUM (FREEZE, ANALYZE) facts;
SELECT rel, member, key, btree, pred FROM sift._test_scope_legs('facts_col', 0);
-- the scope entries are the rows of the active pairs
SELECT (SELECT count(*) FROM memberships WHERE deleted_at IS NULL) AS active_pairs, (SELECT count(*) FROM memberships) AS all_rows;
SELECT * FROM compare();

-- every state of the relation, one after the other: the answers stay the join's
CREATE TABLE states (n int, label text, stmt text);
INSERT INTO states VALUES
	(1, 'soft delete', 'UPDATE memberships SET deleted_at = now() WHERE group_id = 5 AND dim_a_id = 4'),
	(2, 'restore', 'UPDATE memberships SET deleted_at = NULL WHERE group_id = 5 AND dim_a_id = 4'),
	(3, 'update of a column no index holds', 'UPDATE memberships SET note = 1 WHERE group_id = 5 AND dim_a_id = 24'),
	(4, 'soft delete, then update the deleted row', 'UPDATE memberships SET deleted_at = now() WHERE group_id = 5 AND dim_a_id = 44; UPDATE memberships SET note = 2 WHERE group_id = 5 AND dim_a_id = 44'),
	(5, 'insert a soft deleted pair', 'INSERT INTO memberships VALUES (5, 430, now())'),
	(6, 'insert an active pair', 'INSERT INTO memberships VALUES (5, 431, NULL)'),
	(7, 'move a member to another group', 'UPDATE memberships SET group_id = 6 WHERE group_id = 5 AND dim_a_id = 64'),
	(8, 'restore a pair of a member that has another one', 'UPDATE memberships SET deleted_at = NULL WHERE group_id = 5 AND dim_a_id = 44'),
	(9, 'delete an active pair', 'DELETE FROM memberships WHERE group_id = 5 AND dim_a_id = 84'),
	(10, 'delete a soft deleted pair', 'DELETE FROM memberships WHERE group_id = 10 AND dim_a_id = 9'),
	(11, 'soft delete many', 'UPDATE memberships SET deleted_at = now() WHERE dim_a_id % 7 = 0 AND deleted_at IS NULL'),
	(12, 'restore many', 'UPDATE memberships SET deleted_at = NULL WHERE dim_a_id % 14 = 0'),
	(13, 'rebuild the partial btree', 'REINDEX INDEX memberships_active'),
	(14, 'rebuild it concurrently', 'REINDEX INDEX CONCURRENTLY memberships_active'),
	(15, 'a pair after the rebuild', 'INSERT INTO memberships VALUES (7, 432, NULL)');
-- each state is committed, and then read: the answers differing from the join's (differ), the groups that fell back to the entity rows, the journal's records
SELECT stmt FROM (SELECT n, 0 AS k, stmt FROM states
	UNION ALL SELECT n, 1, format('SELECT %s AS n, %L AS state, c.differ, c.fell_back, j.ended, j.inserted FROM compare() c, journal() j', n, label) FROM states) t ORDER BY n, k \gexec
SELECT * FROM compare();
-- a rebuild writes the segments again from the active pairs, and the reads are the units
REINDEX INDEX facts_col;
SELECT * FROM compare();
SELECT (SELECT count(*) FROM memberships WHERE deleted_at IS NULL) AS active_pairs;

-- the btree and the journal index are depended on, and a btree made later changes nothing
SELECT string_agg(refobjid::regclass::text, ' ' ORDER BY refobjid::regclass::text) AS edges FROM pg_depend
WHERE classid = 'pg_class'::regclass AND objid = 'facts_col'::regclass AND refclassid = 'pg_class'::regclass AND deptype = 'n' AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i';
SELECT try('DROP INDEX memberships_active') AS drop_btree;
SELECT try('DROP INDEX memberships_journal') AS drop_journal;
CREATE UNIQUE INDEX memberships_other ON memberships (group_id, dim_a_id) WHERE note IS NULL;
INSERT INTO memberships VALUES (8, 433, NULL);
SELECT * FROM compare();
SELECT btree FROM sift._test_scope_legs('facts_col', 0);
-- two partial unique btrees: the pairs are a guess, REINDEX says so; USING names the one
SELECT try('REINDEX INDEX facts_col') AS two_partial;
SELECT try('ALTER INDEX facts_col SET (scope = ''group_id: dim_a_id -> memberships(dim_a_id, group_id) using memberships_active'')') AS using_active;
SET client_min_messages = notice;
REINDEX INDEX facts_col;
SET client_min_messages = warning;
SELECT btree FROM sift._test_scope_legs('facts_col', 0);
SELECT * FROM compare();
SELECT try('ALTER INDEX facts_col SET (scope = ''group_id: dim_a_id -> memberships(dim_a_id, group_id) using memberships_missing'')') AS using_unknown;
SELECT try('ALTER INDEX facts_col SET (scope = ''group_id: dim_a_id -> memberships(dim_a_id, group_id) using facts_pkey'')') AS using_not_a_pair_btree;
ALTER INDEX facts_col SET (scope = 'group_id: dim_a_id -> memberships(dim_a_id, group_id)');
DROP INDEX memberships_other;

-- a journal index must hear of every pair: partial on the predicate of the pairs is fine, partial on another is not
CREATE INDEX memberships_journal_b ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid') WHERE group_id < 10;
DROP INDEX facts_col;
DROP INDEX memberships_journal;
SELECT try('CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = ''group_id: dim_a_id -> memberships(dim_a_id, group_id)'')') AS journal_other_predicate;
DROP INDEX memberships_journal_b;
CREATE INDEX memberships_journal_c ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid') WHERE deleted_at IS NULL AND group_id > 0;
SELECT try('CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = ''group_id: dim_a_id -> memberships(dim_a_id, group_id)'')') AS journal_weaker_predicate;
DROP INDEX memberships_journal_c;

-- a predicate that is not immutable cannot be the btree's: PostgreSQL refuses it
SELECT try('CREATE UNIQUE INDEX memberships_timed ON memberships (dim_a_id, group_id) WHERE deleted_at IS NULL OR deleted_at > now()') AS time_dependent;

DROP TABLE facts, memberships, states;
DROP FUNCTION try(text), compare(), journal();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
