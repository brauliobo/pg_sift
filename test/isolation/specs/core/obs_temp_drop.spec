# The observability views list what the caller can read and never fail on a relation of another session: a temp sift_table, sift_col or sift index of another session is not
# listed (0A000 before), and a relation dropped by a transaction that commits while the view runs is listed as locked without a wait, not opened after the drop (42P01 before).
# requires: tam col

setup
{
	CREATE TABLE iso_ok (id int, v int) USING sift_table;
	CREATE TABLE iso_od (id int, v int, body text);
	INSERT INTO iso_ok SELECT g, g % 7 FROM generate_series(1, 100) g;
	INSERT INTO iso_od SELECT g, g % 7, 'w' || g FROM generate_series(1, 100) g;
	CREATE INDEX iso_od_col ON iso_od USING sift_col (id, v);
	CREATE INDEX iso_od_bm ON iso_od USING sift (body);
}

teardown
{
	DROP TABLE iso_ok;
	DROP TABLE IF EXISTS iso_od;
}

session s1
step s1temp
{
	CREATE TEMP TABLE iso_tt (id int, v int) USING sift_table;
	INSERT INTO iso_tt SELECT g, g % 7 FROM generate_series(1, 100) g;
	CREATE TEMP TABLE iso_tc (id int, v int, body text);
	INSERT INTO iso_tc SELECT g, g % 7, 'w' || g FROM generate_series(1, 100) g;
	CREATE INDEX iso_tc_col ON iso_tc USING sift_col (id, v);
	CREATE INDEX iso_tc_bm ON iso_tc USING sift (body);
}
step s1untemp	{ DROP TABLE iso_tt, iso_tc; }
step s1begin	{ BEGIN; }
step s1drop		{ DROP TABLE iso_od; }
step s1commit	{ COMMIT; }

session s2
step s2indexes	{ SELECT relation::text, state FROM sift.indexes ORDER BY 1; }
step s2views
{
	SELECT (SELECT count(*) FROM sift.indexes) AS indexes, (SELECT count(*) FROM sift.status()) AS status,
		   (SELECT count(*) >= 0 FROM sift.health()) AS health, (SELECT count(*) >= 0 FROM sift.locks) AS locks, (SELECT count(*) >= 1 FROM sift.activity()) AS activity;
}

# the temp relations of s1 are not listed by s2
permutation s1temp s2indexes s2views s1untemp
# the views read a relation that a transaction drops and commits meanwhile
permutation s1begin s1drop s2indexes s1commit s2views
