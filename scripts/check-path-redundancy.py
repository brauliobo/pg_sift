#!/usr/bin/env python3
"""A file of src/ or tools/vk-v6/tests/ does not repeat in its name the folders it is in.

  x/foo/foo_bar.c      -> x/foo/bar.c        the folder says it (foo_int.h and foo_priv.h are int.h)
  col/f2/f2_scan.c     -> col/f2/scan.c      nor the format of an ancestor (f2_, format2_, v6_, v1_ and the _f2, _v6, _v1 suffix)
  x/foo/foo.c          is the entry file of its folder and stays

Fix the name (scripts/refactor-subfolders.py --apply does it for a whole tree), or move the file to the folder of its context: do not add it to an allow list.
scripts/check-path-redundancy.py [--selftest]
"""
import os
import re
import subprocess
import sys

ROOTS = ('src/', 'tools/vk-v6/tests/')
FORMATS = {'f2': ('f2', 'format2'), 'v6': ('v6',), 'v1': ('v1',)}


def split_name(base):
	stem, ext = os.path.splitext(base)
	return stem, ext


def echo_tokens(comps):
	return {c.replace('-', '_') for c in comps} | {a for c in comps for a in FORMATS.get(c, ())}


def format_tokens(comps):
	return {a for c in comps for a in FORMATS.get(c, ())}


def strip_one(comps, base, area_too=True):
	"""The name without one redundant prefix or suffix, or None when it has none."""
	stem, ext = split_name(base)
	own = comps if area_too else comps[1:]
	for t in sorted(echo_tokens(own), key=len, reverse=True):
		if stem.startswith(t + '_') and len(stem) > len(t) + 1:
			return stem[len(t) + 1:] + ext
	for t in sorted(format_tokens(comps), key=len, reverse=True):
		if stem.endswith('_' + t) and len(stem) > len(t) + 1:
			return stem[:-len(t) - 1] + ext
	return None


def violations(paths):
	for p in paths:
		root = next((r for r in ROOTS if p.startswith(r)), None)
		if root is None:
			continue
		comps = p[len(root):].split('/')[:-1] if root == 'src/' else ['vk-v6']
		if not comps:
			continue
		base = os.path.basename(p)
		fix = strip_one(comps, base)
		if fix:
			yield f'{p}: repeats its folders; call it {fix} (or put it in the folder of its context)'
		elif base == 'priv.h' or base.endswith('_priv.h'):
			yield f'{p}: the private header of a context folder is int.h'


def tracked():
	return subprocess.run(['git', 'ls-files', '--', *ROOTS], check=True, capture_output=True, text=True).stdout.split('\n')


def selftest():
	bad = [
		'src/col/f2/f2_scan.c', 'src/col/f2/track_f2.c', 'src/bm25/bm25_doc.c', 'src/agg/vk/shaders/vk_topn.comp',
		'src/core/maint/maint_cut.c', 'src/bm25/runs/runs_priv.h', 'tools/vk-v6/tests/vk_v6_gen.c', 'src/col/f2/format2_test.c',
	]
	ok = [
		'src/col/f2/f2.h', 'src/col/f2/scope/scope.c', 'src/col/f2/v6/proj/proj.c', 'src/agg/vk/int.h', 'src/col/f2/col_delta/idx.c',
		'src/table/f2/lock/int.h', 'tools/vk-v6/tests/gen.c', 'src/col/f2/format.c',
	]
	got = {v.split(':')[0] for v in violations(bad)}
	assert got == set(bad), sorted(set(bad) - got)
	assert not list(violations(ok)), list(violations(ok))
	print('check-path-redundancy: selftest ok')


def main():
	if sys.argv[1:] == ['--selftest']:
		return selftest()
	found = list(violations(tracked()))
	print('\n'.join(found))
	if found:
		print(f'check-path-redundancy: {len(found)} file(s) repeat the folder or format they are in', file=sys.stderr)
		sys.exit(1)


if __name__ == '__main__':
	main()
