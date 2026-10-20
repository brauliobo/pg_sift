#!/usr/bin/env python3
"""Timed cells of the set filter benchmarks (docs/SET-FILTERS.md) on the data of gen.sql and build.sql.

  bench/sets/run.sh OUT.tsv [--reps N] [--design NAME[,NAME]] [--cells REGEX] [--list]

Every cell is (design, query, argument, mode): its statements run in one psql session, the last one is timed (client side, `\\timing`: planning and
execution). The cells of a query are interleaved in random order with a warm-up run first and the median of the timed runs is reported; the answer
of every cell is compared with the one of the normal plan (`sift.enable_agg = off`). The run holds /tmp/sift-perf.lock (blocking flock) for its whole
length and is meant to run in a systemd scope (run.sh). Connection: the libpq environment (PGHOST, PGPORT, PGDATABASE; the database default is `sets`).
"""
import argparse, fcntl, hashlib, os, random, re, statistics, subprocess, sys, time

GROUPS = {100: 1, 1000: 61, 10000: 81, 100000: 89}	# group_id of a membership group of that many dim_a
LAST = "event_date >= '2025-10-01'"
SEL = ("SELECT channel_id, count(*), round(avg(score)::numeric, 4) FROM events WHERE deleted_at IS NULL AND " + LAST +
	   " AND {P} GROUP BY 1 ORDER BY 1")
MODES = {	# the settings of a cell
	'normal': ["set sift.enable_agg = off"],
	'default': ["set sift.enable_agg = on"],
	'planner-guess': ["set sift.enable_agg = on", "set sift.agg_param_set_size = 0"],
	'no-index-scans': ["set sift.enable_agg = on", "set sift.agg_param_set_size = 0", "set enable_indexscan = off", "set enable_bitmapscan = off"],
	'forced': ["set sift.enable_agg = on", "set seq_page_cost = 1000", "set random_page_cost = 1000"],
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
def dim_b(size):
	"""The dim_a of the group of that size, as the text of an int4 array and as a list of literals."""
	if size not in _members:
		out = psql(["select string_agg(dim_a_id::text, ',' order by dim_a_id) from group_members where group_id = %d and deleted_at is null" % GROUPS[size]], timing=False)[1].strip()
		_members[size] = out
	return _members[size]


def array_cell(size, generic):
	"""The set filter bound as one array parameter: a generic plan (the statement is planned without the array) or a custom one."""
	pre = "set plan_cache_mode = %s" % ('force_generic_plan' if generic else 'force_custom_plan')
	return [pre, SEL.format(P="dim_a_id = ANY($1::int4[])") + "\n\\bind '{%s}' \\g" % dim_b(size)], \
		[pre, "prepare q(int4[]) as " + SEL.format(P="dim_a_id = ANY($1)"), "explain (costs off) execute q('{%s}')" % dim_b(size)]


def design1():
	"""Design 1: the set filter as an array parameter (docs/SET-FILTERS.md section 2)."""
	cells = []
	for size in GROUPS:
		lit = SEL.format(P="dim_a_id IN (%s)" % dim_b(size))
		for mode in ('normal', 'default'):
			cells.append((1, 'set_literal', size, mode, [lit], None))
		for mode in ('planner-guess', 'no-index-scans', 'default', 'normal'):
			run, expl = array_cell(size, True)
			cells.append((1, 'set_array_generic', size, mode, run, expl))
		run, expl = array_cell(size, False)
		cells.append((1, 'set_array_custom', size, 'default', run, expl))
	return cells


DESIGNS = {'1': design1}


def plan_kind(cell):
	_, name, size, mode, run, expl = cell
	if not expl:
		return ''
	try:
		out = psql(MODES[mode] + expl, timing=False)[1]
	except Exception as e:
		return 'error'
	return 'SiftAgg' if 'Custom Scan (SiftAgg)' in out else ('Bitmap' if 'Bitmap' in out else 'Index' if 'Index' in out else 'Seq')


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('out')
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
