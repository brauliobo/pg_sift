#!/usr/bin/env python3
"""Design 5 (docs/SET-FILTERS.md section 6): the write-path costs of an attribute copied into the fact table. Not timed (WAL bytes and unit counts are
deterministic), so no host lock. It changes nothing: every write runs in a transaction that is rolled back, except the late rows, which go to copies.
  bench/sets/denorm.py OUT.tsv
Needs the tables of build.sql and denorm.sql in the database `sets` (PGDATABASE).
"""
import os, re, subprocess, sys

DB = os.environ.get('PGDATABASE', 'sets')
ENV = dict(os.environ, PGOPTIONS='-c search_path=sets,public')
rows = []


def psql(sql):
	p = subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-d', DB], input=sql, env=ENV, capture_output=True, text=True)
	if p.returncode:
		sys.exit(p.stderr + sql)
	return p.stdout.strip()


def wal(stmt):
	"""WAL bytes of one statement (rolled back) and the rows it touched"""
	out = psql("begin;\nselect pg_current_wal_insert_lsn() as a \\gset\n%s\nselect pg_current_wal_insert_lsn() - :'a'::pg_lsn;\nrollback;" % stmt)
	return int(out.split('\n')[-1])


def add(*r):
	rows.append(r)
	print('\t'.join(map(str, r)), flush=True)


def prune(table, where):
	out = psql("set max_parallel_workers_per_gather = 0;\nexplain (analyze, costs off, timing off, summary off, buffers off) "
			   "select channel_id, count(*) from %s where deleted_at is null and %s group by 1" % (table, where))
	f = lambda k: int(re.search(k + r': (\d+)', out).group(1))
	return f('Units'), f('Units Pruned')


add('measure', 'case', 'value', 'unit')
dim_b = psql("select count(*) from dim_a where country_id = 7")
add('dim_a', 'country_id = 7', dim_b, 'dim_a')
# a country moves: the dimension row, against the copy in every event of the entity
dim = wal("update dim_a set country_id = 8 where country_id = 7;")
add('wal', 'update dim_a (dimension)', dim, 'bytes')
for t in ('events_dn_u', 'events_dn'):
	n = psql("select count(*) from %s e join dim_a d on d.id = e.dim_a_id where d.country_id = 7" % t)
	b = wal("update %s e set country_id = 8 from dim_a d where d.id = e.dim_a_id and d.country_id = 7;" % t)
	add('wal', 'update %s (denormalised copy)' % t, b, 'bytes')
	add('rows', 'events updated in %s' % t, n, 'rows')
	add('wal_ratio', 'denormalised / dimension (%s)' % t, '%.0f' % (b / dim), 'x')
# one entity gets a new event: the denormalised row is 2 bytes wider
add('wal', 'insert 1000 events (heap, no copy)', wal("insert into events select * from events limit 1000;"), 'bytes')
add('wal', 'insert 1000 events (denormalised)', wal("insert into events_dn_u select * from events_dn_u limit 1000;"), 'bytes')
# clustering decay: the units of the clustered copy hold few countries; late rows in insertion order land in the last units
for t in ('events_dn', 'events_dn_u'):
	u, p = prune(t, 'country_id = 7')
	add('units', 'country_id = 7 on %s' % t, u, 'units')
	add('units_pruned', 'country_id = 7 on %s' % t, p, 'units')
psql("drop table if exists events_dn_late; create table events_dn_late as select * from events_dn;\n"
	 "create index events_dn_late_col on events_dn_late using sift_col (dim_a_id, channel_id, lang_id, signal, event_date, score, deleted_at, country_id, category_id);\n"
	 "insert into events_dn_late select id + 100000000, dim_a_id, channel_id, lang_id, signal, event_date, score, deleted_at, country_id, category_id "
	 "from events_dn_u where random() < 0.03;\nvacuum analyze events_dn_late;")
u0, p0 = prune('events_dn', 'country_id = 7')
u1, p1 = prune('events_dn_late', 'country_id = 7')
add('units', 'after 3% late rows (insertion order), country_id = 7', u1, 'units')
add('units_pruned', 'after 3% late rows (insertion order), country_id = 7', p1, 'units')
add('late_rows', 'rows in the late tail', psql("select count(*) from events_dn_late where id > 100000000"), 'rows')
psql("drop table events_dn_late")
open(sys.argv[1], 'w').write('\n'.join('\t'.join(map(str, r)) for r in rows) + '\n')
