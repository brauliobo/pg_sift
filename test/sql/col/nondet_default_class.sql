-- A text or varchar column under a nondeterministic collation takes sift.col_text_ops, the default class of its type, at CREATE INDEX: no class is named, as for any other
-- column. The index answers equality, ranges, lists, order and groups as the heap does (the collation decides equality, not the bytes); only a UNIQUE key refuses the column.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
\set VERBOSITY terse
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
CREATE COLLATION nd_ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);

CREATE TABLE nd (id int, name text COLLATE nd_ci, grp varchar(8) COLLATE nd_ci, v int);
INSERT INTO nd SELECT g, CASE WHEN g % 25 = 0 THEN NULL ELSE (ARRAY['alpha', 'Alpha', 'ALPHA', 'beta', 'Beta', 'gamma'])[1 + g % 6] || g % 10 END,
       'g' || g % 4, g % 100 FROM generate_series(1, 3000) g;
CREATE INDEX nd_multi ON nd USING sift_col (id, name, grp, v);
CREATE INDEX nd_name ON nd USING sift_col (name);
CREATE INDEX nd_grp ON nd USING sift_col (grp) INCLUDE (v);
ANALYZE nd;

SELECT i.indexrelid::regclass AS index, (SELECT string_agg(o.opcname, ' ' ORDER BY a.ord) FROM unnest(i.indclass) WITH ORDINALITY a(oid, ord) JOIN pg_opclass o ON o.oid = a.oid) AS classes
FROM pg_index i WHERE i.indrelid = 'nd'::regclass ORDER BY 1;

CREATE UNIQUE INDEX nd_unique ON nd USING sift_col (name);
CREATE UNIQUE INDEX nd_unique_id ON nd USING sift_col (id) INCLUDE (name);

-- the rows of every shape are those of the heap, through the index and through the heap
CREATE FUNCTION nd_rows(q text, via_index bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('enable_seqscan', (NOT via_index)::text, true);
	PERFORM set_config('enable_indexscan', via_index::text, true);
	PERFORM set_config('enable_bitmapscan', via_index::text, true);
	PERFORM set_config('enable_indexonlyscan', via_index::text, true);
	EXECUTE format($q$SELECT coalesce(string_agg(to_jsonb(x)::text, ',' ORDER BY to_jsonb(x)::text), '') FROM (%s) x$q$, q) INTO r;
	RETURN r;
END $$;

SELECT n, shape, nd_rows(shape, true) = nd_rows(shape, false) AS same
FROM unnest(ARRAY[
	'SELECT id FROM nd WHERE name = ''alpha3''', 'SELECT id FROM nd WHERE name = ''ALPHA3''', 'SELECT id FROM nd WHERE name > ''beta5''', 'SELECT id FROM nd WHERE name <= ''Alpha2''',
	'SELECT id FROM nd WHERE name IN (''alpha1'', ''BETA2'', ''x'')', 'SELECT id FROM nd WHERE name IS NULL', 'SELECT id FROM nd WHERE grp = ''G1''', 'SELECT id FROM nd WHERE grp >= ''g2''',
	'SELECT name, id FROM nd ORDER BY name, id LIMIT 50', 'SELECT name, count(*), sum(v) FROM nd GROUP BY name', 'SELECT grp, count(*), min(v), max(v) FROM nd WHERE name = ''beta4'' GROUP BY grp'
]) WITH ORDINALITY s(shape, n) ORDER BY n;

REINDEX INDEX nd_multi;
SELECT nd_rows('SELECT id FROM nd WHERE name = ''alpha3''', true) = nd_rows('SELECT id FROM nd WHERE name = ''alpha3''', false) AS same_after_reindex;

DROP TABLE nd;
DROP FUNCTION nd_rows(text, bool);
DROP COLLATION nd_ci;
DROP EXTENSION pg_sift;
