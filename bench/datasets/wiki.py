"""Simple English Wikipedia articles -> COPY text for the `src` staging table (bench/sql/schema.sql).

    python3 -I wiki.py simplewiki-DATE-pages-articles.xml.bz2 articles.tsv [MAX_ARTICLES]

One row per main-namespace, non-redirect article: id, title + cleaned text, category id (hash of the
first category), revision date, likes (0), url. The cleaning is deliberately crude (templates, tables,
references, links and markup are dropped); it only has to leave plain prose. Wikipedia text is CC BY-SA 4.0.
"""
import bz2, re, sys, zlib
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


def main(src, dst, limit):
    n = 0
    out = open(dst, 'w', encoding='utf-8')
    for _, el in ET.iterparse(bz2.open(src), events=('end',)):
        if el.tag != NS + 'page':
            continue
        if el.findtext(NS + 'ns') == '0' and el.find(NS + 'redirect') is None:
            rev = el.find(NS + 'revision')
            raw = rev.findtext(NS + 'text') or ''
            body = clean(raw)
            if len(body) >= 200:
                title = el.findtext(NS + 'title')
                cats = CATEGORY.findall(raw)
                n += 1
                out.write('\t'.join([str(n), esc(title + '\n' + body), str(zlib.crc32(cats[0].encode()) % 1000) if cats else '\\N',
                                     (rev.findtext(NS + 'timestamp') or '')[:10] or '\\N', '0',
                                     esc('https://simple.wikipedia.org/wiki/' + title.replace(' ', '_'))]) + '\n')
                if limit and n >= limit:
                    break
        el.clear()
    print(n, 'articles')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
