-- an EXISTS in the FILTER of an aggregate: the keys of the table it asks about are one left join the scan reads in its pass, not one probe for each row
-- label: `count(*) FILTER (WHERE EXISTS (SELECT 1 FROM notes n WHERE n.dim_id = f.dim_id))` GROUP BY f.grp over 600,000 rows
-- ordered: yes
--@ setup
CREATE TABLE gx_notes AS SELECT g AS id, (g::bigint * 7 % ({rows} / 10))::int + 1 AS dim_id FROM generate_series(1, {rows} / 30) g;
CREATE INDEX gx_notes_dim ON gx_notes (dim_id);
CREATE TABLE gx_fact AS SELECT g AS id, (g::bigint * 104729 % ({rows} / 10))::int + 1 AS dim_id, g % 17 AS grp FROM generate_series(1, {rows}) g;
CREATE INDEX gx_fact_col ON gx_fact USING sift_col (id, dim_id, grp);
ANALYZE gx_notes;
ANALYZE gx_fact
--@ pg
SELECT grp, count(*) AS n, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM gx_notes m WHERE m.dim_id = f.dim_id)) AS noted FROM gx_fact f GROUP BY grp ORDER BY grp
