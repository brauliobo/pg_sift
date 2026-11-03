-- the bitmaps of the sets of a count(DISTINCT x) give way to hash sets when they pass 1 MB and cost over 128 bytes for each value they hold: ids spread over a wide domain, a few in each group
-- label: `SELECT grp, count(DISTINCT x) .. GROUP BY grp` over four times the rows of the run in a fifth of them groups, `x` spread over a domain of a billion values
-- ordered: yes
--@ setup
CREATE TABLE cds WITH (autovacuum_enabled = false) AS SELECT g AS id, (g % ({rows} / 5)) AS grp, ((g::bigint * 2654435761) % 1000000007)::int AS x FROM generate_series(1, {rows} * 4) g;
CREATE INDEX cds_col ON cds USING sift_col (id, grp, x);
VACUUM (FREEZE, ANALYZE) cds
--@ pg
SELECT grp, count(DISTINCT x) FROM cds GROUP BY grp ORDER BY grp LIMIT 1000
