-- Index waste under churn (bench/churn/waste.sh): one table a lane, the same rows and the same DML in every lane, and after every round the bytes,
-- free blocks and dead data of the lane's index, the WAL and dirtied blocks of each step, and the read cost of its statements.
--
-- lanes: heap (primary key only: the reference for the heap's own WAL and dirtied blocks), btree (one b-tree over the columns the column index holds),
--        col (sift_col over the same columns), gin (GIN over to_tsvector of the text column), bm25 (sift over the text column)
create schema if not exists waste;

-- one value of one metric: stage is dml | vacuum | maint | read | cic | reindex
create table if not exists waste.metric (run text, lane text, size int, round int, stage text, k text, v float8);

create table if not exists waste.note (run text, lane text, size int, round int, k text, t text);

create or replace function waste.add(p_run text, p_lane text, p_size int, p_round int, p_stage text, p_k text, p_v float8) returns void language sql as
$$ insert into waste.metric values (p_run, p_lane, p_size, p_round, p_stage, p_k, p_v) $$;

-- the rows: n documents of about 25 words from a Zipf-like vocabulary, ids in arrival order
create or replace procedure waste.mkmaster(p_n int) language plpgsql as $$
begin
	drop table if exists waste.m;
	create table waste.m as
	select g::bigint as id, mod(g, 20)::int as tenant_id, mod(abs(hashint8(g * 31)::bigint), 5000)::int as foreign_id,
		'kind_' || mod(g, 8) as kind, (mod(abs(hashint8(g * 17)::bigint), 100000) / 100.0)::numeric(12, 2) as value_num, 0 as counter_a,
		timestamptz '2026-01-01' + interval '90 days' * g / p_n as updated_at,
		(select string_agg('w' || (floor(2000 * power(abs(hashint8(g * 131 + k)::bigint)::float8 / 2147483648.0, 2.5)))::int, ' ') from generate_series(1, 25) k) as body,
		jsonb_build_object('m', mod(g, 1000) / 100.0, 'tag', 'tag_' || mod(g, 5)) as doc
	from generate_series(1, p_n) g;
end $$;

-- the index DDL of a lane
create or replace function waste.ddl(p_lane text, p_t text) returns text language sql stable as $$
	select case p_lane
		when 'btree' then format('create index %I on %I (tenant_id, foreign_id, kind, value_num, updated_at)', p_t || '_ix', p_t)
		when 'col'   then format('create index %I on %I using sift_col (tenant_id, foreign_id, kind, value_num, updated_at)%s', p_t || '_ix', p_t,
			coalesce(nullif(current_setting('waste.col_opts', true), ''), ''))
		when 'gin'   then format('create index %I on %I using gin (to_tsvector(''simple'', body))', p_t || '_ix', p_t)
		when 'bm25'  then format('create index %I on %I using sift (body)%s', p_t || '_ix', p_t, coalesce(nullif(current_setting('waste.bm25_opts', true), ''), ''))
	end $$;

create or replace procedure waste.mklane(p_lane text, p_ff int default 85) language plpgsql as $$
declare t text := 'w_' || p_lane;
begin
	execute format('drop table if exists %I', t);
	execute format('create table %I (id bigint, tenant_id int, foreign_id int, kind text, value_num numeric(12,2), counter_a int, updated_at timestamptz, body text, doc jsonb)
		with (autovacuum_enabled = %s, fillfactor = %s)', t, case when current_setting('waste.av', true) = 'on' then 'true' else 'false' end, p_ff);
	execute format('insert into %I select * from waste.m order by id', t);
	execute format('alter table %I add primary key (id)', t);
	if p_lane <> 'heap' then execute waste.ddl(p_lane, t); end if;
end $$;

-- one round of DML on a lane: the same ids in every lane (hash-selected). p_rate percent of the live rows are touched, split
-- upd_idx : upd_plain : upd_body : del : ins (any proportions); upd_idx changes value_num and updated_at (indexed columns of btree and col),
-- upd_plain changes counter_a only (HOT when the page has room), upd_body rewrites the text column (indexed by gin and bm25).
-- p_batch rows a transaction (1 is the UPDATE-by-id shape of an API).
create or replace procedure waste.dml(p_lane text, p_round int, p_rate float8, p_mix int[], p_batch int)
language plpgsql as $$
declare t text := 'w_' || p_lane; live int8; maxid int8; tot float8 := (select sum(x) from unnest(p_mix) x);
	cls text[] := array['upd_idx', 'upd_plain', 'upd_body', 'del']; c int; ids int8[]; i int; n_ins int;
begin
	execute format('select count(*), max(id) from %I', t) into live, maxid;
	for c in 1..4 loop
		if p_mix[c] = 0 then continue; end if;
		execute format($q$select coalesce(array_agg(id), '{}') from %I where mod(abs(hashint8(id * 7 + %s * 1000003 + %s)::bigint), 1000000) < 1000000 * %s$q$,
			t, p_round, c, p_rate / 100.0 * p_mix[c] / tot) into ids;
		i := 1;
		while i <= coalesce(array_length(ids, 1), 0) loop
			if c = 1 then execute format('update %I set value_num = value_num + 1, updated_at = timestamptz ''2026-04-01'' + %s * interval ''1 day'' where id = any($1)', t, p_round) using ids[i:i + p_batch - 1];
			elsif c = 2 then execute format('update %I set counter_a = counter_a + 1 where id = any($1)', t) using ids[i:i + p_batch - 1];
			elsif c = 3 then execute format($u$update %I set body = 'w' || (mod(id + %s, 2000)) || ' ' || body where id = any($1)$u$, t, p_round) using ids[i:i + p_batch - 1];
			else execute format('delete from %I where id = any($1)', t) using ids[i:i + p_batch - 1];
			end if;
			commit;
			i := i + p_batch;
		end loop;
	end loop;
	n_ins := round(p_rate / 100.0 * live * p_mix[5] / tot);
	i := 1;
	while i <= n_ins loop
		execute format($f$insert into %I select %s + g, mod(g, 20), mod(abs(hashint8(g * 31 + %s)::bigint), 5000)::int, 'kind_' || mod(g, 8),
			(mod(abs(hashint8(g * 17 + %s)::bigint), 100000) / 100.0)::numeric(12, 2), 0, timestamptz '2026-04-01' + (%s - 1) * interval '1 day' + g * interval '1 second',
			(select string_agg('w' || (floor(2000 * power(abs(hashint8(g * 131 + k + %s)::bigint)::float8 / 2147483648.0, 2.5)))::int, ' ') from generate_series(1, 25) k),
			jsonb_build_object('m', mod(g, 1000) / 100.0, 'tag', 'tag_' || mod(g, 5)) from generate_series($1, $2) g$f$,
			t, maxid, p_round, p_round, p_round, p_round) using i, least(i + p_batch - 1, n_ins);
		commit;
		i := i + p_batch;
	end loop;
end $$;

-- the state of the index of a lane: bytes of the relation, free blocks inside it and the dead data it still carries (bytes), and its structure counts
create or replace function waste.state(p_lane text) returns table (k text, v float8) language plpgsql as $$
declare ix regclass; r record; sp record;
begin
	if p_lane = 'heap' then
		return query select 'heap_bytes', pg_relation_size(('w_heap')::regclass)::float8;
		return;
	end if;
	ix := ('w_' || p_lane || '_ix')::regclass;
	k := 'idx_bytes'; v := pg_relation_size(ix); return next;
	k := 'heap_bytes'; v := pg_relation_size(('w_' || p_lane)::regclass); return next;
	if p_lane = 'col' then
		select * into r from sift.col_index_info(ix);
		k := 'free_blocks'; v := r.free_blocks; return next;
		k := 'free_extents'; v := r.free_extents; return next;
		k := 'high_water_gap'; v := r.relation_blocks - r.high_water; return next;
		select * into sp from sift.col_space(ix);
		k := 'reusable_blocks'; v := sp.reusable_blocks; return next;
		k := 'pending_blocks'; v := sp.pending_blocks; return next;
		k := 'largest_free_extent'; v := sp.largest_free_extent; return next;
		k := 'shrink_blocks'; v := sp.shrink_blocks; return next;
		k := 'segments'; v := r.segments; return next;
		k := 'buffer_rows'; v := (select coalesce(sum(rows), 0) from sift.col_segments(ix) where kind <> 'immutable'); return next;
		k := 'dead_rows'; v := (select coalesce(sum(dead_rows), 0) from sift.col_segments(ix)); return next;
		-- the share of each segment's bytes its dead rows still occupy
		k := 'dead_bytes'; v := (select coalesce(sum(bytes::float8 * dead_rows / nullif(rows, 0)), 0) from sift.col_segments(ix) where kind = 'immutable'); return next;
		k := 'live_bytes'; v := (select coalesce(sum(bytes), 0) from sift.col_segments(ix) where kind = 'immutable') - v; return next;
	elsif p_lane = 'bm25' then
		select * into r from sift.index_info(ix);
		k := 'free_blocks'; v := r.free_blocks; return next;
		k := 'free_extents'; v := r.free_extents; return next;
		k := 'high_water_gap'; v := r.relation_blocks - r.high_water; return next;
		k := 'segments'; v := r.segments; return next;
		k := 'buffer_rows'; v := (select coalesce(sum(ndocs), 0) from sift.segments(ix) where kind <> 'immutable'); return next;
		k := 'dead_docs'; v := (select coalesce(sum(dead_docs), 0) from sift.segments(ix)); return next;
		k := 'dead_bytes'; v := (select coalesce(sum(bytes::float8 * dead_docs / nullif(ndocs, 0)), 0) from sift.segments(ix) where kind = 'immutable'); return next;
	elsif p_lane = 'gin' then
		-- GIN: the free space of the pages (pageinspect; pgstattuple has no GIN) and whole pages that were deleted
		select coalesce(sum(h.upper - h.lower), 0) as fr, count(*) filter (where position('deleted' in (select flags::text from gin_page_opaque_info(get_raw_page(ix::text, b)))) > 0) as del into r
		from generate_series(1, pg_relation_size(ix) / 8192 - 1) b, lateral page_header(get_raw_page(ix::text, b)) h;
		k := 'free_blocks'; v := r.fr / 8192.0 + r.del; return next;
		k := 'deleted_pages'; v := r.del; return next;
	else
		-- b-tree: free space inside pages and pages without a live entry, dead (LP_DEAD) entries
		select * into r from pgstattuple(ix);
		k := 'free_blocks'; v := r.free_space / 8192.0; return next;
		k := 'dead_bytes'; v := r.dead_tuple_len; return next;
		k := 'live_bytes'; v := r.tuple_len; return next;
		k := 'deleted_pages'; v := (select deleted_pages + empty_pages from pgstatindex(ix)); return next;
	end if;
end $$;

-- the read statements of a lane: median of 3 of ms and the buffers of the last (hit, read) from EXPLAIN (ANALYZE, BUFFERS)
create or replace function waste.qdef(p_lane text, p_q text) returns text language sql immutable as $$
	select case when p_lane in ('heap', 'btree', 'col') then
		case p_q
			when 'grouped'  then format('select kind, count(*), sum(value_num) from w_%s where tenant_id = 7 group by kind', p_lane)
			when 'filter'   then format('select count(*), sum(value_num) from w_%s where tenant_id = 7 and foreign_id = 1234', p_lane)
			when 'range'    then format('select count(*) from w_%s where tenant_id = 7 and updated_at >= timestamptz ''2026-03-01'' and updated_at < timestamptz ''2026-03-10''', p_lane)
		end
	when p_lane = 'gin' then
		case p_q
			when 'search'   then format('select count(*) from w_gin where to_tsvector(''simple'', body) @@ to_tsquery(''simple'', ''w3 & w17'')')
			when 'topn'     then format('select id from w_gin where to_tsvector(''simple'', body) @@ to_tsquery(''simple'', ''w3 | w17'') order by ts_rank(to_tsvector(''simple'', body), to_tsquery(''simple'', ''w3 | w17'')) desc limit 10')
		end
	when p_lane = 'bm25' then
		case p_q
			when 'search'   then 'select count(*) from w_bm25 where body @@> ''w3 w17'''
			when 'topn'     then 'select id from w_bm25 where body @@> ''w3 OR w17'' order by sift.score(ctid) desc limit 10'
		end
	end $$;

create or replace function waste.reads(p_run text, p_lane text, p_size int, p_round int) returns void language plpgsql as $$
declare q text; j jsonb; t float8[]; r int; sqlt text;
begin
	for q in select unnest(case when p_lane in ('heap', 'btree', 'col') then array['grouped', 'filter', 'range'] else array['search', 'topn'] end) loop
		sqlt := waste.qdef(p_lane, q);
		t := '{}';
		for r in 1..3 loop
			execute 'explain (analyze, buffers, format json) ' || sqlt into j;
			t := t || (j->0->>'Execution Time')::float8;
		end loop;
		perform waste.add(p_run, p_lane, p_size, p_round, 'read', q || '_ms', (select percentile_disc(0.5) within group (order by x) from unnest(t) x));
		perform waste.add(p_run, p_lane, p_size, p_round, 'read', q || '_hit', coalesce((jsonb_path_query_first(j, '$.**."Shared Hit Blocks"'))::text::float8, 0));
		perform waste.add(p_run, p_lane, p_size, p_round, 'read', q || '_read', coalesce((jsonb_path_query_first(j, '$.**."Shared Read Blocks"'))::text::float8, 0));
	end loop;
end $$;

-- blocks dirtied by the statements since pg_stat_statements_reset(): p_nested counts the statements a procedure ran (its CALL holds them again)
create or replace function waste.dirtied(p_nested bool) returns float8 language sql as $$
	select coalesce(sum(shared_blks_dirtied), 0)::float8 from pg_stat_statements
	where dbid = (select oid from pg_database where datname = current_database()) and toplevel = not p_nested and query not like '%waste.%' $$;

-- the report: one line a lane and round, the stages side by side (bytes of the index after the round's VACUUM and maintenance, WAL and dirtied blocks of each step)
create or replace view waste.round as
select run, lane, size, round,
	max(v) filter (where stage = 'maint_state' and k = 'idx_bytes') as idx_bytes,
	max(v) filter (where stage = 'maint_state' and k = 'free_blocks') as free_blk,
	max(v) filter (where stage = 'maint_state' and k = 'dead_bytes') as dead_b,
	max(v) filter (where stage = 'maint_state' and k = 'segments') as segs,
	max(v) filter (where stage = 'dml_state' and k = 'buffer_rows') as buf_rows,
	max(v) filter (where stage = 'dml' and k = 'wal_bytes') as dml_wal,
	max(v) filter (where stage = 'vacuum' and k = 'wal_bytes') as vac_wal,
	max(v) filter (where stage = 'maint' and k = 'wal_bytes') as maint_wal,
	max(v) filter (where stage = 'dml' and k = 'dirtied') as dml_dirt,
	max(v) filter (where stage = 'vacuum' and k = 'dirtied') as vac_dirt,
	max(v) filter (where stage = 'vacuum' and k = 'secs') as vac_s,
	max(v) filter (where stage = 'maint' and k = 'secs') as maint_s,
	max(v) filter (where stage = 'maint' and k = 'merges') as merges,
	max(v) filter (where stage = 'maint' and k = 'merge_docs') as merge_docs,
	max(v) filter (where stage = 'maint' and k = 'merge_bytes') as merge_bytes
from waste.metric group by run, lane, size, round;
