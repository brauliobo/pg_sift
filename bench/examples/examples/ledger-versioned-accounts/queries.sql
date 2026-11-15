-- query: amount-by-segment-at-posting
-- kind: range_join_versioned
-- join: inner
SELECT v.segment, count(*) AS n, sum(e.amount) AS amount FROM entry e JOIN account_version v
 ON v.account_id = e.account_id AND e.posted_on >= v.valid_from AND e.posted_on < v.valid_to GROUP BY v.segment;

-- query: high-risk-entries-by-kind
-- kind: filter_dim_attr
-- join: inner
SELECT e.kind, count(*) AS n, sum(e.amount) AS amount FROM entry e JOIN account_version v
 ON v.account_id = e.account_id AND e.posted_on >= v.valid_from AND e.posted_on < v.valid_to WHERE v.risk >= 4 GROUP BY e.kind;

-- query: current-version-segment
-- kind: group_by_dim_attr
-- join: inner
SELECT v.segment, count(DISTINCT e.account_id) AS accounts, sum(e.amount) AS amount FROM entry e JOIN account a ON a.id = e.account_id
JOIN account_version v ON v.account_id = a.id AND v.valid_to = date '9999-12-31' GROUP BY v.segment;

-- query: distinct-accounts-by-segment-at-posting
-- kind: count_distinct_across_join
-- join: inner
SELECT v.segment, count(DISTINCT e.account_id) AS accounts FROM entry e JOIN account_version v
 ON v.account_id = e.account_id AND e.posted_on BETWEEN v.valid_from AND v.valid_to - 1 GROUP BY v.segment;

-- query: entries-without-version
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS uncovered, sum(e.amount) AS amount FROM entry e
WHERE NOT EXISTS (SELECT 1 FROM account_version v WHERE v.account_id = e.account_id AND e.posted_on >= v.valid_from AND e.posted_on < v.valid_to);

-- query: entries-by-segment-left
-- kind: group_by_dim_attr
-- join: left
SELECT v.segment, count(*) AS n, count(v.account_id) AS covered FROM entry e LEFT JOIN account_version v
 ON v.account_id = e.account_id AND e.posted_on >= v.valid_from AND e.posted_on < v.valid_to GROUP BY v.segment;

-- query: top-accounts-in-segment
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT e.account_id, sum(e.amount) AS amount FROM entry e JOIN account_version v
 ON v.account_id = e.account_id AND e.posted_on >= v.valid_from AND e.posted_on < v.valid_to WHERE v.segment = 'w1' GROUP BY e.account_id ORDER BY amount DESC, e.account_id LIMIT 10;

-- query: debit-credit-by-risk
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT v.risk, sum(e.amount) FILTER (WHERE e.amount > 0) AS credit, sum(e.amount) FILTER (WHERE e.amount < 0) AS debit, count(*) AS n
FROM entry e JOIN account_version v ON v.account_id = e.account_id AND e.posted_on >= v.valid_from AND e.posted_on < v.valid_to GROUP BY v.risk;
