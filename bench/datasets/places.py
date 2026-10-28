"""GeoNames places -> COPY text for the `places` table plus a deterministic autocomplete query set.

    python3 -I places.py DIR CITIES_FILE OUTDIR [SEED]

DIR holds countryInfo.txt, admin1CodesASCII.txt and featureCodes_en.txt, CITIES_FILE is a GeoNames
dump such as cities500.txt. Writes OUTDIR/places.tsv (id, name, alternate_names, admin1, country,
country_code, feature, population) and OUTDIR/queries.tsv (id, class, q): 300 typed queries per class
sampled from the places themselves, the way a user types them:
  prefix-1 to prefix-4  first word of a name cut to 1-4 letters      whole-word     a whole first word
  word-prefix-2         two words, the last cut to 2 letters         word-prefix-1  the same cut to 1 letter
  name-region-country   name word, region word, country word        name-country   name word and country word
GeoNames data is CC BY 4.0 (https://www.geonames.org).
"""
import csv, os, random, re, sys

WORD = re.compile(r"[^\W_]+", re.U)


def esc(t):
    return t.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n').replace('\r', '')


def load(path, cols, skip_comments=True):
    with open(path, encoding='utf-8') as f:
        return [row for row in csv.reader((l for l in f if not (skip_comments and l.startswith('#'))), delimiter='\t') if len(row) >= cols]


def main(d, cities, out, seed):
    country = {r[0]: r[4] for r in load(os.path.join(d, 'countryInfo.txt'), 5)}
    admin1 = {r[0]: r[1] for r in load(os.path.join(d, 'admin1CodesASCII.txt'), 2, False)}
    feature = {r[0].replace('.', ''): r[1] for r in load(os.path.join(d, 'featureCodes_en.txt'), 2, False)}
    rows = []
    for r in load(cities, 15, False):
        gid, name, ascii_, alt, cc, a1, fc, fcode, pop = r[0], r[1], r[2], r[3], r[8], r[10], r[6], r[7], r[14]
        rows.append((int(gid), name, alt.replace(',', ' '), admin1.get('%s.%s' % (cc, a1), ''), country.get(cc, ''), cc,
                     feature.get(fc + fcode, ''), int(pop or 0), ascii_))
    rows.sort()
    with open(os.path.join(out, 'places.tsv'), 'w', encoding='utf-8') as f:
        for r in rows:
            f.write('\t'.join([str(r[0])] + [esc(x) for x in r[1:7]] + [str(r[7])]) + '\n')
    rnd = random.Random(seed)
    sample = lambda: rnd.choice(rows)
    words = lambda s: [w.lower() for w in WORD.findall(s)]
    queries = []

    def emit(cls, fn):
        k = 0
        while k < 300:
            r = sample()
            nw, rw, cw = words(r[8]), words(r[3]), words(r[4])
            q = fn(nw, rw, cw)
            if q:
                queries.append((cls, q))
                k += 1

    for k in (1, 2, 3, 4):
        emit('prefix-%d' % k, lambda nw, rw, cw, k=k: nw[0][:k] if nw else None)
    emit('whole-word', lambda nw, rw, cw: nw[0] if nw else None)
    emit('word-prefix-2', lambda nw, rw, cw: nw[0] + ' ' + (rw or cw or ['x'])[0][:2] if nw and (rw or cw) else None)
    emit('word-prefix-1', lambda nw, rw, cw: nw[0] + ' ' + (rw or cw or ['x'])[0][:1] if nw and (rw or cw) else None)
    emit('name-region-country', lambda nw, rw, cw: ' '.join([nw[0], rw[0], cw[0]]) if nw and rw and cw else None)
    emit('name-country', lambda nw, rw, cw: nw[0] + ' ' + cw[0] if nw and cw else None)
    with open(os.path.join(out, 'queries.tsv'), 'w', encoding='utf-8') as f:
        for i, (cls, q) in enumerate(queries, 1):
            f.write('%d\t%s\t%s\n' % (i, cls, esc(q)))
    print(len(rows), 'places,', len(queries), 'queries')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 42)
