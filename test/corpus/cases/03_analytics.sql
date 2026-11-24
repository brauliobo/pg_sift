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
select during, count(*) from spans where dim_a_id <= 20 group by 1 order by 2 desc, 1 limit 10;
-- @case c3_range_contains_by_tenant | range | spans that contain a day, by tenant
select tenant_id, count(*), sum(qty) from spans where during @> date '2025-06-15' group by 1;
-- @case c3_range_seen_overlap | range | timestamp ranges that overlap a window
select tenant_id, count(*) from spans where seen && tstzrange('2025-02-01', '2025-02-15') group by 1;
-- @case c3_range_contained | range | spans inside a quarter
select count(*), avg(amount) from spans where during <@ daterange('2025-01-01', '2025-04-01');
-- @case c3_range_adjacent | range | spans next to a day
select tenant_id, count(*) from spans where during -|- daterange('2025-05-01', '2025-05-10') group by 1;
-- @case c3_range_self_overlap | range | pairs of spans of an entity that overlap
select a.dim_a_id, count(*) from spans a join spans b on b.dim_a_id = a.dim_a_id and b.id > a.id and b.during && a.during where a.dim_a_id <= 30 group by 1;
-- @case c3_range_agg_tenant | range | the union of the spans of a tenant as a multirange
select tenant_id, range_agg(during) from spans where dim_a_id <= 200 group by 1;
-- @case c3_range_lower_month | range | spans by month of their start through lower()
select date_trunc('month', lower(during)) m, count(*), sum(amount) from spans group by 1;
-- @case c3_range_not_empty | range | non-empty spans
select tenant_id, count(*), sum(qty) from spans where not isempty(during) group by 1;
-- @case c3_range_upper_inf_false | range | spans with an upper bound
select count(*), sum(amount) from spans where upper_inf(during) is false and lower_inf(during) is false;
-- @case c3_range_lower_inc | range | spans that include their first day
select tenant_id, count(*) from spans where lower_inc(during) group by 1;
-- @case c3_range_seen_upper_inc | range | timestamp ranges that include their last instant (the parity of the last position)
select count(*) from spans where upper_inc(seen);
-- @case c3_range_bounds_minmax | range | the first start and the last end of the spans
select count(*), min(lower(during)), max(upper(during)) from spans;
-- @case c3_range_upper_key | range | the biggest groups of spans by their end
select upper(during) u, count(*) from spans where dim_a_id <= 50 group by 1 order by 2 desc, 1 limit 5;
-- @case c3_range_lower_where | range | spans that start in a month
select tenant_id, count(*) from spans where lower(during) >= date '2025-03-01' and lower(during) < date '2025-04-01' group by 1;
-- @case c3_range_not_lower_inc | range | spans that do not include their first day
select tenant_id, count(*) from spans where not lower_inc(during) group by 1;
-- @case c3_range_not_seen_upper_inc | range | timestamp ranges that do not include their last instant
select count(*), sum(qty) from spans where not upper_inc(seen);
-- @case c3_range_filter_pivot | range | one row of counts and sums by the shape of the range of each span
select tenant_id, count(*) filter (where isempty(during)) e, count(*) filter (where upper_inc(seen)) u, sum(qty) filter (where lower_inc(during)) q from spans group by 1;
-- @case c3_range_case_pivot | range | sums by a bound test of the range in a CASE
select tenant_id, sum(case when upper_inf(during) then qty else 0 end) a, sum(case when lower_inc(seen) then amount else 0 end) b from spans group by 1;
-- @case c3_range_or_windows | range | spans that overlap a window of dates or of instants
select count(*), sum(qty) from spans where during && daterange('2025-01-01', '2025-02-01') or seen && tstzrange('2025-06-01', '2025-06-15');
-- @case c3_range_not_overlap | range | spans that do not overlap a window (a NULL range is none of them)
select count(*), sum(qty) from spans where (seen && tstzrange('2025-06-01', '2025-06-15')) is not true;
