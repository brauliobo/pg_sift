-- sift.stats() holds what the Custom Scan (SiftAgg) scans of the session did, folded in when a scan ends from the counters EXPLAIN ANALYZE shows:
-- each figure is compared with the one of the statement that made it. sift.col_rollup_misses() lists the scans the pre-aggregates did not answer.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

-- in id order, padded to about 1800 rows a unit
CREATE TABLE ev (id int, grp int, day date, amount int, expr_key int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 7, date '2020-01-01' + i / 300, i % 100, i % 13, repeat('x', 1000) FROM generate_series(0, 59999) i;
CREATE INDEX ev_col ON ev USING sift_col (id, grp, day, amount);
VACUUM (FREEZE, ANALYZE) ev;

CREATE FUNCTION stat(n text) RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce((SELECT value FROM sift.stats() WHERE name = n), 0) $$;

-- the statement's EXPLAIN ANALYZE figures against the counters of a session that ran nothing else
CREATE FUNCTION compare(q text) RETURNS TABLE (what text, same bool) LANGUAGE plpgsql AS $$
DECLARE l text; e jsonb := '{}';
BEGIN
	PERFORM sift.stats_reset();
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '^\s*(Units|Units Pruned|Units Pruned By Set Summary|Units From Pre-aggregates|Rows Scanned|Workers Launched|Rows Vetted By Heap): ' THEN
			e := e || jsonb_build_object(trim(split_part(l, ':', 1)), trim(split_part(l, ':', 2))::int);
		END IF;
	END LOOP;
	RETURN QUERY VALUES
		('scans', stat('scans') = 1),
		('units', stat('units') = (e->>'Units')::int),
		('units_pruned', stat('units_pruned_zone') + stat('units_pruned_set') + stat('units_pruned_search') = (e->>'Units Pruned')::int),
		('units_pruned_set', stat('units_pruned_set') = coalesce((e->>'Units Pruned By Set Summary')::int, 0)),
		('units_preagg', stat('units_preagg') = coalesce((e->>'Units From Pre-aggregates')::int, 0)),
		('rows_decoded', stat('rows_decoded') = (e->>'Rows Scanned')::int),
		('workers_launched', stat('workers_launched') = coalesce((e->>'Workers Launched')::int, 0));
END $$;

-- from pre-aggregates, pruned by zones and scanned
SELECT * FROM compare('SELECT grp, count(*), sum(amount) FROM ev GROUP BY grp');
SELECT stat('units_preagg') > 0 AS from_preagg;
SELECT * FROM compare('SELECT grp, count(*) FROM ev WHERE id BETWEEN 20000 AND 30000 GROUP BY grp');
SELECT stat('units_pruned_zone') > 0 AS pruned_by_zone, stat('units_pruned_set') = 0 AS none_by_set, stat('units_pruned_search') = 0 AS none_by_search;
SELECT * FROM compare('SELECT grp, sum(amount) FROM ev WHERE amount % 3 = 0 GROUP BY grp');
SELECT stat('units_preagg') = 0 AS not_from_preagg, stat('rows_decoded') > 0 AS decoded;

-- the counters add up over statements, and reset to zero
SELECT sift.stats_reset();
SELECT count(*) FROM (SELECT grp, count(*) FROM ev GROUP BY grp) a, (SELECT grp, sum(amount) FROM ev GROUP BY grp) b WHERE a.grp = b.grp;
SELECT grp, count(*) FROM ev GROUP BY grp ORDER BY grp LIMIT 1;
SELECT stat('scans') AS scans, stat('units') >= 2 AS units;
SELECT sift.stats_reset();
SELECT count(*) FILTER (WHERE value <> 0) AS nonzero, count(*) AS counters FROM sift.stats();

-- the workers' share reaches the leader's figures
SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SELECT * FROM compare('SELECT grp, sum(amount) FROM ev WHERE amount % 3 = 0 GROUP BY grp');
SET max_parallel_workers_per_gather = 0;

-- the scans the pre-aggregates did not answer, by reason: a shape they cannot answer is not fixable by the index
SELECT sift.stats_reset();
SELECT grp % 3, count(*) FROM ev GROUP BY 1 ORDER BY 1 LIMIT 1;
SELECT grp, count(DISTINCT amount) FROM ev GROUP BY grp ORDER BY 1 LIMIT 1;
SELECT grp, count(DISTINCT amount) FROM ev GROUP BY grp ORDER BY 1 LIMIT 1;
SELECT grp, count(*) FROM ev GROUP BY grp ORDER BY 1 LIMIT 1;
SELECT reason, fixable, scans, units FROM sift.col_rollup_misses() WHERE rel = 'ev_col'::regclass ORDER BY scans DESC, reason;

-- a rollup that lacks a dim of the query, and units written before the index had the dim
SET sift.agg_rollup_cost = 0; SET sift.col_rollup_worker = off; SET cpu_tuple_cost = 0.5;
CREATE TABLE rr (id int, lines jsonb, pad text) WITH (autovacuum_enabled = false);
INSERT INTO rr
SELECT g, (SELECT jsonb_agg(jsonb_build_object('a', 'a' || (g + e) % 5, 'b', 'b' || (g * e) % 3, 'q', (g * e) % 9) ORDER BY e) FROM generate_series(1, g % 4 + 1) e), repeat('p', 300)
FROM generate_series(1, 12000) g;
CREATE INDEX rr_i ON rr USING sift_col (id, lines) WITH (paths = 'lines[]{a text, b text, q int4}', rollup = 'lines[].a');
VACUUM (FREEZE, ANALYZE) rr;
SELECT sift.stats_reset();
SELECT e.a, count(*) FROM rr d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, q int4) GROUP BY 1 ORDER BY 1 LIMIT 1;
SELECT e.a, e.b, count(*) FROM rr d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, q int4) GROUP BY 1, 2 ORDER BY 1, 2 LIMIT 1;
SELECT reason, fixable, scans, units FROM sift.col_rollup_misses() WHERE rel = 'rr_i'::regclass;
ALTER INDEX rr_i SET (rollup = 'lines[].a, lines[].b');
SELECT sift.stats_reset();
SELECT e.a, e.b, count(*) FROM rr d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, q int4) GROUP BY 1, 2 ORDER BY 1, 2 LIMIT 1;
SELECT reason, fixable, scans, units > 0 AS units FROM sift.col_rollup_misses() WHERE rel = 'rr_i'::regclass;
SELECT stat('units_no_rollup') > 0 AS units_no_rollup;
RESET sift.agg_rollup_cost; RESET sift.col_rollup_worker; RESET cpu_tuple_cost;

-- the lookup tables a later query takes from the cache
CREATE TABLE dims (id int, country int, label text) WITH (autovacuum_enabled = false);
INSERT INTO dims SELECT i, i % 5, 'd' || i FROM generate_series(0, 6) i;
VACUUM (FREEZE, ANALYZE) dims;
SELECT sift.stats_reset();
SELECT d.country, count(*) FROM ev JOIN dims d ON d.id = ev.grp GROUP BY 1 ORDER BY 1 LIMIT 1;
SELECT d.country, count(*) FROM ev JOIN dims d ON d.id = ev.grp GROUP BY 1 ORDER BY 1 LIMIT 1;
SELECT stat('lookup_cache_misses') AS lookup_misses, stat('lookup_cache_hits') AS lookup_hits;

-- a query that kept the normal plan is counted by the slug of its refusal while sift.explain_refusals is on
SELECT sift.stats_reset();
SET client_min_messages = warning;
SELECT d.country, count(*) FROM dims d GROUP BY 1 ORDER BY 1 LIMIT 1;
SELECT name, value FROM sift.stats() WHERE name LIKE 'refused.%' ORDER BY name;
SET sift.explain_refusals = on;
SELECT d.country, count(*) FROM dims d GROUP BY 1 ORDER BY 1 LIMIT 1;
SELECT name, value FROM sift.stats() WHERE name LIKE 'refused.%' ORDER BY name;
RESET sift.explain_refusals; RESET client_min_messages;

-- a statement that fails folds nothing in
SELECT sift.stats_reset();
SELECT grp, sum(amount / (amount - amount)) FROM ev GROUP BY grp;
SELECT stat('scans') AS scans_after_error;

DROP TABLE ev, rr, dims;
DROP FUNCTION stat(text), compare(text);
DROP EXTENSION pg_sift;
