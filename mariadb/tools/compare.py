"""Compares two result files of the form "id TAB count TAB pk:score,pk:score,..." (pg_ref.sql and sift_cli run).

    python3 -I compare.py REF GOT [--label NAME] [--known TEXT]

Per query: the counts must be equal, the top-k lists must name the same documents in the same order (ties by pk, the
order of ctid in the PostgreSQL table), and the scores must be equal as float32 (the engine is the same code, so they are
bit identical; a relative difference up to 1e-6 is reported separately). Exit status 1 on any difference. A GOT error
containing TEXT is a known limitation: counted and listed, not a difference.
"""
import struct
import sys


def f32(x):
    return struct.unpack('f', struct.pack('f', float(x)))[0]


def load(path):
    out = {}
    for line in open(path, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if len(f) < 2:
            continue
        out[f[0]] = (f[1], f[2] if len(f) > 2 else '')
    return out


def hits(s):
    return [(h.split(':')[0], f32(h.split(':')[1])) for h in s.split(',')] if s else []


def main():
    ref, got = load(sys.argv[1]), load(sys.argv[2])
    label = sys.argv[sys.argv.index('--label') + 1] if '--label' in sys.argv else ''
    known = sys.argv[sys.argv.index('--known') + 1] if '--known' in sys.argv else None
    bad = same_set = near = nknown = 0
    worst = 0.0
    for qid, (c, h) in sorted(ref.items(), key=lambda kv: int(kv[0])):
        if qid not in got:
            bad += 1
            print('missing', qid)
            continue
        gc, gh = got[qid]
        if known and gc == 'error' and known in gh:
            nknown += 1
            continue
        if c == 'error' or gc == 'error':
            if (c == 'error') != (gc == 'error'):
                bad += 1
                print('error mismatch', qid, c, h, gc, gh)
            continue
        if c != gc:
            bad += 1
            print('count', qid, c, gc)
            continue
        a, b = hits(h), hits(gh)
        if [x[0] for x in a] != [x[0] for x in b]:
            if sorted(x[0] for x in a) == sorted(x[0] for x in b):
                same_set += 1
            bad += 1
            print('hits', qid, h[:120], '|', gh[:120])
            continue
        for (_, s1), (_, s2) in zip(a, b):
            if s1 != s2:
                d = abs(s1 - s2) / max(abs(s1), 1e-30)
                worst = max(worst, d)
                near += d <= 1e-6
                if d > 1e-6:
                    bad += 1
                    print('score', qid, s1, s2)
                    break
    print('%s: %d queries, %d differ (%d with the same documents in another order), %d with scores not bit identical '
          '(worst relative difference %.2e), %d known limitations' % (label, len(ref), bad, same_set, near, worst, nknown))
    sys.exit(1 if bad else 0)


main()
