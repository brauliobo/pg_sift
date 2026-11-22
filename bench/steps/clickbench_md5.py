"""`md5_equal` of ClickBench results: every engine's answer is decoded into rows of strings, normalized the same way (NULL is its own token, the AVG
columns of the query are rounded to 9 significant digits from their decimal text, everything else is compared exactly), the rows are sorted and
hashed. Two engines agree on a query when the hashes are equal. `ordered_md5` also hashes the rows in the order the engine returned them.

    clickbench_md5.py --selftest"""
import csv
import hashlib
import io
import re
import sys
from decimal import Context, Decimal, InvalidOperation, ROUND_HALF_EVEN

NULL = '\\N'
ESCAPES = {'b': '\b', 'f': '\f', 'n': '\n', 'r': '\r', 't': '\t', 'v': '\v', '0': '\0', "'": "'", '\\': '\\'}
DIGITS = Context(prec=9, rounding=ROUND_HALF_EVEN)


def unescape(cell):
    return None if cell == NULL else re.sub(r'\\(.)', lambda m: ESCAPES.get(m.group(1), m.group(0)), cell)


def decode_tsv(text):
    """PostgreSQL `COPY (query) TO STDOUT` and ClickHouse TabSeparated output: one row a line, \\N for NULL, backslash escapes"""
    return [[unescape(c) for c in line.split('\t')] for line in text.split('\n') if line != '']


def decode_csv_tab(text):
    """DuckDB `COPY (query) TO ... (FORMAT csv, DELIMITER '\\t', NULL '\\N')`: quoted cells, \\N for NULL"""
    return [[None if c == NULL else c for c in row] for row in csv.reader(io.StringIO(text), delimiter='\t', quotechar='"')]


def number(cell):
    d = Decimal(cell)
    if not d.is_finite():
        return cell.lower()
    return '0' if d == 0 else format(DIGITS.plus(d).normalize(), 'f')


def normalize(rows, floats=()):
    def cell(i, c):
        if c is None:
            return NULL
        try:
            return number(c) if i in floats else c
        except InvalidOperation:
            raise ValueError('column %d is not a number: %r' % (i, c[:60]))
    return [tuple(cell(i, c) for i, c in enumerate(r)) for r in rows]


def digest(rows):
    h = hashlib.md5()
    for r in rows:
        h.update('\x1f'.join(r).encode() + b'\x1e')
    return h.hexdigest()


def md5s(rows, floats=()):
    """(md5 of the sorted rows, md5 of the rows as returned, row count)"""
    norm = normalize(rows, floats)
    return digest(sorted(norm)), digest(norm), len(norm)


def first_diff(a, b, floats=()):
    x, y = sorted(normalize(a, floats)), sorted(normalize(b, floats))
    for i in range(max(len(x), len(y))):
        if i >= len(x) or i >= len(y) or x[i] != y[i]:
            return 'row %d: %s != %s (%d vs %d rows)' % (i, (x[i] if i < len(x) else '-'), (y[i] if i < len(y) else '-'), len(x), len(y))
    return ''


def selftest():
    bad = []

    def check(name, ok):
        if not ok:
            bad.append(name)
    check('tsv escapes', decode_tsv("a\\tb\tx\\ny\t\\N\tq\\'z\t\\\\\n") == [['a\tb', 'x\ny', None, "q'z", '\\']])
    check('tsv empty string is not NULL', decode_tsv('\t\\N\n') == [['', None]])
    check('csv', decode_csv_tab('"a\tb"\t""\t\\N\n') == [['a\tb', '', None]])
    pg, ch, ex = [['1234.567890123456']], [['1234.5678901234563']], [['1.2345678901234563e+3']]
    check('floats from numeric, Float64 and scientific text agree', len({md5s(r, (0,))[0] for r in (pg, ch, ex)}) == 1)
    check('floats keep 9 digits', md5s([['2.5e+18']], (0,)) == md5s([['2500000000000000000.000']], (0,)))
    check('floats differ at 1e-6', md5s([['1234.567']], (0,))[0] != md5s([['1234.568']], (0,))[0])
    check('integers are exact', md5s([['9007199254740993']])[0] != md5s([['9007199254740992']])[0])
    check('integer columns are not rounded', md5s([['1234567891']])[0] != md5s([['1234567892']])[0])
    a, b = [['x', '1'], ['y', '2']], [['y', '2'], ['x', '1']]
    check('row order is canonical', md5s(a)[0] == md5s(b)[0] and md5s(a)[1] != md5s(b)[1])
    check('NULL is not an empty string', md5s([[None]])[0] != md5s([['']])[0])
    check('row count', md5s(a)[2] == 2)
    check('first_diff', first_diff(a, [['x', '1'], ['y', '3']]).startswith('row 1'))
    check('first_diff equal', first_diff(a, b) == '')
    try:
        normalize([['abc']], (0,))
        check('a non-number in a float column is an error', False)
    except ValueError:
        pass
    if bad:
        sys.exit('clickbench_md5 selftest failed: %s' % ', '.join(bad))
    print('clickbench_md5 selftest: ok (13 checks)')


if __name__ == '__main__':
    selftest() if sys.argv[1:] == ['--selftest'] else sys.exit(__doc__)
