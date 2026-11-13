"""connected:connected-b: the connection of the child's sift_col index to the sift_col index of its parent. For each n:1 link of the example the
child's own index of sift.sql that holds the foreign key gets `connect = 'fk -> parent(key)'` (a new child index on the key alone when sift.sql has none),
and a parent index that holds the key and the attributes the queries read is added only where sift.sql has none. The join reads the parent's columns from the
leaves of the parent's own index. A link with an expression or
composite key, a parent key that is not an integer primary key, or the table itself as its parent stays unconnected, and so does an example whose fact
is a sift_table (a sift_col index needs a heap table)."""
import re
from collections import defaultdict

IDENT = re.compile(r'^\w+$')
INDEX = re.compile(r'INDEX\s+(\w+)\s+ON\s+(\w+)\s+USING\s+sift_col\s*\(([^)]*)\)', re.I)


def names(text):
    return [c.strip() for c in text.split(',') if c.strip()]


def column_types(schema, table):
    body = re.search(r'CREATE TABLE\s+%s\s*\((.*?)\)\s*(?:PARTITION|USING|;)' % table, schema, re.I | re.S)
    parts, depth, cur = [], 0, ''
    for ch in body.group(1) if body else '':
        depth += (ch == '(') - (ch == ')')
        if ch == ',' and not depth:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    return {p.split()[0]: p.split(None, 1)[1].lower() for p in parts + [cur] if len(p.split()) > 1}


def integer_column(schema, table, col):
    return column_types(schema, table).get(col, '').split()[0:1] in (['int'], ['integer'], ['int2'], ['int4'], ['int8'], ['bigint'], ['smallint'])


def unique_column(schema, table, col):
    return 'primary key' in column_types(schema, table).get(col, '')


def declaration(l):
    return '%s -> %s(%s)' % (l.child_col, l.parent_table, l.parent_col)


def ddl(ex):
    if ex.meta.get('storage') == 'sift_table':
        return None
    have = [(n, t, names(c)) for n, t, c in INDEX.findall(ex.text('sift.sql'))]
    schema = ex.text('schema.sql')
    links = [l for l in ex.links if l.relation == 'n:1' and l.child_table != l.parent_table and IDENT.match(l.child_col) and IDENT.match(l.parent_col)
             and integer_column(schema, l.parent_table, l.parent_col) and unique_column(schema, l.parent_table, l.parent_col)
             and integer_column(schema, l.child_table, l.child_col)]
    out, wanted = [], defaultdict(list)
    for l in links:
        wanted[l.parent_table] += [l.parent_col] + names(l.reads)
    for table, cols in wanted.items():
        cols = list(dict.fromkeys(cols))
        if not any(set(cols) <= set(c) for _, t, c in have if t == table):
            out.append('CREATE INDEX connb_%s_col ON %s USING sift_col (%s);' % (table, table, ', '.join(cols)))
    groups = defaultdict(list)
    for l in links:
        name = next((n for n, t, c in have if t == l.child_table and l.child_col in c), None)
        groups[(l.child_table, name)].append(l)
    for (table, name), group in groups.items():
        option = '; '.join(declaration(l) for l in group)
        if name and re.search(r'CREATE TABLE\s+%s\b[^;]*PARTITION BY' % table, schema, re.I | re.S):
            cols = next(c for n, _, c in have if n == name)
            out.append("DROP INDEX %s;\nCREATE INDEX %s ON %s USING sift_col (%s) WITH (connect = '%s');" % (name, name, table, ', '.join(cols), option))
        elif name:
            out.append("ALTER INDEX %s SET (connect = '%s');\nREINDEX INDEX %s;" % (name, option, name))
        else:
            out.append("CREATE INDEX connb_%s_col ON %s USING sift_col (%s) WITH (connect = '%s');" %
                       (table, table, ', '.join(dict.fromkeys(l.child_col for l in group)), option))
    return '\n'.join(out) or None


def session_sql():
    return []
