-- Element cubes of a unit (docs/COL-PREAGG.md): the histograms of the leaves of a list (an array of objects, an array of
-- scalars, a map) whose rows are the elements, checked against direct SQL over jsonb_array_elements / jsonb_each after a build, DML, VACUUM,
-- promote, merge and a parallel build, on minor 4, minor 3 and mixed indexes. Only units that are the single unit of their superblock and exact
-- (clean, every page all-visible) are compared: the rule under which a consumer may use them.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;

CREATE FUNCTION pa_sb(t tid) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT ((t::text::point)[0])::int / 256 $$;
CREATE FUNCTION fmt(idx regclass) RETURNS text LANGUAGE sql AS
$$ SELECT string_agg(format::text, ',' ORDER BY gen) FROM sift._test_col_formats(idx) $$;

-- the items of document g: absent, null and empty lists, objects with missing and null members, scalar elements, and in fixed id ranges what a
-- histogram cannot hold: a string in an int path (100..110), a number in a text path (5000..5010), more than 256 distinct kinds in a unit
-- (9000..9400), a list that is not an array (14000..14005), an array inside the list (20000..20003)
CREATE FUNCTION mk_items(g int) RETURNS jsonb LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE r jsonb := '[]'; o jsonb; e int;
BEGIN
	IF g BETWEEN 14000 AND 14002 THEN RETURN '{"k": "o"}'; END IF;
	IF g BETWEEN 14003 AND 14005 THEN RETURN '5'; END IF;
	IF g % 17 = 0 THEN RETURN NULL; END IF;
	IF g % 17 = 1 THEN RETURN 'null'; END IF;
	IF g % 17 = 2 THEN RETURN '[]'; END IF;
	FOR e IN 1 .. g % 5 + 1 LOOP
		o := jsonb_build_object('t', 't' || (g + e) % 4, 'x', ((g * e) % 40) * 0.25, 'n', (g + e) % 9 - 4, 'w', (g + e) % 300 * 1000000000::bigint);
		IF (g + e) % 11 <> 0 THEN o := o || jsonb_build_object('k', 'k' || (g * e) % 12); END IF;
		IF (g + e) % 13 = 0 THEN o := o || '{"k": null, "x": null}'; END IF;
		IF (g + e) % 7 = 0 THEN o := o - 'n'; END IF;
		IF g BETWEEN 9000 AND 9400 THEN o := o || jsonb_build_object('k', 'u' || g || '_' || e); END IF;
		r := r || o;
	END LOOP;
	IF g % 29 = 3 THEN r := r || (ARRAY['7', '"str"', 'null', 'true'])[1 + g / 29 % 4]::jsonb; END IF;
	IF g BETWEEN 100 AND 110 THEN r := r || '{"k": "a", "n": "abc"}'; END IF;
	IF g BETWEEN 5000 AND 5010 THEN r := r || '{"k": 5, "n": 1}'; END IF;
	IF g BETWEEN 20000 AND 20003 THEN r := r || '[[1]]'; END IF;
	RETURN r;
END $$;
CREATE FUNCTION mk_tags(g int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN g % 19 = 0 THEN NULL ELSE coalesce((SELECT jsonb_agg(CASE WHEN (g + e) % 8 = 0 THEN NULL ELSE 'g' || (g + e) % 7 END)
	       FROM generate_series(1, g % 4) e), '[]') END $$;
CREATE FUNCTION mk_map(g int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN g % 23 = 0 THEN NULL ELSE coalesce((SELECT jsonb_object_agg('m' || (g + e) % 6, CASE WHEN (g + e) % 9 = 0 THEN NULL ELSE (g * e) % 50 END)
	       FROM generate_series(1, g % 5) e), '{}') END $$;

-- units checked and mismatches of the histogram of a list leaf (dim: leaf name, dimx: the SQL expression of its key as text over e) and one measure
-- (NULL: the counts only; measx its SQL expression, fl: float measure); src is the FROM item that makes e, one row per element
CREATE FUNCTION pe_hist(idx regclass, tbl regclass, src text, dim text, dimx text, meas text, measx text, fl bool, OUT checked bool, OUT bad bigint, OUT stored bool)
LANGUAGE plpgsql AS $f$
DECLARE r record;
BEGIN
	EXECUTE format($q$
		WITH s AS (SELECT * FROM sift.col_unit_hist(%1$L, %2$L, %3$L)),
		     one AS (SELECT sb FROM sift.col_unit_agg(%1$L, 'id') WHERE sb IN (SELECT sb FROM s) GROUP BY sb HAVING count(*) = 1 AND bool_and(exact)),
		     h AS (SELECT pa_sb(d.ctid) AS sb, (%5$s) AS key, count(*) AS n, count(%6$s) AS nn, sum(%6$s)::numeric AS s, sum((%6$s)::float8) AS sf
		           FROM %4$s d CROSS JOIN LATERAL %8$s GROUP BY 1, 2)
		SELECT (SELECT count(*) FROM one) > 0 AS c, count(*) FILTER (WHERE s.n IS DISTINCT FROM h.n OR (s.nn IS NOT NULL AND
		       (s.nn IS DISTINCT FROM h.nn OR (s.nn > 0 AND %7$s)))) AS b,
		       (SELECT count(*) FROM s) > 0 AS st
		FROM (SELECT * FROM s WHERE sb IN (SELECT sb FROM one)) s
		FULL JOIN (SELECT * FROM h WHERE sb IN (SELECT sb FROM one)) h ON s.sb = h.sb AND s.key IS NOT DISTINCT FROM h.key
	$q$, idx, dim, meas, tbl, dimx, coalesce(measx, '1'),
		CASE WHEN fl THEN 's.sum_float IS DISTINCT FROM h.sf' ELSE 's.sum IS DISTINCT FROM h.s' END, src) INTO r;
	checked := r.c;
	bad := r.b;
	stored := r.st;
END $f$;

CREATE TABLE pe (id int, grp int, items jsonb, tags jsonb, m jsonb, pad text);
INSERT INTO pe SELECT g, g % 7, mk_items(g), mk_tags(g), mk_map(g), repeat('x', 400) FROM generate_series(1, 40000) g;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) pe;

CREATE FUNCTION pe_battery(idx regclass) RETURNS TABLE(what text, checked bool, bad bigint) LANGUAGE sql AS $$
	WITH it AS (SELECT $s$jsonb_array_elements(CASE WHEN jsonb_typeof(d.items) = 'array' THEN d.items ELSE '[]' END) e$s$ AS src),
	     ni AS (SELECT $x$CASE WHEN jsonb_typeof(e->'n') = 'number' THEN (e->>'n')::int END$x$ AS n,
	                   $x$CASE WHEN jsonb_typeof(e->'x') = 'number' THEN (e->>'x')::float8 END$x$ AS x,
	                   $x$CASE WHEN jsonb_typeof(e->'w') = 'number' THEN (e->>'w')::int8 END$x$ AS w)
	SELECT 'k/x', c, b FROM it, ni, pe_hist(idx, 'pe', it.src, 'items[].k', 'e->>''k''', 'items[].x', ni.x, true) h(c, b, st)
	UNION ALL SELECT 'k/n', c, b FROM it, ni, pe_hist(idx, 'pe', it.src, 'items[].k', 'e->>''k''', 'items[].n', ni.n, false) h(c, b, st)
	UNION ALL SELECT 'k/w', c, b FROM it, ni, pe_hist(idx, 'pe', it.src, 'items[].k', 'e->>''k''', 'items[].w', ni.w, false) h(c, b, st)
	UNION ALL SELECT 'k', c, b FROM it, pe_hist(idx, 'pe', it.src, 'items[].k', 'e->>''k''', NULL, NULL, false) h(c, b, st)
	UNION ALL SELECT 't/x', c, b FROM it, ni, pe_hist(idx, 'pe', it.src, 'items[].t', 'e->>''t''', 'items[].x', ni.x, true) h(c, b, st)
	UNION ALL SELECT 'n/x', c, b FROM it, ni, pe_hist(idx, 'pe', it.src, 'items[].n', ni.n || '::text', 'items[].x', ni.x, true) h(c, b, st)
	UNION ALL SELECT 'tags', c, b FROM pe_hist(idx, 'pe', 'jsonb_array_elements_text(CASE WHEN jsonb_typeof(d.tags) = ''array'' THEN d.tags ELSE ''[]'' END) e',
	                                            'tags[]', 'e', NULL, NULL, false) h(c, b, st)
	UNION ALL SELECT 'map', c, b FROM pe_hist(idx, 'pe', 'jsonb_each(CASE WHEN jsonb_typeof(d.m) = ''object'' THEN d.m ELSE ''{}'' END) e(key, value)',
	                                           'm.*#key', 'e.key', 'm.*', 'CASE WHEN jsonb_typeof(e.value) = ''number'' THEN (e.value)::text::int END', false) h(c, b, st)
$$;

-- explicit cubes (no byte budget): every histogram the battery reads is in every unit that can hold it
CREATE INDEX pe_col ON pe USING sift_col (id, grp, items, tags, m)
  WITH (paths = 'items[]{k text, n int4, x float8, t text, w int8}, tags[] text, m.* int4',
        preagg = 'cube(grp: id) rollup(items[].k, items[].t, items[].n: items[].x, items[].n, items[].w; tags[]; m.*#key: m.*)');
SELECT fmt('pe_col');
SELECT sift.col_verify('pe_col', true);
SELECT what, checked, bad FROM pe_battery('pe_col');

-- what a histogram cannot hold has none in the units that hold it (the other leaves of the list keep theirs); the rows that hold it are those
-- of cond (a document that is small lands in any unit, so the ids alone do not tell the units)
CREATE FUNCTION units_with(idx regclass, dim text, cond text) RETURNS bigint LANGUAGE plpgsql AS $f$
DECLARE r bigint;
BEGIN
	EXECUTE format('SELECT count(DISTINCT sb) FROM sift.col_unit_hist(%L, %L) WHERE sb IN (SELECT pa_sb(ctid) FROM pe WHERE %s)', idx, dim, cond) INTO r;
	RETURN r;
END $f$;
SELECT units_with('pe_col', 'items[].k', $$items @> '[{"k": 5}]'$$) AS k_number_in_text,
       units_with('pe_col', 'items[].t', $$items @> '[{"k": 5}]'$$) > 0 AS t_still,
       units_with('pe_col', 'items[].n', $$items @> '[{"n": "abc"}]'$$) AS n_string_in_int,
       units_with('pe_col', 'items[].k', $$items @> '[{"n": "abc"}]'$$) > 0 AS k_still,
       units_with('pe_col', 'items[].k', 'id BETWEEN 14000 AND 14005') AS k_list_overflow,
       units_with('pe_col', 'items[].t', 'id BETWEEN 14000 AND 14005') AS t_list_overflow,
       units_with('pe_col', 'items[].k', $$id BETWEEN 20000 AND 20003 AND jsonb_typeof(items) = 'array'$$) AS k_nested_array;
-- more tuples in a unit than the cap (a 32nd of the elements, at least 64; an explicit rollup allows 4096): a rollup drops the dimension of the most distinct values, the other leaves of the list keep theirs
CREATE TABLE pc (id int, items jsonb, pad text);
INSERT INTO pc SELECT g, jsonb_build_array(jsonb_build_object('k', CASE WHEN g <= 3000 THEN 'u' || lpad(g::text, 5, '0') ELSE 'k' || g % 5 END, 't', 't' || g % 3)),
       repeat('x', 300) FROM generate_series(1, 12000) g;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) pc;
CREATE INDEX pc_col ON pc USING sift_col (id, items) WITH (paths = 'items[]{k text, t text}');
CREATE INDEX pc_cube ON pc USING sift_col (id, items) WITH (paths = 'items[]{k text, t text}', preagg = 'rollup(items[].k)');
SELECT (SELECT count(DISTINCT sb) FROM sift.col_unit_agg('pc_col', 'id')) AS units,
       (SELECT count(DISTINCT sb) FROM sift.col_unit_hist('pc_col', 'items[].k')) AS units_with_k,
       (SELECT count(DISTINCT sb) FROM sift.col_unit_hist('pc_col', 'items[].t')) AS units_with_t,
       (SELECT count(DISTINCT sb) FROM sift.col_unit_hist('pc_cube', 'items[].k')) AS units_with_k_explicit;
SELECT pc_col_first_unit_has_k FROM (SELECT min(sb) = 0 AND EXISTS (SELECT 1 FROM sift.col_unit_hist('pc_col', 'items[].k') WHERE sb = 0) AS pc_col_first_unit_has_k
                                     FROM sift.col_unit_agg('pc_col', 'id')) x;
DROP TABLE pc;

-- a measure that overflows in a unit is not stored there, the histogram of the dimension is
SELECT count(*) FILTER (WHERE nn IS NULL) > 0 AS no_n_measure_in_some_unit, count(*) FILTER (WHERE nn IS NOT NULL) > 0 AS n_measure_elsewhere
FROM sift.col_unit_hist('pe_col', 'items[].k', 'items[].n');
-- the rows of a histogram are the elements: the groups of a unit add up to its elements
SELECT count(*) AS units, count(*) FILTER (WHERE e.elems = h.n) AS equal
FROM (SELECT sb, sum(n) AS n FROM sift.col_unit_hist('pe_col', 'items[].t') GROUP BY sb) h
JOIN (SELECT pa_sb(d.ctid) AS sb, count(e) AS elems FROM pe d CROSS JOIN LATERAL
        jsonb_array_elements(CASE WHEN jsonb_typeof(d.items) = 'array' THEN d.items ELSE '[]' END) e GROUP BY 1) e USING (sb);
SELECT preagg_bytes > 0 AS bytes, preagg_units > 0 AS units FROM sift.col_index_info('pe_col');
SELECT leaf, section FROM sift.col_sections('pe_col') WHERE section = 'preagg';

-- the leaf names of the hook: an element leaf shares its path with its list when the list holds scalars or is a map
SELECT count(*) > 0 AS tags, (SELECT count(*) > 0 FROM sift.col_unit_hist('pe_col', 'm.*#key', 'm.*')) AS map FROM sift.col_unit_hist('pe_col', 'tags[]');
SELECT count(*) FROM sift.col_unit_hist('pe_col', 'items[]');

-- DML: inserts (write buffer), updates and deletes make the touched units inexact, never wrong
INSERT INTO pe SELECT g, g % 7, mk_items(g), mk_tags(g), mk_map(g), repeat('x', 400) FROM generate_series(40001, 42000) g;
UPDATE pe SET items = mk_items(id + 3) WHERE id % 97 = 0 AND id < 20000;
DELETE FROM pe WHERE id % 89 = 0 AND id < 20000;
SELECT what, checked, bad FROM pe_battery('pe_col');
SELECT count(*) FILTER (WHERE NOT exact) > 0 AS some_inexact, count(*) FILTER (WHERE exact) > 0 AS some_exact FROM sift.col_unit_agg('pe_col', 'id');
-- VACUUM makes the pages all-visible again but the units still hold the dead rows: still inexact where dirty
VACUUM (DISABLE_PAGE_SKIPPING) pe;
SELECT what, checked, bad FROM pe_battery('pe_col');
SELECT count(*) FILTER (WHERE NOT exact) > 0 AS dirty_inexact FROM sift.col_unit_agg('pe_col', 'id');
-- promotion and a merge rewrite the units: the dead rows are gone and the new units carry new element cubes
SELECT sift.col_promote('pe_col') >= 0 AS promoted;
SELECT sift.col_merge('pe_col', true) >= 0 AS merged;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) pe;
SELECT sift.col_verify('pe_col', true);
SELECT what, checked, bad FROM pe_battery('pe_col');
SELECT count(*) FILTER (WHERE NOT exact) AS inexact FROM sift.col_unit_agg('pe_col', 'id');
SELECT fmt('pe_col');

-- parallel build and REINDEX
SET max_parallel_maintenance_workers = 3;
SET min_parallel_table_scan_size = 0;
SET maintenance_work_mem = '64MB';
REINDEX INDEX pe_col;
SELECT segments > 1 AS workers_wrote_segments FROM sift.col_index_info('pe_col');
SELECT sift.col_verify('pe_col', true);
SELECT what, checked, bad FROM pe_battery('pe_col');
RESET max_parallel_maintenance_workers;
RESET min_parallel_table_scan_size;
RESET maintenance_work_mem;

-- minor 3 beside minor 4 (docs/COL.md section 4): a segment written under minor 3 has no element cubes and the plain cube of 3.0, the merge of both
-- writes the setting's minor
SET sift.col_format_minor = 3;
INSERT INTO pe SELECT g, g % 7, mk_items(g), mk_tags(g), mk_map(g), repeat('x', 400) FROM generate_series(42001, 46000) g;
SELECT sift.col_promote('pe_col') >= 0 AS promoted;
RESET sift.col_format_minor;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) pe;
SELECT fmt('pe_col');
SELECT sift.col_verify('pe_col', true);
SELECT what, checked, bad FROM pe_battery('pe_col');
SELECT (SELECT count(*) FROM sift.col_unit_hist('pe_col', 'items[].t') h WHERE h.segment IN (SELECT gen FROM sift._test_col_formats('pe_col') WHERE format = 3)) AS minor3_elem_groups,
       (SELECT count(*) FROM sift.col_unit_hist('pe_col', 'grp') h WHERE h.segment IN (SELECT gen FROM sift._test_col_formats('pe_col') WHERE format = 3)) > 0 AS minor3_top_level_cube,
       (SELECT count(*) FROM sift.col_unit_hist('pe_col', 'items[].t') h WHERE h.segment IN (SELECT gen FROM sift._test_col_formats('pe_col') WHERE format = 4)) > 0 AS minor4_elem_groups;
SELECT sift.col_merge('pe_col', true) >= 0 AS merged;
SELECT fmt('pe_col');
SELECT sift.col_verify('pe_col', true);
SELECT what, checked, bad FROM pe_battery('pe_col');

-- an index built under minor 3: no element cubes (the plain cube of 3.0 stays), and REINDEX under minor 4 gives them
SET sift.col_format_minor = 3;
CREATE INDEX pe_col3 ON pe USING sift_col (id, grp, items, tags, m)
  WITH (paths = 'items[]{k text, n int4, x float8, t text, w int8}, tags[] text, m.* int4',
        preagg = 'cube(grp: id) rollup(items[].k, items[].t, items[].n: items[].x, items[].n, items[].w; tags[]; m.*#key: m.*)');
RESET sift.col_format_minor;
SELECT fmt('pe_col3');
SELECT sift.col_verify('pe_col3', true);
SELECT (SELECT count(*) FROM sift.col_unit_hist('pe_col3', 'items[].k')) AS minor3_elem_groups,
       (SELECT count(*) FROM sift.col_unit_hist('pe_col3', 'grp')) > 0 AS top_level_cube,
       (SELECT preagg_bytes FROM sift.col_index_info('pe_col3')) < (SELECT preagg_bytes FROM sift.col_index_info('pe_col')) AS smaller;
SELECT (SELECT array_agg((sb, key, n, nn, sum) ORDER BY sb, key) FROM sift.col_unit_hist('pe_col3', 'grp', 'id')) =
       (SELECT array_agg((sb, key, n, nn, sum) ORDER BY sb, key) FROM sift.col_unit_hist('pe_col', 'grp', 'id')) AS same_top_level_cube;
REINDEX INDEX pe_col3;
SELECT fmt('pe_col3');
SELECT (SELECT count(*) FROM sift.col_unit_hist('pe_col3', 'items[].k')) > 0 AS reindexed_elem_groups;
SELECT sift.col_verify('pe_col3', true);
DROP INDEX pe_col3;

-- the default: the cube and the rollups of a unit share the byte budget (2% of the unit, at least 2 kB), the cube first, a rollup drops its dimension of the most distinct values until it fits, and
-- the same bytes come out every time
SET max_parallel_maintenance_workers = 0;
CREATE INDEX pe_def ON pe USING sift_col (id, grp, items, tags, m) WITH (paths = 'items[]{k text, n int4, x float8, t text, w int8}, tags[] text, m.* int4');
SELECT sift.col_verify('pe_def', true);
SELECT what, checked, bad FROM pe_battery('pe_def');
SELECT preagg_bytes AS def_bytes FROM sift.col_index_info('pe_def') \gset
REINDEX INDEX pe_def;
SELECT preagg_bytes = :def_bytes AS deterministic, preagg_bytes < (SELECT preagg_bytes FROM sift.col_index_info('pe_col')) AS smaller_than_explicit
FROM sift.col_index_info('pe_def');
SELECT count(*) FILTER (WHERE d.sb IS NULL) AS units_without_t, count(*) FILTER (WHERE d.sb IS NOT NULL) > 0 AS units_with_t
FROM sift.col_unit_agg('pe_def', 'id') a LEFT JOIN (SELECT DISTINCT sb FROM sift.col_unit_hist('pe_def', 'items[].t')) d USING (sb);
-- off: no histogram anywhere, the exact sums stay
ALTER INDEX pe_def SET (preagg = 'off');
REINDEX INDEX pe_def;
SELECT (SELECT count(*) FROM sift.col_unit_hist('pe_def', 'items[].t')) AS elem_rows, (SELECT count(*) FROM sift.col_unit_hist('pe_def', 'grp')) AS top_rows,
       preagg_bytes > 0 AS has_sums FROM sift.col_index_info('pe_def');
SELECT sift.col_verify('pe_def', true);
DROP INDEX pe_def;
RESET max_parallel_maintenance_workers;

-- options: the names of the lists' leaves, a measure of another list, a list inside a list
CREATE INDEX pe_bad1 ON pe USING sift_col (id, items, tags) WITH (paths = 'items[]{k text, x float8}, tags[] text', preagg = 'rollup(items[].k: tags[])');
CREATE INDEX pe_bad2 ON pe USING sift_col (id, items) WITH (paths = 'items[]{k text, x float8}', preagg = 'cube(id: items[].x)');
CREATE INDEX pe_bad3 ON pe USING sift_col (id, items) WITH (paths = 'items[]{k text, x float8}', preagg = 'rollup(items[].x)');
CREATE INDEX pe_bad4 ON pe USING sift_col (id, items) WITH (paths = 'items[]{k text, x float8}', preagg = 'rollup(items[].nosuch)');
CREATE INDEX pe_bad5 ON pe USING sift_col (id, items) WITH (paths = 'items[]{k text, x float8}', preagg = 'rollup(items[]: items[].x)');
CREATE INDEX pe_bad6 ON pe USING sift_col (id, items) WITH (paths = 'items[]{k text, x float8}', preagg = 'rollup(items[].k: ');
-- a list inside a list is not an index (the cubes are depth 1 because the index is)
CREATE INDEX pe_nest ON pe USING sift_col (id, items) WITH (paths = 'items[]{k text, sub[]{z int4}}');

DROP TABLE pe;
DROP FUNCTION pe_hist(regclass, regclass, text, text, text, text, text, bool), pe_battery(regclass), units_with(regclass, text, text),
              mk_items(int), mk_tags(int), mk_map(int), pa_sb(tid), fmt(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
