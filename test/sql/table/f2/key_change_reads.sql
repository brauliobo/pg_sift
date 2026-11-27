-- Format 2.0 sift_table: the key of a table changes while it has parts (an ALTER TABLE that rewrites it, a primary key added or dropped, key_order set, changed or reset). The parts keep the key
-- they were written in, and the reads prune by the key only the parts that are in it: every table answers as its heap twin, through the plain scan and SiftAgg, after the change, after
-- writes under the new key, deletes, a merge and VACUUM FULL. An ALTER TYPE of the column of a TTL keeps it a date or a timestamp.
CREATE EXTENSION pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;

CREATE TABLE kc_twin (id int8 NOT NULL, k int8, ts timestamptz, v int8);
CREATE TABLE kc_cases (n int GENERATED ALWAYS AS IDENTITY, tbl text, before text, change text);
INSERT INTO kc_cases (tbl, before, change) VALUES
	('kc_alter_type', 'ALTER TABLE %I ADD PRIMARY KEY (id)', 'ALTER TABLE %I ALTER COLUMN v TYPE numeric'),
	('kc_add_pk', 'SELECT 1', 'ALTER TABLE %I ADD PRIMARY KEY (id)'),
	('kc_drop_pk', 'ALTER TABLE %I ADD CONSTRAINT %1$s_pk PRIMARY KEY (id)', 'ALTER TABLE %I DROP CONSTRAINT %1$s_pk'),
	('kc_key_order', $$SELECT sift.table_set_option(%L, 'key_order', 'id')$$, $$SELECT sift.table_set_option(%L, 'key_order', 'k')$$),
	('kc_key_desc', $$SELECT sift.table_set_option(%L, 'key_order', 'k')$$, $$SELECT sift.table_set_option(%L, 'key_order', 'ts desc, k')$$),
	('kc_key_reset', $$SELECT sift.table_set_option(%L, 'key_order', 'k')$$, $$SELECT sift.table_reset_option(%L, 'key_order')$$);

CREATE PROCEDURE kc_write(lo int, hi int) LANGUAGE plpgsql AS $$
DECLARE t text;
BEGIN
	INSERT INTO kc_twin SELECT g, 40000 - g, timestamptz '2024-01-01' - g * interval '1 minute', g FROM generate_series(lo, hi) g;
	FOR t IN SELECT tbl FROM kc_cases LOOP
		EXECUTE format('INSERT INTO %I SELECT * FROM kc_twin WHERE id BETWEEN %s AND %s', t, lo, hi);
	END LOOP;
END $$;

-- the predicates whose count differs from the heap twin's, NULL when none does
CREATE FUNCTION kc_wrong(t text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	q    text;
	bad  text := '';
	a    int8;
	b    int8;
BEGIN
	FOREACH q IN ARRAY ARRAY['id BETWEEN 100 AND 199', 'k BETWEEN 30100 AND 30199', 'k IN (30500, 30501, 39999)', 'id > 29990',
		$q$ts BETWEEN '2023-12-31 20:00' AND '2023-12-31 21:00'$q$, $q$ts < '2023-12-30'$q$, 'v % 7 = 3'] LOOP
		EXECUTE format('SELECT count(*) FROM %I WHERE %s', t, q) INTO a;
		EXECUTE format('SELECT count(*) FROM kc_twin WHERE %s', q) INTO b;
		IF a <> b THEN
			bad := bad || format('%s: %s, the heap %s; ', q, a, b);
		END IF;
	END LOOP;
	RETURN nullif(bad, '');
END $$;

CREATE FUNCTION kc_check(at text) RETURNS TABLE (step text, tbl text, agg text, plain text) LANGUAGE plpgsql AS $$
BEGIN
	FOR tbl IN SELECT c.tbl FROM kc_cases c ORDER BY c.n LOOP
		PERFORM set_config('sift.enable_agg', 'on', true);
		agg := kc_wrong(tbl);
		PERFORM set_config('sift.enable_agg', 'off', true);
		plain := kc_wrong(tbl);
		step := at;
		RETURN NEXT;
	END LOOP;
END $$;

SELECT format('CREATE TABLE %I (id int8 NOT NULL, k int8, ts timestamptz, v int8) USING sift_table', tbl) FROM kc_cases \gexec
SELECT format(before, tbl) FROM kc_cases \gexec
CALL kc_write(1, 20000);
SELECT format('VACUUM %I', tbl) FROM kc_cases \gexec
SELECT format(change, tbl) FROM kc_cases \gexec
SELECT * FROM kc_check('after the change');
CALL kc_write(20001, 30000);
SELECT * FROM kc_check('after writes in the new key');
DELETE FROM kc_twin WHERE id % 5 = 0;
SELECT format('DELETE FROM %I WHERE id %% 5 = 0', tbl) FROM kc_cases \gexec
SELECT * FROM kc_check('after deletes');
SELECT format('SELECT sift.merge(%L, true)', tbl) FROM kc_cases \gexec
SELECT * FROM kc_check('after a merge');
SELECT format('VACUUM FULL %I', tbl) FROM kc_cases \gexec
SELECT * FROM kc_check('after VACUUM FULL');

-- the column of a TTL stays a date or a timestamp
CREATE TABLE kc_ttl (id int, at timestamptz NOT NULL) USING sift_table;
SELECT sift.set_ttl('kc_ttl', 'at', interval '7 days');
ALTER TABLE kc_ttl ALTER COLUMN at TYPE text;
ALTER TABLE kc_ttl ALTER COLUMN at TYPE timestamp;
SELECT atttypid::regtype FROM pg_attribute WHERE attrelid = 'kc_ttl'::regclass AND attname = 'at';

DROP TABLE kc_ttl;
SELECT format('DROP TABLE %I', tbl) FROM kc_cases \gexec
DROP TABLE kc_twin, kc_cases;
DROP FUNCTION kc_check, kc_wrong;
DROP PROCEDURE kc_write;
DROP EXTENSION pg_sift;
