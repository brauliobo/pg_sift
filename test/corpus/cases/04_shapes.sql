-- ===================== HAVING with sub-selects
-- @case c3_having_scalar_avg | having | entities whose total is above ten times the average row
select dim_a_id, sum(amount) from events group by 1 having sum(amount) > (select avg(amount) * 10 from events);
-- @case c3_having_count_scalar | having | entities with more events than the mean entity
select dim_a_id, count(*) from events group by 1 having count(*) > (select count(*) / count(distinct dim_a_id) from events);
-- @case c3_having_in_list | having | groups of entities of one type
select dim_a_id, count(*), sum(qty) from events group by 1 having dim_a_id in (select id from dim_a where type = 'type_1');
-- @case c3_having_in_expression_key | having | groups of an expression of the entity, kept by a list (the HAVING stays on the groups)
select dim_a_id % 10, count(*), sum(qty) from events group by 1 having dim_a_id % 10 in (select id from dim_a where type = 'type_1');
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
-- @case c3_span_length_max | wide | the largest length of a span by tenant: a PostgreSQL-evaluated expression of two dates, priced in the units of the normal plan
select tenant_id, max(ends - starts) from spans group by 1;
-- @case c3_span_length_expr | wide | the largest of an expression of the length
select tenant_id, max((ends - starts) * 2 + 1) from spans group by 1;
-- @case c3_span_length_avg_max | wide | the average and the largest length together (the margin over the normal plan is thin)
select tenant_id, avg(ends - starts), max(ends - starts) from spans group by 1;
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
-- ===================== correlated sub-selects in the select list and HAVING of a grouped query
-- @case c3_select_lookup | subq | a dimension column looked up by the group key
select kind_id, count(*), sum(amount), (select k.name from kinds k where k.id = events.kind_id) from events group by 1;
-- @case c3_select_exists | subq | EXISTS of the group key in the select list
select dim_a_id, count(*), exists (select 1 from dim_b b where b.dim_a_id = events.dim_a_id and b.deleted_at is null) from events group by 1;
-- @case c3_having_outer_agg_sub | subq | HAVING a sub-select over an aggregate of the outer query
select kind_id, sum(qty) from events group by 1 having (select count(*) from kinds where id <= sum(qty) / 1000) > 2;
-- @case c3_having_not_in_count | subq | HAVING an aggregate NOT IN a list
select region, count(*) from events group by 1 having count(*) not in (select count(*) from kinds where enabled);
-- @case c3_rollup_subselect | subq | a sub-select in the output of a ROLLUP
select kind_id, region, count(*), (select count(*) from kinds) from events where kind_id < 5 group by rollup (kind_id, region);
-- @case c3_jsonb_key_filter | pred | counts and sums of the documents that have a key or one of two, by tenant
select tenant_id, count(*) filter (where attrs ? 'a') k, sum(qty) filter (where attrs ?| array['a', 'b']) q, count(*) filter (where attrs ?& array['a', 'b']) kb from events group by 1;
-- @case c3_jsonb_key_or | pred | the documents that have a key or a small dimension
select count(*), sum(qty) from events where attrs ? 'a' or dim_a_id < 50;
-- @case c3_jsonb_key_case | pred | a sum by the key of the document in a CASE
select sum(case when attrs ? 'a' then qty else 0 end), sum(case when attrs ? 'b' then amount end) from events;
-- @case c3_jsonb_key_not_true | pred | the documents that do not have the key, a NULL document among them
select count(*) filter (where (attrs ? 'a') is not true), count(*) filter (where (attrs ? 'a' or attrs ? 'b') is not true) from events;
-- @case c3_jsonb_key_read | pred | the value of a key read beside the test of the key, a number and a text, by tenant
select tenant_id, sum((attrs ->> 'a')::float8) filter (where attrs ? 'a') s, max(attrs ->> 'b') filter (where attrs ? 'b') m, count(*) filter (where attrs ? 'a') c from events group by 1;
-- @case c3_jsonb_key_read_and | pred | the value of a key compared inside the filter of the key test, by tenant
select tenant_id, sum((attrs ->> 'a')::float8) filter (where attrs ? 'a' and (attrs ->> 'a')::float8 > 100) s, count(*) filter (where attrs ? 'a' and (attrs ->> 'a')::float8 < 50) c from events group by 1;

-- ===================== HAVING: conditions on the groups, after the GROUP BY (a WHERE is a condition of the rows, before it)
-- @case having_count_gt | having | HAVING count(*) above a bound
select region, count(*) from events group by 1 having count(*) > 1600;
-- @case having_sum_not_selected | having | HAVING on a sum the select list does not carry
select kind_id from events group by 1 having sum(qty) > 2800;
-- @case having_avg_lt | having | HAVING on an average of a float with NULLs
select status, avg(amount) from events group by 1 having avg(amount) < 500;
-- @case having_min_max_dates | having | HAVING on a min and a max of a date, neither selected
select region from events group by 1 having min(d) = date '2024-01-01' and max(d) >= date '2026-09-26';
-- @case having_range_of_values | having | HAVING on the difference of a max and a min
select tenant_id, max(score) - min(score) from events group by 1 having max(score) - min(score) >= 100;
-- @case having_null_sum | having | HAVING sum IS NULL keeps the groups with no value
select kind_id, sum(amount) from events where amount is null group by 1 having sum(amount) is null;
-- @case having_filter_agg | having | HAVING on an aggregate with a FILTER
select region, count(*) filter (where status = 'paid') p from events group by 1 having count(*) filter (where status = 'paid') > 400;
-- @case having_percentile | having | HAVING on an ordered-set aggregate
select region from events group by 1 having percentile_cont(0.5) within group (order by score) >= 50;
-- @case having_array_length | having | HAVING on the length of an array_agg of distinct values
select region from events group by 1 having cardinality(array_agg(distinct kind_id)) = 40 and count(*) > 1600;
-- @case having_ungrouped_passes | having | HAVING without GROUP BY, the one group passes
select count(*), sum(qty) from events having count(*) > 100;
-- @case having_ungrouped_fails | having | HAVING without GROUP BY, the one group fails
select count(*), sum(qty) from events having count(*) < 100;
-- @case having_ungrouped_no_rows | having | HAVING without GROUP BY, no row: the group is there with a count of 0
select count(*) from events where id < 0 having count(*) = 0;
-- @case having_key_only | having | HAVING on the key only is a WHERE
select kind_id, count(*) from events group by 1 having kind_id < 10;
-- @case having_key_and_count | having | HAVING of a key and an aggregate: the key condition is a WHERE
select kind_id, count(*) from events group by 1 having kind_id < 20 and count(*) > 500;
-- @case having_where_and_having | having | WHERE changes what is added up, HAVING keeps the groups
select region, sum(amount) from events where status = 'paid' group by 1 having sum(amount) > 200000;
-- @case having_text_key_count | having | HAVING on a text key pattern and a count
select label, count(*) from events group by 1 having label like 'label_1%' and count(*) > 3;
-- @case having_date_key_count | having | HAVING on the count of the groups of a date key
select d, count(*) from events group by 1 having count(*) > 25;
-- @case having_top_n | having | HAVING and then the top 10 groups
select dim_a_id, count(*) c from events group by 1 having count(*) > 20 order by c desc, 1 limit 10;
-- @case having_with_ties | having | HAVING and then WITH TIES at the cut
select kind_id, count(*) c from events group by 1 having count(*) > 100 order by c desc fetch first 5 rows with ties;
-- @case having_rollup_count | having | HAVING over the groups of a ROLLUP
select region, status, count(*) from events group by rollup (region, status) having count(*) > 300;
-- @case having_cube_sum | having | HAVING on a sum over the groups of a CUBE
select region, status, sum(qty) from events group by cube (region, status) having sum(qty) > 5000;
-- @case having_join_lookup | having | HAVING over a join to a lookup
select k.name, count(*) from events e join kinds k on k.id = e.kind_id group by 1 having count(*) > 400;
-- @case having_window_over_groups | having | a window function over the groups that pass
select region, count(*), rank() over (order by count(*) desc) from events group by 1 having sum(qty) > 8700;
-- @case having_count_whole_row | having | HAVING count(e) of the whole row is count(*)
select region, count(e) from events e group by 1 having count(e) > 1600;
-- @case having_in_cte | having | a grouped CTE with a HAVING, read by an outer aggregate
with g as (select region, count(*) c from events group by 1 having count(*) > 1600) select count(*), sum(c) from g;
-- @case c3_case_else_count | pred | the rows counted by a CASE whose ELSE is a value, by tenant
select tenant_id, count(case when status = 'paid' then qty else 0 end) a, count(case when status = 'paid' then null else 1 end) b from events group by 1;
-- @case c3_case_else_avg_min | pred | an average, a minimum and a maximum of a CASE with ELSE 0, by tenant
select tenant_id, avg(case when status = 'paid' then qty else 0 end) a, min(case when status = 'paid' then qty else 0 end) mn, max(case when status = 'open' then kind_id else 0 end) mx, avg(case when status = 'paid' then amount else 0 end) am from events group by 1;
-- @case c3_case_else_value | pred | a minimum, a maximum and an average of a CASE whose ELSE is a value, by tenant
select tenant_id, min(case when status = 'paid' then amount else 500 end) mn, max(case when status = 'paid' then qty else 3 end) mx, avg(case when status = 'paid' then qty else 3 end) av from events group by 1;
-- @case bridge_group_join | join | a link table (set_id, dim_a_id) joined and grouped by its set column: a row counts in each of its sets
select b.set_id, count(*), sum(v.amount) from events v join dim_b b on b.dim_a_id = v.dim_a_id and b.deleted_at is null where v.dim_a_id in (select dim_a_id from dim_b where set_id = 4) group by 1;
-- @case bridge_group_left | join | the same through a LEFT JOIN: an entity in no set is a NULL group
select b.set_id, count(*), sum(v.amount) from events v left join dim_b b on b.dim_a_id = v.dim_a_id and b.deleted_at is null where v.dim_a_id between 1 and 40 group by 1;
-- @case bridge_group_distinct | join | distinct counts per set of the link table
select b.set_id, count(distinct v.id), count(distinct v.dim_a_id) from events v join dim_b b on b.dim_a_id = v.dim_a_id where v.dim_a_id in (select dim_a_id from dim_b where set_id = 1) group by 1;
-- @case bridge_group_lateral_exists | join | the sets of the row's entity as a lateral sub-select with EXISTS, grouped by set
select t.id, count(*), sum(v.amount) from events v left join lateral (select tenants.* from tenants where exists (select 1 from dim_b b where b.deleted_at is null and b.set_id = tenants.id and b.dim_a_id = v.dim_a_id)) t on true where v.dim_a_id in (select dim_a_id from dim_b where set_id = 4) group by 1;
-- @case bridge_group_lateral_in | join | the same with IN in the lateral sub-select, an inner lateral join
select t.id, count(*), sum(v.amount) from events v join lateral (select tenants.* from tenants where tenants.id in (select b.set_id from dim_b b where b.deleted_at is null and b.dim_a_id = v.dim_a_id)) t on true where v.dim_a_id in (select dim_a_id from dim_b where set_id = 1) group by 1;
-- @case bridge_group_lateral_filter | join | a condition on the group table inside the lateral sub-select
select t.id, count(*) from events v left join lateral (select tenants.* from tenants where tenants.plan = 'pro' and exists (select 1 from dim_b b where b.set_id = tenants.id and b.dim_a_id = v.dim_a_id)) t on true where v.dim_a_id in (select dim_a_id from dim_b where set_id = 1) group by 1;
-- @case bridge_group_dim_a_cte | join | the entities of a set with their sets as a CTE the planner inlines, the events joined to it
with ents as (select a.id, t.id as set_id from dim_a a left join lateral (select tenants.* from tenants where exists (select 1 from dim_b b where b.deleted_at is null and b.set_id = tenants.id and b.dim_a_id = a.id)) t on true where a.id in (select dim_a_id from dim_b where set_id = 4)) select ents.set_id, count(*), sum(v.amount) from events v join ents on ents.id = v.dim_a_id group by 1;
-- @case bridge_group_lateral_array | join | an ARRAY of the links in the lateral sub-select stays with the normal plan
select t.id, count(*) from events v left join lateral (select tenants.* from tenants where tenants.id = any (array(select b.set_id from dim_b b where b.dim_a_id = v.dim_a_id))) t on true where v.dim_a_id in (select dim_a_id from dim_b where set_id = 1) group by 1;
-- @case partial_probe_join | join | a join to a table whose only btree on the join column is partial (live links), the join's own condition implies its predicate: a probe of it for each key
select b.grp, count(*), sum(v.amount) from events v join link_live b on b.dim_a_id = v.dim_a_id and b.deleted_at is null where v.dim_a_id between 1 and 40 group by 1;
-- @case partial_probe_left | join | the same through a LEFT JOIN
select b.grp, count(*), sum(v.amount), count(b.grp) from events v left join link_live b on b.dim_a_id = v.dim_a_id and b.deleted_at is null where v.dim_a_id between 1 and 40 group by 1;
-- @case partial_probe_not_implied | join | the join's conditions do not imply the predicate of the index: the table is read whole
select b.grp, count(*), sum(v.amount) from events v join link_live b on b.dim_a_id = v.dim_a_id where v.dim_a_id between 1 and 40 group by 1;
-- @case pair_group_lateral_exists | join | the sets of the row's entity as a lateral EXISTS over a link table with a unique (set, entity) pair: the sets are joined once
select t.id, count(*), sum(v.amount) from events v left join lateral (select tenants.* from tenants where exists (select 1 from dim_b_pairs b where b.deleted_at is null and b.set_id = tenants.id and b.dim_a_id = v.dim_a_id)) t on true where v.dim_a_id in (select dim_a_id from dim_b where set_id = 4) group by 1;
-- @case pair_group_lateral_inner | join | the same as an inner lateral join
select t.id, count(*), sum(v.amount) from events v join lateral (select tenants.* from tenants where exists (select 1 from dim_b_pairs b where b.deleted_at is null and b.set_id = tenants.id and b.dim_a_id = v.dim_a_id)) t on true where v.dim_a_id in (select dim_a_id from dim_b where set_id = 1) group by 1;
-- @case pair_group_lateral_filter | join | a condition on the group table inside the lateral sub-select
select t.id, count(*) from events v left join lateral (select tenants.* from tenants where tenants.plan = 'pro' and exists (select 1 from dim_b_pairs b where b.set_id = tenants.id and b.dim_a_id = v.dim_a_id)) t on true where v.dim_a_id in (select dim_a_id from dim_b where set_id = 1) group by 1;

-- ===================== scope index: the key is a parameter or the output of an InitPlan
-- @case scope_param_join_generic | scope | a join to the pairs by a scalar parameter, generic plan
-- @pre set plan_cache_mode = force_generic_plan; prepare sj(int) as select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id where s.tenant_id = $1 group by 1;
execute sj(1 + @K@ % 50);
-- @case scope_param_join_custom | scope | the same, custom plan
-- @pre set plan_cache_mode = force_custom_plan; prepare sj(int) as select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id where s.tenant_id = $1 group by 1;
execute sj(1 + @K@ % 50);
-- @case scope_param_in_generic | scope | IN over the pairs by a scalar parameter, generic plan
-- @pre set plan_cache_mode = force_generic_plan; prepare si(int) as select date_trunc('month', d), count(distinct dim_a_id) from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id = $1) group by 1;
execute si(1 + @K@ % 50);
-- @case scope_param_array_generic | scope | IN over the pairs by an array parameter of three tenants, generic plan
-- @pre set plan_cache_mode = force_generic_plan; prepare sa(int[]) as select status, count(*), sum(amount) from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id = any ($1)) group by 1;
execute sa(array[1 + @K@ % 50, 1 + @K2@ % 50, 7]);
-- @case scope_param_array_nulls | scope | an array parameter with a NULL element, generic plan
-- @pre set plan_cache_mode = force_generic_plan; prepare sa(int[]) as select status, count(*), sum(amount) from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id = any ($1)) group by 1;
execute sa(array[1 + @K@ % 50, null]);
-- @case scope_param_null | scope | a NULL parameter keys nothing
-- @pre set plan_cache_mode = force_generic_plan; prepare si(int) as select status, count(*), sum(amount) from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id = $1) group by 1;
execute si(null);
-- @case scope_param_topn_generic | scope | the latest rows of a tenant by a parameter, generic plan
-- @pre set plan_cache_mode = force_generic_plan; prepare st(int) as select id, d from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id = $1) order by d desc, id limit 10;
execute st(1 + @K@ % 50);
-- @case scope_initplan_array | scope | the keys of a table: ARRAY(SELECT ..) is evaluated when the scan starts
select status, count(*), sum(amount) from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id = any (array(select id from tenants where plan = 'pro' and id < 12))) group by 1;
-- @case scope_initplan_scalar | scope | a scalar sub-select as the key
select status, count(*), sum(amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id where s.tenant_id = (select max(id) from tenants where plan = 'team' and id < 30) group by 1;
-- @case scope_keytable_in | scope | the keys are the rows of a table: s.key IN (SELECT key FROM keys WHERE user_id = 1), read once when the scan starts
select status, count(*), sum(amount) from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id in (select tenant_id from scope_user_keys where user_id = 1)) group by 1;
-- @case scope_keytable_exists | scope | the same under EXISTS, a key of one row
select status, count(*), sum(amount) from scope_events e where exists (select 1 from scope_pairs s where s.dim_a_id = e.dim_a_id and s.tenant_id in (select tenant_id from scope_user_keys where user_id = 2)) group by 1;
-- @case scope_keytable_empty | scope | a table of keys that gives no key reads no unit
select status, count(*), sum(amount) from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id in (select tenant_id from scope_user_keys where user_id = 9)) group by 1;
-- @case scope_keytable_join_one | scope | JOIN keys k ON k.key = s.key where the table gives one key (a primary key on the user)
select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id join scope_home k on k.tenant_id = s.tenant_id where k.user_id = 1 group by 1;
-- @case scope_keytable_join_null | scope | the home key is NULL: no key
select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id join scope_home k on k.tenant_id = s.tenant_id where k.user_id = 3 group by 1;
-- @case scope_keytable_join_many | scope | JOIN keys of a user with three keys counts a row for each pair: the scope read, or the join as written where it costs less
select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id join scope_user_keys k on k.tenant_id = s.tenant_id where k.user_id = 1 group by 1;

-- ===================== scope index: GROUP BY the key of the pairs (every tenant at once)
-- @case scope_group_key | scope | the join to the pairs grouped by the key: every key read from the scope units
select s.tenant_id, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id group by 1;
-- @case scope_group_key_status | scope | the key and a column of the facts, HAVING and ORDER BY the key
select s.tenant_id, e.status, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id group by 1, 2 having count(*) > 50 order by 1, 2;
-- @case scope_group_key_keys | scope | four tenants grouped by the key (the join as written is held for the execution)
select s.tenant_id, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id where s.tenant_id in (1, 2, 3, 4) group by 1;
-- @case scope_group_key_distinct | scope | count(DISTINCT) of the facts' entity for every tenant
select s.tenant_id, count(distinct e.dim_a_id), max(e.d) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id group by 1;
-- @case scope_group_key_topn | scope | the five tenants with the most amount
select s.tenant_id, sum(e.amount) a from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id group by 1 order by a desc, 1 limit 5;
-- @case scope_group_key_param | scope | the keys by an array parameter, grouped by the key, generic plan
-- @pre set plan_cache_mode = force_generic_plan; prepare sg(int[]) as select s.tenant_id, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id where s.tenant_id = any ($1) group by 1;
execute sg(array[3, 9, 20]);
-- @case scope_group_key_expr | scope | the key in an expression keeps the join
select s.tenant_id / 5, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id group by 1;

-- ===================== scope index: the scope read or the join as written, chosen for the keys of the execution
-- @case scope_choice_array_one | scope | an array parameter of one tenant: the scope read costs less than the join the plan holds, generic plan
-- @pre set plan_cache_mode = force_generic_plan; prepare sc1(int[]) as select status, count(*), sum(amount) from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id = any ($1)) group by 1;
execute sc1(array[1 + @K@ % 50]);
-- @case scope_choice_array_many | scope | the same plan with thirty tenants: the join as written costs less and runs (Scope Fallback: join is cheaper)
-- @pre set plan_cache_mode = force_generic_plan; prepare sc1(int[]) as select status, count(*), sum(amount) from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id = any ($1)) group by 1;
execute sc1(array[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30]);
-- @case scope_choice_pairs_few | scope | a join of three tenants counts a row for each pair: the scope read, generic plan
-- @pre set plan_cache_mode = force_generic_plan; prepare sc2(int[]) as select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id where s.tenant_id = any ($1) group by 1;
execute sc2(array[1 + @K@ % 50, 1 + @K2@ % 50, 7]);
-- @case scope_choice_pairs_many | scope | the same with thirty tenants, and a custom plan
-- @pre set plan_cache_mode = force_custom_plan; prepare sc2(int[]) as select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs s on s.dim_a_id = e.dim_a_id where s.tenant_id = any ($1) group by 1;
execute sc2(array[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30]);

-- ===================== scope index over a partial unique btree: the statement states the predicate of the pairs (rows not soft deleted) or counts the rows that are not
-- @case scope_pairs_predicate_join | scope | a join to the pairs of a partial unique btree that states its predicate
select e.status, count(*), sum(e.amount) from scope_events e join scope_grants s on s.dim_a_id = e.dim_a_id where s.tenant_id = 1 + @K@ % 50 and s.deleted_at is null group by 1;
-- @case scope_pairs_predicate_in | scope | IN over the pairs of a partial unique btree that states its predicate
select date_trunc('month', d), count(distinct dim_a_id) from scope_events where dim_a_id in (select dim_a_id from scope_grants where tenant_id = 1 + @K@ % 50 and deleted_at is null) group by 1;
-- @case scope_pairs_predicate_missing | join | the same join without the predicate counts the soft deleted rows: the join as written
select e.status, count(*), sum(e.amount) from scope_events e join scope_grants s on s.dim_a_id = e.dim_a_id where s.tenant_id = 1 + @K@ % 50 group by 1;
-- @case scope_pairs_predicate_in_missing | join | IN over the rows of the relation without the predicate: the join as written
select date_trunc('month', d), count(distinct dim_a_id) from scope_events where dim_a_id in (select dim_a_id from scope_grants where tenant_id = 1 + @K@ % 50) group by 1;

-- ===================== scope index of two paths: the pairs written as the union of the direct grants and the grants of the groups
-- @case scope_union_in | scope | IN over the union of the two paths, a member reached by both counts once
select status, count(*), sum(amount) from scope_events where dim_a_id in (select l.dim_a_id from scope_links l where l.deleted_at is null and l.tenant_id = 1 + @K@ % 50 union select m.dim_a_id from scope_links l join scope_members m on m.group_id = l.group_id where l.deleted_at is null and m.deleted_at is null and l.tenant_id = 1 + @K@ % 50) group by 1;
-- @case scope_union_in_view | scope | IN over the view of the union, three tenants
select date_trunc('month', d), count(distinct dim_a_id) from scope_events where dim_a_id in (select member from scope_pairs_union where tenant_id in (1 + @K@ % 50, 1 + @K2@ % 50, 7)) group by 1;
-- @case scope_union_join_view | scope | a join to the view of the union counts a member once for each pair of one tenant
select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs_union v on v.member = e.dim_a_id where v.tenant_id = 1 + @K@ % 50 group by 1;
-- @case scope_union_join_keys | scope | a join to the view of the union counts a member once for each pair of two tenants
select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs_union v on v.member = e.dim_a_id where v.tenant_id in (1 + @K@ % 50, 1 + @K2@ % 50) group by 1;
-- @case scope_union_join_every_key | scope | a join to the view of the union with no key counts a member once for each of its pairs
select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs_union v on v.member = e.dim_a_id group by 1;
-- @case scope_union_group_key | scope | a join to the view of the union grouped by the key: the totals of every tenant
select v.tenant_id, count(*), sum(e.amount) from scope_events e join scope_pairs_union v on v.member = e.dim_a_id group by 1;
-- @case scope_union_join_all | join | a join to a UNION ALL of the two paths counts a member of both twice: the join as written
select e.status, count(*), sum(e.amount) from scope_events e join (select l.dim_a_id as member, l.tenant_id from scope_links l where l.deleted_at is null union all select m.dim_a_id, l.tenant_id from scope_links l join scope_members m on m.group_id = l.group_id where l.deleted_at is null and m.deleted_at is null) v on v.member = e.dim_a_id where v.tenant_id = 1 + @K@ % 50 group by 1;
-- @case scope_union_missing | join | the direct path without the predicate of its btree counts the soft deleted grants: the join as written
select status, count(*), sum(amount) from scope_events where dim_a_id in (select l.dim_a_id from scope_links l where l.tenant_id = 1 + @K@ % 50 union select m.dim_a_id from scope_links l join scope_members m on m.group_id = l.group_id where l.deleted_at is null and m.deleted_at is null and l.tenant_id = 1 + @K@ % 50) group by 1;
-- @case scope_union_param | scope | the key of the union by a parameter, generic plan
-- @pre set plan_cache_mode = force_generic_plan; prepare su(int) as select e.status, count(*), sum(e.amount) from scope_events e join scope_pairs_union v on v.member = e.dim_a_id where v.tenant_id = $1 group by 1;
execute su(1 + @K@ % 50);

-- ===================== a relation of a scope that no pass takes: the join as written, with the reason
-- @case scope_refused_nested_in | join | the scope relation inside the sub-select of a semi join
select status, count(*), sum(amount) from scope_events where dim_a_id in (select o.dim_a_id from scope_events o where o.dim_a_id in (select dim_a_id from scope_pairs where tenant_id = 1 + @K@ % 50)) group by 1;
-- @case scope_refused_not_in | join | NOT IN over the pairs is a plan of its own
select status, count(*), sum(amount) from scope_events where dim_a_id not in (select dim_a_id from scope_pairs where tenant_id = 1 + @K@ % 50) group by 1;
-- @case scope_refused_two_in | scope | two IN over the pairs: one is read from the units, the other stays a join
select status, count(*), sum(amount) from scope_events where dim_a_id in (select dim_a_id from scope_pairs where tenant_id = 1 + @K@ % 50) and dim_a_id in (select dim_a_id from scope_pairs where tenant_id in (1 + @K@ % 50, 1 + @K2@ % 50)) group by 1;
-- @case scope_array_any | scope | the members as = ANY (ARRAY(SELECT ..)) are planned also as IN (SELECT ..), which the scope read takes
select status, count(*), sum(amount) from scope_events where dim_a_id = any (array(select dim_a_id from scope_pairs where tenant_id = 1 + @K@ % 50)) group by 1;
-- @case scope_array_any_keys | scope | the members of two tenants as = ANY (ARRAY(SELECT ..)) are planned also as IN
select status, count(*), sum(amount) from scope_events where dim_a_id = any (array(select dim_a_id from scope_pairs where tenant_id in (1 + @K@ % 50, 1 + @K2@ % 50))) group by 1;
-- @case scope_array_any_union | scope | the members of the union of the two paths as = ANY (ARRAY(SELECT ..))
select status, count(*), sum(amount) from scope_events where dim_a_id = any (array(select member from scope_pairs_union where tenant_id = 1 + @K@ % 50)) group by 1;
-- @case scope_array_limit | join | the members as = ANY (ARRAY(SELECT .. LIMIT n)) are an InitPlan the IN form does not read: the join as written, with its reason
select status, count(*), sum(amount) from scope_events where dim_a_id = any (array(select dim_a_id from scope_pairs where tenant_id = 1 + @K@ % 50 limit 100)) group by 1;
-- @case scope_array_or | join | the members as = ANY (ARRAY(SELECT ..)) under OR are no semi join: the join as written, with its reason
select status, count(*), sum(amount) from scope_events where dim_a_id = any (array(select dim_a_id from scope_pairs where tenant_id = 1 + @K@ % 50)) or amount = 7 group by 1;


-- ===================== regressions of the weeks before 6.0: the shapes the bugs of the Format 2.0 work were found in
-- @case sip_materialize_nl | join | an aggregate read as the inner side of a nested loop: the planner materializes it and rescans the Materialize
-- @pre set enable_hashjoin = off; set enable_mergejoin = off;
select a.id, s.n, s.total from dim_a a join (select dim_a_id, count(*) n, sum(amount) total from events group by 1) s on s.dim_a_id = a.id where a.id <= 40;
-- @case sip_materialize_twice | join | the same aggregate twice, rescanned under two nested loops
-- @pre set enable_hashjoin = off; set enable_mergejoin = off;
with s as (select dim_a_id, count(*) n, max(amount) top from events where d >= date '2025-01-01' group by 1) select a.id, s1.n, s2.top from dim_a a join s s1 on s1.dim_a_id = a.id join s s2 on s2.dim_a_id = a.id where a.id <= 30;
-- @case sip_materialize_semi | join | a materialized aggregate under a semi join read by a nested loop
-- @pre set enable_hashjoin = off; set enable_mergejoin = off;
select a.id from dim_a a where a.id <= 60 and a.id in (select dim_a_id from events group by 1 having count(*) > 20);
-- @case param_rescan_scalar | subq | a correlated scalar sub-select: one rescan of the aggregate for each outer row, with the entity as a Param
select a.id, (select count(*) from events v where v.dim_a_id = a.id and v.status = 'paid') from dim_a a where a.id <= 150;
-- @case param_rescan_max | subq | the same with max and a date bound that is a Param too
select a.id, (select max(amount) from events v where v.dim_a_id = a.id and v.d >= date '2025-01-01' + (a.id % 30)) from dim_a a where a.id <= 150;
-- @case param_rescan_exists | subq | a correlated EXISTS over a grouped sub-select
select a.id from dim_a a where a.id <= 150 and exists (select 1 from events v where v.dim_a_id = a.id group by v.kind_id having count(*) > 4);
-- @case param_rescan_lateral_group | subq | a LATERAL group by whose filter is the Param of the outer row
select a.id, x.status, x.c, x.s from dim_a a cross join lateral (select status, count(*) c, sum(amount) s from events v where v.dim_a_id = a.id group by 1) x where a.id <= 60;
-- @case param_rescan_having | subq | a sub-select with the Param in its HAVING and a LIMIT
select a.id from dim_a a where a.id <= 150 and (select count(*) from events v where v.dim_a_id = a.id group by v.kind_id order by 1 desc limit 1) > 5;
-- @case tbl_keyset_desc_first | access | the first page of a keyset pagination, newest first, of a sift_table whose rows are in the write stage
-- @pre set enable_seqscan = off; set enable_indexscan = off; set enable_bitmapscan = off;
select id, ts, amount from tbl_log order by ts desc, id desc limit 25;
-- @case tbl_keyset_desc_next | access | the next page after a (ts, id) key
-- @pre set enable_seqscan = off; set enable_indexscan = off; set enable_bitmapscan = off;
select id, ts, amount from tbl_log where (ts, id) < (timestamptz '2026-01-10', 100 + @K@) order by ts desc, id desc limit 25;
-- @case tbl_keyset_desc_entity | access | the pages of one entity, newest first, with a filter
-- @pre set enable_seqscan = off; set enable_indexscan = off; set enable_bitmapscan = off;
select id, ts, amount from tbl_log where dim_a_id <= 40 and status = 'paid' and (ts, id) < (timestamptz '2026-01-20', 100 + @K@) order by ts desc, id desc limit 25;
-- @case tbl_group_write_stage | agg | an aggregate of a sift_table with all rows in the write stage
-- @pre set enable_seqscan = off; set enable_indexscan = off; set enable_bitmapscan = off;
select status, count(*), sum(amount), avg(amount) from tbl_log group by 1;
-- @case tbl_pk_range | access | a primary key range of the sift_table
-- @pre set enable_seqscan = off; set enable_indexscan = off; set enable_bitmapscan = off;
select count(*), sum(amount), max(ts) from tbl_log where id between 100 and 400 + @K@;
-- @case search_join_star | join | BM25 text of one table joined to a dimension and grouped by it
select a.type, count(*) from docs x join dim_a a on a.id = x.dim_a_id where x.body @@> 'plug' group by 1;
-- @case search_join_tenant | join | BM25 text with a date filter, grouped by tenant
select x.tenant_id, count(*) from docs x join tenants t on t.id = x.tenant_id where x.body @@> 'cable OR relay' and x.created_at >= date '2025-01-01' group by 1;
-- @case search_join_topk | join | BM25 text with a join and a total order, limited
select x.id, a.region from docs x join dim_a a on a.id = x.dim_a_id where x.body @@> 'socket' order by x.id limit 30;
-- @case distinct_dict_text | agg | count(DISTINCT) of a text column of 50,000 values or less, per tenant
select tenant_id, count(distinct label) from events group by 1;
-- @case distinct_dict_global | agg | count(DISTINCT) of the same column over all rows
select count(distinct label), count(distinct status) from events;
-- @case distinct_wide_types | agg | count(DISTINCT) of uuid, numeric, text, a wide text, date and float4 columns per group
select g, count(distinct u), count(distinct n), count(distinct t), count(distinct long_t), count(distinct d), count(distinct f4) from typed group by 1;
-- @case distinct_wide_filter | agg | count(DISTINCT) of the wide types under a filter on a boolean and a timestamp
select b, count(distinct u), count(distinct long_t), count(distinct i8) from typed where ts >= timestamptz '2025-02-01' group by 1;
-- @case lateral_jsonb_unnest | join | LATERAL jsonb_array_elements of an indexed document array, grouped by an element field
select i ->> 'kind' as kind, count(*), sum((i ->> 'score')::float8) from jdocs d cross join lateral jsonb_array_elements(d.doc -> 'items') i group by 1;
-- @case lateral_jsonb_unnest_filter | join | the same under a filter on a field of the document and on the entity
select d.doc ->> 'channel', i ->> 'kind', count(*) from jdocs d cross join lateral jsonb_array_elements(d.doc -> 'items') i where d.dim_a_id <= 500 and d.doc ->> 'channel' <> 'shop' group by 1, 2;
-- @case lateral_jsonb_unnest_exists | join | EXISTS over the elements of the document array
select d.doc ->> 'channel', count(*) from jdocs d where exists (select 1 from jsonb_array_elements(d.doc -> 'items') i where (i ->> 'score')::float8 >= 8) group by 1;
-- @case rls_heap_fetch | scope | row level security as another role: a column that no index holds is fetched from the heap for the rows that pass the policy
-- @pre set role corpus_rls; set corpus.tenant = '3';
select dim_a_id, count(*), max(payload) from rls_items group by 1;
-- @case rls_indexed_agg | scope | row level security as another role: an aggregate of indexed columns
-- @pre set role corpus_rls; set corpus.tenant = '2';
select dim_a_id, count(*), sum(amount) from rls_items where dim_a_id <= 800 group by 1;
-- @case rls_join_dim | scope | row level security as another role with a join to a dimension the role may read
-- @pre set role corpus_rls; set corpus.tenant = '4';
select a.type, count(*), sum(r.amount) from rls_items r join dim_a a on a.id = r.dim_a_id group by 1;
