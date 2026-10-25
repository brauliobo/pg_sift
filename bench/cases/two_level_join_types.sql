-- the same arms with the NOT NULL test on the lookup's own column inside the sub-select (`WHERE rm.derived_cat IS NOT NULL`): PostgreSQL turns the outer join of the lookup into an inner one in the arms that test, keeps the outer join in the third, and the third arm counts the tuples the lookup has no row for
-- label: three `UNION ALL` arms over `jsonb_to_recordset(doc['parts'])` and a lateral `LIMIT 1` lookup, `rm.derived_cat IS NOT NULL` in two arms
-- ordered: yes
--@ setup
CREATE TABLE lookup_jt (kind text, sub text, derived_cat text, derived_sub text);
INSERT INTO lookup_jt SELECT 'cat_' || chr(97 + k), 'catg_' || chr(97 + s), 'm' || (k + s) % 3, 'ms' || (k + 2 * s) % 4 FROM generate_series(0, 3) k, generate_series(0, 5) s WHERE (k + s) % 5 <> 0;
CREATE UNIQUE INDEX lookup_jt_key ON lookup_jt (kind, sub);
ANALYZE lookup_jt
--@ pg
WITH flag_a AS (
	SELECT e.id, mo.category, mo.sub, avg(mo.score) AS score, count(e.id) AS n
	FROM events e INNER JOIN LATERAL (
	  SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	  FROM LATERAL (SELECT kind, sub, score FROM jsonb_to_recordset(e.doc['parts']) p(kind text, sub text, score float8) WHERE p.score IS NOT NULL) o
	  LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup_jt m WHERE m.kind = o.kind AND m.sub = o.sub LIMIT 1) rm ON true
	  WHERE rm.derived_cat IS NOT NULL) mo ON true
	WHERE e.deleted_at IS NULL AND e.ts >= DATE '2025-01-01' AND e.ts < DATE '2025-04-01' AND true GROUP BY e.id, mo.category, mo.sub
	UNION ALL
	SELECT e.id, mo.category, NULL::text, avg(mo.score) AS score, count(e.id) AS n
	FROM events e INNER JOIN LATERAL (
	  SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	  FROM LATERAL (SELECT kind, sub, score FROM jsonb_to_recordset(e.doc['parts']) p(kind text, sub text, score float8) WHERE p.score IS NOT NULL) o
	  LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup_jt m WHERE m.kind = o.kind AND m.sub = o.sub LIMIT 1) rm ON true
	  WHERE rm.derived_cat IS NOT NULL) mo ON true
	WHERE e.deleted_at IS NULL AND e.ts >= DATE '2025-01-01' AND e.ts < DATE '2025-04-01' AND true GROUP BY e.id, mo.category
	UNION ALL
	SELECT e.id, NULL::text, NULL::text, avg(mo.score) AS score, count(e.id) AS n
	FROM events e INNER JOIN LATERAL (
	  SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	  FROM LATERAL (SELECT kind, sub, score FROM jsonb_to_recordset(e.doc['parts']) p(kind text, sub text, score float8) WHERE p.score IS NOT NULL) o
	  LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup_jt m WHERE m.kind = o.kind AND m.sub = o.sub LIMIT 1) rm ON true) mo ON true
	WHERE e.deleted_at IS NULL AND e.ts >= DATE '2025-01-01' AND e.ts < DATE '2025-04-01' AND true GROUP BY e.id)
SELECT coalesce(category, 'all_cats') AS category, sub, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, count(DISTINCT id) AS ids
FROM flag_a GROUP BY category, sub ORDER BY 1, 2
