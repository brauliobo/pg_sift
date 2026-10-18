"""Turns the output of `mdbq print` on sift-ref statements into the "id TAB count TAB pk:score,..." lines of compare.py.

    python3 -I json2ref.py STATEMENTS OUTPUT  >  got.tsv

One output line per statement: "qid TAB count TAB json", or "error TAB message" for a failed statement (the ids come from
the statement file, which has the same order).
"""
import json
import re
import sys


def main():
    ids = [re.match(r'SELECT (\d+),', l).group(1) for l in open(sys.argv[1], encoding='utf-8')]
    for qid, line in zip(ids, open(sys.argv[2], encoding='utf-8')):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'error':
            print('%s\terror\t%s' % (qid, f[1]))
            continue
        hits = json.loads(f[2])
        print('%s\t%s\t%s' % (qid, f[1], ','.join('%d:%s' % (h['id'], repr(h['s'])) for h in hits)))


main()
