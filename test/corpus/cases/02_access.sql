-- ===================== predicates
-- @case pred_eq_low_card | pred | equality low cardinality
select count(*) from events where status = 'paid';
-- @case pred_eq_high_card | pred | equality high cardinality label
select count(*), sum(amount) from events where label = 'label_' || (@K@ % 500);
-- @case pred_in_list_status | pred | IN list of 2 statuses plus range
select count(*) from events where status in ('new', 'closed') and d between '2025-01-01' and '2025-06-30';
-- @case pred_range_amount | pred | range on a measure
select count(*) from events where amount between 100 and 110;
-- @case pred_null_heavy | pred | IS NULL on a 60% null column
select count(*) from events where note is null and status = 'open';
-- @case pred_like_prefix | pred | LIKE prefix
select count(*) from events where label like 'label_1234%';
-- @case pred_like_suffix | pred | LIKE suffix
select count(*) from events where label like '%99';
-- @case pred_like_contains | pred | LIKE contains
select count(*) from events where note like '%urgent%';
-- @case pred_ilike | pred | ILIKE
select count(*) from events where note ilike 'NOTE 12%';
-- @case pred_regex | pred | regex
select count(*) from events where note ~ '^note [0-9]{3} urgent$';
-- @case pred_jsonb_contains | pred | jsonb containment
select count(*) from events where attrs @> '{"b":"x","flag":true}';
-- @case pred_jsonb_contains_not_true | pred | jsonb containment IS NOT TRUE
select count(*) from events where (attrs @> '{"b":"x"}') is not true;
-- @case agg_jsonb_contains_case_pivot | agg | CASE on a jsonb containment, ELSE 0
select kind_id, sum(case when attrs @> '{"b":"x"}' then 1 else 0 end) as hits, sum(case when attrs @> '{"b":"x"}' then amount else 0 end) as hit_amount from events group by 1;
-- @case agg_jsonb_contains_filter_not_true | agg | FILTER on a jsonb containment IS NOT TRUE
select status, count(*) filter (where (attrs @> '{"b":"x"}') is not true) as other, count(*) filter (where attrs @> '{"b":"x"}') as hits from events group by 1;
-- @case pred_jsonb_contains_or_not_true | pred | jsonb containment OR a comparison, IS NOT TRUE
select count(*) from events where ((attrs @> '{"b":"x"}') or kind_id = 3) is not true and dim_a_id < 500;
-- @case pred_jsonb_not_contains | pred | NOT of a jsonb containment, the column may be NULL
select count(*) from events where not (attrs @> '{"b":"x"}');
-- @case pred_jsonb_contains_is_false | pred | jsonb containment IS FALSE, the column may be NULL
select count(*), sum(amount) from events where (attrs @> '{"b":"x"}') is false;
-- @case agg_jsonb_not_contains_filter | agg | FILTER on NOT of a jsonb containment, the column may be NULL
select status, count(*) filter (where not (attrs @> '{"b":"x"}')) as other, count(*) filter (where attrs @> '{"b":"x"}') as hits from events group by 1;
-- @case pred_jsonb_is_null | pred | jsonb column IS NULL
select count(*) filter (where attrs is null) as no_doc, count(*) filter (where attrs is not null) as doc from events;
-- @case pred_jsonb_contains_and_is_false | pred | jsonb containment ANDed with a comparison, IS FALSE, the column may be NULL
select count(*), sum(amount) from events where (attrs @> '{"b":"x"}' and kind_id = 3) is false;
-- @case agg_jsonb_not_key_exists_filter | agg | FILTER on NOT of a jsonb key test, the column may be NULL
select status, count(*) filter (where not (attrs ? 'b')) as no_key, count(*) filter (where attrs ? 'b') as key from events group by 1;
-- @case pred_jsonb_is_null_is_false | pred | jsonb column (IS NULL) IS FALSE
select count(*) from events where (attrs is null) is false;
-- @case pred_jsonb_path | pred | jsonb path value
select attrs->>'b', count(*), avg((attrs->>'a')::float8) from events group by 1;
-- @case pred_array_overlap | pred | int[] overlap
select count(*) from events where tags && array[3, 7];
-- @case pred_array_contains | pred | int[] contains
select count(*) from events where tags @> array[5];
-- @case pred_or_mix | pred | multi-column AND/OR mix
select count(*) from events where (status = 'paid' and amount > 900) or (status = 'new' and kind_id = 3) or dim_a_id = @K@;
-- @case pred_correlated_scalar | pred | correlated scalar subquery in the select list
select e.id, (select count(*) from events v where v.dim_a_id = e.id) c from dim_a e where e.id <= 200;
-- @case pred_union_all_arms | pred | UNION ALL of two filtered aggregates
select 'a' k, count(*) from events where d < '2024-06-01' union all select 'b', count(*) from events where d >= '2026-06-01';
-- @case pred_cte_inlined | pred | CTE referenced twice (materialized by planner)
with s as (select dim_a_id, sum(amount) a from events group by 1) select (select max(a) from s), (select count(*) from s where a > 100000);

-- ===================== access patterns
-- @case acc_point_pk | access | point lookup by primary key
select * from events where id = 1 + @K@ * 100;
-- @case acc_point_in_small | access | small IN list on PK
select id, amount from events where id in (11, 2222, 33333, 444444, 555555);
-- @case acc_entity_list | access | list rows of one entity (limit 200)
select id, d, amount, status from events where dim_a_id = @K@ order by d desc, id limit 200;
-- @case acc_entity_count_date | access | one entity count in a date window
select count(*), sum(amount) from events where dim_a_id = @K@ and d between '2025-01-01' and '2025-12-31';
-- @case acc_prepared_generic | access | prepared statement under force_generic_plan
-- @pre set plan_cache_mode = force_generic_plan; prepare pq(int) as select status, count(*), sum(amount) from events where dim_a_id = $1 and d >= date '2025-01-01' group by 1;
execute pq(@K@);
-- @case acc_prepared_custom | access | prepared statement under force_custom_plan
-- @pre set plan_cache_mode = force_custom_plan; prepare pq(int) as select status, count(*), sum(amount) from events where dim_a_id = $1 and d >= date '2025-01-01' group by 1;
execute pq(@K@);
-- @case acc_prepared_auto | access | prepared statement under auto (5 custom then generic)
-- @pre prepare pq(int) as select status, count(*), sum(amount) from events where dim_a_id = $1 and d >= date '2025-01-01' group by 1;
execute pq(@K@);
-- @case acc_prepared_generic_set | access | generic plan with an array parameter
-- @pre set plan_cache_mode = force_generic_plan; prepare pa(int[]) as select count(*), sum(amount) from events where dim_a_id = any ($1);
execute pa(array[@K@, @K2@, 17, 33]);
-- @case acc_partition_prune | access | partition pruning by date
select status, count(*), sum(amount) from events_p where d >= '2026-01-01' group by 1;
-- @case acc_partition_entity | access | partitioned table by entity
select count(*), sum(amount) from events_p where dim_a_id = @K@;
-- @case acc_partition_all | access | partitioned full aggregate
select date_trunc('year', d), count(*) from events_p group by 1;
-- @case acc_wide_toast | access | wide columns not in the index
select count(*), sum(length(payload)) from events where dim_a_id = @K@;
-- @case acc_dup_nonunique | access | non-unique key duplicates
select dim_a_id, count(*) c from events group by 1 order by c desc, 1 limit 5;
-- @case acc_unique_in_group | access | group by unique key
select count(*) from (select id from events group by id) s;

-- ===================== application archetypes
-- @case arch_key_dashboard | arch | per-key dashboard: status counts per month
select date_trunc('month', created_at) m, status, count(*), sum(amount) from events where tenant_id = 1 + @K@ % 50 and created_at >= '2025-01-01' group by 1, 2;
-- @case arch_ecom_revenue_day | arch | e-commerce revenue by day and status
select date_trunc('day', created_at) d, status, count(*), sum(total) from orders where created_at >= '2026-01-01' group by 1, 2;
-- @case arch_ecom_top_products | arch | e-commerce top products
select foreign_id_b, sum(qty) q from order_items group by 1 order by q desc, foreign_id_b limit 10;
-- @case arch_ecom_customer_ltv | arch | customer lifetime value top 20
select customer_id, count(*), sum(total) from orders group by 1 order by sum(total) desc, customer_id limit 20;
-- @case arch_ledger_balance | arch | ledger balance of one account
select sum(amount), count(*) from ledger where account_id = @K@ % 5000 + 1 and posted_at <= '2025-06-30';
-- @case arch_ledger_monthly | arch | ledger monthly by kind
select date_trunc('month', posted_at) m, kind, sum(amount) from ledger group by 1, 2;
-- @case arch_weblog_top_paths | arch | web analytics top paths
select path, count(*) c from weblog where status = 200 group by 1 order by c desc, path limit 20;
-- @case arch_weblog_errors | arch | web analytics 4xx/5xx rate per ua
select ua, count(*) filter (where status >= 400)::float / count(*) from weblog group by 1;
-- @case arch_eav_pivot | arch | EAV pivot of three attributes
select dim_a_id, max(value) filter (where attr = 'attr_1'), max(value) filter (where attr = 'attr_2') from eav where dim_a_id <= 3000 group by 1;
-- @case arch_eav_attr_count | arch | EAV attribute value counts
select value, count(*) from eav where attr = 'attr_3' group by 1;
-- @case arch_graph_degree | arch | graph out-degree top
select src, count(*) c from edges group by 1 order by c desc, src limit 10;
-- @case arch_graph_two_hop | arch | graph 2-hop neighbours of one node
select count(distinct b.dst) from edges a join edges b on b.src = a.dst where a.src = @K@;
-- @case arch_facets_three | arch | facet counts over three columns with a selected filter
select 'status' f, status v, count(*) from events where region = 'region_3' and d >= '2025-01-01' group by 2 union all select 'kind', kind_id::text, count(*) from events where region = 'region_3' and d >= '2025-01-01' group by 2 union all select 'tenant', tenant_id::text, count(*) from events where region = 'region_3' and d >= '2025-01-01' group by 2;
-- @case arch_facets_one_query | arch | facets as GROUPING SETS with filter
select status, kind_id, tenant_id, count(*) from events where region = 'region_3' and d >= '2025-01-01' group by grouping sets ((status), (kind_id), (tenant_id));

-- ===================== loss hunting: statistical aggregates, window pushdown, collection, small statements, expression groups
-- @case s2_stddev_variance | agg | stddev/variance per group
select region, stddev(amount), variance(score) from events group by 1;
-- @case s2_corr_regr | agg | corr and regr_slope
select kind_id, corr(amount, qty), regr_slope(amount, score) from events group by 1;
-- @case s2_bool_agg | agg | bool_and/bool_or
select dim_a_id, bool_and(amount > 1), bool_or(status = 'paid') from events where dim_a_id <= 500 group by 1;
-- @case s2_count_distinct_filter | agg | count(DISTINCT) FILTER
select region, count(distinct dim_a_id) filter (where status = 'paid') from events group by 1;
-- @case s2_avg_nulls | agg | avg over a nullable measure grouped by two columns
select region, status, avg(amount), count(amount) from events group by 1, 2;
-- @case s2_sum_case_pivot | agg | pivot via sum(CASE)
select dim_a_id, sum(case when status = 'paid' then amount else 0 end) paid, sum(case when status = 'open' then amount else 0 end) open from events where dim_a_id <= 1000 group by 1;
-- @case s2_group_lower_text | agg | group by function of text
select lower(region), upper(status), count(*) from events group by 1, 2;
-- @case s2_group_date_bin | agg | date_bin 15 minute on one day
select date_bin('15 minutes', created_at, timestamptz '2025-01-01') b, count(*) from events where created_at >= '2025-03-01' and created_at < '2025-03-08' group by 1;
-- @case s2_group_substr | agg | group by substr(label)
select substr(label, 1, 8), count(*) from events group by 1;
-- @case s2_group_jsonb_key | agg | group by jsonb key with filter on another key
select attrs->>'b' b, count(*) from events where (attrs->>'a')::float8 > 50 group by 1;
-- @case s2_nested_group | agg | group of groups
select c, count(*) from (select dim_a_id, count(*) c from events group by 1) s group by 1 order by 2 desc, 1 limit 20;
-- @case s2_count_over_all | agg | count(*) over () with group by
select region, count(*), count(*) over () from events group by 1;
-- @case s2_having_filter_count | agg | HAVING count(*) > threshold on 50k groups
select label, count(*) from events group by 1 having count(*) >= 40;
-- @case s2_group_1M_like | agg | group by id (600k groups, aggregate sum)
select id, sum(amount) from events where id <= 600000 group by 1 order by 2 desc nulls last, 1 limit 5;
-- @case s2_group_100k_join | agg | 60k groups after join to dimension
select v.label, e.type, count(*) from events v join dim_a e on e.id = v.dim_a_id group by 1, 2;
-- @case s2_group_text_highcard_having | agg | text 50k groups with order and limit 100
select label, sum(amount) s from events group by 1 order by s desc nulls last, label limit 100;

-- @case s2_win_ntile | window | ntile over aggregated groups
select dim_a_id, ntile(4) over (order by sum(amount)) from events group by 1;
-- @case s2_win_first_value | window | first_value per entity ordered by date
select distinct dim_a_id, first_value(amount) over (partition by dim_a_id order by d, id) from events where dim_a_id <= 100;
-- @case s2_win_count_over_partition | window | count(*) over (partition by) on filtered rows
select id, count(*) over (partition by dim_a_id) from events where dim_a_id <= 20 and d >= '2026-01-01';
-- @case s2_win_rn_top1_per_kind | window | latest row per (entity, kind)
select * from (select dim_a_id, kind_id, id, row_number() over (partition by dim_a_id, kind_id order by created_at desc, id desc) rn from events where dim_a_id <= 300) s where rn = 1;
-- @case s2_win_sum_over_group | window | share of total
select region, c, c::float / sum(c) over () from (select region, count(*) c from events group by 1) s;
-- @case s2_win_lag_per_entity | window | lag per entity over monthly counts
select dim_a_id, m, c, lag(c) over (partition by dim_a_id order by m) from (select dim_a_id, date_trunc('month', d) m, count(*) c from events where dim_a_id <= 200 group by 1, 2) s;
-- @case s2_topn_multi_order | window | ORDER BY two columns LIMIT with filter
select id, status, amount from events where kind_id = 5 order by status, amount desc nulls last, id limit 30;
-- @case s2_topn_ties | window | FETCH FIRST WITH TIES
select id, score from events order by score desc fetch first 5 rows with ties;
-- @case s2_topn_group_limit | window | top 5 groups by count
select kind_id, count(*) from events group by 1 order by 2 desc, 1 limit 5;
-- @case s2_topn_text_order | window | ORDER BY text LIMIT
select label, id from events order by label, id limit 20;
-- @case s2_topn_offset_deep | window | OFFSET 100000 LIMIT 10
select id, created_at from events order by created_at, id offset 100000 limit 10;

-- @case s2_lateral_count | join | LATERAL count per entity
select e.id, x.c from dim_a e cross join lateral (select count(*) c from events v where v.dim_a_id = e.id and v.d >= '2026-01-01') x where e.id <= 300;
-- @case s2_lateral_jsonb_each | join | jsonb_each lateral on a filtered entity
select r.key, count(*) from events v cross join lateral jsonb_each(v.attrs) r where v.dim_a_id = @K@ group by 1;
-- @case s2_unnest_array_group | join | unnest(int[]) group
select t, count(*) from events, unnest(tags) t where dim_a_id <= 100 group by 1;
-- @case s2_join_self_entities | join | self-join via peers-like pairs
select count(*) from events a join dim_a e on e.id = a.dim_a_id join dim_a f on f.region = e.region and f.id = e.id + 1 where a.d >= '2026-01-01';
-- @case s2_exists_two_levels | join | EXISTS over events for entities in a set
select count(*) from dim_a e where exists (select 1 from events v where v.dim_a_id = e.id and v.status = 'paid' and v.d >= '2026-06-01');
-- @case s2_in_subselect_agg | join | IN (select with aggregate)
select count(*) from events where dim_a_id in (select dim_a_id from events where d >= '2026-06-01' group by 1 having count(*) > 20);
-- @case s2_cte_two_aggs_join | join | two CTE aggregates joined
with a as (select dim_a_id, count(*) c from events group by 1), b as (select dim_a_id, sum(amount) s from events where status = 'paid' group by 1) select count(*), sum(c), sum(s) from a join b using (dim_a_id);
-- @case s2_cross_join_gs | join | events cross generate_series bucket
select g, count(*) from generate_series(1, 5) g join events v on v.kind_id = g * 3 group by 1;
-- @case s2_union_all_groups | join | UNION ALL of two group aggregates
select 'x' k, status, count(*) from events where dim_a_id < 100 group by 2 union all select 'y', status, count(*) from events where dim_a_id >= 1900 group by 2;
-- @case s2_orders_items_nested | join | orders with item totals (join + group by order)
select o.status, avg(t.n), avg(t.s) from orders o join (select order_id, count(*) n, sum(qty * price) s from order_items group by 1) t on t.order_id = o.id group by 1;

-- @case s2_small_group_one_entity | small | group by status for one entity
select status, count(*), sum(amount) from events where dim_a_id = @K@ group by 1;
-- @case s2_small_max_date_entity | small | max(d) of one entity
select max(d), min(d) from events where dim_a_id = @K@;
-- @case s2_small_count_entity_kind | small | count with two equalities
select count(*) from events where dim_a_id = @K@ and kind_id = 3;
-- @case s2_small_tenant_status | small | tenant + status count
select count(*) from events where tenant_id = 1 + @K@ % 50 and status = 'paid';
-- @case s2_small_exists_entity | small | EXISTS one entity row
select exists (select 1 from events where dim_a_id = @K@ and status = 'closed');
-- @case s2_small_two_entities_sum | small | sum for two entities
select dim_a_id, sum(amount) from events where dim_a_id in (@K@, @K2@) group by 1;
-- @case s2_small_distinct_kinds | small | distinct kinds of an entity
select distinct kind_id from events where dim_a_id = @K@ order by 1;
-- @case s2_small_order_items_customer | small | one customer's order totals
select count(*), sum(total) from orders where customer_id = @K@ % 20000 + 1;
-- @case s2_small_sensor_last | small | last reading of one sensor
select ts, value from readings where sensor_id = @K@ % 500 + 1 order by ts desc limit 1;
-- @case s2_small_eav_entity | small | EAV attributes of one entity
select attr, value from eav where dim_a_id = @K@ order by attr;
-- @case s2_small_graph_out | small | out-neighbours of a node
select dst from edges where src = @K@ order by dst;
-- @case s2_small_ledger_recent | small | recent postings of an account
select id, amount from ledger where account_id = @K@ % 5000 + 1 order by posted_at desc, id desc limit 20;

-- @case s2_pred_between_ts | pred | timestamptz range on events
select count(*), avg(amount) from events where created_at >= '2025-06-01 00:00' and created_at < '2025-06-02 00:00';
-- @case s2_pred_in_text_many | pred | IN list of 5 labels
select count(*) from events where label in ('label_1', 'label_2', 'label_3', 'label_4', 'label_5');
-- @case s2_pred_text_eq_group | pred | equality on region group by status
select status, count(*) from events where region = 'region_5' group by 1;
-- @case s2_pred_not_equal | pred | <> predicate
select count(*) from events where status <> 'paid' and kind_id <> 3;
-- @case s2_pred_coalesce | pred | coalesce in predicate
select count(*) from events where coalesce(amount, 0) > 990;
-- @case s2_pred_func_lower | pred | function of a column in WHERE
select count(*) from events where lower(status) = 'paid' and extract(year from d) = 2025;
-- @case s2_pred_text_range | pred | text range
select count(*) from events where label >= 'label_10000' and label < 'label_10100';
-- @case s2_pred_exists_jsonb_key | pred | jsonb key existence
select count(*) from events where attrs ? 'a' and dim_a_id < 500;
-- @case s2_pred_array_any | pred | = ANY(tags)
select count(*) from events where 7 = any (tags) and d >= '2026-01-01';
-- @case s2_pred_bool_flag | pred | jsonb boolean path
select count(*) from events where (attrs->>'flag')::boolean and dim_a_id between 100 and 400;
-- @case s2_pred_not_null_group | pred | IS NOT NULL group
select kind_id, count(*) from events where note is not null group by 1;

