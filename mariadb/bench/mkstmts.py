"""SQL statement files for the MariaDB tests and benchmark (one statement per line, run by mdbq).

    python3 -I mkstmts.py KIND FILE [CLASS] [--index=wiki] [--k=10] [--table=docs] [--limit=N]

FILE of the sift-ref and sift-qmatches kinds: lines "id TAB siftql query" (genq.py). FILE of the others: the benchmark
trace, lines "id TAB mode TAB term term ..." (mode and/or/phrase, the plain lowercase words of a substring of an
article); CLASS keeps only that mode, --limit keeps the first N lines of it. Kinds:
  sift-topk      top-k joined with the table, the way an application would use it (JSON_TABLE + join, ordered by rank)
  sift-raw       the UDF alone: SELECT sift_topk(...)
  sift-count     SELECT sift_count(...)
  sift-ref       qid, count and the JSON top-k, for the comparison with pg_sift (json2ref.py, compare.py)
  sift-matches   qid and the JSON array of all matching keys of a trace query (set comparison with InnoDB)
  inno-topk      InnoDB FULLTEXT: ORDER BY MATCH ... DESC LIMIT k; and/phrase in boolean mode, or in natural language
  inno-topk-bool the same with OR in boolean mode
  inno-count     SELECT COUNT(*) ... MATCH ... IN BOOLEAN MODE
  inno-matches   qid and the ids of all matches
"""
import sys


def lit(s):
    return "'" + s.replace('\\', '\\\\').replace("'", "''") + "'"


def siftql(mode, terms):
    return {'and': ' AND '.join(terms), 'or': ' OR '.join(terms), 'phrase': '"' + ' '.join(terms) + '"'}[mode]


def inno(mode, terms, boolean_or):
    if mode == 'and':
        return ' '.join('+' + t for t in terms), True
    if mode == 'phrase':
        return '"' + ' '.join(terms) + '"', True
    return ' '.join(terms), boolean_or


def match(arg, boolean):
    return "MATCH(body) AGAINST(%s%s)" % (lit(arg), ' IN BOOLEAN MODE' if boolean else '')


COLS = "(n FOR ORDINALITY, id BIGINT PATH '$.id', s DOUBLE PATH '$.s')"


def sift_stmt(kind, qid, q, index, k):
    if kind == 'sift-topk':
        return ("SELECT d.id, j.s FROM JSON_TABLE(sift_topk('%s', %s, %d), '$[*]' COLUMNS %s) j "
                "JOIN docs d ON d.id = j.id ORDER BY j.n" % (index, q, k, COLS))
    if kind == 'sift-raw':
        return "SELECT sift_topk('%s', %s, %d)" % (index, q, k)
    if kind == 'sift-count':
        return "SELECT sift_count('%s', %s)" % (index, q)
    if kind == 'sift-ref':
        return "SELECT %s, sift_count('%s', %s), sift_topk('%s', %s, %d)" % (qid, index, q, index, q, k)
    if kind == 'sift-matches':
        return "SELECT %s, sift_matches('%s', %s)" % (qid, index, q)


def inno_stmt(kind, qid, mode, terms, k, table):
    arg, boolean = inno(mode, terms, kind == 'inno-topk-bool')
    m = match(arg, boolean)
    if kind in ('inno-topk', 'inno-topk-bool'):
        return "SELECT id FROM %s WHERE %s ORDER BY %s DESC LIMIT %d" % (table, m, m, k)
    if kind == 'inno-count':
        return "SELECT COUNT(*) FROM %s WHERE %s" % (table, match(arg, True))
    if kind == 'inno-matches':
        return "SELECT %s, id FROM %s WHERE %s" % (qid, table, match(arg, True))
    raise SystemExit('unknown kind ' + kind)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opt = {a.split('=')[0]: a.split('=')[1] for a in sys.argv[1:] if a.startswith('--') and '=' in a}
    kind, path = args[0], args[1]
    only = args[2] if len(args) > 2 else None
    index, k, limit = opt.get('--index', 'wiki'), int(opt.get('--k', 10)), int(opt.get('--limit', 1 << 30))
    n = 0
    for line in open(path, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if kind == 'sift-ref':
            print(sift_stmt(kind, f[0], lit(f[1]), index, k))
            continue
        if only and f[1] != only:
            continue
        n += 1
        if n > limit:
            break
        terms = f[2].split()
        if kind.startswith('sift-'):
            print(sift_stmt(kind, f[0], lit(siftql(f[1], terms)), index, k))
        else:
            print(inno_stmt(kind, f[0], f[1], terms, k, opt.get("--table", "docs")))


main()
