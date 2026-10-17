"""Simple English Wikipedia articles -> COPY text for the `src` staging table (bench/sql/schema.sql).

    python3 -I wiki.py simplewiki-DATE-pages-articles.xml.bz2 articles.tsv.gz [MAX_ARTICLES]

One row per main-namespace, non-redirect article: id, title + cleaned text, category id (hash of the
first category), revision date, likes (0), url. The cleaning is deliberately crude (templates, tables,
references, links and markup are dropped); it only has to leave plain prose. Wikipedia text is CC BY-SA 4.0.

The output is gzip (a name ending in .gz) or plain text. The dump is decompressed by a bzip2 process (lbzip2 when installed), the pages
are cleaned by JOBS (default 4) worker processes, in batches whose results are written in page order: the file is byte for byte what
a single process writes.
"""
import collections, multiprocessing, os, re, shutil, subprocess, sys, zlib
import xml.etree.ElementTree as ET

NS = '{http://www.mediawiki.org/xml/export-0.11/}'
CATEGORY = re.compile(r'\[\[Category:([^\]|]+)', re.I)
COMMENT = re.compile(r'<!--.*?-->', re.S)
REF = re.compile(r'<ref[^>/]*/>|<ref[^>]*>.*?</ref>', re.S | re.I)
TAGS = re.compile(r'</?[a-zA-Z][^>]*>')
TABLE = re.compile(r'\{\|.*?\|\}', re.S)
TEMPLATE = re.compile(r'\{\{[^{}]*\}\}')
FILELINK = re.compile(r'\[\[(?:File|Image|Category|[a-z]{2,3}):[^\[\]]*(?:\[\[[^\]]*\]\][^\[\]]*)*\]\]', re.I)
LINK = re.compile(r'\[\[(?:[^\]|]*\|)?([^\]]*)\]\]')
EXTLINK = re.compile(r'\[https?://\S+(?:\s+([^\]]*))?\]')
QUOTES = re.compile(r"'{2,}")
HEADING = re.compile(r'^=+\s*(.*?)\s*=+\s*$', re.M)
LISTMARK = re.compile(r'^[*#:;]+\s*', re.M)
SPACES = re.compile(r'[ \t]+')


def clean(text):
    text = COMMENT.sub('', text)
    text = REF.sub('', text)
    text = TABLE.sub('', text)
    for _ in range(6):                      # templates nest
        new = TEMPLATE.sub('', text)
        if new == text:
            break
        text = new
    text = FILELINK.sub('', text)
    text = LINK.sub(r'\1', text)
    text = EXTLINK.sub(lambda m: m.group(1) or '', text)
    text = TAGS.sub('', text)
    text = QUOTES.sub('', text)
    text = HEADING.sub(r'\1', text)
    text = LISTMARK.sub('', text)
    text = SPACES.sub(' ', text)
    return '\n'.join(line.strip() for line in text.split('\n') if line.strip())


def esc(t):
    return t.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n').replace('\r', '')


def convert(batch):
    """[(title, raw wikitext, timestamp)] -> the COPY text lines (without the id) of the pages that leave 200 characters of prose"""
    out = []
    for title, raw, stamp in batch:
        body = clean(raw)
        if len(body) >= 200:
            cats = CATEGORY.findall(raw)
            out.append((esc(title + '\n' + body), str(zlib.crc32(cats[0].encode()) % 1000) if cats else '\\N', stamp[:10] or '\\N',
                        esc('https://simple.wikipedia.org/wiki/' + title.replace(' ', '_'))))
    return out


def batches(dump, size=64):
    """the main-namespace, non-redirect pages of the dump in batches of (title, raw text, timestamp)"""
    cmd = shutil.which('lbzip2') or 'bzip2'
    with subprocess.Popen([cmd, '-dc', dump], stdout=subprocess.PIPE) as proc:
        batch = []
        for _, el in ET.iterparse(proc.stdout, events=('end',)):
            if el.tag != NS + 'page':
                continue
            if el.findtext(NS + 'ns') == '0' and el.find(NS + 'redirect') is None:
                rev = el.find(NS + 'revision')
                batch.append((el.findtext(NS + 'title'), rev.findtext(NS + 'text') or '', rev.findtext(NS + 'timestamp') or ''))
                if len(batch) == size:
                    yield batch
                    batch = []
            el.clear()
        if batch:
            yield batch
        proc.kill()


def main(src, dst, limit):
    n = 0
    jobs = int(os.environ.get('JOBS', '4'))
    sink = subprocess.Popen(['gzip', '-n', '-3'], stdin=subprocess.PIPE, stdout=open(dst, 'wb')) if dst.endswith('.gz') else None
    out = sink.stdin if sink else open(dst, 'wb')
    pending, pool = collections.deque(), multiprocessing.get_context('fork').Pool(jobs)

    def emit(rows):
        nonlocal n
        for title_body, cat, stamp, url in rows:
            if limit and n >= limit:
                return True
            n += 1
            out.write(('\t'.join([str(n), title_body, cat, stamp, '0', url]) + '\n').encode('utf-8'))
        return bool(limit and n >= limit)

    done = False
    for batch in batches(src):
        pending.append(pool.apply_async(convert, (batch,)))
        while len(pending) >= 4 * jobs and not done:   # bounded: the reader does not run ahead of the workers
            done = emit(pending.popleft().get())
        if done:
            break
    while pending and not done:
        done = emit(pending.popleft().get())
    pool.terminate()
    out.close()
    if sink:
        sink.wait()
    print(n, 'articles')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
