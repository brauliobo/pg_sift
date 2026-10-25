-- three arms of a two-level aggregate that read the columns of a lateral `LIMIT 1` lookup: the first two test the lookup's column for NOT NULL and the third does not, so PostgreSQL leaves the condition at the join of the arms that test it and in the WHERE of the third; they are one scan
-- label: three `UNION ALL` arms over `jsonb_to_recordset(doc['parts'])` and a lateral `LIMIT 1` lookup, `mo.category IS NOT NULL` in two arms
-- ordered: yes
--@ setup
CREATE TABLE lookup_arms (kind text, sub text, derived_cat text, derived_sub text);
INSERT INTO lookup_arms SELECT 'cat_' || chr(97 + k), 'catg_' || chr(97 + s), 'm' || (k + s) % 3, 'ms' || (k + 2 * s) % 4 FROM generate_series(0, 3) k, generate_series(0, 5) s WHERE (k + s) % 5 <> 0;
CREATE UNIQUE INDEX lookup_arms_key ON lookup_arms (kind, sub);
ANALYZE lookup_arms
--@ pg
WITH flag_a AS (
	SELECT e.id, mo.category, mo.sub, avg(mo.score) AS score, count(e.id) AS n
	FROM events e INNER JOIN LATERAL (
	  SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	  FROM LATERAL (SELECT kind, sub, score FROM jsonb_to_recordset(e.doc['parts']) p(kind text, sub text, score float8) WHERE p.score IS NOT NULL) o
	  LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup_arms m WHERE m.kind = o.kind AND m.sub = o.sub LIMIT 1) rm ON true) mo ON true
	WHERE e.deleted_at IS NULL AND e.ts >= DATE '2025-01-01' AND e.ts < DATE '2025-04-01' AND mo.category IS NOT NULL GROUP BY e.id, mo.category, mo.sub
	UNION ALL
	SELECT e.id, mo.category, NULL::text, avg(mo.score) AS score, count(e.id) AS n
	FROM events e INNER JOIN LATERAL (
	  SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	  FROM LATERAL (SELECT kind, sub, score FROM jsonb_to_recordset(e.doc['parts']) p(kind text, sub text, score float8) WHERE p.score IS NOT NULL) o
	  LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup_arms m WHERE m.kind = o.kind AND m.sub = o.sub LIMIT 1) rm ON true) mo ON true
	WHERE e.deleted_at IS NULL AND e.ts >= DATE '2025-01-01' AND e.ts < DATE '2025-04-01' AND mo.category IS NOT NULL GROUP BY e.id, mo.category
	UNION ALL
	SELECT e.id, NULL::text, NULL::text, avg(mo.score) AS score, count(e.id) AS n
	FROM events e INNER JOIN LATERAL (
	  SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	  FROM LATERAL (SELECT kind, sub, score FROM jsonb_to_recordset(e.doc['parts']) p(kind text, sub text, score float8) WHERE p.score IS NOT NULL) o
	  LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup_arms m WHERE m.kind = o.kind AND m.sub = o.sub LIMIT 1) rm ON true) mo ON true
	WHERE e.deleted_at IS NULL AND e.ts >= DATE '2025-01-01' AND e.ts < DATE '2025-04-01' AND true GROUP BY e.id)
SELECT coalesce(category, 'all_cats') AS category, sub, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, count(DISTINCT id) AS ids
FROM flag_a GROUP BY category, sub ORDER BY 1, 2
