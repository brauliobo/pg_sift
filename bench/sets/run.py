#!/usr/bin/env python3
"""Timed cells of the set filter benchmarks (docs/SET-FILTERS.md) on the data of gen.sql and build.sql.

  bench/sets/run.sh OUT.tsv [--reps N] [--design NAME[,NAME]] [--cells REGEX] [--list]

Every cell is (design, query, argument, mode): its statements run in one psql session, the last one is timed (client side, `\\timing`: planning and
execution). The cells of a query are interleaved in random order with a warm-up run first and the median of the timed runs is reported; the answer
of every cell is compared with the one of the normal plan (`sift.enable_agg = off`). The run holds /tmp/sift-perf.lock (blocking flock) for its whole
length and is meant to run in a systemd scope (run.sh). Connection: the libpq environment (PGHOST, PGPORT, PGDATABASE; the database default is `sets`).
"""
import argparse, fcntl, hashlib, os, random, re, statistics, subprocess, sys, time

GROUPS = {100: 1, 1000: 61, 2000: 91, 5000: 95, 10000: 81, 100000: 89}	# group_id of a membership group of that many entities
LAST = "event_date >= '2025-10-01'"
SEL = ("SELECT channel_id, count(*), round(avg(score)::numeric, 4) FROM events WHERE deleted_at IS NULL AND " + LAST +
	   " AND {P} GROUP BY 1 ORDER BY 1")
MODES = {	# the settings of a cell
	'normal': ["set sift.enable_agg = off"],
	'default': ["set sift.enable_agg = on"],
	'planner-guess': ["set sift.enable_agg = on", "set sift.agg_param_set_size = 0"],
	'no-index-scans': ["set sift.enable_agg = on", "set sift.agg_param_set_size = 0", "set enable_indexscan = off", "set enable_bitmapscan = off"],
	'forced': ["set sift.enable_agg = on", "set seq_page_cost = 1000", "set random_page_cost = 1000"],
	# the join methods off: the aggregate pushdown is the plan, with the generic lookup join and with the byte table of a semi join
	'siftagg-generic-join': ["set sift.enable_agg = on", "set sift.agg_semi_lut = off", "set enable_hashjoin = off", "set enable_nestloop = off", "set enable_mergejoin = off"],
	# the lookup join with the whole joined table loaded, with only the rows its own conditions can match, and the default (byte table of a semi join too)
	'siftagg-lookup-full': ["set sift.enable_agg = on", "set sift.agg_semi_lut = off", "set sift.agg_lookup_pushdown = off", "set enable_hashjoin = off", "set enable_nestloop = off", "set enable_mergejoin = off"],
	'siftagg-lookup-pushdown': ["set sift.enable_agg = on", "set sift.agg_semi_lut = off", "set sift.agg_lookup_pushdown = on", "set enable_hashjoin = off", "set enable_nestloop = off", "set enable_mergejoin = off"],
	'siftagg-semi-table': ["set sift.enable_agg = on", "set sift.agg_semi_lut = on", "set enable_hashjoin = off", "set enable_nestloop = off", "set enable_mergejoin = off"],
}
COMMON = ["set max_parallel_workers_per_gather = 2", "set jit = off"]
ENV = dict(os.environ, LC_ALL='C', PGOPTIONS='-c search_path=sets,public')
DB = os.environ.get('PGDATABASE', 'sets')


def psql(statements, timing=True, timeout=600):
	script = ''.join(x + ';\n' for x in COMMON + statements[:-1]) + ('\\timing on\n' if timing else '') + statements[-1]
	script += '\n' if statements[-1].rstrip().endswith('\\g') or statements[-1].rstrip().endswith(';') else ';\n'
	p = subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-d', DB], input=script, env=ENV, capture_output=True, text=True, timeout=timeout)
	if p.returncode:
		raise RuntimeError(p.stderr.strip()[:400] + ' :: ' + statements[-1][:200])
	t = re.findall(r'Time: ([0-9.]+) ms', p.stdout)
	return (float(t[-1]) if t else None), re.sub(r'^Time: .*$', '', p.stdout, flags=re.M)


_members = {}
def members(size):
	"""The entities of the group of that size, as the text of an int4 array and as a list of literals."""
	if size not in _members:
		out = psql(["select string_agg(entity_id::text, ',' order by entity_id) from group_members where group_id = %d and deleted_at is null" % GROUPS[size]], timing=False)[1].strip()
		_members[size] = out
	return _members[size]


def array_cell(size, generic):
	"""The set filter bound as one array parameter: a generic plan (the statement is planned without the array) or a custom one."""
	pre = "set plan_cache_mode = %s" % ('force_generic_plan' if generic else 'force_custom_plan')
	return [pre, SEL.format(P="entity_id = ANY($1::int4[])") + "\n\\bind '{%s}' \\g" % members(size)], \
		[pre, "prepare q(int4[]) as " + SEL.format(P="entity_id = ANY($1)"), "explain (costs off) execute q('{%s}')" % members(size)]


def design1():
	"""Design 1: the set filter as an array parameter (docs/SET-FILTERS.md section 2)."""
	cells = []
	for size in GROUPS:
		lit = SEL.format(P="entity_id IN (%s)" % members(size))
		for mode in ('normal', 'default'):
			cells.append((1, 'set_literal', size, mode, [lit], None))
		for mode in ('planner-guess', 'no-index-scans', 'default', 'normal'):
			run, expl = array_cell(size, True)
			cells.append((1, 'set_array_generic', size, mode, run, expl))
		run, expl = array_cell(size, False)
		cells.append((1, 'set_array_custom', size, 'default', run, expl))
	return cells


def design2():
	"""Design 2: semi and anti joins from a byte table over the keys (docs/SET-FILTERS.md section 3)."""
	cells = []
	member = "m.entity_id = events.entity_id AND m.group_id = %d AND m.deleted_at IS NULL"
	attr = "e.id = events.entity_id AND e.%s"
	queries = []
	for size, g in GROUPS.items():
		queries.append(('member_in-%d' % size, "entity_id IN (SELECT entity_id FROM group_members WHERE group_id = %d AND deleted_at IS NULL)" % g))
	for size in (1000, 100000):
		queries.append(('member_exists-%d' % size, "EXISTS (SELECT 1 FROM group_members m WHERE %s)" % (member % GROUPS[size])))
		queries.append(('member_not_exists-%d' % size, "NOT EXISTS (SELECT 1 FROM group_members m WHERE %s)" % (member % GROUPS[size])))
	for label, cond in (('country', 'country_id = 7'), ('category', 'category_id = 3'), ('tier_region', 'tier = 2 AND region_id < 4')):
		queries.append(('attr_in-' + label, "entity_id IN (SELECT id FROM entities WHERE %s)" % cond))
		queries.append(('attr_exists-' + label, "EXISTS (SELECT 1 FROM entities e WHERE %s)" % (attr % cond)))
		queries.append(('attr_not_exists-' + label, "NOT EXISTS (SELECT 1 FROM entities e WHERE %s)" % (attr % cond)))
	for arg, pred in queries:
		q = SEL.format(P=pred)
		for mode in ('normal', 'default', 'siftagg-generic-join', 'siftagg-semi-table'):
			cells.append((2, 'semi_lut', arg, mode, [q], ["explain (costs off) " + q]))
	return cells


def design4():
	"""Design 4: the rows of the lookup table its own conditions cannot match are not loaded (docs/SET-FILTERS.md section 4)."""
	cells = []
	queries = []
	for size, g in GROUPS.items():
		queries.append(('member_in-%d' % size, SEL.format(P="entity_id IN (SELECT entity_id FROM group_members WHERE group_id = %d AND deleted_at IS NULL)" % g)))
	for label, cond in (('country', 'country_id = 7'), ('category', 'category_id = 3'), ('tier_region', 'tier = 2 AND region_id < 4')):
		queries.append(('attr_in-' + label, SEL.format(P="entity_id IN (SELECT id FROM entities WHERE %s)" % cond)))
		queries.append(('attr_join-' + label, "SELECT e.region_id, count(*), round(avg(score)::numeric, 4) FROM events JOIN entities e ON e.id = events.entity_id "
											   "WHERE deleted_at IS NULL AND %s AND e.%s GROUP BY 1 ORDER BY 1" % (LAST, cond.replace(' AND ', ' AND e.'))))
	for arg, q in queries:
		for mode in ('normal', 'siftagg-lookup-full', 'siftagg-lookup-pushdown', 'siftagg-semi-table'):
			cells.append((4, 'lookup_load', arg, mode, [q], ["explain (costs off) " + q]))
	return cells


DESIGNS = {'1': design1, '2': design2, '4': design4}


def plan_kind(cell):
	_, name, size, mode, run, expl = cell
	if not expl:
		return ''
	try:
		out = psql(MODES[mode] + expl, timing=False)[1]
	except Exception as e:
		return 'error'
	return 'SiftAgg' if 'Custom Scan (SiftAgg)' in out else ('Join' if 'Join' in out else 'Bitmap' if 'Bitmap' in out else 'Index' if 'Index' in out else 'Seq')


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('out', nargs='?')
	ap.add_argument('--reps', type=int, default=5)
	ap.add_argument('--design', default=','.join(DESIGNS))
	ap.add_argument('--cells', default='')
	ap.add_argument('--list', action='store_true')
	a = ap.parse_args()
	cells = []
	for d in a.design.split(','):
		cells += DESIGNS[d]()
	if a.cells:
		cells = [c for c in cells if re.search(a.cells, '%s/%s/%s' % (c[1], c[2], c[3]))]
	if a.list:
		for c in cells:
			print(c[0], c[1], c[2], c[3])
		return
	lockf = open('/tmp/sift-perf.lock', 'w')
	while True:
		try:
			fcntl.flock(lockf, fcntl.LOCK_EX)
			break
		except OSError:
			time.sleep(5)
	load0 = open('/proc/loadavg').read().split()[:3]
	times = {i: [] for i in range(len(cells))}
	digest = {}
	for rep in range(a.reps + 1):
		order = list(range(len(cells)))
		random.Random(rep).shuffle(order)
		for i in order:
			d, name, size, mode, run, expl = cells[i]
			t, out = psql(MODES[mode] + run)
			if rep == 0:
				digest[i] = hashlib.md5(out.encode()).hexdigest()
			else:
				times[i].append(t)
	ref = {}
	for i, c in enumerate(cells):
		if c[3] == 'normal':
			ref[(c[1].split('_')[0], c[2])] = digest[i]
	load1 = open('/proc/loadavg').read().split()[:3]
	with open(a.out, 'w') as f:
		f.write('# date %s load %s -> %s reps %d\n' % (time.strftime('%Y-%m-%d %H:%M'), ' '.join(load0), ' '.join(load1), a.reps))
		f.write('design\tquery\targ\tmode\tplan\tmedian_ms\tmin_ms\truns\tresult_ok\n')
		for i, c in enumerate(cells):
			d, name, size, mode, run, expl = c
			ts = times[i]
			ok = digest[i] == ref.get((name.split('_')[0], size), digest[i])
			f.write('\t'.join(map(str, [d, name, size, mode, plan_kind(c), round(statistics.median(ts), 2), round(min(ts), 2), len(ts), ok])) + '\n')
	print(open(a.out).read())


if __name__ == '__main__':
	main()
