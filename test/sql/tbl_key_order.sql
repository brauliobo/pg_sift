-- The key_order option of a sift_table: VACUUM FULL writes the rows in the order of the key, the table says how far its rows are from it, and every
-- other statement (CLUSTER by an index, a table without the option) is unchanged.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

-- the shapes a key may have, one table for each: loaded in a scattered order, the key set, rewritten; what the rewrite kept and gave is read back
CREATE TABLE ko_cases (n int GENERATED ALWAYS AS IDENTITY, label text, key text, keyrow text, before text, after text, phys text, sorted text, scattered float8, sorted_upk float8);
INSERT INTO ko_cases (label, key, keyrow) VALUES ('integer', 'k', 'k'), ('timestamp', 'ts', 'ts'), ('text', 's', 's'), ('integer, timestamp', 'k, ts', '(k, ts)'),
	('float with NULLs', 'f', 'f');
SELECT format('CREATE TABLE ko_%s (id int, k int, ts timestamptz, s text, f float8) USING sift_table', n) FROM ko_cases \gexec
SELECT format($f$INSERT INTO ko_%s
	SELECT i, (hashint8(i) & 2147483647) %% 500, timestamptz '2024-01-01' + ((hashint8(i * 3) & 2147483647) %% 100000) * interval '1 minute',
		   'v' || (hashint8(i * 5) & 2147483647) %% 700, CASE WHEN i %% 10 = 0 THEN NULL ELSE ((hashint8(i * 7) & 2147483647) %% 1000)::float8 / 8 END
	FROM generate_series(1, 150000) i$f$, n) FROM ko_cases \gexec
SELECT format('VACUUM ko_%s', n) FROM ko_cases \gexec
SELECT format('SELECT sift.table_set_option(%L, %L, %L)', 'ko_' || n, 'key_order', key) FROM ko_cases \gexec
SELECT format('UPDATE ko_cases SET before = (SELECT md5(string_agg(t::text, $$|$$ ORDER BY id)) FROM ko_%1$s t), scattered = (SELECT units_per_key FROM sift.table_layouts(%2$L)) WHERE n = %1$s',
	n, 'ko_' || n) FROM ko_cases \gexec
SELECT format('VACUUM FULL ko_%s', n) FROM ko_cases \gexec
SELECT format('UPDATE ko_cases SET after = (SELECT md5(string_agg(t::text, $$|$$ ORDER BY id)) FROM ko_%1$s t), sorted_upk = (SELECT units_per_key FROM sift.table_layouts(%2$L)),
	phys = (SELECT md5(string_agg(%3$s::text, $$,$$ ORDER BY ctid)) FROM ko_%1$s), sorted = (SELECT md5(string_agg(%3$s::text, $$,$$ ORDER BY %4$s)) FROM ko_%1$s) WHERE n = %1$s',
	n, 'ko_' || n, keyrow, key) FROM ko_cases \gexec

-- the rows come back unchanged and in the order of the key (NULLs last); where the first key column has zones (integer, date, timestamp) a sorted table reads one unit
-- for an equality, and the table loaded in scattered order read them all
SELECT label, before = after AS same_rows, phys = sorted AS in_key_order, sorted_upk = 1 AS sorted_reads_one_unit, scattered > 2 AS scattered_reads_many
FROM ko_cases ORDER BY n;
SELECT format('DROP TABLE ko_%s', n) FROM ko_cases \gexec
DROP TABLE ko_cases;

-- the option itself: what it refuses, and that a setting cannot hold it
CREATE TABLE ko (id int, k int, p point, s text, "Mixed" int) USING sift_table;
CREATE TABLE ko_heap (id int);
CREATE FUNCTION ko_err(key text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift.table_set_option('ko', 'key_order', key);
	RETURN 'ok: ' || sift.table_get_option('ko', 'key_order');
EXCEPTION WHEN OTHERS THEN
	RETURN sqlerrm;
END $$;
SELECT key, ko_err(key) FROM (VALUES ('k'), (' k , id '), ('K'), ('"Mixed"'), ('Mixed'), ('nope'), ('k, k'), ('p'), (''), ('k,'),
	('id, k, s, "Mixed", id2, id3, id4, id5, id6')) AS t(key);
SELECT sift.table_reset_option('ko', 'key_order');
SELECT sift.table_get_option('ko', 'key_order') = '' AS none_after_reset, (SELECT source FROM sift.table_options('ko') WHERE name = 'key_order') AS source;
SET sift.table_key_order = 'k';
SHOW sift.table_key_order;
SELECT sift.table_set_option('ko_heap', 'key_order', 'id');

-- the status view: a table with no key, with one set on an empty table, with its rows scattered, sorted again
SELECT layout, kind, key_order, rows, units_per_key, restore FROM sift.table_layouts('ko');
SELECT sift.table_set_option('ko', 'key_order', 'k');
SELECT layout, kind, key_order, rows, units_per_key, restore FROM sift.table_layouts('ko');
INSERT INTO ko SELECT i, (hashint8(i) & 2147483647) % 400, NULL, 'x', i FROM generate_series(1, 120000) i;
VACUUM ko;
SELECT layout, kind, key_order, rows, units_per_key > 2 AS scattered, restore IS NOT NULL AS says_how_to_restore FROM sift.table_layouts('ko');
RESET client_min_messages;
\set VERBOSITY terse
VACUUM ko;
SELECT sift.table_set_option('ko', 'key_order', 'k, id');
\set VERBOSITY default
SET client_min_messages = warning;
VACUUM FULL ko;
SELECT layout, kind, key_order, rows, units_per_key, restore FROM sift.table_layouts('ko');
VACUUM ko;

-- rows loaded after the rewrite are in arrival order: the table drifts, says so, and the rewrite puts it right
INSERT INTO ko SELECT 200000 + i, (hashint8(i * 11) & 2147483647) % 400, NULL, 'y', i FROM generate_series(1, 30000) i;
VACUUM ko;
SELECT units_per_key > 1.5 AS drifted, restore IS NOT NULL AS says_how_to_restore FROM sift.table_layouts('ko');
RESET client_min_messages;
\set VERBOSITY terse
VACUUM ko;
\set VERBOSITY default
SET client_min_messages = warning;
VACUUM FULL ko;
SELECT rows, units_per_key, restore FROM sift.table_layouts('ko');

-- an index of the table is right after the rewrite; CLUSTER by an index is the index's order, not the key's
CREATE INDEX ko_k ON ko (k);
CREATE UNIQUE INDEX ko_id ON ko (id);
VACUUM FULL ko;
SET enable_seqscan = off;
SELECT count(*) AS by_index, count(*) = (SELECT count(*) FROM ko WHERE k + 0 = 7) AS equals_scan FROM ko WHERE k = 7;
RESET enable_seqscan;
CLUSTER ko USING ko_id;
SELECT (SELECT count(*) FROM (SELECT id, lag(id) OVER (ORDER BY ctid) AS prev FROM ko) x WHERE id < prev) AS ids_out_of_order_by_ctid,
	   (SELECT count(*) FROM (SELECT k, lag(k) OVER (ORDER BY ctid) AS prev FROM ko) x WHERE k < prev) > 0 AS keys_out_of_order_by_ctid;
VACUUM FULL ko;
SELECT (SELECT count(*) FROM (SELECT k, lag(k) OVER (ORDER BY ctid) AS prev FROM ko) x WHERE k < prev) AS keys_out_of_order_by_ctid;

-- a table without the option rewrites as before (the same order and the same units)
CREATE TABLE ko_plain (id int, k int) USING sift_table;
INSERT INTO ko_plain SELECT i, (hashint8(i) & 2147483647) % 400 FROM generate_series(1, 50000) i;
VACUUM FULL ko_plain;
SELECT (SELECT count(*) FROM (SELECT id, lag(id) OVER (ORDER BY ctid) AS prev FROM ko_plain) x WHERE id < prev) AS ids_out_of_order_by_ctid;

DROP TABLE ko, ko_heap, ko_plain;
DROP FUNCTION ko_err(text);
DROP EXTENSION pg_sift;
