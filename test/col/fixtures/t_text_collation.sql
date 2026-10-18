-- trap: min and max of text under a non-C collation (the database is C)
-- shape: trap
-- expect: any
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'und-x-icu')
-- key: -
-- scenario: yes
SELECT grp, min(t COLLATE "und-x-icu") AS icu_lo, max(t COLLATE "und-x-icu") AS icu_hi, min(t COLLATE "C") AS c_lo, max(t COLLATE "C") AS c_hi,
       count(DISTINCT t) AS n
FROM t_text GROUP BY grp ORDER BY grp
