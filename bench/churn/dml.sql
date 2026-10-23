-- churn.dml(arm, cycle, rate, upd, del, ins, hot, hot_pct, batch): one cycle of DML, identical for every arm (bench/churn/churn.sh).
-- rate: percent of the live rows touched, split upd:del:ins (any proportions); hot: percent of the touches that fall in the newest hot_pct percent of the
-- ids (0 = uniform); batch: rows per transaction. A row is picked by a hash of its id and the cycle, so every arm touches the same rows.
create or replace procedure churn.dml(p_arm text, p_cycle int, p_rate float8, p_upd int, p_del int, p_ins int, p_hot int, p_hot_pct int, p_batch int)
language plpgsql as $$
declare t text := 'ev_' || p_arm; live int8; maxid int8; hotfrom int8; touches float8; hot_rows int8; ph float8; pc float8;
	ids_u int8[]; ids_d int8[]; n_ins int; i int; tot float8 := p_upd + p_del + p_ins;
begin
	execute format('select count(*), max(id) from %I', t) into live, maxid;
	touches := p_rate / 100.0 * live;
	hotfrom := maxid - (maxid * p_hot_pct / 100);
	execute format('select count(*) filter (where id > %s) from %I', hotfrom, t) into hot_rows;
	-- per-row probability of an update or a delete, in the hot tail and outside it
	if p_hot > 0 and hot_rows > 0 then
		ph := least(1.0, touches * (p_upd + p_del) / tot * p_hot / 100.0 / hot_rows);
		pc := least(1.0, touches * (p_upd + p_del) / tot * (100 - p_hot) / 100.0 / greatest(live - hot_rows, 1));
	else
		ph := touches * (p_upd + p_del) / tot / live; pc := ph;
	end if;
	execute format($q$select coalesce(array_agg(id) filter (where fate < %s), '{}'), coalesce(array_agg(id) filter (where fate >= %s), '{}') from (
		select id, mod(abs(hashint8(id * 7 + %s)::bigint), 100) as fate from %I
		where mod(abs(hashint8(id + %s * 1000003)::bigint), 1000000) < 1000000 * (case when id > %s then %s else %s end)) s$q$,
		100.0 * p_upd / greatest(p_upd + p_del, 1), 100.0 * p_upd / greatest(p_upd + p_del, 1), p_cycle, t, p_cycle, hotfrom, ph, pc) into ids_u, ids_d;
	n_ins := round(touches * p_ins / tot);
	i := 1;
	while i <= coalesce(array_length(ids_u, 1), 0) loop
		execute format('update %I set amount = amount + 1 where id = any($1)', t) using ids_u[i:i + p_batch - 1];
		commit;
		i := i + p_batch;
	end loop;
	i := 1;
	while i <= coalesce(array_length(ids_d, 1), 0) loop
		execute format('delete from %I where id = any($1)', t) using ids_d[i:i + p_batch - 1];
		commit;
		i := i + p_batch;
	end loop;
	i := 1;
	while i <= n_ins loop
		execute format($f$insert into %I select %s + g, timestamptz '2026-04-01' + (%s - 1) * interval '1 day' + g * interval '1 second', mod(abs(hashint8(g * 31 + %s)::bigint), 10000)::int,
			'cat_' || chr(97 + mod(g, 8)), (mod(abs(hashint8(g * 17 + %s)::bigint), 100000) / 100.0)::numeric(12,2),
			jsonb_build_object('m', mod(g, 1000) / 100.0, 'tag', 'catg_' || chr(97 + mod(g, 5)), 'pad', repeat(md5(g::text), coalesce(nullif(current_setting('churn.pad', true), ''), '12')::int)) from generate_series($1, $2) g$f$,
			t, maxid, p_cycle, p_cycle, p_cycle) using i, least(i + p_batch - 1, n_ins);
		commit;
		i := i + p_batch;
	end loop;
end $$;
