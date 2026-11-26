-- sift.shared_memory(): the shape of the status of the shared tables, and what it says of the ones that are made
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SELECT a.name, format_type(a.typ, NULL) AS type
FROM pg_proc p, unnest(p.proargnames, p.proargmodes, p.proallargtypes) WITH ORDINALITY a(name, mode, typ, ord)
WHERE p.pronamespace = 'sift'::regnamespace AND p.proname = 'shared_memory' AND a.mode IN ('o', 't')
ORDER BY a.ord;

-- tables are made by what runs: a column index read in a session that builds its dictionaries
CREATE TABLE sm (id int PRIMARY KEY, k int, url text);
INSERT INTO sm SELECT g, g % 40, 'https://example.com/' || md5((g % 500)::text) FROM generate_series(1, 20000) g;
CREATE INDEX sm_col ON sm USING sift_col (id, k, url);
SELECT count(*) FROM (SELECT url, count(*) FROM sm GROUP BY url) s;
SELECT count(*) FROM (SELECT url, count(*) FROM sm GROUP BY url) s;

SELECT bool_and(shared) AS all_shared, bool_and(mapped_bytes > 0) AS all_mapped, bool_and(reason IS NULL OR reason <> '') AS reasons_read,
       bool_and(used_bytes IS NULL OR used_bytes <= mapped_bytes) AS used_inside, bool_and((hits IS NULL) = (misses IS NULL)) AS counters_together,
       bool_and(hits >= 0 AND misses >= 0) AS counters_counted
FROM sift.shared_memory();
SELECT name FROM sift.shared_memory() WHERE name IN ('sift_col_dict', 'sift_col_leaf_stats') ORDER BY name;
SELECT configured_bytes BETWEEN pg_size_bytes(current_setting('sift.shared_cache_kb')) * 16 / 100 - 1024 AND pg_size_bytes(current_setting('sift.shared_cache_kb')) * 16 / 100 AS leaf_share_of_setting, mapped_bytes >= configured_bytes AS leaf_mapped, used_bytes > 0 AS leaf_used, hits + misses > 0 AS leaf_asked
FROM sift.shared_memory() WHERE name = 'sift_col_leaf_stats';

DROP TABLE sm;
DROP EXTENSION pg_sift;
