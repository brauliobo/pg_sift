-- SQL keys and the grouping-set discriminator share the mask's full key space.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather=0;
CREATE TABLE events(id int,a int,b int,c int,d int,e int,f int,g int,h int,payload text);
INSERT INTO events SELECT i,i%3,i%2,i%3,i%2,i%3,i%2,i%3,
 CASE WHEN i%11=0 THEN NULL ELSE i%2 END,repeat('payload',50) FROM generate_series(1,100000)i;
CREATE INDEX events_col ON events USING sift_col(id,a,b,c,d,e,f,g,h);
VACUUM(FREEZE,ANALYZE)events;
CREATE FUNCTION check_query(q text)RETURNS TABLE(pushed bool,differences bigint)LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
 SET LOCAL sift.enable_agg=off;
 EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS '||q;
 SET LOCAL sift.enable_agg=on;
 EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS '||q;
 SELECT count(*)INTO differences FROM((TABLE r_off EXCEPT ALL TABLE r_on)UNION ALL(TABLE r_on EXCEPT ALL TABLE r_off))d;
 pushed:=false;
 FOR line IN EXECUTE 'EXPLAIN(COSTS OFF) '||q LOOP pushed:=pushed OR line LIKE '%Custom Scan (SiftAgg)%';END LOOP;
 DROP TABLE r_off,r_on;
 RETURN NEXT;
END$$;
-- Nine physical keys: eight ordinary SQL columns and the set number.
-- A repeated set preserves duplicate rows; GROUPING distinguishes a real NULL key.
SELECT * FROM check_query($q$
 SELECT a,b,c,d,e,f,g,h,GROUPING(a,h) AS mask,count(*) AS n,count(DISTINCT id) AS ids
 FROM events GROUP BY GROUPING SETS((a,b,c,d,e,f,g,h),(a,b,c,d,e,f,g,h),(a,h),())
$q$);
-- Many keys must start in hash mode instead of overflowing a product of direct capacities.
-- Repeated expressions with different types remain distinct SQL group keys.
SELECT * FROM check_query($q$
 SELECT a AS a1, a::bigint AS a2, a::text AS a3, b AS b1, b::bigint AS b2, b::text AS b3,
 c AS c1, c::bigint AS c2, c::text AS c3, d AS d1, d::bigint AS d2, d::text AS d3,
 e AS e1, e::bigint AS e2, e::text AS e3, f AS f1, f::bigint AS f2, f::text AS f3,
 g AS g1, g::bigint AS g2, g::text AS g3, h AS h1, h::bigint AS h2, h::text AS h3,count(*) AS n
 FROM events GROUP BY 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24
$q$);
DROP FUNCTION check_query(text);
DROP TABLE events;
DROP EXTENSION pg_sift;
