-- Churn simulator for sift_col indexes (bench/churn/churn.sh): synthetic events, deterministic hash-selected DML, and the state of an index
-- after every cycle: segments, dead rows, dirty units, pre-aggregate coverage, zone-map tightness, the latency of three query shapes.
create schema if not exists churn;

create table if not exists churn.state (
	scn text, arm text, cycle int, stage text,
	heap_bytes int8, idx_bytes int8, idx_live_bytes int8, heap_pages int8, all_visible int8,
	segs int, seg_min int8, seg_med int8, seg_max int8, buf_rows int8, rows_imm int8, dead_rows int8, dirty_segs int,
	units_listed int, units_covered int, units_exact int, zone_width float8,
	primary key (scn, arm, cycle, stage));

create table if not exists churn.qres (
	scn text, arm text, cycle int, stage text, q text, ms float8, units int, pre int, pruned int, rows_scanned int8, vetted int8,
	primary key (scn, arm, cycle, stage, q));

-- one row per step of a cycle: wall seconds, WAL bytes, bytes of the segments the step wrote
-- merges, max_merge_ms: the merge steps of the step in the server log (CHURN_LOG) and the longest, which is how long the maintenance lock was held
create table if not exists churn.log (scn text, arm text, cycle int, step text, secs float8, wal_bytes int8, seg_bytes int8, load1 float8, merges int, max_merge_ms int);


-- and a group-by that sums a float8 json path and always reads the rows
create or replace function churn.qdef(p_q text, p_tbl text) returns text language sql immutable as $$
	select case p_q
		when 'rollup' then format('select kind, count(*), sum(amount) from %I group by kind', p_tbl)
		when 'window' then format('select kind, count(*), sum(amount) from %I where ts >= timestamptz ''2026-01-01'' + interval ''90 days'' * %s group by kind', p_tbl, 0.97)
		when 'scan'   then format('select kind, count(*), sum((payload->>''m'')::float8) from %I group by kind', p_tbl)
	end $$;

-- one query: median of 3 execution times, the counters of the last run
create or replace function churn.runq(p_sql text)
returns table (ms float8, units int, pre int, pruned int, rows_scanned int8, vetted int8) language plpgsql as $$
declare j jsonb; t float8[] := '{}';
begin
	perform set_config('sift.enable_agg', 'on', true);
	for i in 1..3 loop
		execute 'explain (analyze, format json) ' || p_sql into j;
		t := t || (j->0->>'Execution Time')::float8;
	end loop;
	select percentile_disc(0.5) within group (order by x) into ms from unnest(t) x;
	units := (jsonb_path_query_first(j, '$.**."Units"'))::text::int;
	pre := (jsonb_path_query_first(j, '$.**."Units From Pre-aggregates"'))::text::int;
	pruned := (jsonb_path_query_first(j, '$.**."Units Pruned"'))::text::int;
	rows_scanned := (jsonb_path_query_first(j, '$.**."Rows Scanned"'))::text::int8;
	vetted := (jsonb_path_query_first(j, '$.**."Rows Vetted By Heap"'))::text::int8;
	return next;
end $$;

-- the state of arm p_arm (table ev_<arm>, index ev_<arm>_col) after a stage of a cycle
create or replace function churn.measure(p_scn text, p_arm text, p_cycle int, p_stage text, p_queries bool default true)
returns void language plpgsql as $$
declare t text := 'ev_' || p_arm; ix text := t || '_col'; q text; r record;
begin
	delete from churn.state where scn = p_scn and arm = p_arm and cycle = p_cycle and stage = p_stage;
	delete from churn.qres where scn = p_scn and arm = p_arm and cycle = p_cycle and stage = p_stage;
	insert into churn.state
	select p_scn, p_arm, p_cycle, p_stage, pg_relation_size(t::regclass), pg_relation_size(ix::regclass),
		(select coalesce(sum(bytes), 0) from sift.col_segments(ix) where kind = 'immutable'),
		(select relpages from pg_class where oid = t::regclass), (select all_visible from pg_visibility_map_summary(t::regclass)),
		(select count(*) from sift.col_segments(ix) where kind = 'immutable'),
		(select min(bytes) from sift.col_segments(ix) where kind = 'immutable'),
		(select percentile_disc(0.5) within group (order by bytes) from sift.col_segments(ix) where kind = 'immutable'),
		(select max(bytes) from sift.col_segments(ix) where kind = 'immutable'),
		(select coalesce(sum(rows), 0) from sift.col_segments(ix) where kind <> 'immutable'),
		(select coalesce(sum(rows), 0) from sift.col_segments(ix) where kind = 'immutable'),
		(select coalesce(sum(dead_rows), 0) from sift.col_segments(ix)), (select count(*) from sift.col_segments(ix) where dirty),
		(select count(*) from sift.col_unit_agg(ix, 'amount')), (select units from sift.col_index_info(ix)),
		(select count(*) from sift.col_unit_agg(ix, 'amount') where exact),
		-- zone-map tightness: the units a point lookup of ts reads in expectation, the sum of the width of each unit's ts zone over the whole range (1 = ideal)
		(select sum(extract(epoch from (max::timestamptz - min::timestamptz))) / nullif(extract(epoch from (max(max::timestamptz) - min(min::timestamptz))), 0)
		   from sift.col_unit_agg(ix, 'ts'));
	if not p_queries then return; end if;
	for q in select unnest(array['rollup', 'window', 'scan']) loop
		select * into r from churn.runq(churn.qdef(q, t));
		insert into churn.qres values (p_scn, p_arm, p_cycle, p_stage, q, r.ms, r.units, r.pre, r.pruned, r.rows_scanned, r.vetted);
	end loop;
end $$;

-- the master data: n events in arrival order over 90 days; key uniform, 8 kinds, a padded payload (about 450 bytes a row)
create or replace procedure churn.mkmaster(p_n int) language plpgsql as $$
begin
	drop table if exists churn.m;
	create table churn.m as
	select g::bigint as id, timestamptz '2026-01-01' + interval '90 days' * g / p_n as ts, mod(abs(hashint8(g * 31)::bigint), 10000)::int as key,
		'cat_' || chr(97 + mod(g, 8)) as kind, (mod(abs(hashint8(g * 17)::bigint), 100000) / 100.0)::numeric(12,2) as amount,
		jsonb_build_object('m', mod(g, 1000) / 100.0, 'tag', 'asp_' || chr(97 + mod(g, 5)), 'pad', repeat(md5(g::text), 12)) as payload
	from generate_series(1, p_n) g;
end $$;

-- a copy of the master with its index; p_opts: index options ('' for none), p_ff the fillfactor
create or replace procedure churn.mkarm(p_arm text, p_ff int, p_opts text default '')
language plpgsql as $$
declare t text := 'ev_' || p_arm;
begin
	execute format('drop table if exists %I', t);
	execute format('create table %I (id bigint, ts timestamptz, key int, kind text, amount numeric(12,2), payload jsonb) with (autovacuum_enabled = false, fillfactor = %s)', t, p_ff);
	execute format('insert into %I select * from churn.m order by id', t);
	execute format('alter table %I add primary key (id)', t);
	execute format('create index %I on %I using sift_col (ts, key, kind, amount, payload) with (paths = ''payload.m float8, payload.tag text''%s)', t || '_col', t,
		case when p_opts = '' then '' else ', ' || p_opts end);
end $$;


-- the answers of the three shapes (floats rounded), as one digest: equal for every arm, the DML being the same, whatever the maintenance did
create or replace function churn.digest(p_arm text, p_agg bool default true) returns text language plpgsql as $$
declare t text := 'ev_' || p_arm; d text := ''; r text;
begin
	perform set_config('sift.enable_agg', case when p_agg then 'on' else 'off' end, true);
	foreach r in array array[
		format('select kind, count(*), sum(amount) from %I group by kind', t),
		format('select kind, count(*), sum(amount) from %I where ts >= timestamptz ''2026-01-01'' + interval ''90 days'' * 0.97 group by kind', t),
		format('select kind, count(*), round(sum((payload->>''m'')::float8)::numeric, 3) from %I group by kind', t)] loop
		execute format('select md5(string_agg(x::text, '','' order by x::text)) from (%s) x', r) into r;
		d := d || r;
	end loop;
	return md5(d);
end $$;
