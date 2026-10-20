#!/usr/bin/env python3
"""The array parameter set filter by key type (docs/SET-FILTERS.md): one generic plan, `col = ANY($1)`, for int2, int4, int8, text, date, timestamp and
timestamptz keys at 1000, 2000 and 5000 elements (and 10000, 100000 as headroom with --sizes), the normal plan against the pushed one. The table is
bench/sets/types.sql. The run holds /tmp/sift-perf.lock for its whole length; run it in a systemd scope like run.sh.
  bench/sets/types.py OUT.tsv [--reps N] [--sizes 1000,2000,5000]
"""
import argparse, fcntl, os, re, statistics, subprocess, sys

DB = os.environ.get('PGDATABASE', 'types')
COLS = [('int2', 'k2'), ('int4', 'k4'), ('int8', 'k8'), ('text', 'kt'), ('date', 'kd'), ('timestamp', 'kts'), ('timestamptz', 'ktz')]
SETUP = "set max_parallel_workers_per_gather = 2; set jit = off; set plan_cache_mode = force_generic_plan;"


def psql(sql):
	p = subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-d', DB], input=sql, env=dict(os.environ, LC_ALL='C'), capture_output=True, text=True)
	if p.returncode:
		sys.exit(p.stderr)
	return p.stdout


def pick(col, n):
	"""n distinct keys of the column, as an array literal"""
	rows = psql("select v from (select distinct %s v from t tablesample bernoulli(5)) d order by md5(v::text) limit %d" % (col, n)).split('\n')
	return "'{%s}'" % ','.join('"%s"' % r for r in rows if r)


def timed(typ, col, lit, mode):
	sql = ("%s set sift.enable_agg = %s;\nprepare q(%s[]) as select g, count(*), sum(amount) from t where %s = any($1) group by g order by g;\n"
		   "\\timing on\nexecute q(%s);\n") % (SETUP, mode, typ, col, lit)
	out = psql(sql)
	return float(re.findall(r'Time: ([0-9.]+) ms', out)[-1]), out.split('Time:')[0]


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('out')
	ap.add_argument('--reps', type=int, default=5)
	ap.add_argument('--sizes', default='1000,2000,5000')
	a = ap.parse_args()
	with open('/tmp/sift-perf.lock', 'w') as lockf:
		fcntl.flock(lockf, fcntl.LOCK_EX)
		with open(a.out, 'w') as out:
			out.write('type\telements\tnormal_ms\tpushed_ms\tratio\tplan\tsame_answer\n')
			for typ, col in COLS:
				for n in map(int, a.sizes.split(',')):
					lit = pick(col, n)
					res, ans = {}, {}
					for mode in ('off', 'on'):
						ts = []
						for r in range(a.reps + 1):
							t, ans[mode] = timed(typ, col, lit, mode)
							if r:
								ts.append(t)
						res[mode] = statistics.median(ts)
					plan = psql("%s prepare q(%s[]) as select g, count(*) from t where %s = any($1) group by g;\nexplain (costs off) execute q(%s);" % (SETUP, typ, col, lit))
					row = [typ, n, '%.1f' % res['off'], '%.1f' % res['on'], '%.2f' % (res['off'] / res['on']), 'SiftAgg' if 'SiftAgg' in plan else 'normal', ans['off'] == ans['on']]
					out.write('\t'.join(map(str, row)) + '\n')
					out.flush()


main()
