-- count(DISTINCT) of a text and an integer column over partitions that have their own dictionaries
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(DISTINCT tag) AS tags, count(DISTINCT qty) AS quantities, count(DISTINCT d) AS days FROM t_part GROUP BY grp ORDER BY grp
