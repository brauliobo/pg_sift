# Exact BM25 Full-Text Search (`sift`)

The `sift` index access method adds exact BM25 ranked full-text search inside PostgreSQL transactions — supporting boolean logic, phrases, prefixes, wildcards, regular expressions, fuzzy matching, filter pushdown, weighted fields, and exact facets. BM25 uses on-disk Format `1.7` (fully backward-compatible from 5.x; no `REINDEX` required when upgrading to 6.0).

## 1. Creating a `sift` Index and `INCLUDE` Filters

```sql
CREATE INDEX docs_body ON docs
  USING sift (body)
  INCLUDE (category, published)
  WITH (stemmer = 'en');

SELECT id, title, sift.score(ctid) AS score
FROM docs
WHERE body @@> '(battery OR charger) AND "friendly support" NOT squeaky'
  AND category = 1
ORDER BY sift.score(ctid) DESC
LIMIT 10;
```

- **One key per index:** Index a `text` column or an immutable expression (`coalesce(title, '') || ' ' || coalesce(body, '')`). `NULL` values are skipped.
- **`INCLUDE (col1, ...)` (up to 8 scalar columns):** Stores `boolean`, `smallint`, `integer`, `bigint`, `date`, `timestamp`, and `timestamptz` columns inside the BM25 segment with per-superblock min/max bounds. The ranker skips entire superblocks outside the filter range and drops non-matching documents before scoring (`EXPLAIN` shows `Include Tests: N`). Supported on heap tables.
- **Filter pushdown (`sift.filter_pushdown = on`):** For columns indexed by separate B-tree, BRIN, or GIN indexes, the planner builds a bitmap mask and feeds it into the ranked scan when cheaper than post-filtering (`off`, `on`, or `force`).
- **Index-only `count(*)`:** `SELECT count(*) FROM docs WHERE body @@> 'wireless';` runs as a `SiftCount` scan without heap fetches (gated by `sift.enable_scan`).

## 2. SiftQL Query Syntax

`AND`, `OR`, and `NOT` are uppercase keywords; whitespace acts as `AND`. Precedence is `NOT` > `AND` > `OR`. A query may be only a `NOT`: `NOT wire` matches every row with a text that `wire` does not (rows with a NULL text never match), and scores `0`. Queries may hold up to 65,535 leaves, 1,024 levels of nesting, phrases of 4,096 terms and boosts up to `1e18`.

| Syntax | Example | Behavior |
|---|---|---|
| Boolean / grouping | `(case OR clip) NOT squeaky` | Exact boolean match and BM25 sum |
| Set of alternatives | `{case clip squeaky}` | Shorthand `OR` over words or patterns |
| Phrase | `"friendly support"` | Adjacent terms in order (uses stored positions; heap recheck if `positions = off`) |
| Prefix & wildcard | `batt*`, `sq?eaky` | Expands dictionary terms (`prefix_index` accelerates short prefixes) |
| Regular expression | `/sq.*y/` | PostgreSQL regex without back-references |
| Fuzzy match | `frendly~1`, `headphone~2` | Edit distance 1–2 (first character fixed; clamped by word length) |
| Term boost | `friendly^3` | Multiplies term BM25 contribution by boost factor |
| Array of queries | `body @@> ANY(ARRAY['a','b'])` | Equivalent to `a OR b` (ranked and counted) |

Patterns expanding past `sift.max_expansions` (default `512`) still match every document accurately, scored as a single combined term.

## 3. Ranking and `sift.score(ctid)`

- `ORDER BY sift.score(ctid) DESC LIMIT k` runs a block-max WAND ranked scan that skips blocks unable to enter the top `k` (ties break by `ctid`).
- `WHERE sift.score(ctid) > 0.5` pushes simple threshold comparisons (`>`, `>=`, `<`, `<=`) against constants or parameters directly into the scan (`Score Filter`). Any other condition on the score is tested on the row the scan scores.
- `ORDER BY category, sift.score(ctid) DESC LIMIT k` scores only rows tied with the `k`-th key value (`Order: category, score DESC`).
- `UPDATE`, `DELETE`, `MERGE` and `SELECT ... FOR UPDATE / FOR SHARE` read `sift.score(ctid)` in their own scan of the table, including a row a concurrent transaction changed (it is matched and scored again from its new text). The score is not available in `RETURNING`: select it in a subquery or `WITH` query of the statement.
- A table that no `sift` index scan serves (no index, `sift.enable_scan = off`, a partition without an index, row-level security without `sift.rls_scores`) is scored by `SiftScoreScan`, which reads the heap twice: the first pass counts the documents and the document frequencies, the second scores the rows with the formulas of the index. The statistics are those of the rows the role may see. A leaf that matches several words (prefix, wildcard, regular expression, fuzzy) weighs by the number of documents it matches, and the BM25 parameters are the defaults. Without a `@@>` condition every score is `0`.

## 4. Combining BM25 Search with `sift_col` Analytics

When a table has both a `sift` index and a `sift_col` index, `SiftAgg` uses the BM25 match set as the row selection for grouped aggregations:

```sql
SELECT category, count(*), avg(price)
FROM items
WHERE body @@> 'wireless headphones'
GROUP BY category;
```

## 5. Analyzers, Stemmers, and Prefixes

| Reloption | Default | Purpose |
|---|---|---|
| `analyzer` | `'text'` | `'text'` (Unicode word tokenizer) or `'keyword'` (whole string as one token) |
| `stemmer` | `''` | `'en'` (built-in Snowball English) or any `pg_ts_config` (`'german'`, `'public.my_cfg'`) |
| `case_folding` / `accent_folding` | `true` / `true` | Lowercases and strips diacritics (`résumé` → `resume`; accent folding defaults off for `pg_ts_config`) |
| `positions` | `true` | Stores term positions for fast phrase queries (`false` shrinks index; phrases recheck heap) |
| `prefix_index` | `0` | Indexes prefixes of length `1..N` (`0..15`) for sub-millisecond autocomplete (`h*`) |
| `k1` / `b` | `1.2` / `0.75` | Standard Okapi BM25 saturation and length-normalization parameters |

Inspect tokenization at any time with `SELECT token, pos FROM sift.tokenize('Running Cafés', stemmer => 'en');`. Because analyzer settings are immutable on disk, change them via `CREATE INDEX CONCURRENTLY` and rename.

## 6. Weighted Fields (`sift.sections` and `sift.field_query`)

Concatenate multiple columns into named sections using `sift.sections(...)` and declare section names in `fields`:

```sql
CREATE INDEX items_fts ON items
  USING sift (sift.sections(title, body, tags))
  WITH (fields = 'title,body,tags', stemmer = 'en');

SELECT id, title, sift.score(ctid) AS score
FROM items
WHERE sift.sections(title, body, tags)
  @@> sift.field_query('wireless headphones', 'title^3, body, tags^0.5')
ORDER BY sift.score(ctid) DESC
LIMIT 10;
```

Each term outside an explicit `field:` qualifier runs across every listed field with its weight (`0 < w <= 10000`), summing per-field BM25 scores (equivalent to Elasticsearch `most_fields`). A phrase is scoped like a word (`title:"red wire"` is held by the title alone); an unscoped phrase never holds across the boundary of two sections. Regexes and unscoped `NOT` terms match the whole document.

## 7. Exact Facets (`sift.facet_counts`)

Compute multiple exact, multi-select facets (terms, ranges, histograms, date histograms, arrays) in one snapshot-consistent call. When a `sift_col` index covers the faceted columns, each facet runs through `SiftAgg`.

```sql
SELECT * FROM sift.facet_counts('items', 'title', 'wireless',
  '[{"expr":"brand","size":10,"missing":true},
    {"expr":"price","edges":[100,300]},
    {"expr":"added","unit":"month","fill":true},
    {"expr":"tags","array":true}]',
  'active', hits => true);
```

| JSON Key | Purpose |
|---|---|
| `expr` / `name` | Column expression to facet on and optional output label |
| `type` | `terms` (default), `range` (`edges`), `histogram` (`interval`, `offset`), `date_histogram` (`unit`) |
| `size` / `order` | Top N buckets for `terms`; order by `count`, `count asc`, `value`, `value desc` |
| `missing` / `array` / `fill` | Include `NULL` bucket; count array elements once per row; emit empty buckets (max 10,000) |
| `selected` / `filter` | Active user selections for multi-select faceting (excluded from own facet's scan) |

Inspect the generated query with `sift.facet_sql(...)`.

## 8. Row-Level Security (`sift.rls_scores`)

Roles subject to RLS policies do not use ranked index scans by default because BM25 corpus statistics (document count, term document frequencies, average length) span the whole index. Their `sift.score()` is computed by `SiftScoreScan` from the rows the role may see.

- **Opt-in:** A superuser can enable ranked scans for a role via `ALTER ROLE app SET sift.rls_scores = on;`. Policy predicates run before query filters, hidden rows are never returned, and `EXPLAIN` omits hidden-row counters.
- **Security caveat:** Because BM25 scores depend on global term frequencies, a user who knows their own document lengths can infer corpus-level term prevalence. Keep `sift.rls_scores = off` or partition one index per tenant when tenants must not observe corpus statistics.

## 9. Standalone SQLite and MariaDB Ports

The portable BM25 search core (`src/portable.list`) also ships as standalone extensions for [SQLite](../sqlite/README.md) and [MariaDB](../mariadb/README.md). The columnar analytics engine (`sift_col`, `sift_table`, `SiftAgg`) is PostgreSQL-only.
