-- sift.stats(local => true) holds what the Custom Scan (SiftAgg) scans of the session did, folded in when a scan ends from the counters EXPLAIN ANALYZE shows (its Granules):
-- each figure is compared with the one of the statement that made it, and the rows decoded with whether the statement scanned a granule.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

-- in id order, padded to about 1800 rows a unit; grp is connected to the dimension dims
CREATE TABLE dims (id int PRIMARY KEY, country int, label text) WITH (autovacuum_enabled = false);
INSERT INTO dims SELECT i, i % 5, 'd' || i FROM generate_series(0, 6) i;
CREATE TABLE ev (id int, grp int, day date, amount int, expr_key int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 7, date '2020-01-01' + i / 300, i % 100, i % 13, repeat('x', 1000) FROM generate_series(0, 59999) i;
SET client_min_messages = warning;
CREATE INDEX dims_col ON dims USING sift_col (id, country);
CREATE INDEX ev_col ON ev USING sift_col (id, grp, day, amount) WITH (rollup_columns = 'off', connect = 'grp -> dims(id)');
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) ev, dims;

CREATE FUNCTION stat(n text) RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce((SELECT value FROM sift.stats(local => true) WHERE name = n), 0) $$;

-- the statement's EXPLAIN ANALYZE figures against the counters of a session that ran nothing else
CREATE FUNCTION compare(q text) RETURNS TABLE (what text, same bool) LANGUAGE plpgsql AS $$
DECLARE p jsonb; g jsonb;
BEGIN
	PERFORM sift.stats_reset();
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	g := jsonb_path_query_first(p, 'strict $.** ? (exists(@."Granules"))')->'Granules';
	RETURN QUERY VALUES
		('scans', stat('scans') = 1),
		('units', stat('units') = (g->>'Scanned')::int),
		('units_pruned', stat('units_pruned_zone') + stat('units_pruned_set') + stat('units_pruned_search') = (g->>'Pruned By Zone Map')::int + (g->>'Pruned By Skip Index')::int),
		('units_pruned_set', stat('units_pruned_set') = (g->>'Pruned By Skip Index')::int),
		('units_preagg', stat('units_preagg') = (g->>'Answered By Pre-aggregate')::int),
		('rows_decoded', (stat('rows_decoded') > 0) = ((g->>'Scanned')::int > 0)),
		('workers_launched', stat('workers_launched') = coalesce((jsonb_path_query_first(p, 'strict $.** ? (exists(@."Workers Launched"))')->>'Workers Launched')::int, 0));
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
SELECT count(*) FILTER (WHERE value <> 0) AS nonzero, count(*) AS counters FROM sift.stats(local => true);

-- the workers' share reaches the leader's figures
SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SELECT * FROM compare('SELECT grp, sum(amount) FROM ev WHERE amount % 3 = 0 GROUP BY grp');
SET max_parallel_workers_per_gather = 0;

-- the lookup tables a later query takes from the cache: the dimension of the connection of ev_col, which keeps a key journal
SELECT sift.stats_reset();
SELECT d.country, count(*) FROM ev JOIN dims d ON d.id = ev.grp GROUP BY 1 ORDER BY 1 LIMIT 1;
SELECT d.country, count(*) FROM ev JOIN dims d ON d.id = ev.grp GROUP BY 1 ORDER BY 1 LIMIT 1;
SELECT stat('lookup_cache_misses') AS lookup_misses, stat('lookup_cache_hits') AS lookup_hits;

-- a query that kept the normal plan is counted by the slug of its refusal while sift.explain_refusals is on
CREATE TABLE bare AS SELECT id, country FROM dims;
SELECT sift.stats_reset();
SET client_min_messages = warning;
SELECT b.country, count(*) FROM bare b GROUP BY 1 ORDER BY 1 LIMIT 1;
SELECT name, value FROM sift.stats(local => true) WHERE name LIKE 'refused.%' ORDER BY name;
SET sift.explain_refusals = on;
SELECT b.country, count(*) FROM bare b GROUP BY 1 ORDER BY 1 LIMIT 1;
SELECT name, value FROM sift.stats(local => true) WHERE name LIKE 'refused.%' ORDER BY name;
RESET sift.explain_refusals; RESET client_min_messages;

-- a statement that fails folds nothing in
SELECT sift.stats_reset();
SELECT grp, sum(amount / (amount - amount)) FROM ev GROUP BY grp;
SELECT stat('scans') AS scans_after_error;

DROP TABLE ev, dims, bare;
DROP FUNCTION stat(text), compare(text);
DROP EXTENSION pg_sift;
