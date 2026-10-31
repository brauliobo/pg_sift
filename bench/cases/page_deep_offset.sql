-- a deep page: the rows 5,000 to 5,100 of the newest-first order of a set of parents, with a lateral sub-select that aggregates the rows' jsonb array
-- label: `.. LEFT JOIN LATERAL (aggregate over jsonb_to_recordset(doc['parts'])) .. WHERE parent_id IN (set of 340 parents) ORDER BY ts DESC, id LIMIT 100 OFFSET 5000`
-- ordered: yes
--@ setup
CREATE TABLE pd_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int NOT NULL);
INSERT INTO pd_links SELECT p, 4, p FROM generate_series(1, (SELECT count(*) * 85 / 100 FROM dims)) p;
CREATE INDEX pd_links_set ON pd_links (set_id, parent_id);
ANALYZE pd_links
--@ pg
SELECT e.id, e.ts, e.amount, d.tier, o.n_parts, o.avg_score
FROM events e JOIN dims d ON d.id = e.parent_id AND d.active
LEFT JOIN LATERAL (SELECT count(*) AS n_parts, avg(x.score) AS avg_score FROM jsonb_to_recordset(e.doc['parts']) x(kind text, sub text, score float8) WHERE x.score IS NOT NULL) o ON true
WHERE e.deleted_at IS NULL AND e.parent_id IN (SELECT parent_id FROM pd_links WHERE set_id = 4) AND e.ts >= date '2024-06-01'
ORDER BY e.ts DESC NULLS LAST, e.id LIMIT 100 OFFSET 5000
