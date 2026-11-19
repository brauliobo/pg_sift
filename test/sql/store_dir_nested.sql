-- The chained directory with nested bounds, against a model: partitions whose lower bounds are of one to three key components and an address (an ordinal of four base-64 digits in
-- the model), split, merged and rewritten at random between the entries of no partition, with leaves of 3 entries and interior pages of 8 references so that the chain is two levels deep.
-- After every edit the bounds read back as the ordinals the model holds, and a read of the chain by a prefix of 0 to 3 components, or of 3 and an address, reads the entries of no partition
-- and those of the partitions whose range holds a key with the prefix, as the model says and no others.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_dir_flat_max = 4;
SET sift.test_dir_leaf_max = 3;
SET sift.test_dir_root_max = 8;
CREATE TABLE dm (v int);
CREATE INDEX dm_idx ON dm USING sift_store_test (v);
SELECT sift._test_dir_edit('dm_idx', 'ins', 0, 3) > 0 AS no_partitions;
CREATE TABLE model (pos int, gen int8, o int8);
INSERT INTO model SELECT ord, gen, NULL FROM sift._test_dir_list('dm_idx');
SELECT sift._test_dir_nest('dm_idx', 3, 0, ARRAY[0, 70000, 140000]::int8[]) > 0 AS first_partitions;
INSERT INTO model SELECT g, (SELECT max(gen) FROM model) + g - 2, (ARRAY[0, 70000, 140000])[g - 2] FROM generate_series(3, 5) g;

CREATE FUNCTION digits(o int8, k int) RETURNS int[] LANGUAGE sql IMMUTABLE AS
	$$ SELECT array_agg(((o >> (6 * (3 - i))) & 63)::int ORDER BY i) FROM generate_series(0, k - 1) i $$;

-- the gens a read by the prefix is to return, by the model: all of no partition, and the partitions that meet [a, b)
CREATE FUNCTION expected(a int8, b int8) RETURNS SETOF int8 LANGUAGE sql AS $$
	SELECT gen FROM model WHERE o IS NULL
	UNION ALL
	SELECT m.gen FROM model m WHERE m.o IS NOT NULL AND m.o < b
		AND COALESCE((SELECT min(n.o) FROM model n WHERE n.o IS NOT NULL AND n.pos > m.pos), b) > a
$$;

CREATE FUNCTION check_prefixes(tries int) RETURNS int LANGUAGE plpgsql AS $$
DECLARE bad int := 0; k int; o int8; d int[]; t int8; a int8; b int8; w int8;
BEGIN
	FOR i IN 1..tries LOOP
		SELECT m.o INTO o FROM model m WHERE m.o IS NOT NULL ORDER BY random() LIMIT 1;
		k := floor(random() * 5)::int;
		t := -1;
		IF k = 4 THEN k := 3; t := 1 + floor(random() * 63)::int; END IF;
		IF random() < 0.3 THEN o := floor(random() * 16777216)::int8; END IF;
		d := digits(o, k);
		w := 1::int8 << (6 * (4 - k));
		IF t >= 0 THEN a := (o >> 6 << 6) + t; b := a + 1; ELSE a := o >> (6 * (4 - k)) << (6 * (4 - k)); b := a + w; END IF;
		IF k = 0 THEN d := ARRAY[]::int[]; END IF;
		IF EXISTS (SELECT 1 FROM (SELECT gen FROM sift._test_dir_prefix('dm_idx', d, t) EXCEPT SELECT * FROM expected(a, b)) x)
		   OR EXISTS (SELECT 1 FROM (SELECT * FROM expected(a, b) EXCEPT SELECT gen FROM sift._test_dir_prefix('dm_idx', d, t)) y) THEN
			bad := bad + 1;
		END IF;
	END LOOP;
	RETURN bad;
END $$;

CREATE FUNCTION same_as_model() RETURNS boolean LANGUAGE sql AS $$
	SELECT NOT EXISTS (SELECT ord, gen FROM sift._test_dir_list('dm_idx') EXCEPT SELECT pos, gen FROM model)
	   AND NOT EXISTS (SELECT pos, gen FROM model EXCEPT SELECT ord, gen FROM sift._test_dir_list('dm_idx'))
	   AND NOT EXISTS (SELECT first, ord FROM sift._test_dir_nested('dm_idx') EXCEPT SELECT pos, o FROM model WHERE o IS NOT NULL)
	   AND NOT EXISTS (SELECT pos, o FROM model WHERE o IS NOT NULL EXCEPT SELECT first, ord FROM sift._test_dir_nested('dm_idx'))
$$;

-- 400 random edits: a split, merge or rewrite of the partitions from pos, a deletion of some, or an insertion of entries of no partition in front
CREATE FUNCTION run(steps int) RETURNS TABLE (mismatches int, prefix_mismatches int, deepest int, biggest int) LANGUAGE plpgsql AS $$
DECLARE
	bad int := 0; pbad int := 0; deep int := 0; big int := 0; z int; cnt int; p int; nold int; nnew int; lo int8; hi int8; span int8; base int8; ords int8[]; g int8; kind int;
BEGIN
	PERFORM setseed(0.42);
	FOR i IN 1..steps LOOP
		SELECT count(*) INTO z FROM model WHERE o IS NULL;
		SELECT count(*) INTO cnt FROM model;
		kind := floor(random() * 10)::int;
		IF kind = 0 AND z < 6 THEN
			g := sift._test_dir_edit('dm_idx', 'ins', z, 1);
			UPDATE model SET pos = pos + 1 WHERE pos >= z;
			INSERT INTO model VALUES (z, g, NULL);
		ELSIF kind = 1 AND cnt - z > 8 THEN
			p := z + floor(random() * (cnt - z - 2))::int;
			nold := 1 + floor(random() * 2)::int;
			PERFORM sift._test_dir_edit('dm_idx', 'del', p, nold);
			DELETE FROM model WHERE pos >= p AND pos < p + nold;
			UPDATE model SET pos = pos - nold WHERE pos >= p + nold;
		ELSE
			p := z + floor(random() * (cnt - z + 1))::int;
			nold := least(floor(random() * 4)::int, cnt - p);
			SELECT COALESCE(max(o), -1) INTO lo FROM model WHERE pos = p - 1;
			SELECT COALESCE(min(o), 16777216) INTO hi FROM model WHERE pos = p + nold;
			nnew := 1 + floor(random() * 4)::int;
			IF hi - lo - 1 < nnew OR cnt + 4 - nold > 40 THEN CONTINUE; END IF;
			span := CASE floor(random() * 4)::int WHEN 0 THEN hi - lo - 1 WHEN 1 THEN least(hi - lo - 1, 262144) WHEN 2 THEN least(hi - lo - 1, 4096) ELSE least(hi - lo - 1, 64) END;
			span := greatest(span, nnew);
			base := lo + 1 + floor(random() * (hi - lo - span))::int8;
			SELECT array_agg(v ORDER BY v) INTO ords FROM (SELECT DISTINCT base + floor(random() * span)::int8 AS v FROM generate_series(1, nnew * 3) LIMIT nnew) s;
			nnew := array_length(ords, 1);
			g := sift._test_dir_nest('dm_idx', p, nold, ords);
			DELETE FROM model WHERE pos >= p AND pos < p + nold;
			UPDATE model SET pos = pos + nnew - nold WHERE pos >= p + nold;
			INSERT INTO model SELECT p + j - 1, g + j - 1, ords[j] FROM generate_series(1, nnew) j;
		END IF;
		IF NOT same_as_model() THEN bad := bad + 1; END IF;
		pbad := pbad + check_prefixes(6);
		IF i % 50 = 0 AND sift._test_dir_check('dm_idx') <= 0 THEN bad := bad + 1; END IF;
	END LOOP;
	SELECT count(*) INTO big FROM model;
	SELECT max(CASE WHEN o IS NULL THEN 0 WHEN o & 63 <> 0 THEN 4 WHEN o >> 6 & 63 <> 0 THEN 3 WHEN o >> 12 & 63 <> 0 THEN 2 ELSE 1 END) INTO deep FROM model;
	RETURN QUERY SELECT bad, pbad, deep, big;
END $$;

SELECT * FROM run(400);
SELECT same_as_model() AS directory_equals_model, sift._test_dir_check('dm_idx') > 0 AS chain_sound;
SELECT chained, nsegs > 24 AS two_levels FROM sift._test_dir_info('dm_idx');
-- a read by a prefix leaves out pages of the chain: the most pages one read touched, against the pages of the chain
SELECT max(pages) < (SELECT chain_blocks FROM sift._test_dir_info('dm_idx')) AS fewer_pages_than_the_chain
FROM (SELECT (SELECT max(pages) FROM sift._test_dir_prefix('dm_idx', digits(o, 3), -1)) AS pages FROM model WHERE o IS NOT NULL) r;
DROP FUNCTION run(int), check_prefixes(int), same_as_model(), expected(int8, int8), digits(int8, int);
DROP TABLE model, dm;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
