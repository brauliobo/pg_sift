-- The probe rewrite (a join to a large btree table or an IN over one becomes a lateral probe of its btree for each key the scan meets) must leave a statement that names its keys a scan:
-- the entities come from IN (SELECT ..) in a CTE, a VALUES list, a sub-select or a DISTINCT sub-select, are joined to a dimension by its key, and the events of those entities are
-- read with the keys a lateral jsonb_each unnests, joined to a table keyed by two columns. A table below the size a probe is made for (10,000 rows) is joined as written, a table above
-- it is probed: the dimension and the table the keys come from are 9,000 or 12,000 rows, the tables the events join are 12,000 rows, and every combination has one scan and the rows of the plan without it.
-- A probe made of a table that only the keys reach (or inside the level that names the keys) left the keys with no join condition once the statement was flattened: join_condition, the normal plan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
-- the normal plan costs less than the scan at this size: the scan is forced, so that the test shows whether the statement can be planned as a scan at all
SET sift.agg_force = on;

CREATE TABLE dim9 (id int PRIMARY KEY, kind text NOT NULL) WITH (autovacuum_enabled = false);
CREATE TABLE dim12 (LIKE dim9 INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO dim9 SELECT i, 'k' || i % 4 FROM generate_series(1, 9000) i;
INSERT INTO dim12 SELECT i, 'k' || i % 4 FROM generate_series(1, 12000) i;
-- the keys: group 1 holds 11 entities (a twelfth is removed), the rest of the rows are other groups
CREATE TABLE keys9 (grp_id int NOT NULL, dim_id int NOT NULL, gone timestamptz) WITH (autovacuum_enabled = false);
CREATE TABLE keys12 (LIKE keys9) WITH (autovacuum_enabled = false);
INSERT INTO keys9 SELECT CASE WHEN i <= 11 THEN 1 ELSE 100 + i % 50 END, i, NULL FROM generate_series(1, 9000) i;
INSERT INTO keys12 SELECT CASE WHEN i <= 11 THEN 1 ELSE 100 + i % 50 END, i, NULL FROM generate_series(1, 12000) i;
INSERT INTO keys9 VALUES (1, 12, now());
INSERT INTO keys12 VALUES (1, 12, now());
CREATE INDEX keys9_key ON keys9 (grp_id, dim_id);
CREATE INDEX keys12_key ON keys12 (grp_id, dim_id);
-- the tables of the events: 12,000 rows each, so that they are probed
CREATE TABLE kinds (id int PRIMARY KEY, enabled bool NOT NULL);
INSERT INTO kinds SELECT i, i % 3 <> 0 FROM generate_series(1, 12000) i;
CREATE TABLE links (id int PRIMARY KEY, url text NOT NULL);
INSERT INTO links SELECT i, 'u' || i FROM generate_series(0, 11999) i;
CREATE TABLE key_labels (kind_id int, name text, shown text, UNIQUE (kind_id, name));
INSERT INTO key_labels SELECT 1 + i % 12000, 'n' || i, 'shown' || i FROM generate_series(1, 12000) i;
INSERT INTO key_labels VALUES (1, 'a', 'alpha'), (2, 'b', 'beta');
CREATE TABLE ev (id int PRIMARY KEY, dim_id int NOT NULL, kind_id int NOT NULL, link_id int, d date NOT NULL, flags jsonb NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + i % 40, 1 + i % 3, CASE WHEN i % 4 = 0 THEN NULL ELSE i % 50 END, date '2026-01-01' + i % 200, jsonb_build_object('a', i % 5, 'b', i % 3) FROM generate_series(1, 6000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, dim_id, kind_id, link_id, d, flags) WITH (track_changes = 'off', paths = 'flags.* float8');
VACUUM (FREEZE, ANALYZE) dim9, dim12, keys9, keys12, kinds, links, key_labels, ev;

CREATE FUNCTION shape_q(form text, link text, dim text, keys text) RETURNS text LANGUAGE sql AS $$
	SELECT format('WITH %s,
		evs AS (SELECT ev.*, coalesce(ents.kind, ''-'') AS kind FROM ev INNER JOIN ents ON ents.id = ev.dim_id LEFT JOIN links ON links.id = ev.link_id INNER JOIN kinds ON kinds.id = ev.kind_id AND kinds.enabled IS TRUE WHERE ev.d >= ''2026-02-01''),
		parsed AS (SELECT evs.id, evs.kind, m.label, (m.value #>> ''{}'')::float AS v FROM evs INNER JOIN LATERAL (SELECT coalesce(key_labels.shown, r.key) AS label, r.value FROM jsonb_each(evs.flags) AS r(key, value) LEFT JOIN key_labels ON key_labels.kind_id = evs.kind_id AND key_labels.name = r.key) m ON true)
		SELECT label, kind, round(avg(v)::numeric, 6), count(DISTINCT id) FROM parsed GROUP BY label, kind ORDER BY label, kind',
		CASE form
			WHEN 'keys_cte' THEN format('ids AS MATERIALIZED (SELECT DISTINCT d.id FROM %1$s d WHERE d.id IN (SELECT dim_id FROM %2$s WHERE gone IS NULL AND grp_id = 1)),
				ents AS (SELECT d.* FROM ids %3$s JOIN %1$s d ON d.id = ids.id)', dim, keys, link)
			WHEN 'values' THEN format('ents AS (SELECT d.* FROM (VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10), (11)) AS k(id) %2$s JOIN %1$s d ON d.id = k.id)', dim, link)
			WHEN 'subselect' THEN format('ents AS (SELECT d.* FROM (SELECT dim_id AS id FROM %2$s WHERE gone IS NULL AND grp_id = 1) k %3$s JOIN %1$s d ON d.id = k.id)', dim, keys, link)
			WHEN 'distinct_subselect' THEN format('ents AS (SELECT d.* FROM (SELECT DISTINCT dim_id AS id FROM %2$s WHERE gone IS NULL AND grp_id = 1) k %3$s JOIN %1$s d ON d.id = k.id)', dim, keys, link)
		END)
$$;
CREATE FUNCTION scans(sql text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		n := n + (l ~ 'Custom Scan \(SiftAgg\)')::int;
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN scan THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', sql) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION check_shape(form text, link text, dim_rows int, keys_rows int, OUT shape text, OUT in_the_scan int, OUT same_rows_as_without bool, OUT has_rows bool) LANGUAGE plpgsql AS $$
DECLARE q text := shape_q(form, CASE link WHEN 'inner' THEN 'INNER' ELSE 'LEFT' END, 'dim' || dim_rows / 1000, 'keys' || keys_rows / 1000); a text; b text; BEGIN
	shape := format('%s, %s join, dimension %s, keys %s', form, link, dim_rows, keys_rows);
	PERFORM set_config('sift.enable_agg', 'on', true);
	in_the_scan := scans(q);
	a := answer(q, true);
	b := answer(q, false);
	same_rows_as_without := a = b;
	has_rows := length(a) > 0;
END $$;

SELECT c.* FROM unnest(ARRAY['keys_cte', 'values', 'subselect', 'distinct_subselect']) AS f(form),
	unnest(ARRAY['inner', 'left']) AS j(link),
	unnest(ARRAY[9000, 12000]) AS d(dim_rows),
	unnest(ARRAY[9000, 12000]) AS k(keys_rows),
	LATERAL check_shape(f.form, j.link, d.dim_rows, k.keys_rows) c
WHERE f.form <> 'values' OR k.keys_rows = 9000
ORDER BY f.form, j.link, d.dim_rows, k.keys_rows;

DROP FUNCTION check_shape(text, text, int, int);
DROP FUNCTION answer(text, bool);
DROP FUNCTION scans(text);
DROP FUNCTION shape_q(text, text, text, text);
DROP TABLE ev, key_labels, links, kinds, keys12, keys9, dim12, dim9;
DROP EXTENSION pg_sift;
