-- The rewrite of a sift_col index by range (sift.col_auto_merge_partition_kb): the index is kept as segments of about that size, a step of the rewrite
-- takes one of them, the one the churn left dirty, and the partitions it did not touch are not written at all; the small segments the promotions left over
-- the partitions are merged into the ones they share superblocks with. The answers never change and the index verifies after every step.
CREATE EXTENSION pg_sift;
SET sift.agg_dead_delta = 0;	-- a unit with a dead row is scanned here (col_dead_delta answers it net of the dead rows)
SET sift.async_maintenance = off;
-- the overlays are cut by superblock here: the units a TID-range overlay (sift.col_unit_floor, col_unit_floor) lists are no fragmentation to fold
SET sift.col_unit_floor = 0;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE items (id int, key int, kind text, amount numeric(10,2), pad text, tag text, tag2 text) WITH (autovacuum_enabled = false);
INSERT INTO items SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 1000), md5(g::text), md5(g::text || 'b') FROM generate_series(1, 40000) g;
CREATE TABLE items_plain (LIKE items) WITH (autovacuum_enabled = false);
INSERT INTO items_plain SELECT * FROM items ORDER BY id;
-- the partitions are cut by their bytes: the stored form of a big dictionary (minor 14) would double the index, so dict_lookup = off
CREATE INDEX items_col ON items USING sift_col (id, key, kind, amount, tag, tag2) WITH (dict_lookup = off);
CREATE INDEX items_plain_col ON items_plain USING sift_col (id, key, kind, amount, tag, tag2) WITH (dict_lookup = off);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) items, items_plain;
-- the rewrite of the worker is what is looked at: neither the rewrite of a lone mostly dead segment nor the moves of VACUUM's space step may change the segments
ALTER INDEX items_col SET (dead_rewrite_pct = 100);
ALTER INDEX items_plain_col SET (dead_rewrite_pct = 100);
SET sift.vacuum_rewrite_limit = 0;

-- whether none, some or all the units of the SiftAgg node were answered from the pre-aggregates, and whether the three ways (pre-aggregates, scan, heap) agree
CREATE FUNCTION pt_state(tbl text) RETURNS TABLE (from_preagg text, same bool) LANGUAGE plpgsql AS $$
DECLARE l text; u int; p int; q text := format('SELECT kind, count(*), sum(amount) FROM %s GROUP BY kind', tbl); a text; b text; c text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		u := coalesce(substring(l from '^\s*Units: (\d+)')::int, u);
		p := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, p);
	END LOOP;
	SET LOCAL sift.agg_preagg = on;
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO a;
	SET LOCAL sift.agg_preagg = off;
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO b;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO c;
	RETURN QUERY SELECT CASE WHEN p = 0 THEN 'none' WHEN p = u THEN 'all' ELSE 'some' END, a = b AND b = c;
END $$;

-- the segments: how many, how many hold a dead row, how many rows are in the write buffer
CREATE FUNCTION pt_index(idx regclass) RETURNS TABLE (segments int, dirty int, buffer_rows int) LANGUAGE sql AS $$
	SELECT (count(*) FILTER (WHERE kind = 'immutable'))::int, (count(*) FILTER (WHERE dirty))::int, (coalesce(sum(rows) FILTER (WHERE kind <> 'immutable'), 0))::int
	FROM sift.col_segments(idx) $$;

-- the same work on both tables: n of m of the rows with an id from from_id on are updated, those of the next residue deleted, and 200 rows are inserted
CREATE PROCEDURE pt_churn(n int, from_id int, m int) LANGUAGE plpgsql AS $$
DECLARE t text;
BEGIN
	FOREACH t IN ARRAY ARRAY['items', 'items_plain'] LOOP
		EXECUTE format('UPDATE %s SET amount = amount + 1 WHERE id >= %s AND id %% %s = %s', t, from_id, m, n % m);
		EXECUTE format('DELETE FROM %s WHERE id >= %s AND id %% %s = %s', t, from_id, m, (n + m / 2) % m);
		EXECUTE format('INSERT INTO %s SELECT %s * 100000 + g, g %% 1000, ''cat_'' || (g %% 8), g %% 997, repeat(''x'', 1000), md5((%s * 100000 + g)::text), md5((%s * 100000 + g)::text || ''b'') FROM generate_series(1, 200) g', t, n, n, n);
	END LOOP;
END $$;

-- the partition size: 128 kB, which makes about a dozen partitions of the index (the segments of 600 rows over a few units are under half of it, the overlays)
CREATE FUNCTION pt_partition_kb(idx regclass) RETURNS bool LANGUAGE sql AS $$
	SELECT set_config('sift.col_auto_merge_partition_kb', '128', false) IS NOT NULL $$;
SELECT pt_partition_kb('items_col') AS partition_kb_set;
SELECT 'built' AS step, * FROM pt_index('items_col');

-- 1. churn of the whole table: every unit holds a dead row, the first step cuts the one segment into partitions
CALL pt_churn(1, 0, 100);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) items, items_plain;
SELECT 'cycle 1' AS step, * FROM pt_state('items');
SELECT sift.col_maintenance_due('items_col', false) AS due;
SELECT sift.col_maintain('items_col') AS rewritten;
SELECT segments >= 8 AS partitions, dirty, buffer_rows FROM pt_index('items_col');
SELECT sift.col_maintain('items_col') AS rewritten_again;
SELECT 'split' AS step, * FROM pt_state('items');
SELECT sift.col_verify('items_col', true);
SELECT bool_and(rows > 0) AS no_empty_partition, max(bytes) < 2 * min(bytes) AS even FROM sift.col_segments('items_col') WHERE kind = 'immutable' AND bytes > 100000;

-- 2. churn of the newest 5% of the ids: far from dirty_pct of the units of the index, but all of those of the last partition. The rule on the whole index is not met
CREATE TEMP TABLE before AS SELECT gen FROM sift.col_segments('items_col') WHERE kind = 'immutable';
CALL pt_churn(2, 38000, 4);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) items, items_plain;
SELECT 'window' AS step, * FROM pt_state('items');
SELECT segments >= 8 AS partitions, dirty > 0 AS some_dirty, dirty < 4 AS few_dirty FROM pt_index('items_col');
SET sift.col_auto_merge_partition_kb = 0;
SELECT sift.col_maintenance_due('items_col', false) AS due_whole_index;
SELECT pt_partition_kb('items_col') AS partition_kb_set;
SELECT sift.col_maintenance_due('items_col', false) AS due_by_range;
SELECT sift.col_maintain('items_col') AS rewritten;
-- the partitions the churn did not reach are the segments they were, the write buffer is promoted, no dead row is left
SELECT count(*) FILTER (WHERE gen IN (SELECT gen FROM before)) >= (SELECT count(*) FROM before) - 3 AS partitions_kept,
       count(*) FILTER (WHERE gen NOT IN (SELECT gen FROM before)) BETWEEN 1 AND 4 AS partitions_written, count(*) FILTER (WHERE dirty) AS dirty
FROM sift.col_segments('items_col') WHERE kind = 'immutable';
SELECT segments >= 8 AS partitions, dirty, buffer_rows FROM pt_index('items_col');
SELECT 'windowed' AS step, * FROM pt_state('items');
SELECT sift.col_maintain('items_col') AS rewritten_again;
SELECT sift.col_verify('items_col', true);

-- 3. the merge tiers leave the partitions alone: eight segments of one size are merged by the policy of an index with no partitions
SELECT sift.col_merge('items_col') AS merged_partitions;
SELECT segments >= 8 AS partitions FROM pt_index('items_col');
SET sift.col_auto_merge_partition_kb = 0;
SELECT sift.col_merge('items_col') > 0 AS merged_without_partitions;
SELECT segments < 8 AS fewer FROM pt_index('items_col');
SELECT sift.col_verify('items_col', true);

-- 4. churn of the whole table on the index that is one segment again: cut into partitions, and then a step for each of them (every segment is new)
SELECT pt_partition_kb('items_col') AS partition_kb_set;
CALL pt_churn(3, 0, 50);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) items, items_plain;
SELECT sift.col_maintain('items_col') AS cut;
CALL pt_churn(4, 0, 50);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) items, items_plain;
CREATE TEMP TABLE before2 AS SELECT gen FROM sift.col_segments('items_col') WHERE kind = 'immutable';
SELECT sift.col_maintenance_due('items_col', false) AS due;
SELECT sift.col_maintain('items_col') AS rewritten;
SELECT segments >= 8 AS partitions, dirty, buffer_rows FROM pt_index('items_col');
SELECT count(*) FILTER (WHERE gen IN (SELECT gen FROM before2)) AS old_segments_left FROM sift.col_segments('items_col') WHERE kind = 'immutable';
SELECT 'all dirty' AS step, * FROM pt_state('items');
SELECT sift.col_verify('items_col', true);

-- 5. rows inserted into the holes the deleted ones left go to superblocks the partitions have: units listed twice. The overlay is merged into the partitions it shares them with
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) items, items_plain;
INSERT INTO items SELECT 2000000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 1000), md5((2000000 + g)::text), md5((2000000 + g)::text || 'b') FROM generate_series(1, 500) g;
INSERT INTO items_plain SELECT 2000000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 1000), md5((2000000 + g)::text), md5((2000000 + g)::text || 'b') FROM generate_series(1, 500) g;
SELECT sift.col_promote('items_col') > 0 AS promoted;
CREATE TEMP TABLE before3 AS SELECT gen FROM sift.col_segments('items_col') WHERE kind = 'immutable';
SELECT (SELECT sum(units) FROM sift.col_segments('items_col')) > (SELECT units FROM sift.col_index_info('items_col')) AS listed_twice;
SET sift.col_auto_merge_buffer_pct = 100;
SET sift.col_auto_merge_dirty_pct = 100;
SELECT set_config('sift.col_auto_merge_fragmentation', (SELECT floor(100.0 * (SELECT sum(units) FROM sift.col_segments('items_col')) / (SELECT units FROM sift.col_index_info('items_col'))) / 100)::text, false) IS NOT NULL AS fragmentation_set;
SELECT sift.col_maintenance_due('items_col', false) AS due;
SELECT sift.col_maintain('items_col') AS rewritten;
SELECT (SELECT sum(units) FROM sift.col_segments('items_col')) = (SELECT units FROM sift.col_index_info('items_col')) AS none_listed_twice;
SELECT segments >= 8 AS partitions, dirty, buffer_rows FROM pt_index('items_col');
SELECT count(*) FILTER (WHERE gen IN (SELECT gen FROM before3)) > 0 AS partitions_kept FROM sift.col_segments('items_col') WHERE kind = 'immutable';
SELECT sift.col_maintain('items_col') AS rewritten_again, coalesce(sift.col_maintenance_due('items_col', false), 'not due') AS due_again;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) items;
SELECT 'merged' AS step, * FROM pt_state('items');
SELECT sift.col_verify('items_col', true);

-- the answers are those of the table that was never maintained
SELECT (SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM items GROUP BY kind) x)
     = (SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM items_plain GROUP BY kind) x) AS same_as_not_maintained;

DROP TABLE items, items_plain;
DROP FUNCTION pt_state(text);
DROP FUNCTION pt_index(regclass);
DROP FUNCTION pt_partition_kb(regclass);
DROP PROCEDURE pt_churn(int, int, int);
DROP EXTENSION pg_sift;
