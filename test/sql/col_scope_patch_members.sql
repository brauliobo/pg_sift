-- The patch of a scope read (the entity rows of the members whose pairs changed since the stamp, copied for each pair of the key) costs the same for an integer, a text and a uuid member: it looks at the
-- member leaf of the entity units and decodes only the rows of the members that changed, with their addresses and the columns the read uses, and the member leaf is not copied unless the read uses it.
-- Each read is md5 equal to the plain join, the patch has the rows of the changed members, and the blocks the read hits stay within the figure of the cell (a decode of every row, or of every leaf, hits more).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
SET sift.test_scope_patch = on;

-- 600 members of 100 facts in 12 units, 40 keys of one or two members in three; the pairs of 75 members (one in eight) get the key 17 after the stamp
CREATE FUNCTION tables(typ text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	sqltype text := CASE typ WHEN 'int' THEN 'int' WHEN 'text' THEN 'text' ELSE 'uuid' END;
BEGIN
	EXECUTE format('CREATE TABLE pairs_%1$s (m %2$s NOT NULL, k int NOT NULL, PRIMARY KEY (m, k))', typ, sqltype);
	EXECUTE format('INSERT INTO pairs_%1$s SELECT %3$s, 1 + (m * 7 + j * 31) %% 40 FROM generate_series(1, 600) m, generate_series(0, 2) j WHERE j <= m %% 3 ON CONFLICT DO NOTHING',
		typ, sqltype, CASE typ WHEN 'int' THEN 'm' WHEN 'text' THEN '''member-'' || m' ELSE 'md5(m::text)::uuid' END);
	EXECUTE format('CREATE INDEX pairs_%1$s_journal ON pairs_%1$s USING sift_col (m, k) WITH (track_changes = ''ctid'')', typ);
	EXECUTE format('CREATE TABLE facts_%1$s (id int PRIMARY KEY, m %2$s, at date, v int) WITH (autovacuum_enabled = false)', typ, sqltype);
	EXECUTE format('INSERT INTO facts_%1$s SELECT i, %3$s, date ''2025-01-01'' + i %% 300, i %% 100 FROM generate_series(1, 60000) i',
		typ, sqltype, CASE typ WHEN 'int' THEN '1 + (i::bigint * 7919) % 600' WHEN 'text' THEN '''member-'' || (1 + (i::bigint * 7919) % 600)' ELSE 'md5((1 + (i::bigint * 7919) % 600)::text)::uuid' END);
END $$;

SELECT tables(t) FROM unnest(ARRAY['int', 'text', 'uuid']) t;
VACUUM (FREEZE, ANALYZE) facts_int, pairs_int, facts_text, pairs_text, facts_uuid, pairs_uuid;
SELECT format('CREATE INDEX facts_%1$s_col ON facts_%1$s USING sift_col (id, m, at, v) WITH (scope = ''k: m -> pairs_%1$s(m, k)'')', t) FROM unnest(ARRAY['int', 'text', 'uuid']) t \gexec
VACUUM (FREEZE, ANALYZE) facts_int, facts_text, facts_uuid;

-- the cells: after the stamp, the pairs of one member in eight get the key 17
INSERT INTO pairs_int SELECT m, 17 FROM generate_series(1, 600) m WHERE m % 8 = 1 ON CONFLICT DO NOTHING;
INSERT INTO pairs_text SELECT 'member-' || m, 17 FROM generate_series(1, 600) m WHERE m % 8 = 1 ON CONFLICT DO NOTHING;
INSERT INTO pairs_uuid SELECT md5(m::text)::uuid, 17 FROM generate_series(1, 600) m WHERE m % 8 = 1 ON CONFLICT DO NOTHING;

CREATE FUNCTION cell(typ text, OUT same_rows boolean, OUT patched_units int, OUT delta_rows int, OUT hit int) LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT count(*), sum(v) FROM facts_%1$s e WHERE e.m IN (SELECT m FROM pairs_%1$s WHERE k = 17)', typ);
	l text;
	a text;
	b text;
BEGIN
	EXECUTE q INTO a;
	EXECUTE q INTO a;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Scope Patched Units' THEN patched_units := regexp_replace(l, '\D', '', 'g'); END IF;
		IF l ~ 'Scope Delta Rows' THEN delta_rows := regexp_replace(l, '\D', '', 'g'); END IF;
		IF l ~ 'Buffers: shared hit' AND hit IS NULL THEN hit := (regexp_match(l, 'hit=(\d+)'))[1]; END IF;
	END LOOP;
	EXECUTE q INTO b;
	PERFORM set_config('sift.agg_scope', 'off', true);
	EXECUTE q INTO a;
	PERFORM set_config('sift.agg_scope', 'on', true);
	same_rows := a = b;
END $$;
-- a server built with CATCACHE_FORCE_RELEASE reads the catalog again at every lookup: a hundred lookups of a type hit buffers there and none in another build, and the read hits 150 to 190 blocks for the same plan
CREATE FUNCTION catalog_reread() RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(format_type(23, NULL)) FROM generate_series(1, 100)' LOOP
		IF l ~ 'Buffers: shared hit' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SELECT catalog_reread() AS catalog_reread \gset
SELECT t AS member_type, c.same_rows, c.patched_units, c.delta_rows, c.hit <= CASE WHEN :'catalog_reread' THEN 200 ELSE 75 END AS hits_within_the_cell FROM unnest(ARRAY['int', 'text', 'uuid']) t, LATERAL cell(t) c;

DROP FUNCTION cell, tables, catalog_reread;
DROP TABLE facts_int, facts_text, facts_uuid, pairs_int, pairs_text, pairs_uuid CASCADE;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
