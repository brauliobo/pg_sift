#!/usr/bin/env python3
"""Exact-revision PostgreSQL 18 paired elapsed-time performance gate."""
import os
import sys

# The code is in bench/steps/perf_gate/, a part for each responsibility, run in the namespace of this file in this order: the tests patch the names of this module.
PARTS = ('prelude', 'verdict', 'process', 'revision', 'sizes', 'case', 'insert_control', 'insert_pair', 'insert_cost', 'measure', 'report', 'run')
PART_FILES = tuple('bench/steps/perf_gate/%s.py' % name for name in PARTS)
for _name in PARTS:
    _path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'perf_gate', _name + '.py')
    with open(_path) as _file:
        exec(compile(_file.read(), _path, 'exec'), globals())
del _name, _path, _file

if __name__=='__main__':
    sys.exit(main())
