-- inet, cidr, macaddr, macaddr8, timetz, bit and varbit in a sift_col index: the column engine stores the image of the
-- datum and SiftAgg reads it as a dictionary of datums, so a key, an aggregate, a conjunct, an OR, a FILTER or an expression is PostgreSQL's own
-- function on the value (equality, hash and order are the type's; only a bit or varbit key, which has no hash, is grouped by image). Every query runs with and without the
-- pushdown and the rows must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;

CREATE TABLE nt (id int, grp int, ip inet, cd cidr, mac macaddr, mac8 macaddr8, tz timetz, b bit(8), vb varbit, pt point)
	WITH (autovacuum_enabled = false);
INSERT INTO nt SELECT i, i % 7,
	CASE WHEN i % 9 = 0 THEN NULL ELSE (ARRAY['10.0.0.1', '10.0.0.1/24', '10.0.0.1/32', '10.0.0.2/24', '192.168.1.5', '0.0.0.0/0', '::1', '2001:db8::1',
		'2001:0db8:0:0:0:0:0:1', '2001:db8::1/64', '::ffff:10.0.0.1', '::ffff:10.0.0.1/128', '10.0.0.0/8'])[1 + i % 13]::inet END,
	CASE WHEN i % 10 = 0 THEN NULL ELSE (ARRAY['10.0.0.0/24', '10.0.0.0/8', '192.168.0.0/16', '::/0', '2001:db8::/32'])[1 + i % 5]::cidr END,
	CASE WHEN i % 11 = 0 THEN NULL ELSE (ARRAY['08:00:2b:01:02:03', '08-00-2B-01-02-03', '0800.2b01.0203', 'AA:BB:CC:DD:EE:FF', 'ff:ff:ff:ff:ff:ff'])[1 + i % 5]::macaddr END,
	CASE WHEN i % 12 = 0 THEN NULL ELSE (ARRAY['08:00:2b:01:02:03:04:05', '08-00-2B-01-02-03-04-05', 'aabbccddeeff0011', 'ff:ff:ff:ff:ff:ff:ff:ff'])[1 + i % 4]::macaddr8 END,
	CASE WHEN i % 13 = 0 THEN NULL ELSE (ARRAY['12:00+01', '11:00+00', '12:00+00', '24:00:00-15:59', '00:00:00+15:59', '04:05:06.5-03'])[1 + i % 6]::timetz END,
	CASE WHEN i % 14 = 0 THEN NULL ELSE (ARRAY[B'00000000', B'00000001', B'10000000', B'11111111'])[1 + i % 4] END,
	CASE WHEN i % 15 = 0 THEN NULL WHEN i % 97 = 0 THEN ('x' || (SELECT string_agg(md5(i || '-' || k), '') FROM generate_series(1, 400) k))::varbit
		ELSE (ARRAY['', '0', '00', '000', '1', '10'])[1 + i % 6]::varbit END,
	point(i % 5, i % 3)
FROM generate_series(1, 6000) i;
CREATE INDEX nt_col ON nt USING sift_col (id, grp, ip, cd, mac, mac8, tz, b, vb);
VACUUM (FREEZE, ANALYZE) nt;
SELECT sift.col_verify('nt_col', true);
SELECT leaf, path, type, kind FROM sift.col_leaves('nt_col') ORDER BY leaf;
SELECT format FROM sift.col_segments('nt_col') WHERE kind = 'immutable';

-- the rows of both plans as the text PostgreSQL prints them (the columns of a query are named), or their error; the off plan hashes (a sort keeps an arbitrary one of equal keys)
CREATE FUNCTION chk(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.agg_check_outcome(q) $$;


-- keys
SELECT chk($$SELECT ip::text AS k, count(*) AS c FROM nt GROUP BY ip$$) AS inet_key,
       chk($$SELECT cd::text AS k, count(*) AS c FROM nt GROUP BY cd$$) AS cidr_key,
       chk($$SELECT mac::text AS k, count(*) AS c FROM nt GROUP BY mac$$) AS macaddr_key,
       chk($$SELECT mac8::text AS k, count(*) AS c FROM nt GROUP BY mac8$$) AS macaddr8_key,
       chk($$SELECT tz::text AS k, count(*) AS c FROM nt GROUP BY tz$$) AS timetz_key,
       chk($$SELECT b::text AS k, count(*) AS c FROM nt GROUP BY b$$) AS bit_key,
       chk($$SELECT length(vb) AS k, count(*) AS c, count(DISTINCT vb) AS d FROM nt GROUP BY length(vb)$$) AS varbit_length,
       chk($$SELECT count(*) AS groups FROM (SELECT vb FROM nt GROUP BY vb) g$$) AS varbit_groups,
       chk($$SELECT ip::text AS a, mac::text AS b, count(*) AS c FROM nt GROUP BY ip, mac$$) AS two_keys;
-- equal values are one group whatever their spelling, different masks and different zones are not
SELECT count(*) AS inet_groups, count(DISTINCT ip) AS inet_distinct FROM (SELECT ip FROM nt GROUP BY ip) g;
SET sift.enable_agg = off;
SELECT count(*) AS inet_groups_heap FROM (SELECT ip FROM nt GROUP BY ip) g;
SELECT count(*) AS mac_groups_heap, (SELECT count(*) FROM (SELECT tz FROM nt GROUP BY tz) g) AS timetz_groups_heap FROM (SELECT mac FROM nt GROUP BY mac) g;
RESET sift.enable_agg;
SELECT count(*) AS mac_groups, (SELECT count(*) FROM (SELECT tz FROM nt GROUP BY tz) g) AS timetz_groups FROM (SELECT mac FROM nt GROUP BY mac) g;
-- aggregates, conjuncts, OR, FILTER, expressions
SELECT chk($$SELECT grp AS g, min(ip)::text AS a, max(ip)::text AS b, min(cd)::text AS c, max(cd)::text AS d, min(tz)::text AS e, max(tz)::text AS f FROM nt GROUP BY grp$$) AS minmax,
       chk($$SELECT grp AS g, count(DISTINCT ip) AS a, count(DISTINCT cd) AS b, count(DISTINCT mac) AS c, count(DISTINCT mac8) AS d, count(DISTINCT tz) AS e, count(DISTINCT b) AS f, count(DISTINCT vb) AS h FROM nt GROUP BY grp$$) AS distinct_,
       chk($$SELECT count(*) AS c FROM nt WHERE ip = '10.0.0.1'$$) AS inet_eq,
       chk($$SELECT count(*) AS c FROM nt WHERE ip = '10.0.0.1/24' OR ip = '10.0.0.1/32'$$) AS inet_mask,
       chk($$SELECT count(*) AS lt, count(*) FILTER (WHERE ip > '10.0.0.1/24') AS gt, count(*) FILTER (WHERE ip <> '::1') AS ne FROM nt WHERE ip < '192.168.0.0'$$) AS inet_order,
       chk($$SELECT count(*) AS inside, count(*) FILTER (WHERE ip >>= '10.0.0.1/32') AS contains, count(*) FILTER (WHERE ip && '10.0.0.0/24') AS overlaps FROM nt WHERE ip <<= '10.0.0.0/8'$$) AS inet_containment,
       chk($$SELECT count(*) AS c FROM nt WHERE ip IN ('::1', '2001:db8::1', '10.0.0.0/8') OR cd >>= '10.0.0.7'$$) AS in_lists,
       chk($$SELECT count(*) AS c FROM nt WHERE mac = '08:00:2b:01:02:03' AND mac8 <> 'aabbccddeeff0011'$$) AS mac_conjuncts,
       chk($$SELECT count(*) AS c FROM nt WHERE tz = '12:00+01' OR tz > '12:00+00'$$) AS timetz_conjuncts,
       chk($$SELECT count(*) AS c FROM nt WHERE b = B'00000001' OR vb = B'0'$$) AS bit_conjuncts,
       chk($$SELECT count(*) AS c FROM nt WHERE vb = B'0' OR vb = B'00'$$) AS varbit_length_matters,
       chk($$SELECT grp AS g, count(*) FILTER (WHERE tz < '12:00+00') AS c1, count(*) FILTER (WHERE ip <<= '10.0.0.0/8') AS c2, count(*) FILTER (WHERE vb > B'0') AS c3 FROM nt GROUP BY grp$$) AS filter_,
       chk($$SELECT host(ip) AS h, family(ip) AS f, masklen(ip) AS m, count(*) AS c FROM nt GROUP BY 1, 2, 3$$) AS inet_expressions,
       chk($$SELECT CASE WHEN ip <<= '10.0.0.0/8' THEN 'ten' WHEN family(ip) = 6 THEN 'six' ELSE 'other' END AS k, count(*) AS c FROM nt GROUP BY 1$$) AS case_,
       chk($$SELECT trunc(mac)::text AS k, count(*) AS c FROM nt GROUP BY 1$$) AS mac_trunc,
       chk($$SELECT max(tz::time)::text AS a, max(extract(hour FROM tz)) AS b FROM nt$$) AS timetz_expressions,
       chk($$SELECT (b & B'00001111')::text AS k, sum(bit_count(vb)) AS s FROM nt GROUP BY 1$$) AS bit_expressions,
       chk($$SELECT count(*) AS c FROM nt WHERE ip IS NULL OR cd IS NULL OR mac IS NULL OR mac8 IS NULL OR tz IS NULL OR b IS NULL OR vb IS NULL$$) AS nulls;
-- a column no index holds keeps the normal plan
SELECT chk($$SELECT pt::text AS k, count(*) AS c FROM nt GROUP BY pt::text$$) AS point_key,
       chk($$SELECT grp AS g, count(DISTINCT ip) AS c FROM nt GROUP BY grp$$) AS the_rest;

-- parameters
PREPARE p_n(inet, timetz, varbit) AS SELECT grp AS g, count(*) AS c FROM nt WHERE ip <<= $1 OR tz = $2 OR vb = $3 GROUP BY grp;
SET plan_cache_mode = force_generic_plan;
SELECT chk($$EXECUTE p_n('10.0.0.0/8', '12:00+01', B'00')$$) AS generic, chk($$EXECUTE p_n(NULL, NULL, NULL)$$) AS generic_null;
RESET plan_cache_mode;
DEALLOCATE ALL;

-- the write buffer, DELETE, UPDATE, merge
INSERT INTO nt SELECT 10000 + i, i % 7, ((i % 250) || '.1.2.3/' || (8 + i % 20))::inet, ('10.' || i % 250 || '.0.0/16')::cidr, ('00:11:22:33:44:' || lpad(to_hex(i % 256), 2, '0'))::macaddr,
	('00:11:22:ff:fe:33:44:' || lpad(to_hex(i % 256), 2, '0'))::macaddr8, ((i % 24) || ':00:00' || (CASE WHEN i % 2 = 0 THEN '+' ELSE '-' END) || (i % 12))::timetz, (i % 256)::bit(8), (i % 64)::bit(6)::varbit, NULL
FROM generate_series(1, 1000) i;
CREATE FUNCTION shapes(tag text) RETURNS text LANGUAGE sql AS $$
	SELECT concat_ws(' ', tag,
		chk('SELECT ip::text AS k, count(*) AS c FROM nt GROUP BY ip'),
		chk('SELECT cd::text AS k, count(*) AS c, min(ip)::text AS a FROM nt GROUP BY cd'),
		chk('SELECT mac::text AS k, count(*) AS c FROM nt GROUP BY mac'),
		chk('SELECT tz::text AS k, count(*) AS c, max(tz)::text AS a FROM nt GROUP BY tz'),
		chk('SELECT b::text AS k, count(DISTINCT vb) AS c FROM nt GROUP BY b'),
		chk('SELECT grp AS g, count(*) AS c FROM nt WHERE ip <<= ''10.0.0.0/8'' OR tz > ''12:00+00'' GROUP BY grp'))
$$;
SELECT shapes('buffered');
DELETE FROM nt WHERE id % 5 = 1;
UPDATE nt SET ip = '10.9.9.9/24', tz = '01:02:03+04', vb = B'1111', mac = 'AA-BB-CC-00-00-01' WHERE id % 11 = 2;
SELECT shapes('updated');
SELECT sift.col_promote('nt_col');
VACUUM nt;
SELECT sift.col_merge('nt_col', true);
SELECT shapes('merged');
SELECT sift.col_verify('nt_col', true);
REINDEX INDEX nt_col;
SELECT shapes('reindexed');

-- parallel workers
RESET seq_page_cost;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET seq_page_cost = 1000;
SELECT chk($$SELECT grp AS g, count(*) AS c, min(ip)::text AS a, max(tz)::text AS b FROM nt GROUP BY grp$$) AS parallel_aggs,
       chk($$SELECT ip::text AS k, count(*) AS c FROM nt GROUP BY ip$$) AS parallel_key;
RESET max_parallel_workers_per_gather;

-- an index of old types still writes the old minors; a sift_table keeps its own layout for these types
CREATE TABLE ot (id int, grp int, v text);
INSERT INTO ot SELECT i, i % 5, i::text FROM generate_series(1, 2000) i;
CREATE INDEX ot_col ON ot USING sift_col (id, grp, v);
SELECT format FROM sift.col_segments('ot_col') WHERE kind = 'immutable';
CREATE TABLE tt (id int, ip inet, cd cidr, mac macaddr, mac8 macaddr8, tz timetz, b bit(8), vb varbit) USING sift_table;
INSERT INTO tt SELECT i, ('10.0.' || i % 250 || '.1')::inet, ('10.' || i % 250 || '.0.0/16')::cidr, '08:00:2b:01:02:03', '08:00:2b:01:02:03:04:05', '12:00+01', B'00000001', '10'::varbit FROM generate_series(1, 20000) i;
VACUUM tt;
SELECT kind, format, count(*) FROM sift.table_segments('tt') GROUP BY 1, 2 ORDER BY 1, 2;
SELECT count(DISTINCT ip) AS ips, count(DISTINCT cd) AS cds, max(ip)::text AS mx, min(tz)::text AS mn FROM tt;
DROP TABLE tt, ot;

-- CREATE INDEX still refuses what has no leaf form (the point column above is simply left out of nt_col)
CREATE TABLE rt (id int, p point, tv tsvector, x xml, j json);
CREATE INDEX rt_p ON rt USING sift_col (id, p);
CREATE INDEX rt_t ON rt USING sift_col (id, tv);
CREATE INDEX rt_x ON rt USING sift_col (id, x);
CREATE INDEX rt_j ON rt USING sift_col (id, j);
DROP TABLE rt;

DROP TABLE nt;
DROP FUNCTION chk(text), shapes(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
