-- The corpus: 307 typified statements of test/corpus/lab.sql (aggregates, joins and key sets, windows and top-N, time series, predicates, access patterns, archetypes), run by
-- test/corpus/run.py (scripts/corpus.sh), each in a new backend with sift off and on. Format:
--   -- @case slug | family | note
--   -- @pre   <one line of SQL run before the statement in every session>   (zero or more)
--   -- @txn                                                                  (the statement runs between BEGIN and ROLLBACK)
--   <statement>;     @K@ and @K2@ are replaced by an entity id that is the same for both modes

-- ===================== aggregation shapes
-- @case agg_count_star | agg | count(*) no filter
select count(*) from events;
-- @case agg_basic_five | agg | count/sum/avg/min/max one row with a date filter
select count(*), sum(amount), avg(amount), min(amount), max(amount) from events where d >= date '2025-03-01' and d < date '2025-06-01';
-- @case agg_filter_clause | agg | FILTER aggregates by status
select dim_a_id, count(*) filter (where status='paid') paid, sum(amount) filter (where status='open') open_amt, avg(score) filter (where qty > 5) from events where kind_id < 10 group by 1;
-- @case agg_having | agg | HAVING on a sum
select dim_a_id, sum(amount) s from events group by 1 having sum(amount) > 150000;
-- @case agg_distinct_pk | agg | count(DISTINCT id) over a join
select e.type, count(distinct v.id) from events v join dim_a e on e.id = v.dim_a_id group by 1;
-- @case agg_distinct_entity | agg | count(DISTINCT dim_a_id) per status (non-unique column)
select status, count(distinct dim_a_id) from events group by 1;
-- @case agg_distinct_multi | agg | two DISTINCT aggregates
select region, count(distinct dim_a_id), count(distinct kind_id) from events group by 1;
-- @case agg_percentile | agg | ordered-set percentile_cont
select status, percentile_cont(0.5) within group (order by amount) from events group by 1;
-- @case agg_mode | agg | ordered-set mode
select region, mode() within group (order by status) from events group by 1;
-- @case agg_array_agg | agg | array_agg of small groups
select kind_id, array_agg(distinct status order by status) from events where dim_a_id < 50 group by 1;
-- @case agg_string_agg | agg | string_agg
select dim_a_id, string_agg(distinct status, ',' order by status) from events where dim_a_id <= 100 group by 1;
-- @case agg_json_agg | agg | json_agg per group
select kind_id, json_agg(id order by id) from events where dim_a_id = @K@ group by 1;
-- @case agg_jsonb_object_agg | agg | jsonb_object_agg
select region, jsonb_object_agg(status, c) from (select region, status, count(*) c from events group by 1, 2) s group by 1;
-- @case agg_rollup | agg | ROLLUP
select region, status, count(*), sum(amount) from events group by rollup (region, status);
-- @case agg_cube | agg | CUBE
select region, status, kind_id < 10 as k, count(*) from events group by cube (region, status, kind_id < 10);
-- @case agg_grouping_sets | agg | GROUPING SETS
select region, status, count(*), sum(qty) from events group by grouping sets ((region), (status), ());
-- @case agg_group_date_month | agg | group by date_trunc month
select date_trunc('month', d) m, count(*), sum(amount) from events group by 1;
-- @case agg_group_date_week | agg | group by date_trunc week of timestamptz
select date_trunc('week', created_at) w, count(*) from events where dim_a_id < 100 group by 1;
-- @case agg_group_extract | agg | extract(dow)
select extract(dow from d)::int dow, count(*), avg(amount) from events group by 1;
-- @case agg_group_case | agg | group by CASE expression
select case when amount < 100 then 'low' when amount < 500 then 'mid' else 'high' end b, count(*) from events group by 1;
-- @case agg_group_many_cols | agg | group by 5 columns
select region, status, kind_id, qty, score / 10, count(*) from events group by 1, 2, 3, 4, 5;
-- @case agg_group_highcard_50k | agg | 50k groups (label)
select label, count(*), sum(amount) from events group by 1;
-- @case agg_group_highcard_300k | agg | 300k groups (id/2)
select id / 2 g, count(*) from events group by 1;
-- @case agg_group_highcard_limit | agg | 50k groups then top 10
select label, count(*) c from events group by 1 order by c desc, label limit 10;
-- @case agg_no_group_filter | agg | single row with entity equality
select count(*), sum(amount) from events where dim_a_id = @K@;
-- @case agg_null_heavy | agg | NULL-heavy column
select count(note), count(*), count(*) filter (where note is null) from events where kind_id = 7;
-- @case agg_text_minmax | agg | min/max of text
select region, min(label), max(label) from events group by 1;
-- @case agg_numeric_sum | agg | numeric sum (ledger)
select kind, sum(amount), avg(amount) from ledger group by 1;

-- ===================== joins, semi/anti, key sets
-- @case join_one_dim | join | join to one dimension
select e.type, count(*), sum(v.amount) from events v join dim_a e on e.id = v.dim_a_id group by 1;
-- @case join_two_dims | join | two dimensions
select e.type, k.name, count(*) from events v join dim_a e on e.id = v.dim_a_id join kinds k on k.id = v.kind_id where k.enabled group by 1, 2;
-- @case join_four_dims | join | four dimensions
select t.plan, e.type, k.name, e.region, count(*) from events v join dim_a e on e.id = v.dim_a_id join kinds k on k.id = v.kind_id join tenants t on t.id = v.tenant_id where v.d >= '2025-01-01' group by 1, 2, 3, 4;
-- @case join_left_dim | join | LEFT JOIN to a dimension with a missing key
select k.name, count(*) from events v left join kinds k on k.id = v.kind_id and k.enabled group by 1;
-- @case join_set_in_subselect_11 | join | key set of 11 via IN (SELECT ...) from membership
select count(*), sum(amount) from events where dim_a_id in (select dim_a_id from dim_b where set_id = 1 and deleted_at is null);
-- @case join_set_in_subselect_75_group | join | set of 75, join dimension, group
select e.type, count(*), sum(v.amount) from events v join dim_a e on e.id = v.dim_a_id where e.id in (select dim_a_id from dim_b where set_id = 2 and deleted_at is null) group by 1;
-- @case join_set_in_subselect_1000 | join | set of 1000
select count(*), avg(amount) from events where dim_a_id in (select dim_a_id from dim_b where set_id = 5);
-- @case join_set_cte_materialized | join | MATERIALIZED key CTE
with ids as materialized (select distinct dim_a_id from dim_b where set_id = 1 and deleted_at is null) select v.status, count(*) from events v join ids on ids.dim_a_id = v.dim_a_id group by 1;
-- @case join_set_bigdim | join | key set joined through a 1M-row dimension (RC1 class)
select b.grp, count(*), sum(v.amount) from events v join lookup_big b on b.id = v.dim_a_id where b.dim_a_id in (select dim_a_id from dim_b where set_id = 1) group by 1;
-- @case join_set_bigdim_cte | join | key set from a big dimension through a CTE
with ids as materialized (select distinct id from lookup_big where grp = 7 and id < 2001) select v.region, count(*) from events v join ids on ids.id = v.dim_a_id group by 1;
-- @case semi_exists | join | EXISTS correlated
select count(*) from events v where exists (select 1 from dim_b m where m.dim_a_id = v.dim_a_id and m.set_id = 2);
-- @case anti_not_exists | join | NOT EXISTS
select count(*) from events v where not exists (select 1 from dim_b m where m.dim_a_id = v.dim_a_id and m.set_id = 5);
-- @case anti_not_in | join | NOT IN subselect
select count(*) from events where dim_a_id not in (select dim_a_id from dim_b where set_id = 2);
-- @case in_array_10 | join | = ANY(array) of 10
select count(*), sum(amount) from events where dim_a_id = any (array[3,17,44,120,300,555,901,1200,1500,1999]);
-- @case in_values_1k | join | VALUES list of 1000
select count(*) from events where dim_a_id in (select * from (values (1),(2),(3),(5),(8),(13),(21),(34),(55),(89),(144),(233),(377),(610),(987),(1597)) v);
-- @case in_array_1k_gen | join | = ANY of generate_series 1000
select count(*), sum(qty) from events where dim_a_id = any (array(select generate_series(1, 2000, 2)));
-- @case in_array_100k | join | = ANY array of 100k ids on id
select count(*) from events where id = any (array(select generate_series(1, 200000, 2)));
-- @case join_orders_items | join | e-commerce join + group by category
select p.category, count(*), sum(i.qty * i.price) from order_items i join products p on p.id = i.foreign_id_b group by 1;
-- @case join_orders_customer | join | orders by country through customers
select c.country, count(*), sum(o.total) from orders o join customers c on c.id = o.customer_id where o.created_at >= '2025-06-01' group by 1;
-- @case join_items_orders_filter | join | items filtered by order attribute
select count(*), sum(i.price) from order_items i join orders o on o.id = i.order_id where o.status = 'returned' and o.created_at >= '2025-09-01';

-- ===================== windows, ordering, top-N
-- @case win_row_number_top3 | window | top 3 per entity by row_number
select * from (select dim_a_id, id, amount, row_number() over (partition by dim_a_id order by amount desc nulls last, id) rn from events where dim_a_id <= 200) s where rn <= 3;
-- @case win_rank_group | window | rank over aggregated groups
select * from (select dim_a_id, sum(amount) s, rank() over (order by sum(amount) desc) r from events group by 1) x where r <= 20;
-- @case win_lag_month | window | lag over monthly aggregates
select m, c, c - lag(c) over (order by m) from (select date_trunc('month', d) m, count(*) c from events group by 1) s;
-- @case win_running_total | window | running total over daily aggregates
select d, sum(sum(amount)) over (order by d) from events where d >= '2025-01-01' and d < '2025-03-01' group by d;
-- @case win_moving_avg | window | ROWS 6 PRECEDING moving average over daily counts
select d, avg(count(*)) over (order by d rows between 6 preceding and current row) from events where dim_a_id < 300 group by d;
-- @case win_distinct_on | window | DISTINCT ON latest per entity
select distinct on (dim_a_id) dim_a_id, id, created_at from events where dim_a_id <= 300 order by dim_a_id, created_at desc, id;
-- @case topn_order_limit | window | ORDER BY ... LIMIT without filter
select id, amount from events order by amount desc nulls last, id limit 20;
-- @case topn_order_limit_filter | window | ORDER BY ... LIMIT with entity filter (index order)
select id, created_at from events where dim_a_id = @K@ order by created_at desc, id limit 25;
-- @case topn_lateral_per_entity | window | top-N per group via LATERAL
select e.id, x.id, x.amount from dim_a e cross join lateral (select id, amount from events v where v.dim_a_id = e.id order by amount desc nulls last, id limit 3) x where e.id <= 100;
-- @case page_keyset | window | keyset pagination
select id, created_at, amount from events where dim_a_id = @K@ and (created_at, id) < (timestamptz '2025-06-01', 100000000) order by created_at desc, id desc limit 50;
-- @case page_offset | window | OFFSET pagination deep
select id, d from events where dim_a_id = @K@ order by d desc, id limit 50 offset 200;
-- @case minmax_by_group | window | min/max by group with index
select dim_a_id, min(d), max(d) from events group by 1;

-- ===================== time series
-- @case ts_daily_gapfill | time | generate_series gap filling
select g::date, coalesce(c, 0) from generate_series(date '2025-01-01', date '2025-03-31', interval '1 day') g left join (select d, count(*) c from events where dim_a_id = @K@ group by 1) s on s.d = g::date;
-- @case ts_last_30_days | time | last-N-days window
select status, count(*), sum(amount) from events where created_at >= timestamptz '2026-09-01' - interval '30 days' and created_at < timestamptz '2026-09-01' group by 1;
-- @case ts_yoy_selfjoin | time | year over year self join
select a.m, a.c, b.c from (select date_trunc('month', d) m, count(*) c from events where d >= '2025-01-01' and d < '2026-01-01' group by 1) a left join (select date_trunc('month', d) + interval '1 year' m, count(*) c from events where d >= '2024-01-01' and d < '2025-01-01' group by 1) b on b.m = a.m;
-- @case ts_readings_bucket | time | IoT hourly buckets of one sensor
select date_trunc('hour', ts) h, avg(value), min(value), max(value) from readings where sensor_id = @K@ % 500 + 1 group by 1;
-- @case ts_readings_all_daily | time | IoT daily avg all sensors
select date_trunc('day', ts) d, avg(value), count(*) from readings group by 1;
-- @case ts_readings_latest | time | latest reading per sensor
select distinct on (sensor_id) sensor_id, ts, value from readings order by sensor_id, ts desc;
-- @case ts_sessionize | time | session counts via lag
select count(*) filter (where gap > interval '30 minutes' or gap is null) from (select ts - lag(ts) over (partition by session_id order by ts) gap from weblog where session_id <= 2000) s;
-- @case ts_weblog_status_hour | time | web analytics status by hour
select date_trunc('hour', ts) h, status, count(*) from weblog where ts >= '2026-03-10' and ts < '2026-03-12' group by 1, 2;

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

-- ===================== rolling and moving windows over date_trunc buckets
-- @case c2_roll3_month_sum | window | 3-month moving sum of monthly totals
select m, s, sum(s) over (order by m rows between 2 preceding and current row) from (select date_trunc('month', d) m, sum(amount) s from events group by 1) t;
-- @case c2_roll4_week_avg_count | window | 4-week moving average of weekly counts for one tenant
select w, c, avg(c) over (order by w rows between 3 preceding and current row) from (select date_trunc('week', created_at) w, count(*) c from events where tenant_id = 7 group by 1) t;
-- @case c2_roll_range_interval_days | window | 7-day range window over daily counts
select dy, c, sum(c) over (order by dy range between interval '6 days' preceding and current row) from (select d::timestamp dy, count(*) c from events where d >= date '2025-01-01' and d < date '2025-04-01' group by d) t;
-- @case c2_cum_entity_month | window | cumulative amount by month for one entity
select m, s, sum(s) over (order by m) from (select date_trunc('month', d) m, sum(amount) s from events where dim_a_id = @K@ group by 1) t;
-- @case c2_roll_lag_delta_quarter | window | quarter over quarter change
select q, s, s - lag(s) over (order by q) delta from (select date_trunc('quarter', d) q, sum(amount) s from events group by 1) t;
-- ===================== percent of total and ratios
-- @case c2_pct_total_status | window | share of each status in the count
select status, count(*), 100.0 * count(*) / sum(count(*)) over () from events group by 1;
-- @case c2_pct_total_region_kind | window | share of a kind in its region
select region, kind_id, sum(amount), sum(amount) / sum(sum(amount)) over (partition by region) from events where kind_id <= 8 group by 1, 2;
-- @case c2_ratio_paid_share | agg | paid amount over total amount per region
select region, sum(amount) filter (where status = 'paid') / nullif(sum(amount), 0) from events group by 1;
-- @case c2_ratio_avg_over_avg | agg | ratio of two averages per kind
select kind_id, avg(amount) / nullif(avg(score), 0) from events group by 1;
-- @case c2_pct_of_grand_total_cte | agg | CTE read twice for a share of the grand total
with t as (select region, sum(amount) s from events group by 1) select region, s, s / (select sum(s) from t) from t;
-- ===================== count distinct with FILTER, HAVING
-- @case c2_cd_filter_status | agg | count(DISTINCT) with FILTER on two predicates
select region, count(distinct dim_a_id) filter (where status = 'paid'), count(distinct dim_a_id) filter (where amount > 500) from events group by 1;
-- @case c2_cd_filter_month_active | agg | monthly active entities with a flag
select date_trunc('month', d) m, count(distinct dim_a_id), count(distinct dim_a_id) filter (where qty > 5) from events group by 1;
-- @case c2_having_cd_kind | agg | HAVING on count(DISTINCT)
select kind_id, count(distinct dim_a_id) from events group by 1 having count(distinct dim_a_id) > 100;
-- @case c2_having_avg_count | agg | HAVING on an average and a count
select region, status, count(*), avg(amount) from events group by 1, 2 having avg(amount) > 495 and count(*) > 300;
-- @case c2_having_sum_ratio | agg | HAVING on a ratio of sums
select dim_a_id, sum(amount) a, sum(qty) q from events group by 1 having sum(amount) / nullif(sum(qty), 0) > 120;
-- ===================== top-k per group
-- @case c2_topk_tenant_amount | window | top 3 events per tenant by amount
select * from (select tenant_id, id, amount, row_number() over (partition by tenant_id order by amount desc nulls last, id) rn from events) t where rn <= 3;
-- @case c2_topk_rank_kind_score | window | rank() <= 2 per kind with ties
select * from (select kind_id, id, score, rank() over (partition by kind_id order by score desc) rk from events) t where rk <= 2;
-- @case c2_topk_dense_region_qty | window | dense_rank() <= 2 per region
select * from (select region, id, qty, dense_rank() over (partition by region order by qty desc) rk from events) t where rk <= 2;
-- @case c2_topk_array_slice | agg | five best event ids per kind through array_agg
select kind_id, (array_agg(id order by amount desc nulls last, id))[1:5] from events group by 1;
-- @case c2_topk_two_columns_entity | window | top 2 by created_at per entity with several columns
select * from (select dim_a_id, id, created_at, status, amount, row_number() over (partition by dim_a_id order by created_at desc, id) rn from events where dim_a_id <= 300) t where rn <= 2;
-- ===================== calendar buckets with generate_series
-- @case c2_cal_week_gapfill | ts | weekly counts with every week of the range
select w, coalesce(c, 0) from generate_series(date '2025-01-06', date '2025-06-30', interval '1 week') w left join (select date_trunc('week', d)::date wk, count(*) c from events group by 1) e on e.wk = w::date;
-- @case c2_cal_month_gapfill_sum | ts | monthly sums with every month of the range
select m::date, coalesce(s, 0) from generate_series(date '2024-01-01', date '2026-09-01', interval '1 month') m left join (select date_trunc('month', d)::date mo, sum(amount) s from events group by 1) e on e.mo = m::date;
-- @case c2_cal_quarter_kind | ts | quarters by kind group
select date_trunc('quarter', created_at) q, kind_id, count(*) from events where kind_id <= 5 group by 1, 2;
-- @case c2_cal_dow_hour | ts | day of week and hour of day
select extract(dow from created_at) dw, extract(hour from created_at) hr, count(*) from events group by 1, 2;
-- @case c2_cal_cross_join_days | ts | calendar of days cross joined with a kind filter
select c.dy, count(e.id) from (select generate_series(date '2025-02-01', date '2025-02-28', interval '1 day')::date as dy) c left join events e on e.d = c.dy and e.kind_id = 3 group by 1;
-- ===================== cohorts and retention
-- @case c2_cohort_first_month | agg | entities by month of first event
with f as (select dim_a_id, min(date_trunc('month', d)) m0 from events group by 1) select m0, count(*) from f group by 1;
-- @case c2_retention_cohort_activity | agg | cohort month against activity month
with f as (select dim_a_id, min(date_trunc('month', d)) m0 from events group by 1) select f.m0, date_trunc('month', e.d) m, count(distinct e.dim_a_id) from events e join f on f.dim_a_id = e.dim_a_id group by 1, 2;
-- @case c2_returning_two_months | agg | entities active in a month and the next
with m as (select dim_a_id, date_trunc('month', d) mo from events group by 1, 2) select a.mo, count(*) from m a join m b on b.dim_a_id = a.dim_a_id and b.mo = a.mo + interval '1 month' group by 1;
-- ===================== anti-joins and semi-joins
-- @case c2_anti_orders_no_items | join | orders with no item above a price
select count(*) from orders o where not exists (select 1 from order_items i where i.order_id = o.id and i.price > 150);
-- @case c2_semi_exists_set | join | events of the entities of a set, grouped
select kind_id, count(*), sum(amount) from events e where exists (select 1 from dim_b b where b.dim_a_id = e.dim_a_id and b.set_id = 2 and b.deleted_at is null) group by 1;
-- @case c2_semi_in_country | join | orders of the customers of one country by month
select date_trunc('month', created_at) m, count(*), sum(total) from orders where customer_id in (select id from customers where country = 'br') group by 1;
-- ===================== UNION ALL arms and CTEs
-- @case c2_union_arms_filters | agg | four arms with different filters
select 'paid' arm, count(*), sum(amount) from events where status = 'paid' union all select 'open', count(*), sum(amount) from events where status = 'open' union all select 'big', count(*), sum(amount) from events where amount > 900 union all select 'late', count(*), sum(amount) from events where d >= date '2026-01-01';
-- @case c2_union_all_sources_daily | agg | daily counts of two tables
select dy, sum(c) from (select ts::date dy, count(*) c from weblog group by 1 union all select created_at::date, count(*) from orders group by 1) t group by 1;
-- @case c2_cte_twice_selfjoin | agg | one CTE joined to itself
with t as (select region, kind_id, sum(amount) s from events group by 1, 2) select a.region, a.kind_id, a.s / b.s from t a join t b on b.region = a.region and b.kind_id = 1 where a.kind_id <= 5;
-- @case c2_cte_two_levels | agg | CTE over CTE with a filter in between
with a as (select dim_a_id, sum(amount) s from events group by 1), b as (select dim_a_id, s from a where s > 1000) select count(*), avg(s) from b;
-- ===================== scalar subqueries, LATERAL
-- @case c2_scalar_total_in_select | agg | grand total in the select list
select status, count(*), (select count(*) from events) from events group by 1;
-- @case c2_lateral_top3_amount | join | three largest events per entity
select a.id, l.id, l.amount from dim_a a cross join lateral (select id, amount from events e where e.dim_a_id = a.id order by amount desc nulls last, id limit 3) l where a.id <= 100;
-- ===================== coalesce, case, aggregates of lists
-- @case c2_coalesce_measures | agg | coalesce inside sums
select region, sum(coalesce(amount, 0)), avg(coalesce(score, 0)), count(amount) from events group by 1;
-- @case c2_case_size_bands | agg | group by a CASE band of the amount
select case when amount < 100 then 'small' when amount < 500 then 'medium' else 'large' end band, count(*), sum(qty) from events group by 1;
-- @case c2_string_agg_status_entity | agg | status history string per entity
select dim_a_id, string_agg(status, ',' order by d, id) from events where dim_a_id <= 40 group by 1;
-- @case c2_jsonb_agg_entity | agg | jsonb_agg of measures of one entity
select jsonb_agg(jsonb_build_object('d', d, 'a', amount) order by d, id) from events where dim_a_id = @K@;
-- @case c2_array_agg_kinds_region | agg | distinct kinds per region
select region, array_agg(distinct kind_id order by kind_id) from events group by 1;
-- @case c2_bool_or_and_region | agg | bool_or and bool_and
select region, bool_or(status = 'shipped'), bool_and(amount is not null) from events group by 1;
-- @case c2_minmax_ts_span | agg | min and max timestamps with the span
select kind_id, min(created_at), max(created_at), max(created_at) - min(created_at) from events group by 1;
-- @case c2_age_fixed_ref | agg | average age against a fixed timestamp
select status, avg(timestamptz '2026-06-01' - created_at) from events group by 1;
-- @case c2_interval_filter_window | agg | filter by an interval before a date
select count(*), sum(amount) from events where created_at >= date '2026-03-01' - interval '90 days' and created_at < date '2026-03-01';
-- ===================== ordered-set aggregates
-- @case c2_percentile_array | agg | quartiles per region
select region, percentile_cont(array[0.25, 0.5, 0.75]) within group (order by amount) from events group by 1;
-- @case c2_percentile_disc_kind | agg | percentile_disc per kind
select kind_id, percentile_disc(0.9) within group (order by score) from events group by 1;
-- @case c2_mode_status_kind | agg | mode of the status per kind
select kind_id, mode() within group (order by status) from events group by 1;
-- ===================== pagination and DISTINCT ON
-- @case c2_group_limit_offset | agg | page of a ranked grouped output
select dim_a_id, sum(amount) from events group by 1 order by 2 desc nulls last, 1 limit 20 offset 40;
-- @case c2_group_count_page | agg | page ordered by a count
select dim_a_id, count(*) from events group by 1 order by 2 desc, 1 limit 10 offset 5;
-- @case c2_distinct_on_latest_kind | window | latest event per kind
select distinct on (kind_id) kind_id, id, d, amount from events order by kind_id, d desc, id desc;
-- @case c2_distinct_on_entity_status | window | first event per entity and status
select distinct on (dim_a_id, status) dim_a_id, status, id, created_at from events where dim_a_id <= 200 order by dim_a_id, status, created_at, id;
-- ===================== cross joins and expression filters
-- @case c2_cross_values_status | join | VALUES list of statuses joined to the counts
select v.s, count(e.id) from (values ('new'), ('paid'), ('refunded')) v(s) left join events e on e.status = v.s group by 1;
-- @case c2_filter_lower_label | pred | lower() of a text column
select count(*), sum(amount) from events where lower(label) = 'label_17';
-- @case c2_filter_date_trunc_eq | pred | date_trunc equality on a timestamp
select dim_a_id, count(*) from events where date_trunc('month', created_at) = timestamptz '2025-03-01' group by 1;
-- @case c2_filter_cast_date | pred | timestamp cast to date in a range
select status, count(*) from events where created_at::date between date '2025-05-01' and date '2025-05-31' group by 1;
-- @case c2_filter_like_prefix | pred | LIKE prefix on a text column
select count(*), avg(amount) from events where note like 'note 12%';
-- @case c2_filter_not_distinct | pred | IS NOT DISTINCT FROM against a constant
select region, count(*) from events where status is not distinct from 'paid' and note is not distinct from null group by 1;
-- @case c2_filter_extract_year | pred | extract on a date
select extract(month from d) mo, count(*), sum(amount) from events where extract(year from d) = 2025 group by 1;
-- ===================== other tables
-- @case c2_orders_month_country | join | monthly revenue by customer country
select date_trunc('month', o.created_at) m, c.country, sum(o.total), count(*) from orders o join customers c on c.id = o.customer_id group by 1, 2;
-- @case c2_weblog_error_ratio_hour | agg | share of errors per hour
select date_trunc('hour', ts) h, count(*) filter (where status >= 500)::float / count(*) from weblog where ts < timestamptz '2026-03-05' group by 1;
-- @case c2_readings_hour_stats | agg | hourly average, min and max per sensor subset
select sensor_id, date_trunc('hour', ts) h, avg(value), min(value), max(value) from readings where sensor_id <= 20 group by 1, 2;
-- @case c2_events_p_month_status | agg | partitioned table by month and status
select date_trunc('month', d) m, status, count(*), sum(amount) from events_p group by 1, 2;
-- @case c2_eav_pivot_two | agg | pivot of two attributes
select dim_a_id, max(value) filter (where attr = 'attr_1'), max(value) filter (where attr = 'attr_2') from eav where dim_a_id <= 3000 group by 1;

-- ===================== a table left-joined to a join of two small tables
-- @case c2_cross_kinds_regions | join | every kind and region with its count
select k.name, r.region, count(e.id) from kinds k cross join (select distinct region from dim_a) r left join events e on e.kind_id = k.id and e.region = r.region where k.id <= 8 group by 1, 2;
-- @case c2_cross_kinds_regions_sums | join | the same with count(*), a sum and a max, and a condition on the events
select k.name, r.region, count(*), sum(e.qty), max(e.amount) from kinds k cross join (select distinct region from dim_a) r left join events e on e.kind_id = k.id and e.region = r.region and e.status = 'paid' group by 1, 2;
-- ===================== range types
-- @case c3_range_overlap_count | range | spans overlapping a window
select count(*) from spans where during && daterange('2025-01-01', '2025-03-01');
-- @case c3_range_bounds_plain | range | the same window through the plain bounds
select tenant_id, count(*), sum(qty) from spans where starts <= date '2025-06-15' and ends > date '2025-06-15' group by 1;
-- @case c3_range_group_by_range | range | groups by the range itself
select during, count(*) from spans where dim_a_id <= 20 group by 1 order by 2 desc limit 10;
-- ===================== HAVING with sub-selects
-- @case c3_having_scalar_avg | having | entities whose total is above ten times the average row
select dim_a_id, sum(amount) from events group by 1 having sum(amount) > (select avg(amount) * 10 from events);
-- @case c3_having_count_scalar | having | entities with more events than the mean entity
select dim_a_id, count(*) from events group by 1 having count(*) > (select count(*) / count(distinct dim_a_id) from events);
-- @case c3_having_in_list | having | groups of entities of one type
select dim_a_id, count(*), sum(qty) from events group by 1 having dim_a_id in (select id from dim_a where type = 'type_1');
-- @case c3_having_exists_kind | having | kinds with an enabled row
select kind_id, count(*) from events group by 1 having exists (select 1 from kinds k where k.id = events.kind_id and k.enabled);
-- @case c3_having_max_recent | having | entities with an event in the last 30 days of the data
select dim_a_id, max(d) from events group by 1 having max(d) > (select max(d) - 30 from events);
-- @case c3_having_avg_region | having | tenants whose average score is above that of one region
select tenant_id, avg(score) from events group by 1 having avg(score) > (select avg(score) from events where region = 'region_1');
-- @case c3_having_corr_count | having | entities with more events than rows in a membership table
select dim_a_id, count(*) from events group by 1 having count(*) > (select count(*) from dim_b b where b.dim_a_id = events.dim_a_id);
-- @case c3_having_not_exists_deleted | having | entities with no deleted membership
select dim_a_id, count(*) from events where dim_a_id <= 300 group by 1 having not exists (select 1 from dim_b b where b.dim_a_id = events.dim_a_id and b.deleted_at is not null);
-- @case c3_having_select_scalar | having | share of the grand total, with the group kept by a sub-select
select region, sum(amount), sum(amount) / (select sum(amount) from events) from events group by 1 having sum(amount) > (select sum(amount) / 20 from events);
-- @case c3_having_avg_of_groups | having | regions above the average region count
select region, count(*) from events group by 1 having count(*) > (select avg(c) from (select count(*) c from events group by region) x);
-- @case c3_having_two_sub | having | between two sub-selects
select kind_id, sum(qty) from events group by 1 having sum(qty) > (select min(q) from (select sum(qty) q from events group by kind_id) a) and count(*) < (select max(c) from (select count(*) c from events group by kind_id) b);
-- ===================== wide tables with many measures
-- @case c3_wide_sum_all_float | wide | twelve float sums by tenant
select tenant_id, sum(m01), sum(m02), sum(m03), sum(m04), sum(m05), sum(m06), sum(m07), sum(m08), sum(m09), sum(m10), sum(m11), sum(m12) from wide group by 1;
-- @case c3_wide_sum_all_int | wide | twelve integer sums by kind
select kind_id, sum(n01), sum(n02), sum(n03), sum(n04), sum(n05), sum(n06), sum(n07), sum(n08), sum(n09), sum(n10), sum(n11), sum(n12) from wide group by 1;
-- @case c3_wide_avg_mixed | wide | averages of both kinds by status
select status, avg(m01), avg(m03), avg(m09), avg(m10), avg(n01), avg(n05), avg(n09), avg(n12) from wide group by 1;
-- @case c3_wide_minmax | wide | min and max of six measures by tenant
select tenant_id, min(m01), max(m01), min(m05), max(m05), min(n02), max(n02), min(n07), max(n07), min(m09), max(m09) from wide group by 1;
-- @case c3_wide_count_cols | wide | count of each nullable measure by kind
select kind_id, count(*), count(m09), count(m10), count(n09), count(m01), count(n01) from wide group by 1;
-- @case c3_wide_filter_each | wide | sums with a FILTER of each status
select tenant_id, sum(m01) filter (where status = 'new'), sum(m01) filter (where status = 'paid'), sum(m02) filter (where status = 'open'), sum(m02) filter (where status = 'closed'), count(*) filter (where status = 'shipped') from wide group by 1;
-- @case c3_wide_expr_sums | wide | sums of products and sums of columns
select tenant_id, sum(m01 * m02), sum(m03 + m04), sum(m05 - m06), sum(n01 * n02), sum(m07 / nullif(m08, 0)) from wide group by 1;
-- @case c3_wide_three_keys | wide | ten measures by three keys
select tenant_id, status, kind_id, sum(m01), sum(m02), sum(m03), sum(m04), sum(m05), avg(n01), avg(n02), max(n03), min(n04), count(*) from wide group by 1, 2, 3;
-- @case c3_wide_ungrouped_many | wide | twenty aggregates over the whole table
select count(*), sum(m01), sum(m02), sum(m03), sum(m04), avg(m05), avg(m06), avg(m07), avg(m08), min(m09), max(m10), sum(n01), sum(n02), sum(n03), avg(n04), avg(n05), min(n06), max(n07), sum(n08), sum(n09), sum(n10) from wide;
-- @case c3_wide_stddev | wide | spread of six measures by status
select status, stddev(m01), variance(m02), stddev(m03), stddev(n01), variance(n02), stddev(n03) from wide group by 1;
-- @case c3_wide_live_only | wide | twelve sums over the rows that are not deleted
select tenant_id, sum(m01), sum(m02), sum(m03), sum(m04), sum(n01), sum(n02), sum(n03), sum(n04), count(*) from wide where deleted_at is null group by 1;
-- @case c3_wide_deleted_share | wide | deleted share of the sum by tenant
select tenant_id, sum(m01) filter (where deleted_at is not null) / nullif(sum(m01), 0), count(*) filter (where deleted_at is not null) from wide group by 1;
-- @case c3_wide_month_series | wide | six measures by month
select date_trunc('month', created_at) m, sum(m01), sum(m02), avg(m03), max(n01), count(*) from wide where deleted_at is null group by 1;
-- ===================== many small groups and few huge ones
-- @case c3_groups_label_count | groups | one group for each of 50,000 labels
select label, count(*) from events group by 1;
-- @case c3_groups_label_having | groups | labels that repeat
select label, count(*), sum(amount) from events group by 1 having count(*) > 5;
-- @case c3_groups_entity_kind | groups | one group for each entity and kind
select dim_a_id, kind_id, count(*), sum(qty) from events group by 1, 2;
-- @case c3_groups_entity_day | groups | one group for each entity and day
select dim_a_id, d, count(*) from events group by 1, 2;
-- @case c3_groups_top_labels | groups | the ten busiest labels
select label, count(*) from events group by 1 order by 2 desc, 1 limit 10;
-- @case c3_groups_one_group | groups | a constant group over every row
select count(*), sum(amount), avg(qty) from events group by ();
-- @case c3_groups_distinct_entities | groups | distinct entities by tenant and region
select tenant_id, region, count(distinct dim_a_id), count(distinct kind_id) from events group by 1, 2;
-- @case c3_groups_ids_modulo | groups | groups of a hashed id
select (id % 5000) g, count(*), sum(amount) from events group by 1;
-- ===================== tenant and soft-delete filters
-- @case c3_tenant_status | tenant | one tenant by status
select status, count(*), sum(amount) from events where tenant_id = 7 group by 1;
-- @case c3_tenant_in_list | tenant | five tenants by month
select tenant_id, date_trunc('month', created_at) m, count(*) from events where tenant_id in (3, 7, 11, 19, 23) group by 1, 2;
-- @case c3_tenant_range_created | tenant | one tenant in a window
select kind_id, count(*), avg(amount) from events where tenant_id = 12 and created_at >= '2025-01-01' and created_at < '2025-04-01' group by 1;
-- @case c3_tenant_live_wide | tenant | one tenant's live rows
select status, count(*), sum(m01), avg(n01) from wide where tenant_id = 5 and deleted_at is null group by 1;
-- @case c3_tenant_deleted_only | tenant | deleted rows of a tenant by month
select date_trunc('month', deleted_at) m, count(*), sum(m02) from wide where tenant_id = 9 and deleted_at is not null group by 1;
-- @case c3_tenant_join_dim | tenant | tenant plan through the dimension
select t.plan, count(*), sum(e.amount) from events e join dim_a a on a.id = e.dim_a_id join tenants t on t.id = a.tenant_id where e.tenant_id <= 25 group by 1;
-- ===================== window frames over aggregates
-- @case c3_frame_centered | frame | three-month centered moving sum
select m, s, sum(s) over (order by m rows between 1 preceding and 1 following) from (select date_trunc('month', d) m, sum(amount) s from events group by 1) t;
-- @case c3_frame_range_days | frame | seven-day range average of daily counts
select d, c, avg(c) over (order by d range between interval '6 days' preceding and current row) from (select d, count(*) c from events where d >= '2025-01-01' and d < '2025-04-01' group by 1) t;
-- @case c3_frame_running | frame | running total by month
select m, s, sum(s) over (order by m rows between unbounded preceding and current row) from (select date_trunc('month', d) m, sum(amount) s from events group by 1) t;
-- @case c3_frame_following | frame | total from each month to the end
select m, s, sum(s) over (order by m rows between current row and unbounded following) from (select date_trunc('month', d) m, sum(amount) s from events group by 1) t;
-- @case c3_frame_groups | frame | groups frame over equal counts
select c, n, sum(n) over (order by c groups between 1 preceding and current row) from (select count(*) c, count(distinct dim_a_id) n from events group by tenant_id) t;
-- @case c3_frame_interval_range | frame | one-month range window over timestamps
select m, s, sum(s) over (order by m range between interval '1 month' preceding and current row) from (select date_trunc('month', created_at) m, sum(qty) s from events group by 1) t;
-- @case c3_frame_first_last | frame | first and last value of a partition frame
select tenant_id, m, s, first_value(s) over w, last_value(s) over w from (select tenant_id, date_trunc('quarter', d) m, sum(amount) s from events where tenant_id <= 5 group by 1, 2) t window w as (partition by tenant_id order by m rows between unbounded preceding and unbounded following);
-- @case c3_frame_lag_lead | frame | previous and next quarter
select q, s, lag(s, 2) over (order by q), lead(s) over (order by q) from (select date_trunc('quarter', d) q, sum(amount) s from events group by 1) t;
-- @case c3_frame_ntile | frame | quartile of entities by total
select dim_a_id, s, ntile(4) over (order by s desc) from (select dim_a_id, sum(amount) s from events where dim_a_id <= 400 group by 1) t;
-- @case c3_frame_percent_rank | frame | rank shares of kinds
select kind_id, c, percent_rank() over (order by c), cume_dist() over (order by c) from (select kind_id, count(*) c from events group by 1) t;
-- @case c3_frame_partition_rows | frame | per-tenant moving sum of three months
select tenant_id, m, sum(s) over (partition by tenant_id order by m rows between 2 preceding and current row) from (select tenant_id, date_trunc('month', d) m, sum(qty) s from events where tenant_id <= 8 group by 1, 2) t;
-- @case c3_frame_exclude | frame | moving sum without the current row
select m, s, sum(s) over (order by m rows between 2 preceding and 2 following exclude current row) from (select date_trunc('month', d) m, sum(qty) s from events group by 1) t;
-- @case c3_frame_nth_value | frame | second best month per tenant
select tenant_id, m, nth_value(s, 2) over (partition by tenant_id order by s desc rows between unbounded preceding and unbounded following) from (select tenant_id, date_trunc('month', d) m, sum(amount) s from events where tenant_id <= 6 group by 1, 2) t;
-- ===================== GROUPING SETS, ROLLUP and CUBE
-- @case c3_gs_rollup_tenant_status | gsets | rollup of tenant and status
select tenant_id, status, count(*), sum(amount) from events where tenant_id <= 10 group by rollup (tenant_id, status);
-- @case c3_gs_cube_region_status | gsets | cube of region and status
select region, status, count(*), avg(score) from events group by cube (region, status);
-- @case c3_gs_sets_three | gsets | region, status and the total
select region, status, count(*), sum(qty) from events group by grouping sets ((region), (status), ());
-- @case c3_gs_grouping_fn | gsets | the grouping() flag kept by a HAVING
select region, kind_id, grouping(region, kind_id) g, count(*) from events group by rollup (region, kind_id) having grouping(region, kind_id) < 3;
-- @case c3_gs_rollup_month | gsets | rollup of year and month
select extract(year from d) y, extract(month from d) m, count(*), sum(amount) from events group by rollup (1, 2);
-- @case c3_gs_filtered | gsets | grouping sets under a filter
select region, status, count(*) from events where kind_id <= 10 and created_at >= '2025-01-01' group by grouping sets ((region, status), (region), (status));
-- @case c3_gs_cube_three | gsets | cube of three small columns
select region, status, kind_id % 3 k, count(*) from events group by cube (region, status, kind_id % 3);
-- @case c3_gs_rollup_limit | gsets | rollup with order and limit
select tenant_id, kind_id, sum(amount) s from events group by rollup (tenant_id, kind_id) order by s desc nulls last limit 20;
-- @case c3_gs_count_distinct | gsets | count distinct under rollup
select region, status, count(distinct dim_a_id) from events group by rollup (region, status);
-- @case c3_gs_join_rollup | gsets | rollup over a join
select a.type, a.region, count(*), sum(e.amount) from events e join dim_a a on a.id = e.dim_a_id group by rollup (a.type, a.region);
-- @case c3_gs_rollup_avg | gsets | rollup of kind and status with averages
select kind_id, status, avg(amount), avg(qty), max(score) from events where kind_id <= 12 group by rollup (kind_id, status);
-- @case c3_gs_wide_rollup | gsets | rollup over the wide table
select tenant_id, status, sum(m01), sum(m02), sum(n01), count(*) from wide where tenant_id <= 12 group by rollup (tenant_id, status);
