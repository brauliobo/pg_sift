-- ranges and networks in a sift_col index keep the bounds of their values as integer lanes: a conjunct with a
-- containment operator is answered from the lanes, "Wide Filters In Source", and the rows must be the heap's: every operator against every constant,
-- before and after writes, a merge and a REINDEX. The bounds that do not fit a lane (2^62 and beyond, timestamp infinity) and an IPv6 mask over 62
-- bits are decided by PostgreSQL's operator.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;

CREATE TABLE rt (id serial PRIMARY KEY, a int4range, d daterange, t tsrange, z int8range, w tstzrange) WITH (autovacuum_enabled = false);
INSERT INTO rt (a, d, t, z, w)
SELECT int4range(l, u, b), daterange(date '2000-01-01' + l, date '2000-01-01' + u, b),
       tsrange(timestamp '2000-01-01' + l * interval '1 microsecond', timestamp '2000-01-01' + u * interval '1 microsecond', b),
       int8range(l, u, b), tstzrange(timestamptz '2000-01-01 00:00+00' + l * interval '1 microsecond', timestamptz '2000-01-01 00:00+00' + u * interval '1 microsecond', b)
FROM generate_series(-1, 4) l, generate_series(-1, 4) u, unnest(ARRAY['[)', '[]', '()', '(]']) b WHERE l <= u;
INSERT INTO rt (a, d, t, z, w)
SELECT int4range(NULL, u, b), daterange(NULL, date '2000-01-01' + u, b), tsrange(NULL, timestamp '2000-01-01' + u * interval '1 microsecond', b), int8range(NULL, u, b),
       tstzrange(NULL, timestamptz '2000-01-01 00:00+00' + u * interval '1 microsecond', b)
FROM generate_series(-1, 4) u, unnest(ARRAY['[)', '[]', '()', '(]']) b;
INSERT INTO rt (a, d, t, z, w)
SELECT int4range(l, NULL, b), daterange(date '2000-01-01' + l, NULL, b), tsrange(timestamp '2000-01-01' + l * interval '1 microsecond', NULL, b), int8range(l, NULL, b),
       tstzrange(timestamptz '2000-01-01 00:00+00' + l * interval '1 microsecond', NULL, b)
FROM generate_series(-1, 4) l, unnest(ARRAY['[)', '[]', '()', '(]']) b;
INSERT INTO rt (a, d, t, z, w) VALUES ('(,)', '(,)', '(,)', '(,)', '(,)'), ('empty', 'empty', 'empty', 'empty', 'empty'), (NULL, NULL, NULL, NULL, NULL),
	(int4range(2147483640, 2147483647), NULL, tsrange('-infinity', '2000-01-01'), int8range(4611686018427387904, 4611686018427387910), tstzrange('2000-01-01', 'infinity')),
	(NULL, NULL, tsrange('2000-01-01', 'infinity', '[]'), int8range(9223372036854775800, NULL), tstzrange('-infinity', 'infinity', '[]')),
	(NULL, NULL, tsrange('infinity', 'infinity', '[]'), int8range(NULL, -4611686018427387905), NULL);

CREATE TABLE nt (id serial PRIMARY KEY, ip inet, cd cidr) WITH (autovacuum_enabled = false);
INSERT INTO nt (ip) SELECT x::inet FROM unnest(ARRAY[
	'0.0.0.0/0', '20.0.0.0/8', '20.0.0.0/9', '20.128.0.0/9', '20.1.0.0/16', '20.1.2.0/24', '20.1.2.3/32', '20.1.2.3/24', '20.1.2.3', '20.255.255.255/32', '11.0.0.0/8',
	'30.30.0.0/16', '30.30.1.0/24', '30.30.1.77/25', '255.255.255.255/32', '255.255.255.254/31', '127.0.0.1/8', '1.2.3.4/0', '::/0', '::1/128', '::ffff:20.0.0.1/128',
	'::ffff:20.0.0.0/104', '2001:db8::/32', '2001:db8::/48', '2001:db8:0:1::/64', '2001:db8:0:1::1/128', '2001:db8:0:1::1/65', '2001:db8:0:1::8000:0/97',
	'2001:db8:0:1:8000::/65', '2001:db8:0:2::/64', '2001:db8:0:1:ffff:ffff:ffff:ffff/128', 'fe80::/10', 'fe80::1/64', 'ff00::/8', 'ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/128',
	'8000::/1', '7fff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/128', '8000::/128', '::/128', '2001:db8::1/127', '2001:db8::1/96', '2001:db8::/16']) x;
INSERT INTO nt (ip) VALUES (NULL);
UPDATE nt SET cd = network(ip) WHERE ip IS NOT NULL;

CREATE INDEX rt_col ON rt USING sift_col (id, a, d, t, z, w);
CREATE INDEX nt_col ON nt USING sift_col (id, ip, cd);
VACUUM (FREEZE, ANALYZE) rt, nt;
SELECT sift.col_verify('rt_col', true), sift.col_verify('nt_col', true);
SELECT leaf, path, type, kind FROM sift.col_leaves('rt_col') ORDER BY leaf;
SELECT leaf, path, type, kind FROM sift.col_leaves('nt_col') ORDER BY leaf;
SELECT DISTINCT format FROM sift.col_segments('rt_col') WHERE kind = 'immutable';

-- the count of a query with the pushdown (and the lanes' filter in the plan) and without it
CREATE FUNCTION same(q text, OUT equal boolean, OUT lanes boolean) LANGUAGE plpgsql AS $$
DECLARE n_on bigint; n_off bigint; l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.agg_force', 'on', true);
	lanes := false;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP lanes := lanes OR l ~ 'Wide Filters In Source'; END LOOP;
	EXECUTE q INTO n_on;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE q INTO n_off;
	equal := n_on = n_off;
END $$;

CREATE TABLE ranges (col text, ty text);
INSERT INTO ranges VALUES ('a', 'int4range'), ('d', 'daterange'), ('t', 'tsrange'), ('z', 'int8range'), ('w', 'tstzrange');
CREATE TABLE rconst AS SELECT row_number() OVER () k, * FROM (
	SELECT int4range(l, u, b) a, daterange(date '2000-01-01' + l, date '2000-01-01' + u, b) d,
		tsrange(timestamp '2000-01-01' + l * interval '1 microsecond', timestamp '2000-01-01' + u * interval '1 microsecond', b) t,
		int8range(l, u, b) z, tstzrange(timestamptz '2000-01-01 00:00+00' + l * interval '1 microsecond', timestamptz '2000-01-01 00:00+00' + u * interval '1 microsecond', b) w
	FROM generate_series(0, 3) l, generate_series(0, 3) u, unnest(ARRAY['[)', '[]', '()', '(]']) b WHERE l <= u
	UNION ALL SELECT '(,)', '(,)', '(,)', '(,)', '(,)'
	UNION ALL SELECT 'empty', 'empty', 'empty', 'empty', 'empty'
	UNION ALL SELECT int4range(NULL, 2), daterange(NULL, date '2000-01-03'), tsrange(NULL, timestamp '2000-01-01 00:00:00.000002'), int8range(NULL, 2), tstzrange(NULL, timestamptz '2000-01-01 00:00:00.000002+00')
	UNION ALL SELECT int4range(2, NULL, '()'), daterange(date '2000-01-03', NULL, '()'), tsrange(timestamp '2000-01-01 00:00:00.000002', NULL, '()'), int8range(2, NULL, '()'),
		tstzrange(timestamptz '2000-01-01 00:00:00.000002+00', NULL, '()')
	UNION ALL SELECT int4range(2147483640, 2147483647), NULL, tsrange('2000-01-01', 'infinity', '[]'), int8range(4611686018427387904, 4611686018427387910), tstzrange('-infinity', '2000-01-01')) c;
CREATE TABLE econst (col text, e text);
INSERT INTO econst VALUES ('a', '3::int4'), ('a', '(-2147483648)::int4'), ('a', '2147483647::int4'), ('d', 'date ''2000-01-03'''), ('d', '''infinity''::date'), ('d', '''-infinity''::date'),
	('t', 'timestamp ''2000-01-01 00:00:00.000003'''), ('t', '''infinity''::timestamp'), ('z', '3::int8'), ('z', '9223372036854775807::int8'), ('z', '4611686018427387904::int8'),
	('w', 'timestamptz ''2000-01-01 00:00:00.000003+00'''), ('w', '''-infinity''::timestamptz');

-- every operator against every constant, per column type; and the elements, which are on either side
CREATE FUNCTION range_matrix() RETURNS TABLE (col text, op text, equal boolean, lanes boolean) LANGUAGE plpgsql AS $$
DECLARE r record; o text; lit text; s record; allequal boolean; alllanes boolean;
BEGIN
	FOR r IN SELECT * FROM ranges ORDER BY col LOOP
		FOREACH o IN ARRAY ARRAY['@>', '<@', '&&', '<<', '>>', '&<', '&>', '-|-'] LOOP
			allequal := true; alllanes := true;
			FOR lit IN EXECUTE format('SELECT quote_literal(%I::text) FROM rconst WHERE %I IS NOT NULL ORDER BY k', r.col, r.col) LOOP
				SELECT * INTO s FROM same(format('SELECT count(*) FROM rt WHERE %I %s %s::%s', r.col, o, lit, r.ty));
				allequal := allequal AND s.equal; alllanes := alllanes AND s.lanes;
			END LOOP;
			col := r.col; op := o; equal := allequal; lanes := alllanes; RETURN NEXT;
		END LOOP;
		allequal := true; alllanes := true;
		FOR lit IN SELECT e FROM econst WHERE econst.col = r.col LOOP
			SELECT * INTO s FROM same(format('SELECT count(*) FROM rt WHERE %I @> %s', r.col, lit));
			allequal := allequal AND s.equal; alllanes := alllanes AND s.lanes;
			SELECT * INTO s FROM same(format('SELECT count(*) FROM rt WHERE %s <@ %I', lit, r.col));
			allequal := allequal AND s.equal; alllanes := alllanes AND s.lanes;
		END LOOP;
		col := r.col; op := 'element'; equal := allequal; lanes := alllanes; RETURN NEXT;
	END LOOP;
END $$;

CREATE TABLE nconst AS SELECT row_number() OVER () k, x::inet n FROM unnest(ARRAY[
	'0.0.0.0/0', '20.0.0.0/8', '20.0.0.0/9', '20.1.0.0/16', '20.1.2.0/24', '20.1.2.3/32', '20.1.2.3/24', '20.1.2.3', '20.255.255.255', '11.0.0.0/8', '30.30.1.0/24', '30.30.1.200/25',
	'255.255.255.255', '0.0.0.0', '::/0', '::1', '::ffff:20.0.0.1', '2001:db8::/32', '2001:db8::/48', '2001:db8:0:1::/64', '2001:db8:0:1::1/128', '2001:db8:0:1::1/65', '2001:db8:0:1::/97',
	'2001:db8:0:1:8000::/65', '2001:db8:0:2::/64', '2001:db8:0:1:ffff:ffff:ffff:ffff', 'fe80::1/64', '8000::/1', 'ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff', '::/128', '2001:db8::/127',
	'2001:db8::/96', '2001:db8::/16']) x;
CREATE FUNCTION net_matrix() RETURNS TABLE (col text, op text, equal boolean, lanes boolean) LANGUAGE plpgsql AS $$
DECLARE c text; o text; lit text; s record; allequal boolean; alllanes boolean;
BEGIN
	FOREACH c IN ARRAY ARRAY['ip', 'cd'] LOOP
		FOREACH o IN ARRAY ARRAY['<<', '<<=', '>>', '>>=', '&&'] LOOP
			allequal := true; alllanes := true;
			FOR lit IN SELECT quote_literal(n::text) FROM nconst ORDER BY k LOOP
				SELECT * INTO s FROM same(format('SELECT count(*) FROM nt WHERE %I %s %s::inet', c, o, lit));
				allequal := allequal AND s.equal; alllanes := alllanes AND s.lanes;
				SELECT * INTO s FROM same(format('SELECT count(*) FROM nt WHERE %s::inet %s %I', lit, o, c));
				allequal := allequal AND s.equal; alllanes := alllanes AND s.lanes;
			END LOOP;
			col := c; op := o; equal := allequal; lanes := alllanes; RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;

SELECT * FROM range_matrix() ORDER BY col, op;
SELECT * FROM net_matrix() ORDER BY col, op;

-- writes after the build (the write buffer), a merge and a REINDEX
INSERT INTO rt (a, d, t, z, w) SELECT int4range(i, i + 3), daterange(date '2000-01-01' + i, date '2000-01-01' + i + 3), tsrange(timestamp '2000-01-01' + i * interval '1 microsecond', NULL),
	int8range(i, NULL), tstzrange(NULL, timestamptz '2000-01-01 00:00+00' + i * interval '1 microsecond', '[]') FROM generate_series(1, 40) i;
INSERT INTO nt (ip) SELECT ('20.1.' || i || '.1/' || (16 + i))::inet FROM generate_series(1, 8) i;
UPDATE nt SET cd = network(ip) WHERE cd IS NULL AND ip IS NOT NULL;
UPDATE rt SET a = int4range(1, 5) WHERE id % 7 = 0;
DELETE FROM rt WHERE id % 11 = 0;
SELECT bool_and(equal), bool_and(lanes) FROM range_matrix();
SELECT bool_and(equal), bool_and(lanes) FROM net_matrix();
VACUUM (FREEZE) rt, nt;
SELECT sift.col_merge('rt_col', true) >= 0, sift.col_merge('nt_col', true) >= 0;
SELECT sift.col_verify('rt_col', true), sift.col_verify('nt_col', true);
SELECT bool_and(equal), bool_and(lanes) FROM range_matrix();
SELECT bool_and(equal), bool_and(lanes) FROM net_matrix();
REINDEX INDEX rt_col;
REINDEX INDEX nt_col;
SELECT bool_and(equal), bool_and(lanes) FROM range_matrix();
SELECT bool_and(equal), bool_and(lanes) FROM net_matrix();

-- a column that is read as a value, grouped and compared keeps working
SELECT (same('SELECT count(*) FROM nt WHERE ip = ''20.1.2.3/24''::inet')).equal, (same('SELECT count(*) FROM nt WHERE cd IN (''20.0.0.0/8'', ''::/0'')')).equal,
       (same('SELECT count(*) FROM rt WHERE a = int4range(1, 5)')).equal, (same('SELECT count(*) FROM rt WHERE t = ''empty''::tsrange')).equal;
SELECT count(DISTINCT a), count(*) FILTER (WHERE a = int4range(1, 5)), count(*) FILTER (WHERE a IS NULL) FROM rt;
SELECT count(*) FROM (SELECT ip FROM nt GROUP BY ip) g;

DROP TABLE rt, nt, ranges, rconst, econst, nconst;
DROP FUNCTION same(text), range_matrix(), net_matrix();
DROP EXTENSION pg_sift;
