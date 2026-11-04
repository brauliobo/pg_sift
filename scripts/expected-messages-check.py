#!/usr/bin/env python3
# expected-messages-check.py [BASE]   After a change of the wording of the messages the expected files of test/expected are regenerated; this proves that the regeneration changed only messages.
# For every file of test/expected that differs from BASE (default HEAD) it compares the lines of the two versions, and every line that was removed or added must be a message line:
# NOTICE, WARNING, INFO, DETAIL, HINT, CONTEXT or LOG (the lines psql prints for them), or the continuation of a DETAIL or HINT (the value lines of the summary of sift.index_choices,
# or a prose line of a DETAIL that spans several lines), which follows a message line. Anything else (a row of a result, an ERROR, a plan line) is a real change and is listed.
# Prints "N files, M lines changed, K non-message lines"; exit 1 when K is not 0.
import difflib
import re
import subprocess
import sys

base = sys.argv[1] if len(sys.argv) > 1 else 'HEAD'
MESSAGE = re.compile(r'^(NOTICE|WARNING|INFO|DETAIL|HINT|CONTEXT|LOG):  ')
DETAIL = re.compile(r'^(DETAIL|HINT):  ')
VALUES = re.compile(r'^(advice: |limit: )?[a-z_]+ = ')
# a prose line: no table bar, no indent, no result footer or error, at least four words
PROSE = re.compile(r'^[^\s(\-|][^|]*$')


def git(*args):
    return subprocess.run(['git', *args], capture_output=True, text=True, check=True).stdout


def continues(line):
    return bool(VALUES.match(line)) or (bool(PROSE.match(line)) and not line.startswith('ERROR') and len(line.split()) >= 4)


def message_flags(lines):
    """True for the lines that belong to a message: a message line, or a continuation after a DETAIL or HINT"""
    flags, open_detail = [], False
    for line in lines:
        if MESSAGE.match(line):
            ok, open_detail = True, bool(DETAIL.match(line))
        elif open_detail and continues(line):
            ok = True
        else:
            ok, open_detail = False, False
        flags.append(ok)
    return flags


files = git('diff', '--name-only', base, '--', 'test/expected').split()
changed = bad = 0
for path in files:
    old = git('show', '%s:%s' % (base, path)).split('\n')
    try:
        with open(path, encoding='utf-8') as f:
            new = f.read().split('\n')
    except FileNotFoundError:
        continue
    of, nf = message_flags(old), message_flags(new)
    for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
        if op == 'equal':
            continue
        for k in range(i1, i2):
            changed += 1
            if not of[k]:
                bad += 1
                print('%s: removed: %s' % (path, old[k][:140]))
        for k in range(j1, j2):
            changed += 1
            if not nf[k]:
                bad += 1
                print('%s: added: %s' % (path, new[k][:140]))
print('%d files, %d lines changed, %d non-message lines' % (len(files), changed, bad))
sys.exit(1 if bad else 0)
