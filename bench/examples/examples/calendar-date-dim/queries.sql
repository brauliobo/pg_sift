-- query: sales-by-quarter
-- kind: group_by_dim_attr
-- join: inner
SELECT c.quarter, count(*) AS n, sum(s.amount) AS amount FROM sale s JOIN calendar c ON c.day = s.day GROUP BY c.quarter;

-- query: holiday-sales-by-month
-- kind: filter_dim_attr
-- join: inner
SELECT c.month, count(*) AS n, sum(s.amount) AS amount FROM sale s JOIN calendar c ON c.day = s.day WHERE c.is_holiday AND c.dow <> 0 GROUP BY c.month;

-- query: distinct-places-by-weekday
-- kind: count_distinct_across_join
-- join: inner
SELECT c.dow, count(DISTINCT s.store) AS places, count(DISTINCT s.day) AS days FROM sale s JOIN calendar c ON c.day = s.day GROUP BY c.dow;

-- query: sales-days-beyond-calendar
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS n, count(DISTINCT s.day) AS days FROM sale s WHERE NOT EXISTS (SELECT 1 FROM calendar c WHERE c.day = s.day);

-- query: calendar-days-with-no-sales
-- kind: anti_join_count
-- join: right
SELECT c.quarter, count(*) AS days FROM sale s RIGHT JOIN calendar c ON c.day = s.day WHERE s.id IS NULL GROUP BY c.quarter;

-- query: zero-filled-days-by-month
-- kind: group_by_dim_attr
-- join: left
SELECT c.month, count(DISTINCT c.day) AS days, count(s.id) AS sales, coalesce(sum(s.amount), 0) AS amount FROM calendar c LEFT JOIN sale s ON s.day = c.day GROUP BY c.month;

-- query: top-days
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT c.day, c.dow, sum(s.amount) AS amount FROM sale s JOIN calendar c ON c.day = s.day GROUP BY c.day, c.dow ORDER BY amount DESC, c.day LIMIT 10;

-- query: weekend-and-holiday-mix
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT c.quarter, sum(s.amount) FILTER (WHERE c.dow IN (0, 6)) AS weekend, sum(s.amount) FILTER (WHERE c.is_holiday) AS holiday, count(*) AS n FROM sale s JOIN calendar c ON c.day = s.day GROUP BY c.quarter;

-- query: sales-in-holiday-windows
-- kind: range_join_versioned
-- join: inner
SELECT c.quarter, count(*) AS n FROM sale s JOIN calendar c ON s.day >= c.day AND s.day < c.day + 1 AND c.is_holiday GROUP BY c.quarter;
