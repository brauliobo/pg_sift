-- query: hours-by-manager-dept
-- kind: self_join
-- join: inner
SELECT m.dept, count(*) AS sheets, sum(t.hours) AS hours FROM timesheet t JOIN employee e ON e.id = t.employee_id JOIN employee m ON m.id = e.manager_id GROUP BY m.dept;

-- query: reports-per-manager
-- kind: self_join
-- join: inner
SELECT m.level, count(*) AS reports, count(DISTINCT m.id) AS managers FROM employee e JOIN employee m ON m.id = e.manager_id GROUP BY m.level;

-- query: employees-without-reports
-- kind: anti_join_count
-- join: anti
SELECT e.dept, count(*) AS leaves FROM employee e WHERE NOT EXISTS (SELECT 1 FROM employee r WHERE r.manager_id = e.id) GROUP BY e.dept;

-- query: same-dept-as-manager
-- kind: self_join
-- join: inner
SELECT count(*) AS same_dept, count(*) FILTER (WHERE e.active AND m.active) AS both_active FROM employee e JOIN employee m ON m.id = e.manager_id WHERE e.dept = m.dept;

-- query: skip-level-chain
-- kind: chain
-- join: inner
SELECT g.dept, count(*) AS n FROM employee e JOIN employee m ON m.id = e.manager_id JOIN employee g ON g.id = m.manager_id GROUP BY g.dept;

-- query: top-managers-by-team-hours
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT e.manager_id, sum(t.hours) AS hours FROM timesheet t JOIN employee e ON e.id = t.employee_id WHERE e.manager_id IS NOT NULL GROUP BY e.manager_id ORDER BY hours DESC, e.manager_id LIMIT 10;

-- query: hours-by-level-and-manager-status
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT e.level, sum(t.hours) FILTER (WHERE m.active) AS under_active, sum(t.hours) FILTER (WHERE NOT m.active) AS under_inactive, count(DISTINCT t.employee_id) AS people
FROM timesheet t JOIN employee e ON e.id = t.employee_id JOIN employee m ON m.id = e.manager_id GROUP BY e.level;

-- query: employees-without-timesheets
-- kind: anti_join_count
-- join: left
SELECT count(*) AS idle FROM employee e LEFT JOIN timesheet t ON t.employee_id = e.id WHERE t.id IS NULL;

-- query: reports-within-two-levels-of-root
-- kind: graph_edges
-- join: inner
SELECT count(*) AS people, count(DISTINCT g.id) AS reach FROM employee m JOIN employee e ON e.manager_id = m.id LEFT JOIN employee g ON g.manager_id = e.id WHERE m.manager_id IS NULL;
