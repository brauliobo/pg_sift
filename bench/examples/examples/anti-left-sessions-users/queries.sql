-- query: sessions-without-user-left-null
-- kind: anti_join_count
-- join: left
SELECT count(*) AS sessions FROM session s LEFT JOIN users u ON u.id = s.user_id WHERE u.id IS NULL;

-- query: orphan-sessions-not-exists
-- kind: anti_join_count
-- join: anti
-- plan: SiftAgg
-- found: yes
SELECT count(*) AS orphans FROM session s WHERE s.user_id IS NOT NULL AND NOT EXISTS (SELECT 1 FROM users u WHERE u.id = s.user_id);

-- query: users-without-sessions-by-plan
-- kind: anti_join_count
-- join: anti
SELECT u.plan, count(*) AS users FROM users u WHERE NOT EXISTS (SELECT 1 FROM session s WHERE s.user_id = u.id) GROUP BY u.plan;

-- query: sessions-by-plan-null-extended
-- kind: group_by_dim_attr
-- join: left
SELECT u.plan, count(*) AS sessions, count(u.id) AS matched, sum(s.duration) AS duration FROM session s LEFT JOIN users u ON u.id = s.user_id GROUP BY u.plan;

-- query: sessions-not-in-plan
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS sessions FROM session s WHERE s.user_id NOT IN (SELECT id FROM users WHERE plan = 'w1');

-- query: full-join-matching-counts
-- kind: anti_join_count
-- join: full
SELECT count(*) FILTER (WHERE s.id IS NULL) AS users_only, count(*) FILTER (WHERE u.id IS NULL) AS sessions_only,
       count(*) FILTER (WHERE s.id IS NOT NULL AND u.id IS NOT NULL) AS both FROM session s FULL JOIN users u ON u.id = s.user_id;

-- query: right-join-users-by-country
-- kind: group_by_dim_attr
-- join: right
SELECT u.country, count(s.id) AS sessions, count(*) AS rows FROM session s RIGHT JOIN users u ON u.id = s.user_id GROUP BY u.country;

-- query: distinct-users-by-plan
-- kind: count_distinct_across_join
-- join: inner
SELECT u.plan, count(DISTINCT s.user_id) AS users, avg(s.duration)::numeric(10,2) AS avg_duration FROM session s JOIN users u ON u.id = s.user_id GROUP BY u.plan;

-- query: top-users-by-duration
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT u.id, u.plan, sum(s.duration) AS duration FROM session s JOIN users u ON u.id = s.user_id GROUP BY u.id, u.plan ORDER BY duration DESC, u.id LIMIT 10;

-- query: sessions-with-user-semi
-- kind: semi_join_count
-- join: semi
-- plan: SiftAgg
-- found: yes
SELECT count(*) AS sessions FROM session s WHERE EXISTS (SELECT 1 FROM users u WHERE u.id = s.user_id);

-- query: sessions-with-user-join-count
-- kind: semi_join_count
-- join: inner
-- plan: SiftAgg
-- found: yes
SELECT count(*) AS sessions FROM session s JOIN users u ON u.id = s.user_id;

-- query: sessions-in-users-duration
-- kind: semi_join_count
-- join: semi
-- plan: SiftAgg
-- found: yes
SELECT count(*) AS sessions, sum(s.duration) AS duration FROM session s WHERE s.user_id IN (SELECT id FROM users);
