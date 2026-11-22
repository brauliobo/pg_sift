-- Compaction of format major 2 by level: a batch of 2048 rows or more is written as a part of level 1 at once, four parts of a level become one of the level above (never above level 2) with
-- their granules copied as they are, and the bytes written over many batches stay a few times the bytes ingested.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

CREATE TABLE c (id bigint, tenant int, status text, name text, amount numeric(12, 2), score float8);
CREATE INDEX c_idx ON c USING sift_col (id, tenant, status, name, amount, score) WITH (key_order = 'id');

-- Ingest batches of ascending keys, each compacted by the plan of a vacuum (what the plan names is compacted here, group by group, so that the bytes can be counted)
CREATE TEMP TABLE batch_stats (ingested bigint, written bigint, copied bigint, granules bigint, writer_max bigint);
CREATE PROCEDURE f2_batches(n int, rows_per_batch int) LANGUAGE plpgsql AS $$
DECLARE plan text; grp text; r record; ingested bigint := 0; written bigint := 0; copied bigint := 0; granules bigint := 0; writer_max bigint := 0; before_bytes bigint;
BEGIN
	FOR b IN 0 .. n - 1 LOOP
		SELECT sum(x) INTO before_bytes FROM unnest(sift._test_f2_part_bytes('c_idx', 1, NULL)) x;
		INSERT INTO c SELECT b * rows_per_batch + i, i % 40, (ARRAY['new', 'open', 'closed', 'paused', 'failed'])[1 + i % 5], 'user-' || (i % 3000), (i % 9000) / 100.0, (i % 977) / 10.0
		FROM generate_series(1, rows_per_batch) i;
		COMMIT;
		ingested := ingested + (SELECT sum(x) FROM unnest(sift._test_f2_part_bytes('c_idx', 1, NULL)) x) - before_bytes;
		plan := sift._test_f2_plan('c_idx', 1, NULL);
		FOREACH grp IN ARRAY string_to_array(plan, ' / ') LOOP
			CONTINUE WHEN grp = '';
			SELECT * INTO r FROM sift._test_f2_compact('c_idx', 1, NULL, string_to_array(grp, ',')::int8[]);
			written := written + r.bytes_out;
			copied := copied + r.granules_copied;
			granules := granules + r.granules_written;
			writer_max := greatest(writer_max, r.writer_bytes);
		END LOOP;
	END LOOP;
	INSERT INTO batch_stats VALUES (ingested, written, copied, granules, writer_max);
END $$;

-- 12 batches of 16000 rows: each is a part of level 1, every fourth of them joins the level above
CALL f2_batches(12, 16000);
SELECT ingested > 0 AS ingested, written > 0 AS compacted, round((ingested + written)::numeric / ingested, 1) <= 3.5 AS write_amplification_ok,
	   copied > 0 AS granules_copied, writer_max > 0 AS writer_measured, writer_max <= 8 * 1024 * 1024 AS writer_within_8mb
FROM batch_stats;
SELECT sift._test_f2_check_keys('c_idx');
SELECT level, count(*) AS parts, sum(nrows) AS rows FROM sift._test_f2_parts('c_idx', 1, NULL) GROUP BY level ORDER BY level;
SELECT f2_same('SELECT id, name FROM c WHERE id BETWEEN 100000 AND 100500') AS range_read, f2_same('SELECT count(*), sum(tenant) FROM c WHERE status = ''open''') AS filter_read;

-- Four parts of the first level with ascending keys are one part of the second (the rows of a part written at level 1 are not frozen yet, so the merge writes them again)
CREATE TABLE d (id bigint, tenant int, status text, v float8);
CREATE INDEX d_idx ON d USING sift_col (id, tenant, status, v) WITH (key_order = 'id');
DO $$ BEGIN
	FOR b IN 0 .. 3 LOOP
		INSERT INTO d SELECT b * 16000 + i, i % 40, (ARRAY['new', 'open', 'closed'])[1 + i % 3], (i % 977) / 10.0 FROM generate_series(1, 16000) i;
		COMMIT;
	END LOOP;
END $$;
SELECT level, count(*) AS parts, sum(nrows) AS rows FROM sift._test_f2_parts('d_idx', 1, NULL) GROUP BY level ORDER BY level;
SELECT sift._test_f2_plan('d_idx', 1, NULL) ~ '^\d+(,\d+){3}$' AS level_one_in_one_group;
SELECT done, rows_in, rows_out
FROM sift._test_f2_compact('d_idx', 1, NULL, (SELECT array_agg(part_gen)::int8[] FROM sift._test_f2_parts('d_idx', 1, NULL) WHERE level = 1));
SELECT level, count(*) AS parts, sum(nrows) AS rows FROM sift._test_f2_parts('d_idx', 1, NULL) GROUP BY level ORDER BY level;
SELECT sift._test_f2_check_keys('d_idx');
SELECT f2_same('SELECT id, status, v FROM d WHERE id BETWEEN 15000 AND 25000') AS joined_range, f2_same('SELECT count(*) FROM d WHERE status = ''closed''') AS joined_filter;

-- The rows a vacuum finds dead are dropped when their part is rewritten
DELETE FROM d WHERE id % 2 = 0;
VACUUM d;
SELECT f2_same('SELECT id FROM d WHERE id < 30000') AS after_vacuum;
SELECT sift._test_f2_check_keys('d_idx');

-- String columns that every input part codes by a dictionary of its part: the dictionaries are merged once and the rows are read as the codes of the merged dictionary (no string of a row is
-- copied, hashed or compared); the batches have strings of their own, so the parts differ and their granules cannot be copied. What is written is the bytes that reading the strings writes
CREATE PROCEDURE f2_dict_load(t text) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE %I (id bigint, tenant int, status text, name text, note text)', t);
	EXECUTE format('CREATE INDEX %I ON %I USING sift_col (id, tenant, status, name, note) WITH (key_order = ''id'')', t || '_idx', t);
	FOR b IN 0 .. 3 LOOP
		EXECUTE format($q$INSERT INTO %I SELECT $1 * 16000 + i, i %% 40, (ARRAY['new', 'open', 'closed', 'paused', 'failed', 'batch' || $1])[1 + i %% 6], 'user-' || ($1 * 500 + i %% 2000) || 'x',
			CASE WHEN i %% 7 = 0 THEN NULL ELSE 'n' || (i %% 50) || 'x' END FROM generate_series(1, 16000) i$q$, t) USING b;
		COMMIT;
	END LOOP;
END $$;
CALL f2_dict_load('dr1');
CALL f2_dict_load('dr2');
SELECT level, count(*) AS parts, sum(nrows) AS rows FROM sift._test_f2_parts('dr1_idx', 1, NULL) GROUP BY level ORDER BY level;
CREATE TEMP TABLE dr_run AS
SELECT 'codes' AS how, c.* FROM sift._test_f2_compact('dr1_idx', 1, NULL, (SELECT array_agg(part_gen)::int8[] FROM sift._test_f2_parts('dr1_idx', 1, NULL) WHERE level = 1), false, true) c;
INSERT INTO dr_run
SELECT 'strings', c.* FROM sift._test_f2_compact('dr2_idx', 1, NULL, (SELECT array_agg(part_gen)::int8[] FROM sift._test_f2_parts('dr2_idx', 1, NULL) WHERE level = 1), false, false) c;
SELECT how, done, rows_in = rows_out AS all_rows, granules_copied, dict_cols_remapped, dict_codes_remapped = (SELECT count(status) + count(name) + count(note) FROM dr1) AS every_code_remapped
FROM dr_run ORDER BY how;
SELECT (SELECT bytes_out FROM dr_run WHERE how = 'codes') = (SELECT bytes_out FROM dr_run WHERE how = 'strings') AS same_size,
	   sift._test_f2_part_digest('dr1_idx', 1, NULL) = sift._test_f2_part_digest('dr2_idx', 1, NULL) AS same_bytes;
SELECT sift._test_f2_check_keys('dr1_idx');
SELECT f2_same('SELECT id, status, name, note FROM dr1 WHERE id BETWEEN 15000 AND 25000') AS merged_range, f2_same('SELECT count(*), count(note) FROM dr1 WHERE status = ''batch2''') AS batch_string,
	   f2_same('SELECT id FROM dr1 WHERE name = ''user-1234x'' AND note IS NULL') AS merged_name, f2_same('SELECT count(*) FROM dr1 WHERE note = ''n7x''') AS note_with_nulls;

-- When the strings that are left no longer need a dictionary (the dead rows were most of the repeats) the part is written with none, as a build from them would
CREATE TABLE dr3 (id bigint, tag text);
CREATE INDEX dr3_idx ON dr3 USING sift_col (id, tag) WITH (key_order = 'id');
DO $$ BEGIN
	FOR b IN 0 .. 3 LOOP
		INSERT INTO dr3 SELECT b * 16000 + i, 'v' || ((b * 16000 + i) % 5999) || 'x' FROM generate_series(1, 16000) i;
		COMMIT;
	END LOOP;
END $$;
VACUUM dr3;
SELECT (SELECT bytes FROM sift.index_size_breakdown('dr3_idx') WHERE category = 'dicts') > 0 AS dictionaries_before;
DELETE FROM dr3 WHERE id % 10 <> 0;
VACUUM dr3;
SELECT (SELECT bytes FROM sift.index_size_breakdown('dr3_idx') WHERE category = 'dicts') AS dictionary_bytes_after, count(*) AS rows_left FROM dr3;
SELECT sift._test_f2_check_keys('dr3_idx') ~ ' exact$' AS keys_exact;
SELECT f2_same('SELECT * FROM dr3 WHERE tag = ''v123x'''), f2_same('SELECT count(*) FROM dr3 WHERE id BETWEEN 30000 AND 50000');

DROP TABLE c, d, dr1, dr2, dr3;
DROP PROCEDURE f2_batches(int, int), f2_dict_load(text);
DROP FUNCTION f2_same(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
