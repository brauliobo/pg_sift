-- The corpus: typified statements of test/corpus/lab.sql (aggregates, joins and key sets, windows and top-N, time series, predicates, access patterns, archetypes), run by
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
-- @case join_set_in_pairs_10 | join | set of 10 ids via IN (SELECT ...) over a table of 12,010 unique pairs (a probe of it is priced as the set)
select kind_id, count(*), sum(amount) from events where dim_a_id in (select dim_a_id from links_a where set_id = 1) group by 1;
-- @case join_set_in_pairs_200 | join | set of 200 ids via IN (SELECT ...) over a table of 12,010 unique pairs (a probe of it is priced as the set)
select kind_id, count(*), sum(amount) from events where dim_a_id in (select dim_a_id from links_a where set_id = 2) group by 1;
-- @case join_set_in_pairs_1800 | join | set of 1800 ids via IN (SELECT ...) over a table of 12,010 unique pairs (a probe of it is priced as the set)
select kind_id, count(*), sum(amount) from events where dim_a_id in (select dim_a_id from links_a where set_id = 3) group by 1;
-- @case join_set_in_pairs_all | join | set of 2000 ids via IN (SELECT ...) over a table of 12,010 unique pairs (a probe of it is priced as the set)
select kind_id, count(*), sum(amount) from events where dim_a_id in (select dim_a_id from links_a where set_id = 4) group by 1;
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

